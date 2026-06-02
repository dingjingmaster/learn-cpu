/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/*
 * 二级 LLVM JIT 驱动。
 *
 * T2C 会把热点基本块转换为 LLVM IR，调用 LLVM 优化和执行引擎生成宿主机器码。
 * 本文件管理 LLVM 模块、执行引擎、inline cache、jit cache 和后台编译结果，
 * 并与 cache_lock 协作处理系统模式下的地址空间失效。
 */

#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <llvm/Config/llvm-config.h>
#include <stdlib.h>

/* LLVM 版本兼容性检查。
 * T2C 需要 LLVM 18-21，原因是依赖以下 API：
 * - LLVMRunPasses（新 pass manager，LLVM 13 加入）
 * - 9 参数版本 LLVMGetInlineAsm（CanThrow 参数在 LLVM 13 加入）
 * - LLVMBuildAtomicRMW（LLVM 18-21 中保持稳定）
 * - LLVMCreateTargetMachine（LLVM 18-21 中保持稳定）
 *
 * NOTE：LLVM 22+ 可能会用 ORC JIT 取代 MCJIT。
 * 升级到 LLVM 21 以后版本时，需要检查：
 * - MCJIT 弃用状态
 * - LLVMGetInlineAsm 签名变化
 * - aarch64 JIT 的 code model 默认值
 */
#if LLVM_VERSION_MAJOR < 18
#error "T2C 需要 LLVM 18 或更高版本。当前 LLVM 版本为 " LLVM_VERSION_STRING
#elif LLVM_VERSION_MAJOR > 21
#warning "检测到 LLVM 版本大于 21。T2C 目前测试覆盖 LLVM 18-21。"
#endif

#include "jit.h"
#include "mpool.h"
#include "riscv_private.h"

#define MAX_BLOCKS 8152

struct LLVM_block_map_entry {
    uint32_t pc;
    LLVMBasicBlockRef block;
};

struct LLVM_block_map {
    uint32_t count;
    struct LLVM_block_map_entry map[MAX_BLOCKS];
};

FORCE_INLINE void t2c_block_map_insert(struct LLVM_block_map *map,
                                       LLVMBasicBlockRef *entry,
                                       uint32_t pc)
{
    struct LLVM_block_map_entry map_entry = {
        .block = *entry,
        .pc = pc,
    };
    map->map[map->count++] = map_entry;
    return;
}

FORCE_INLINE LLVMBasicBlockRef t2c_block_map_search(struct LLVM_block_map *map,
                                                    uint32_t pc)
{
    for (uint32_t i = 0; i < map->count; i++) {
        if (map->map[i].pc == pc) {
            return map->map[i].block;
        }
    }
    return NULL;
}

/* T2C_OP 为每条 RISC-V 指令生成代码，并批量更新周期计数。
 *
 * 周期计数优化：不再每条指令都更新 rv->csr_cycle，而是在本地计数器（alloca）
 * 中累加，只在基本块出口写回 csr_cycle。这显著减少内存访问；LLVM 的 mem2reg
 * pass 会把该 alloca 提升为寄存器，使逐指令累加几乎没有额外成本。
 *
 * insn_counter 参数是在 t2c_compile() 函数入口创建的 alloca。任何
 * LLVMBuildRetVoid() 之前，都必须调用 T2C_STORE_TIMER，把累计值刷新到
 * rv->csr_cycle。
 */
#define T2C_OP(inst, code)                                                     \
    static void t2c_##inst(                                                    \
        LLVMBuilderRef *builder UNUSED, LLVMTypeRef *param_types UNUSED,       \
        LLVMValueRef start UNUSED, LLVMBasicBlockRef *entry UNUSED,            \
        LLVMBuilderRef *taken_builder UNUSED,                                  \
        LLVMBuilderRef *untaken_builder UNUSED, riscv_t *rv UNUSED,            \
        uint64_t mem_base UNUSED, block_t *block UNUSED, rv_insn_t *ir UNUSED, \
        LLVMValueRef insn_counter UNUSED)                                      \
    {                                                                          \
        /* 递增本地指令计数器，之后会被 LLVM 提升为寄存器。 */                \
        LLVMValueRef cnt =                                                     \
            LLVMBuildLoad2(*builder, LLVMInt64Type(), insn_counter, "");       \
        cnt = LLVMBuildAdd(*builder, cnt,                                      \
                           LLVMConstInt(LLVMInt64Type(), 1, false), "");       \
        LLVMBuildStore(*builder, cnt, insn_counter);                           \
        code;                                                                  \
    }

#define T2C_LLVM_GEN_ADDR(reg, rv_member, ir_member)                          \
    FORCE_INLINE LLVMValueRef t2c_gen_##reg##_addr(                           \
        LLVMValueRef start, LLVMBuilderRef *builder, UNUSED rv_insn_t *ir)    \
    {                                                                         \
        LLVMValueRef offset = LLVMConstInt(                                   \
            LLVMInt32Type(),                                                  \
            offsetof(riscv_t, rv_member) / sizeof(int) + ir_member, true);    \
        return LLVMBuildInBoundsGEP2(*builder, LLVMInt32Type(),               \
                                     LLVMGetParam(start, 0), &offset, 1, ""); \
    }

T2C_LLVM_GEN_ADDR(rs1, X, ir->rs1);
T2C_LLVM_GEN_ADDR(rs2, X, ir->rs2);
T2C_LLVM_GEN_ADDR(rd, X, ir->rd);
#if RV32_HAS(EXT_C)
T2C_LLVM_GEN_ADDR(ra, X, rv_reg_ra);
T2C_LLVM_GEN_ADDR(sp, X, rv_reg_sp);
#endif
T2C_LLVM_GEN_ADDR(PC, PC, 0);
T2C_LLVM_GEN_ADDR(csr_cycle, csr_cycle, 0);

#define T2C_LLVM_GEN_STORE_IMM32(builder, val, addr) \
    LLVMBuildStore(builder, LLVMConstInt(LLVMInt32Type(), val, true), addr)

#define T2C_LLVM_GEN_LOAD_VMREG(reg, size, addr) \
    LLVMValueRef val_##reg =                     \
        LLVMBuildLoad2(*builder, LLVMInt##size##Type(), addr, "");

#define T2C_LLVM_GEN_ALU32_IMM(op, dst, imm) \
    LLVMBuild##op(*builder, dst, LLVMConstInt(LLVMInt32Type(), imm, true), "")

#define T2C_LLVM_GEN_ALU64_IMM(op, dst, imm) \
    LLVMBuild##op(*builder, dst, LLVMConstInt(LLVMInt64Type(), imm, true), "")

#define T2C_LLVM_GEN_CMP(cond, rs1, rs2) \
    LLVMValueRef cmp = LLVMBuildICmp(*builder, LLVMInt##cond, rs1, rs2, "")

#define T2C_LLVM_GEN_CMP_IMM32(cond, rs1, imm)      \
    LLVMValueRef cmp =                              \
        LLVMBuildICmp(*builder, LLVMInt##cond, rs1, \
                      LLVMConstInt(LLVMInt32Type(), imm, false), "")

/* 在基本块退出前，把累计指令数写入 rv->csr_cycle。
 * 每次 LLVMBuildRetVoid() 之前都会调用该宏来刷新计数器。
 * insn_counter 是一个 alloca，LLVM 的 mem2reg 会把它提升为寄存器。
 *
 * 这里使用原子加（LLVMBuildAtomicRMW）保证线程安全：
 * - 调试器或监视器并发读取 csr_cycle 时，不会看到撕裂读
 * - 相比非原子的 load-add-store 序列，只需要一条原子指令
 * - Monotonic 顺序足够，因为不需要与其他内存操作同步
 *
 * 使用 csr_cycle 而不是 timer 可以保证：
 * - SYSTEM 模式：定时器中断仍然正确工作（timer = csr_cycle + offset）
 * - 非 SYSTEM 模式：RDCYCLE 指令返回准确计数
 */
#define T2C_STORE_TIMER(bldr, start_val, counter)                         \
    do {                                                                  \
        LLVMValueRef _cycle_ptr =                                         \
            t2c_gen_csr_cycle_addr(start_val, &(bldr), NULL);             \
        LLVMValueRef _cnt =                                               \
            LLVMBuildLoad2(bldr, LLVMInt64Type(), counter, "");           \
        LLVMBuildAtomicRMW(bldr, LLVMAtomicRMWBinOpAdd, _cycle_ptr, _cnt, \
                           LLVMAtomicOrderingMonotonic, false);           \
    } while (0)

UNUSED FORCE_INLINE LLVMValueRef t2c_gen_mem_loc(LLVMValueRef start,
                                                 LLVMBuilderRef *builder,
                                                 UNUSED rv_insn_t *ir,
                                                 uint64_t mem_base)
{
    LLVMValueRef val_rs1 =
        LLVMBuildZExt(*builder,
                      LLVMBuildLoad2(*builder, LLVMInt32Type(),
                                     t2c_gen_rs1_addr(start, builder, ir), ""),
                      LLVMInt64Type(), "");
    LLVMValueRef addr =
        T2C_LLVM_GEN_ALU64_IMM(Add, val_rs1, ir->imm + mem_base);
    addr = LLVMBuildIntToPtr(*builder, addr,
                             LLVMPointerType(LLVMInt32Type(), 0), "");
    return addr;
}

/* 从 rv->io 结构体中加载函数指针并调用。
 *
 * byte_offset 参数是从 riscv_t 起始位置到目标函数指针的字节偏移。调用方应使用：
 *   - ecall：offsetof(riscv_t, io) + offsetof(riscv_io_t, on_ecall)
 *   - ebreak：offsetof(riscv_t, io) + offsetof(riscv_io_t, on_ebreak)
 *
 * 无论 RV32_HAS(SYSTEM) 是否启用，该做法都正确；系统模式会向 riscv_io_t
 * 追加额外 MMU 函数指针。
 *
 * 这里手动执行指针算术（PtrToInt -> Add -> IntToPtr）来计算正确地址，避免
 * GEP 与结构体布局不匹配的问题；该问题在 Apple Silicon 上可能导致崩溃。
 */
FORCE_INLINE void t2c_gen_call_io_func(LLVMValueRef start,
                                       LLVMBuilderRef *builder,
                                       LLVMTypeRef *param_types,
                                       size_t byte_offset)
{
    /* 将 rv 指针转为整数，加上偏移后再转回指针。 */
    LLVMValueRef rv_ptr = LLVMGetParam(start, 0);
    LLVMValueRef rv_int =
        LLVMBuildPtrToInt(*builder, rv_ptr, LLVMInt64Type(), "");
    LLVMValueRef offset_val = LLVMConstInt(LLVMInt64Type(), byte_offset, false);
    LLVMValueRef func_ptr_addr = LLVMBuildAdd(*builder, rv_int, offset_val, "");
    LLVMValueRef func_ptr_ptr = LLVMBuildIntToPtr(
        *builder, func_ptr_addr,
        LLVMPointerType(LLVMPointerType(LLVMVoidType(), 0), 0), "");

    /* 加载函数指针并发起调用。 */
    LLVMValueRef io_func = LLVMBuildLoad2(
        *builder, LLVMPointerType(LLVMVoidType(), 0), func_ptr_ptr, "io_func");
    LLVMBuildCall2(*builder,
                   LLVMFunctionType(LLVMVoidType(), param_types, 1, 0), io_func,
                   &rv_ptr, 1, "");
}

static LLVMTypeRef t2c_jit_cache_func_type;
static LLVMTypeRef t2c_jit_cache_struct_type;
static LLVMTypeRef t2c_inline_cache_struct_type;

#include "t2c_template.c"
#undef T2C_OP

static const void *dispatch_table[] = {
/* RV32 指令。 */
#define _(inst, can_branch, insn_len, translatable, reg_mask) \
    [rv_insn_##inst] = t2c_##inst,
    RV_INSN_LIST
#undef _
/* 宏操作融合指令。 */
#define _(inst) [rv_insn_##inst] = t2c_##inst,
        FUSE_INSN_LIST
#undef _
};

FORCE_INLINE bool t2c_insn_is_terminal(uint8_t opcode)
{
    switch (opcode) {
    case rv_insn_ecall:
    case rv_insn_ebreak:
    case rv_insn_jalr:
    case rv_insn_mret:
#if RV32_HAS(SYSTEM)
    case rv_insn_sret:
#endif
#if RV32_HAS(EXT_C)
    case rv_insn_cjalr:
    case rv_insn_cjr:
    case rv_insn_cebreak:
#endif
        return true;
    }
    return false;
}

typedef void (*t2c_codegen_block_func_t)(LLVMBuilderRef *builder UNUSED,
                                         LLVMTypeRef *param_types UNUSED,
                                         LLVMValueRef start UNUSED,
                                         LLVMBasicBlockRef *entry UNUSED,
                                         LLVMBuilderRef *taken_builder UNUSED,
                                         LLVMBuilderRef *untaken_builder UNUSED,
                                         riscv_t *rv UNUSED,
                                         uint64_t mem_base UNUSED,
                                         block_t *block UNUSED,
                                         rv_insn_t *ir UNUSED,
                                         LLVMValueRef insn_counter UNUSED);

static void t2c_trace_ebb(LLVMBuilderRef *builder,
                          LLVMTypeRef *param_types UNUSED,
                          LLVMValueRef start,
                          LLVMBasicBlockRef *entry,
                          riscv_t *rv,
                          block_t *block,
                          set_t *set,
                          struct LLVM_block_map *map,
                          LLVMValueRef insn_counter)
{
    rv_insn_t *ir = block->ir_head;

    if (set_has(set, ir->pc))
        return;
    set_add(set, ir->pc);
    t2c_block_map_insert(map, entry, ir->pc);
    LLVMBuilderRef tk = NULL, utk = NULL;

    /* 入口处只获取一次 mem_base，避免每条指令重复读取。 */
    vm_attr_t *priv = PRIV(rv);
    uint64_t mem_base = (uint64_t) ((memory_t *) priv->mem)->mem_base;

    while (1) {
        ((t2c_codegen_block_func_t) dispatch_table[ir->opcode])(
            builder, param_types, start, entry, &tk, &utk, rv, mem_base, block,
            ir, insn_counter);
        if (!ir->next)
            break;
        ir = ir->next;
    }

    if (!t2c_insn_is_terminal(ir->opcode)) {
        /* 对带 fall-through 后继的非分支指令，使用当前 builder。
         * 这类指令处理器不会创建独立的 taken/untaken 路径。
         * 分支处理器（jal、beq 等）会自行设置 tk/utk；非分支处理器
         * （lw、sw、add 等）不会。
         */
        if (!tk && ir->branch_taken)
            tk = *builder;
        if (!utk && ir->branch_untaken)
            utk = *builder;

        if (ir->branch_untaken) {
            /* 缓存 untaken_pc，避免与主线程发生竞争。 */
            uint32_t untaken_pc = ir->branch_untaken->pc;
            if (set_has(set, untaken_pc)) {
                LLVMBuildBr(utk, t2c_block_map_search(map, untaken_pc));
            } else {
                block_t *blk = cache_get(rv->block_cache, untaken_pc, false);
                if (blk && blk->translatable
#if RV32_HAS(SYSTEM)
                    && blk->satp == block->satp
#endif
                ) {
                    LLVMBasicBlockRef untaken_entry =
                        LLVMAppendBasicBlock(start, "untaken_entry");
                    LLVMBuilderRef untaken_builder = LLVMCreateBuilder();
                    LLVMPositionBuilderAtEnd(untaken_builder, untaken_entry);
                    LLVMBuildBr(utk, untaken_entry);
                    t2c_trace_ebb(&untaken_builder, param_types, start,
                                  &untaken_entry, rv, blk, set, map,
                                  insn_counter);
                    LLVMDisposeBuilder(untaken_builder);
                }
            }
        }
        if (ir->branch_taken) {
            uint32_t taken_pc = ir->branch_taken->pc;
            if (set_has(set, taken_pc)) {
                LLVMBuildBr(tk, t2c_block_map_search(map, taken_pc));
            } else {
                /* 使用已保存的 taken_pc，而不是再次读取 ir->branch_taken->pc，
                 * 避免与主线程发生竞争。
                 */
                block_t *blk = cache_get(rv->block_cache, taken_pc, false);
                if (blk && blk->translatable
#if RV32_HAS(SYSTEM)
                    && blk->satp == block->satp
#endif
                ) {
                    LLVMBasicBlockRef taken_entry =
                        LLVMAppendBasicBlock(start, "taken_entry");
                    LLVMBuilderRef taken_builder = LLVMCreateBuilder();
                    LLVMPositionBuilderAtEnd(taken_builder, taken_entry);
                    LLVMBuildBr(tk, taken_entry);
                    t2c_trace_ebb(&taken_builder, param_types, start,
                                  &taken_entry, rv, blk, set, map,
                                  insn_counter);
                    LLVMDisposeBuilder(taken_builder);
                }
            }
        }
    }
}

void t2c_compile(riscv_t *rv, block_t *block, pthread_mutex_t *cache_lock)
{
    /* 已经编译过时直接跳过，这是防御性检查。 */
    if (ATOMIC_LOAD(&block->hot2, ATOMIC_ACQUIRE)) {
        pthread_mutex_unlock(cache_lock);
        return;
    }

    LLVMModuleRef module = LLVMModuleCreateWithName("my_module");
    /* 构造与 riscv_internal 布局匹配的 LLVM 结构体类型。
     *
     * 实际 riscv_internal 结构体布局（见 riscv_private.h）：
     *   1. bool halt（1 字节 + padding）
     *   2. uint32_t X[32]（128 字节）
     *   3. uint32_t PC（4 字节）
     *   4. uint64_t timer（8 字节）
     *   5. riscv_user_t data（指针，8 字节）
     *   6. riscv_io_t io（函数指针）
     *
     * 注意：启用 SYSTEM/EXT_F 等配置后，实际结构体中可能还有额外字段。
     * io 结构体偏移由 t2c_gen_call_io_func 中的 offsetof() 计算。
     */
    LLVMTypeRef io_members[] = {
        LLVMPointerType(LLVMVoidType(), 0), LLVMPointerType(LLVMVoidType(), 0),
        LLVMPointerType(LLVMVoidType(), 0), LLVMPointerType(LLVMVoidType(), 0),
        LLVMPointerType(LLVMVoidType(), 0), LLVMPointerType(LLVMVoidType(), 0),
        LLVMPointerType(LLVMVoidType(), 0), LLVMPointerType(LLVMVoidType(), 0),
        LLVMPointerType(LLVMVoidType(), 0), LLVMPointerType(LLVMVoidType(), 0),
        LLVMPointerType(LLVMVoidType(), 0), LLVMPointerType(LLVMVoidType(), 0)};
    LLVMTypeRef struct_io = LLVMStructType(io_members, 12, false);
    LLVMTypeRef arr_X = LLVMArrayType(LLVMInt32Type(), 32);
    /* 匹配 riscv_internal 的实际字段顺序。 */
    LLVMTypeRef rv_members[] = {
        LLVMInt8Type(),                     /* halt */
        arr_X,                              /* X[32] */
        LLVMInt32Type(),                    /* PC */
        LLVMInt64Type(),                    /* timer */
        LLVMPointerType(LLVMVoidType(), 0), /* data */
        struct_io                           /* io */
    };
    LLVMTypeRef struct_rv = LLVMStructType(rv_members, 6, false);
    LLVMTypeRef param_types[] = {LLVMPointerType(struct_rv, 0)};
    LLVMValueRef start =
        LLVMAddFunction(module, "t2c_block",
                        LLVMFunctionType(LLVMVoidType(), param_types, 1, 0));

    /* 通过 jit_cache 查找并调用 T2C 基本块的函数类型。
     * 必须匹配真实 T2C 基本块签名：void f(riscv_t *rv)。
     * 为保证跨基本块调用正确，这里使用指针类型，而不是 i64。
     */
    LLVMTypeRef t2c_args[1] = {LLVMPointerType(LLVMVoidType(), 0)};
    t2c_jit_cache_func_type =
        LLVMFunctionType(LLVMVoidType(), t2c_args, 1, false);

    /* jit_cache 结构体：{ uint32_t seq, [pad], uint64_t key, void *entry }
     * C 结构体中，seq 后面有 4 字节 padding，用于让 key 按 8 字节对齐。
     * LLVM 不会自动补这个 padding，因此这里显式添加 i32 pad。
     * 字段索引：0=seq，1=pad，2=key，3=entry。
     */
    LLVMTypeRef jit_cache_memb[4] = {LLVMInt32Type(), LLVMInt32Type(),
                                     LLVMInt64Type(),
                                     LLVMPointerType(LLVMVoidType(), 0)};
    t2c_jit_cache_struct_type = LLVMStructType(jit_cache_memb, 4, false);

    /* inline_cache 结构体：{ uint64_t key, void *entry }
     * 字段索引：0=key，1=entry。该布局天然对齐，不需要 padding。
     */
    LLVMTypeRef inline_cache_memb[2] = {LLVMInt64Type(),
                                        LLVMPointerType(LLVMVoidType(), 0)};
    t2c_inline_cache_struct_type = LLVMStructType(inline_cache_memb, 2, false);

    LLVMBasicBlockRef first_block = LLVMAppendBasicBlock(start, "first_block");
    LLVMBuilderRef first_builder = LLVMCreateBuilder();
    LLVMPositionBuilderAtEnd(first_builder, first_block);

    /* 在入口块中创建指令计数器 alloca，方便 mem2reg 提升。
     * LLVM 的 mem2reg pass 会把入口块中的 alloca 提升为 SSA 寄存器，消除逐指令
     * 内存访问。计数器初始值为 0，每个 T2C_OP 递增，只有基本块出口才更新时间。
     */
    LLVMValueRef insn_counter =
        LLVMBuildAlloca(first_builder, LLVMInt64Type(), "insn_counter");
    LLVMBuildStore(first_builder, LLVMConstInt(LLVMInt64Type(), 0, false),
                   insn_counter);

    LLVMBasicBlockRef entry = LLVMAppendBasicBlock(start, "entry");
    LLVMBuilderRef builder = LLVMCreateBuilder();
    LLVMPositionBuilderAtEnd(builder, entry);
    LLVMBuildBr(first_builder, entry);
    /* 在堆上分配 set，避免栈溢出。
     * 系统模式下 set_t 大小为 256KB（1024 * 32 * 8 字节），不适合放在栈上。
     */
    set_t *set = malloc(sizeof(set_t));
    if (!set) {
        rv_log_error("为 T2C 编译分配集合失败");
        LLVMDisposeBuilder(first_builder);
        LLVMDisposeBuilder(builder);
        LLVMDisposeModule(module);
        pthread_mutex_unlock(cache_lock);
        return;
    }
    set_reset(set);
    struct LLVM_block_map map;
    map.count = 0;
    /* 将自定义 IR 翻译为 LLVM IR。 */
    t2c_trace_ebb(&builder, param_types, start, &entry, rv, block, set, &map,
                  insn_counter);

    block->is_compiling = true; /* 标记该块繁忙，防止被淘汰。 */

    /* 昂贵的 LLVM 编译期间释放锁。
     * IR 翻译已经完成；在需要写回结果前，不再访问 block 字段。这样 SFENCE.VMA
     * 可以用较小延迟继续执行。
     */
    pthread_mutex_unlock(cache_lock);

    /* 将 LLVM IR 交给 LLVM 后端处理。 */
    char *error = NULL, *triple = LLVMGetDefaultTargetTriple();
    LLVMExecutionEngineRef engine;
    LLVMTargetRef target;
    LLVMLinkInMCJIT();
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
#if defined(__aarch64__)
    /* 初始化 asm parser，以支持 JIT 中的内联汇编。
     * t2c_jit_cache_helper 发射 ARM64 ISB 指令时需要它。
     */
    LLVMInitializeNativeAsmParser();
#endif
    if (LLVMGetTargetFromTriple(triple, &target, &error) != 0) {
        rv_log_fatal("创建 LLVM target 失败");
        abort();
    }
    /* JIT 代码使用 PIC 重定位模式，便于处理间接调用。
     * code model 选择：
     * - Apple Silicon（ARM64 macOS）：使用 Small model，避免 Large model 为
     *   64 位常量生成 movz/movk 序列时触发 MCJIT 问题。ARM64 寻址模式较受限，
     *   Large model 在这里容易出问题。
     * - 其他平台：按 LLVM MCJIT 推荐使用 Large model。
     */
#if defined(__aarch64__) && defined(__APPLE__)
    LLVMCodeModel code_model = LLVMCodeModelSmall;
#else
    LLVMCodeModel code_model = LLVMCodeModelLarge;
#endif
    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
        target, triple, LLVMGetHostCPUName(), LLVMGetHostCPUFeatures(),
        LLVMCodeGenLevelNone, LLVMRelocPIC, code_model);
    LLVMPassBuilderOptionsRef pb_option = LLVMCreatePassBuilderOptions();
    /* 对生成的 IR 运行 LLVM 优化 pass。
     *
     * 优化等级可通过 CONFIG_T2C_OPT_LEVEL（Kconfig）配置：
     *   O0：不优化（编译最快，仅用于调试）
     *   O1：基础优化（编译速度约比 O3 快 50%）
     *   O2：平衡编译耗时和运行性能
     *   O3：激进优化，运行性能最好（生产默认值）
     *
     * system_jit_defconfig 使用 O1，以加快 CI 启动测试。
     * jit_defconfig 使用 O3（默认值），面向生产运行性能。
     */
#ifndef CONFIG_T2C_OPT_LEVEL
#define CONFIG_T2C_OPT_LEVEL 3
#endif
    static_assert(CONFIG_T2C_OPT_LEVEL >= 0 && CONFIG_T2C_OPT_LEVEL <= 3,
                  "T2C 优化等级必须在 0-3 之间");
    static const char *const t2c_opt_passes[] = {
        "default<O0>",
        "default<O1>",
        "default<O2>",
        "default<O3>",
    };
    LLVMRunPasses(module, t2c_opt_passes[CONFIG_T2C_OPT_LEVEL], tm, pb_option);

    /* 使用 LLVMCreateMCJITCompilerForModule 并显式传入选项。
     * 不同于 LLVMCreateExecutionEngineForModule，该接口会尊重 code model 设置；
     * 对必须使用 Small model 的 Apple Silicon 来说这很关键。
     */
    struct LLVMMCJITCompilerOptions options;
    LLVMInitializeMCJITCompilerOptions(&options, sizeof(options));
    options.OptLevel = CONFIG_T2C_OPT_LEVEL;
    options.CodeModel = code_model;

    if (LLVMCreateMCJITCompilerForModule(&engine, module, &options,
                                         sizeof(options), &error) != 0) {
        rv_log_fatal("创建 MCJIT 执行引擎失败：%s", error);
        LLVMDisposeMessage(error);
        abort();
    }

    /* 取得函数指针，先存在局部变量里。
     * block->func 只在持有 cache_lock 时写入，避免与读取 block->func 的淘汰路径
     * 发生数据竞争。
     */
    exec_t2c_func_t func =
        (exec_t2c_func_t) LLVMGetPointerToGlobal(engine, start);

    /* 清理 LLVM 资源；module 的所有权已经交给 execution engine。 */
    LLVMDisposeBuilder(first_builder);
    LLVMDisposeBuilder(builder);
    LLVMDisposePassBuilderOptions(pb_option);
    LLVMDisposeTargetMachine(tm);
    LLVMDisposeMessage(triple);

    /* 重新获取锁以更新共享状态。
     * 所有 block 字段写入都必须在锁内完成，避免数据竞争。
     */
    pthread_mutex_lock(cache_lock);

    block->is_compiling = false;

    /* 防御性检查：如果 LLVM 未能生成代码，不要标记为已编译。
     * 同时必须销毁 engine，避免内存泄漏。
     */
    if (!func) {
        /* 检查基本块是否已被淘汰；如果是，则释放它和它的 IR。 */
        if (block->should_free) {
            /* 释放主线程在延迟淘汰期间跳过的 IR。 */
            for (rv_insn_t *ir = block->ir_head, *next_ir; ir; ir = next_ir) {
                next_ir = ir->next;
                if (ir->fuse)
                    mpool_free(rv->fuse_mp, ir->fuse);
                mpool_free(rv->block_ir_mp, ir);
            }
            mpool_free(rv->block_mp, block);
        }
        LLVMDisposeExecutionEngine(engine);
        pthread_mutex_unlock(cache_lock);
        free(set);
        return;
    }

    /* 检查编译期间基本块是否已被淘汰。
     * 如果已淘汰，当前线程负责释放它。
     */
    if (block->should_free) {
        /* 销毁由当前线程持有的 engine。 */
        LLVMDisposeExecutionEngine(engine);
        /* 释放主线程在延迟淘汰期间跳过的 IR。 */
        for (rv_insn_t *ir = block->ir_head, *next_ir; ir; ir = next_ir) {
            next_ir = ir->next;
            if (ir->fuse)
                mpool_free(rv->fuse_mp, ir->fuse);
            mpool_free(rv->block_ir_mp, ir);
        }
        mpool_free(rv->block_mp, block);
        pthread_mutex_unlock(cache_lock);
        free(set);
        return;
    }

#if RV32_HAS(SYSTEM)
    uint64_t key = (uint64_t) block->pc_start | ((uint64_t) block->satp << 32);

    /* 重新获取锁后检查 invalidated 标志。如果编译期间执行过 SFENCE.VMA，
     * 该标志会被置位且 jit_cache 已清空。此时不能重新加入过期条目，并需销毁
     * engine 以避免泄漏。
     */
    if (block->invalidated) {
        LLVMDisposeExecutionEngine(engine);
        pthread_mutex_unlock(cache_lock);
        free(set);
        return;
    }
#else
    uint64_t key = (uint64_t) block->pc_start;
#endif

    /* 在锁内写入 block 字段，避免与淘汰路径发生数据竞争。 */
    block->func = func;
    block->llvm_engine = engine;

    jit_cache_update(rv->jit_cache, key, block->func);

    /* 原子 release 存储确保其他线程观察到 hot2=true 前，已经能看到 block->func 和
     * jit_cache 的全部写入。它与 rv_step() 中的 acquire 加载配对。
     */
    ATOMIC_STORE(&block->hot2, true, ATOMIC_RELEASE);

    pthread_mutex_unlock(cache_lock);
    free(set);
}

struct jit_cache *jit_cache_init()
{
    return calloc(N_JIT_CACHE_ENTRIES, sizeof(struct jit_cache));
}

void jit_cache_exit(struct jit_cache *cache)
{
    free(cache);
}

struct inline_cache *inline_cache_init(void)
{
    return calloc(N_INLINE_CACHE_ENTRIES, sizeof(struct inline_cache));
}

void inline_cache_exit(struct inline_cache *cache)
{
    free(cache);
}

/* 清空所有 inline cache 条目。
 * 在 rs1=0 的 SFENCE.VMA（全量刷新）或模拟器重置时调用。
 * inline cache 只有主线程读写，因此不需要 seqlock。
 */
void inline_cache_clear(struct inline_cache *cache)
{
    memset(cache, 0, N_INLINE_CACHE_ENTRIES * sizeof(struct inline_cache));
}

/* 清空指定 VA 页对应的 inline cache 条目。
 * 在带具体地址的 SFENCE.VMA 中调用；只清理 PC 落在目标页中的条目。
 */
void inline_cache_clear_page(struct inline_cache *cache,
                             uint32_t va,
                             uint32_t satp)
{
    uint32_t va_page = va & ~(RV_PG_SIZE - 1);

    for (uint32_t i = 0; i < N_INLINE_CACHE_ENTRIES; i++) {
        uint64_t key = cache[i].key;
        if (!key)
            continue;

        uint32_t entry_pc = (uint32_t) key;
        uint32_t entry_satp = (uint32_t) (key >> 32);

        if (entry_satp == satp) {
            uint32_t entry_page = entry_pc & ~(RV_PG_SIZE - 1);
            if (entry_page == va_page) {
                cache[i].key = 0;
                cache[i].entry = NULL;
            }
        }
    }
}

/* 清空匹配指定 key 的 inline cache 条目。
 * 淘汰已编译基本块时使用，避免留下过期 entry 指针。
 */
void inline_cache_clear_key(struct inline_cache *cache, uint64_t key)
{
    if (!key)
        return;

    for (uint32_t i = 0; i < N_INLINE_CACHE_ENTRIES; i++) {
        if (cache[i].key == key) {
            cache[i].key = 0;
            cache[i].entry = NULL;
        }
    }
}

/* 释放 T2C 编译基本块时销毁 LLVM execution engine。
 * engine 持有 block->func 指向的代码内存，因此必须在释放 block 前销毁，
 * 避免悬垂指针。
 */
void t2c_dispose_engine(void *engine)
{
    if (engine)
        LLVMDisposeExecutionEngine((LLVMExecutionEngineRef) engine);
}

/* clear_cache_hot 回调的包装函数，用于销毁基本块的 LLVM engine。
 * 关闭阶段通过 clear_cache_hot 调用，清理所有剩余基本块。
 * 同时把 llvm_engine 和 func 置为 NULL，避免 use-after-free。
 *
 * DISABLE_UBSAN_FUNC：禁用 UBSAN 函数指针类型检查。
 * LLVM 的 cflags 可能导致 t2c.c 与 cache.c 之间的函数类型元数据不匹配；
 * 通过 clear_func_t 调用时可能触发误报。
 */
DISABLE_UBSAN_FUNC
void t2c_dispose_block_engine(void *block)
{
    block_t *blk = (block_t *) block;
    if (blk && blk->llvm_engine) {
        LLVMDisposeExecutionEngine((LLVMExecutionEngineRef) blk->llvm_engine);
        blk->llvm_engine = NULL;
        blk->func = NULL; /* func 原本指向 engine 管理的内存。 */
    }
}

void jit_cache_update(struct jit_cache *cache, uint64_t key, void *entry)
{
    /* 掩码前先将高 32 位（satp）与低 32 位（pc）异或。
     * 这样可把不同地址空间的条目分散到表中，降低多个进程共享虚拟地址时的缓存抖动。
     */
    uint32_t pos =
        ((uint32_t) key ^ (uint32_t) (key >> 32)) & (N_JIT_CACHE_ENTRIES - 1);

    /* Seqlock 写入模式：
     * 1. 把 seq 递增为奇数，表示正在写入
     * 2. 写入 entry 和 key（原子 relaxed/release，用于避免与读者数据竞争）
     * 3. 把 seq 递增为偶数，表示写入完成
     * seq 上的 release 顺序保证读者看到一致状态。
     */
    uint32_t seq = ATOMIC_LOAD(&cache[pos].seq, ATOMIC_RELAXED);
    ATOMIC_STORE(&cache[pos].seq, seq + 1, ATOMIC_RELEASE); /* 奇数 = 写入中 */
    ATOMIC_STORE(&cache[pos].entry, entry, ATOMIC_RELEASE);
    ATOMIC_STORE(&cache[pos].key, key, ATOMIC_RELEASE);
    ATOMIC_STORE(&cache[pos].seq, seq + 2, ATOMIC_RELEASE); /* 偶数 = 完成 */
}

void jit_cache_clear(struct jit_cache *cache)
{
    /* 使用 seqlock 模式清空所有条目，保证线程安全的失效操作。 */
    for (uint32_t i = 0; i < N_JIT_CACHE_ENTRIES; i++) {
        uint32_t seq = ATOMIC_LOAD(&cache[i].seq, ATOMIC_RELAXED);
        ATOMIC_STORE(&cache[i].seq, seq + 1,
                     ATOMIC_RELEASE); /* 奇数 = 写入中 */
        ATOMIC_STORE(&cache[i].entry, NULL, ATOMIC_RELEASE);
        ATOMIC_STORE(&cache[i].key, 0, ATOMIC_RELEASE);
        ATOMIC_STORE(&cache[i].seq, seq + 2, ATOMIC_RELEASE); /* 偶数 = 完成 */
    }
}

/* 按指定 VA 页和 SATP 选择性清理 jit_cache 条目。
 * 对按地址执行的 SFENCE.VMA 来说，这比 jit_cache_clear() 更高效，可避免不必要地
 * 失效无关条目。
 *
 * 调用方必须持有 cache_lock（rv->cache_lock），以便与 T2C 编译线程同步。
 * T2C 线程通过 jit_cache_update() 更新 jit_cache 条目时也持有该锁。
 * 如果没有这把锁：
 * 1. T2C 线程可能在当前线程读/清理条目的同时写入条目
 * 2. 竞争可能导致半写入 key 被错误匹配
 * 3. 条目可能在 T2C 刚写入后立即被清掉，造成无效工作
 *
 * 这里的 seqlock 模式只保护主线程的 JIT cache 查找不看到撕裂读，并不为写者提供
 * 互斥。主线程（SFENCE.VMA）与 T2C 线程（基本块编译）之间的写者互斥由
 * cache_lock 提供。
 */
void jit_cache_clear_page(struct jit_cache *cache, uint32_t va, uint32_t satp)
{
    uint32_t va_page = va & ~(RV_PG_SIZE - 1);

    for (uint32_t i = 0; i < N_JIT_CACHE_ENTRIES; i++) {
        uint64_t key = ATOMIC_LOAD(&cache[i].key, ATOMIC_RELAXED);
        if (!key)
            continue;

        uint32_t entry_pc = (uint32_t) key;
        uint32_t entry_satp = (uint32_t) (key >> 32);

        /* 匹配 SATP 相同且 PC 落在目标页中的条目。 */
        if (entry_satp == satp) {
            uint32_t entry_page = entry_pc & ~(RV_PG_SIZE - 1);
            if (entry_page == va_page) {
                /* 使用 seqlock 模式清理。 */
                uint32_t seq = ATOMIC_LOAD(&cache[i].seq, ATOMIC_RELAXED);
                ATOMIC_STORE(&cache[i].seq, seq + 1,
                             ATOMIC_RELEASE); /* 奇数 = 写入中 */
                ATOMIC_STORE(&cache[i].entry, NULL, ATOMIC_RELEASE);
                ATOMIC_STORE(&cache[i].key, 0, ATOMIC_RELEASE);
                ATOMIC_STORE(&cache[i].seq, seq + 2,
                             ATOMIC_RELEASE); /* 偶数 = 完成 */
            }
        }
    }
}
