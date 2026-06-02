/*
 * 二级 JIT 指令模板。
 *
 * 本文件把 rv32emu 的自定义 IR 映射为 LLVM IR，并通过 LLVM-C API 构建 IR。
 * 构建出的 LLVM IR 会交给 LLVM 后端执行优化，再交给执行引擎编译为宿主机器码，
 * 最终返回可直接调用的函数指针。T2C_OP 处理器的语义必须与 rv32_template.c
 * 中的解释器实现保持一致。
 */

T2C_OP(nop, { return; })

T2C_OP(lui, {
    T2C_LLVM_GEN_STORE_IMM32(*builder, ir->imm,
                             t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(auipc, {
    T2C_LLVM_GEN_STORE_IMM32(*builder, ir->pc + ir->imm,
                             t2c_gen_rd_addr(start, builder, ir));
})

/* 按 PC 查询基本块，并判断该块是否仍可用于跳转。 */
static bool t2c_check_valid_blk(riscv_t *rv, block_t *block UNUSED, uint32_t pc)
{
    block_t *blk = cache_get(rv->block_cache, pc, false);
    if (!blk || !blk->translatable)
        return false;

#if RV32_HAS(SYSTEM)
    if (blk->satp != block->satp)
        return false;
#endif

    return true;
}

T2C_OP(jal, {
    if (ir->rd)
        T2C_LLVM_GEN_STORE_IMM32(*builder, ir->pc + 4,
                                 t2c_gen_rd_addr(start, builder, ir));

    if (ir->branch_taken &&
        t2c_check_valid_blk(rv, block, ir->branch_taken->pc)) {
        *taken_builder = *builder;
    } else {
        T2C_LLVM_GEN_STORE_IMM32(*builder, ir->pc + ir->imm,
                                 t2c_gen_PC_addr(start, builder, ir));
        T2C_STORE_TIMER(*builder, start, insn_counter);
        LLVMBuildRetVoid(*builder);
    }
})

FORCE_INLINE void t2c_jit_cache_helper(LLVMBuilderRef *builder,
                                       LLVMValueRef start,
                                       LLVMValueRef addr,
                                       riscv_t *rv UNUSED,
                                       block_t *block UNUSED,
                                       rv_insn_t *ir,
                                       LLVMValueRef insn_counter)
{
    /* 用 inline cache + seqlock 模式解析间接跳转。
     *
     * 快路径（inline cache 命中）：
     *   1. 从 inline_cache[hash] 加载缓存 key。
     *   2. 与目标地址比较，匹配则直接调用缓存入口。
     *   3. ARM64 上不需要 ISB，因为该目标此前已经成功执行过。
     *
     * 慢路径（inline cache 未命中，查 jit_cache）：
     *   1. 加载 seq1（acquire），若为奇数表示写入中，回退解释器。
     *   2. 加载 key（acquire）并与期望值比较，不匹配则回退。
     *   3. 加载 entry（acquire），确保相关数据加载完成。
     *   4. 加载 seq2（monotonic），若 seq1 != seq2 或 entry == NULL 则回退。
     *   5. 用 (key, entry) 更新 inline cache，供下次使用。
     *   6. ARM64 新块路径执行 ISB，然后调用 entry。
     *
     * 对返回、虚调用等稳定分支模式，inline cache 通常可提供约 90% 命中率，热点
     * 路径因此能避开 seqlock 开销和 ISB。
     */
    LLVMValueRef rv_param = LLVMGetParam(start, 0);

    /* 只计算一次期望 key，inline cache 和 jit_cache 共用。 */
#if RV32_HAS(SYSTEM)
    LLVMValueRef satp_offset_early =
        LLVMConstInt(LLVMInt64Type(), offsetof(riscv_t, csr_satp), false);
    LLVMValueRef satp_ptr_early = LLVMBuildInBoundsGEP2(
        *builder, LLVMInt8Type(), rv_param, &satp_offset_early, 1, "");
    LLVMValueRef satp_early =
        LLVMBuildLoad2(*builder, LLVMInt32Type(), satp_ptr_early, "");
    LLVMValueRef addr64 =
        LLVMBuildIntCast2(*builder, addr, LLVMInt64Type(), false, "");
    LLVMValueRef satp64 =
        LLVMBuildIntCast2(*builder, satp_early, LLVMInt64Type(), false, "");
    LLVMValueRef satp_shifted = LLVMBuildShl(
        *builder, satp64, LLVMConstInt(LLVMInt64Type(), 32, false), "");
    LLVMValueRef expected_key =
        LLVMBuildAdd(*builder, addr64, satp_shifted, "expected_key");
#else
    LLVMValueRef expected_key = LLVMBuildIntCast2(
        *builder, addr, LLVMInt64Type(), false, "expected_key");
#endif

    /* === INLINE CACHE 快路径 === */

    /* 加载 inline_cache 基址。 */
    LLVMValueRef ic_offset =
        LLVMConstInt(LLVMInt64Type(), offsetof(riscv_t, inline_cache), false);
    LLVMValueRef ic_ptr = LLVMBuildInBoundsGEP2(*builder, LLVMInt8Type(),
                                                rv_param, &ic_offset, 1, "");
    LLVMValueRef ic_base = LLVMBuildLoad2(
        *builder, LLVMPointerType(t2c_inline_cache_struct_type, 0), ic_ptr, "");

    /* 计算 inline cache 索引：使用不同于 jit_cache 的哈希以分散负载。
     * 高位 XOR 低位能改善分布。
     */
    LLVMValueRef ic_addr_high = LLVMBuildLShr(
        *builder, addr, LLVMConstInt(LLVMInt32Type(), 12, false), "");
    LLVMValueRef ic_addr_mixed = LLVMBuildXor(*builder, addr, ic_addr_high, "");
#if RV32_HAS(SYSTEM)
    LLVMValueRef ic_hash_xor =
        LLVMBuildXor(*builder, ic_addr_mixed, satp_early, "");
    LLVMValueRef ic_hash = LLVMBuildAnd(
        *builder, ic_hash_xor,
        LLVMConstInt(LLVMInt32Type(), N_INLINE_CACHE_ENTRIES - 1, false), "");
#else
    LLVMValueRef ic_hash = LLVMBuildAnd(
        *builder, ic_addr_mixed,
        LLVMConstInt(LLVMInt32Type(), N_INLINE_CACHE_ENTRIES - 1, false), "");
#endif

    /* 获取 inline cache 条目指针。 */
    LLVMValueRef ic_idx =
        LLVMBuildIntCast2(*builder, ic_hash, LLVMInt64Type(), false, "");
    LLVMValueRef ic_element_ptr = LLVMBuildInBoundsGEP2(
        *builder, t2c_inline_cache_struct_type, ic_base, &ic_idx, 1, "");

    /* 从 inline cache 加载缓存的 key 和 entry。 */
    LLVMValueRef ic_key_ptr = LLVMBuildStructGEP2(
        *builder, t2c_inline_cache_struct_type, ic_element_ptr, 0, "");
    LLVMValueRef ic_key =
        LLVMBuildLoad2(*builder, LLVMInt64Type(), ic_key_ptr, "ic_key");

    LLVMValueRef ic_entry_ptr = LLVMBuildStructGEP2(
        *builder, t2c_inline_cache_struct_type, ic_element_ptr, 1, "");
    LLVMValueRef ic_entry = LLVMBuildLoad2(
        *builder, LLVMPointerType(LLVMVoidType(), 0), ic_entry_ptr, "ic_entry");

    /* 判断 inline cache 是否命中：key 匹配且 entry 非 NULL。 */
    LLVMValueRef ic_key_match =
        LLVMBuildICmp(*builder, LLVMIntEQ, ic_key, expected_key, "");
    LLVMValueRef ic_entry_valid = LLVMBuildIsNotNull(*builder, ic_entry, "");
    LLVMValueRef ic_hit =
        LLVMBuildAnd(*builder, ic_key_match, ic_entry_valid, "ic_hit");

    /* 为 inline cache 命中和未命中路径创建基本块。 */
    LLVMBasicBlockRef ic_hit_block = LLVMAppendBasicBlock(start, "ic_hit");
    LLVMBuilderRef ic_hit_builder = LLVMCreateBuilder();
    LLVMPositionBuilderAtEnd(ic_hit_builder, ic_hit_block);

    LLVMBasicBlockRef ic_miss_block = LLVMAppendBasicBlock(start, "ic_miss");
    LLVMBuilderRef ic_miss_builder = LLVMCreateBuilder();
    LLVMPositionBuilderAtEnd(ic_miss_builder, ic_miss_block);

    LLVMBuildCondBr(*builder, ic_hit, ic_hit_block, ic_miss_block);

    /* === INLINE CACHE 命中路径（快） ===
     * 不需要 ISB：该目标此前已经成功执行，当时指令缓存已保持一致。
     */
    T2C_STORE_TIMER(ic_hit_builder, start, insn_counter);
    LLVMValueRef ic_call_args[1] = {rv_param};
    LLVMBuildCall2(ic_hit_builder, t2c_jit_cache_func_type, ic_entry,
                   ic_call_args, 1, "");
    LLVMBuildRetVoid(ic_hit_builder);

    /* === INLINE CACHE 未命中路径（慢，使用 seqlock jit_cache） === */

    LLVMBasicBlockRef seq_even = LLVMAppendBasicBlock(start, "seq_even");
    LLVMBuilderRef seq_even_builder = LLVMCreateBuilder();
    LLVMPositionBuilderAtEnd(seq_even_builder, seq_even);

    LLVMBasicBlockRef key_match = LLVMAppendBasicBlock(start, "key_match");
    LLVMBuilderRef key_match_builder = LLVMCreateBuilder();
    LLVMPositionBuilderAtEnd(key_match_builder, key_match);

    LLVMBasicBlockRef call_jit = LLVMAppendBasicBlock(start, "call_jit");
    LLVMBuilderRef call_builder = LLVMCreateBuilder();
    LLVMPositionBuilderAtEnd(call_builder, call_jit);

    LLVMBasicBlockRef fallback = LLVMAppendBasicBlock(start, "fallback");
    LLVMBuilderRef fallback_builder = LLVMCreateBuilder();
    LLVMPositionBuilderAtEnd(fallback_builder, fallback);

    /* 加载 jit_cache 基址。 */
    LLVMValueRef jit_cache_offset =
        LLVMConstInt(LLVMInt64Type(), offsetof(riscv_t, jit_cache), false);
    LLVMValueRef jit_cache_ptr = LLVMBuildInBoundsGEP2(
        ic_miss_builder, LLVMInt8Type(), rv_param, &jit_cache_offset, 1, "");
    LLVMValueRef base = LLVMBuildLoad2(
        ic_miss_builder, LLVMPointerType(t2c_jit_cache_struct_type, 0),
        jit_cache_ptr, "");

    /* 计算 jit_cache 索引。 */
    LLVMValueRef addr_high = LLVMBuildLShr(
        ic_miss_builder, addr, LLVMConstInt(LLVMInt32Type(), 12, false), "");
    LLVMValueRef addr_mixed =
        LLVMBuildXor(ic_miss_builder, addr, addr_high, "");
#if RV32_HAS(SYSTEM)
    LLVMValueRef hash_xor =
        LLVMBuildXor(ic_miss_builder, addr_mixed, satp_early, "");
    LLVMValueRef hash = LLVMBuildAnd(
        ic_miss_builder, hash_xor,
        LLVMConstInt(LLVMInt32Type(), N_JIT_CACHE_ENTRIES - 1, false), "");
#else
    LLVMValueRef hash = LLVMBuildAnd(
        ic_miss_builder, addr_mixed,
        LLVMConstInt(LLVMInt32Type(), N_JIT_CACHE_ENTRIES - 1, false), "");
#endif

    /* 获取 jit_cache 条目指针。 */
    LLVMValueRef cast =
        LLVMBuildIntCast2(ic_miss_builder, hash, LLVMInt64Type(), false, "");
    LLVMValueRef element_ptr = LLVMBuildInBoundsGEP2(
        ic_miss_builder, t2c_jit_cache_struct_type, base, &cast, 1, "");

    /* 步骤 1：加载 seq1（acquire）。奇数表示写入正在进行。 */
    LLVMValueRef seq_ptr = LLVMBuildStructGEP2(
        ic_miss_builder, t2c_jit_cache_struct_type, element_ptr, 0, "");
    LLVMValueRef seq1 =
        LLVMBuildLoad2(ic_miss_builder, LLVMInt32Type(), seq_ptr, "");
    LLVMSetOrdering(seq1, LLVMAtomicOrderingAcquire);
    LLVMValueRef seq_odd = LLVMBuildAnd(
        ic_miss_builder, seq1, LLVMConstInt(LLVMInt32Type(), 1, false), "");
    LLVMValueRef is_even =
        LLVMBuildICmp(ic_miss_builder, LLVMIntEQ, seq_odd,
                      LLVMConstInt(LLVMInt32Type(), 0, false), "");
    LLVMBuildCondBr(ic_miss_builder, is_even, seq_even, fallback);

    /* 步骤 2：加载 key（acquire）并与期望值比较。 */
    LLVMValueRef jc_key_ptr = LLVMBuildStructGEP2(
        seq_even_builder, t2c_jit_cache_struct_type, element_ptr, 2, "");
    LLVMValueRef jc_key =
        LLVMBuildLoad2(seq_even_builder, LLVMInt64Type(), jc_key_ptr, "");
    LLVMSetOrdering(jc_key, LLVMAtomicOrderingAcquire);
    LLVMSetAlignment(jc_key, 8);

    LLVMValueRef jc_key_cmp =
        LLVMBuildICmp(seq_even_builder, LLVMIntEQ, jc_key, expected_key, "");
    LLVMBuildCondBr(seq_even_builder, jc_key_cmp, key_match, fallback);

    /* 步骤 3：加载 entry（acquire）。 */
    LLVMValueRef jc_entry_ptr = LLVMBuildStructGEP2(
        key_match_builder, t2c_jit_cache_struct_type, element_ptr, 3, "");
    LLVMValueRef entry =
        LLVMBuildLoad2(key_match_builder, LLVMPointerType(LLVMVoidType(), 0),
                       jc_entry_ptr, "");
    LLVMSetOrdering(entry, LLVMAtomicOrderingAcquire);
    LLVMSetAlignment(entry, 8);

    /* 步骤 4：加载 seq2（monotonic），确认 seq1 == seq2 且 entry 非 NULL。 */
    LLVMValueRef seq2 =
        LLVMBuildLoad2(key_match_builder, LLVMInt32Type(), seq_ptr, "");
    LLVMSetOrdering(seq2, LLVMAtomicOrderingMonotonic);

    LLVMValueRef seq_cmp =
        LLVMBuildICmp(key_match_builder, LLVMIntEQ, seq1, seq2, "");
    LLVMValueRef entry_not_null =
        LLVMBuildIsNotNull(key_match_builder, entry, "");
    LLVMValueRef valid =
        LLVMBuildAnd(key_match_builder, seq_cmp, entry_not_null, "");
    LLVMBuildCondBr(key_match_builder, valid, call_jit, fallback);

    /* 步骤 5：用新查到的 entry 更新 inline cache。
     * 这会为后续相同目标填充快路径。更新只发生在主线程，因此不需要原子操作。
     */
    LLVMBuildStore(call_builder, expected_key, ic_key_ptr);
    LLVMBuildStore(call_builder, entry, ic_entry_ptr);

#if defined(__aarch64__)
    /* 只有慢路径上的新发现基本块需要 ARM64 ISB。inline cache 命中路径会跳过 ISB，
     * 因为目标此前已成功执行，指令缓存当时已经一致。
     */
    LLVMTypeRef isb_func_type = LLVMFunctionType(LLVMVoidType(), NULL, 0, 0);
    LLVMValueRef isb_asm =
        LLVMGetInlineAsm(isb_func_type, "isb", 3, "", 0, true, false,
                         LLVMInlineAsmDialectATT, 0);
    LLVMBuildCall2(call_builder, isb_func_type, isb_asm, NULL, 0, "");
#endif

    T2C_STORE_TIMER(call_builder, start, insn_counter);
    LLVMValueRef t2c_args[1] = {rv_param};
    LLVMBuildCall2(call_builder, t2c_jit_cache_func_type, entry, t2c_args, 1,
                   "");
    LLVMBuildRetVoid(call_builder);

    /* 回退：seq 为奇数、key 不匹配或 seq 变化时返回解释器。 */
    LLVMBuildStore(fallback_builder, addr,
                   t2c_gen_PC_addr(start, &fallback_builder, ir));
    T2C_STORE_TIMER(fallback_builder, start, insn_counter);
    LLVMBuildRetVoid(fallback_builder);

    /* 释放临时 builder。 */
    LLVMDisposeBuilder(ic_hit_builder);
    LLVMDisposeBuilder(ic_miss_builder);
    LLVMDisposeBuilder(seq_even_builder);
    LLVMDisposeBuilder(key_match_builder);
    LLVMDisposeBuilder(call_builder);
    LLVMDisposeBuilder(fallback_builder);
}

T2C_OP(jalr, {
    /* 保存间接地址的寄存器必须先加载，避免后续操作覆盖其值。
     */
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    val_rs1 = T2C_LLVM_GEN_ALU32_IMM(Add, val_rs1, ir->imm);
    val_rs1 = T2C_LLVM_GEN_ALU32_IMM(And, val_rs1, ~1U);

    if (ir->rd)
        T2C_LLVM_GEN_STORE_IMM32(*builder, ir->pc + 4,
                                 t2c_gen_rd_addr(start, builder, ir));

    t2c_jit_cache_helper(builder, start, val_rs1, rv, block, ir, insn_counter);
})

#define BRANCH_FUNC(type, cond)                                             \
    T2C_OP(type, {                                                          \
        LLVMValueRef addr_PC = t2c_gen_PC_addr(start, builder, ir);         \
        T2C_LLVM_GEN_LOAD_VMREG(rs1, 32,                                    \
                                t2c_gen_rs1_addr(start, builder, ir));      \
        T2C_LLVM_GEN_LOAD_VMREG(rs2, 32,                                    \
                                t2c_gen_rs2_addr(start, builder, ir));      \
        T2C_LLVM_GEN_CMP(cond, val_rs1, val_rs2);                           \
        LLVMBasicBlockRef taken = LLVMAppendBasicBlock(start, "taken");     \
        LLVMBuilderRef builder2 = LLVMCreateBuilder();                      \
        LLVMPositionBuilderAtEnd(builder2, taken);                          \
        if (ir->branch_taken &&                                             \
            t2c_check_valid_blk(rv, block, ir->branch_taken->pc)) {         \
            *taken_builder = builder2;                                      \
        } else {                                                            \
            T2C_LLVM_GEN_STORE_IMM32(builder2, ir->pc + ir->imm, addr_PC);  \
            T2C_STORE_TIMER(builder2, start, insn_counter);                 \
            LLVMBuildRetVoid(builder2);                                     \
            LLVMDisposeBuilder(builder2);                                   \
        }                                                                   \
        LLVMBasicBlockRef untaken = LLVMAppendBasicBlock(start, "untaken"); \
        LLVMBuilderRef builder3 = LLVMCreateBuilder();                      \
        LLVMPositionBuilderAtEnd(builder3, untaken);                        \
        if (ir->branch_untaken &&                                           \
            t2c_check_valid_blk(rv, block, ir->branch_untaken->pc)) {       \
            *untaken_builder = builder3;                                    \
        } else {                                                            \
            T2C_LLVM_GEN_STORE_IMM32(builder3, ir->pc + 4, addr_PC);        \
            T2C_STORE_TIMER(builder3, start, insn_counter);                 \
            LLVMBuildRetVoid(builder3);                                     \
            LLVMDisposeBuilder(builder3);                                   \
        }                                                                   \
        LLVMBuildCondBr(*builder, cmp, taken, untaken);                     \
    })

BRANCH_FUNC(beq, EQ)
BRANCH_FUNC(bne, NE)
BRANCH_FUNC(blt, SLT)
BRANCH_FUNC(bge, SGE)
BRANCH_FUNC(bltu, ULT)
BRANCH_FUNC(bgeu, UGE)

#if RV32_HAS(SYSTEM)

#include "system.h"

#define t2c_mmu_wrapper(opcode) t2c_mmu_wrapper_##opcode

/* T2C_MMU_LOAD：为 MMU 读操作生成 LLVM IR。
 * 运行时从 rv->io 加载函数指针，避免 ASLR 导致的地址问题。
 * 参数：
 *   opcode: 指令名（lb、lh、lw、lbu、lhu）。
 *   io_field: riscv_io_t 中的字段名（mmu_read_b、mmu_read_s、mmu_read_w）。
 *   bits: 返回值位宽（8、16、32）。
 *   is_signed: 是否对结果做符号扩展。
 *
 * 优化说明：每次调用 t2c_mmu_wrapper_* 都会从 rv->io 加载 MMU 函数指针。若同一
 * 基本块内有多条同类型内存操作，会产生冗余加载。LLVM O3 的 early-cse（公共子
 * 表达式消除）通常能移除这些冗余，因为：
 *   1. rv->io 相对 rv 参数的偏移是常量。
 *   2. 指针值在基本块执行期间不变。
 *   3. Memory SSA 能跟踪加载依赖。
 * 如果 profiling 显示这里仍是瓶颈，可考虑把函数指针加载提升到基本块入口，并
 * 通过上下文结构传递。
 */
#define T2C_MMU_LOAD(opcode, io_field, bits, is_signed)                       \
    static void t2c_mmu_wrapper_##opcode(LLVMBuilderRef *builder,             \
                                         LLVMValueRef start, rv_insn_t *ir)   \
    {                                                                         \
        LLVMValueRef val_rs1 =                                                \
            LLVMBuildLoad2(*builder, LLVMInt32Type(),                         \
                           t2c_gen_rs1_addr(start, builder, ir), "");         \
        LLVMValueRef vaddr = T2C_LLVM_GEN_ALU32_IMM(Add, val_rs1, ir->imm);   \
        /* MMU 读函数：uint##bits##_t fn(riscv_t *rv, uint32_t vaddr)。       \
         * 使用正确的 32 位 vaddr 类型以匹配 C 函数签名。 */                \
        LLVMTypeRef param_types[] = {LLVMPointerType(LLVMVoidType(), 0),      \
                                     LLVMInt32Type()};                        \
        LLVMTypeRef mmu_fn_type =                                             \
            LLVMFunctionType(LLVMInt##bits##Type(), param_types, 2, 0);       \
        /* 运行时从 rv->io 加载 MMU 函数指针。                               \
         * 避免嵌入会被 ASLR 破坏的编译期地址。                             \
         * 偏移 = offsetof(riscv_t, io) + offsetof(riscv_io_t, io_field)。 */ \
        LLVMValueRef rv_param = LLVMGetParam(start, 0);                       \
        LLVMValueRef fn_offset = LLVMConstInt(                                \
            LLVMInt64Type(),                                                  \
            offsetof(riscv_t, io) + offsetof(riscv_io_t, io_field), false);   \
        LLVMValueRef fn_ptr_loc = LLVMBuildInBoundsGEP2(                      \
            *builder, LLVMInt8Type(), rv_param, &fn_offset, 1, "");           \
        LLVMValueRef mmu_fn_ptr = LLVMBuildLoad2(                             \
            *builder, LLVMPointerType(mmu_fn_type, 0), fn_ptr_loc, "");       \
        LLVMValueRef params[] = {rv_param, vaddr};                            \
        LLVMValueRef ret =                                                    \
            LLVMBuildCall2(*builder, mmu_fn_type, mmu_fn_ptr, params, 2, ""); \
        ret =                                                                 \
            LLVMBuildIntCast2(*builder, ret, LLVMInt32Type(), is_signed, ""); \
        LLVMBuildStore(*builder, ret, t2c_gen_rd_addr(start, builder, ir));   \
    }

/* T2C_MMU_STORE：为 MMU 写操作生成 LLVM IR。
 * 运行时从 rv->io 加载函数指针，避免 ASLR 导致的地址问题。
 * 参数：
 *   opcode: 指令名（sb、sh、sw）。
 *   io_field: riscv_io_t 中的字段名（mmu_write_b、mmu_write_s、mmu_write_w）。
 *   val_bits: 写入值参数位宽（8、16、32）。
 */
#define T2C_MMU_STORE(opcode, io_field, val_bits)                             \
    static void t2c_mmu_wrapper_##opcode(LLVMBuilderRef *builder,             \
                                         LLVMValueRef start, rv_insn_t *ir)   \
    {                                                                         \
        LLVMValueRef val_rs1 =                                                \
            LLVMBuildLoad2(*builder, LLVMInt32Type(),                         \
                           t2c_gen_rs1_addr(start, builder, ir), "");         \
        LLVMValueRef vaddr = T2C_LLVM_GEN_ALU32_IMM(Add, val_rs1, ir->imm);   \
        /* MMU 写函数：void fn(riscv_t *rv, uint32_t vaddr, val)。           \
         * 使用正确类型以匹配 C 函数签名。 */                                \
        LLVMTypeRef param_types[] = {LLVMPointerType(LLVMVoidType(), 0),      \
                                     LLVMInt32Type(),                         \
                                     LLVMInt##val_bits##Type()};              \
        LLVMTypeRef mmu_fn_type =                                             \
            LLVMFunctionType(LLVMVoidType(), param_types, 3, 0);              \
        /* 运行时从 rv->io 加载 MMU 函数指针。                               \
         * 避免嵌入会被 ASLR 破坏的编译期地址。                             \
         * 偏移 = offsetof(riscv_t, io) + offsetof(riscv_io_t, io_field)。 */ \
        LLVMValueRef rv_param = LLVMGetParam(start, 0);                       \
        LLVMValueRef fn_offset = LLVMConstInt(                                \
            LLVMInt64Type(),                                                  \
            offsetof(riscv_t, io) + offsetof(riscv_io_t, io_field), false);   \
        LLVMValueRef fn_ptr_loc = LLVMBuildInBoundsGEP2(                      \
            *builder, LLVMInt8Type(), rv_param, &fn_offset, 1, "");           \
        LLVMValueRef mmu_fn_ptr = LLVMBuildLoad2(                             \
            *builder, LLVMPointerType(mmu_fn_type, 0), fn_ptr_loc, "");       \
        T2C_LLVM_GEN_LOAD_VMREG(rs2, val_bits,                                \
                                t2c_gen_rs2_addr(start, builder, ir));        \
        LLVMValueRef params[] = {rv_param, vaddr, val_rs2};                   \
        LLVMBuildCall2(*builder, mmu_fn_type, mmu_fn_ptr, params, 3, "");     \
    }

T2C_MMU_LOAD(lb, mmu_read_b, 8, true);
T2C_MMU_LOAD(lbu, mmu_read_b, 8, false);
T2C_MMU_LOAD(lh, mmu_read_s, 16, true);
T2C_MMU_LOAD(lhu, mmu_read_s, 16, false);
T2C_MMU_LOAD(lw, mmu_read_w, 32, true);

T2C_MMU_STORE(sb, mmu_write_b, 8);
T2C_MMU_STORE(sh, mmu_write_s, 16);
T2C_MMU_STORE(sw, mmu_write_w, 32);

/* clwsp 的 MMU 包装器：通过 MMU 从 sp + imm 加载 32 位字。 */
static void t2c_mmu_wrapper_clwsp(LLVMBuilderRef *builder,
                                  LLVMValueRef start,
                                  rv_insn_t *ir)
{
    /* 加载 sp（x2）并加上立即数偏移。 */
    LLVMValueRef val_sp = LLVMBuildLoad2(
        *builder, LLVMInt32Type(), t2c_gen_sp_addr(start, builder, ir), "");
    LLVMValueRef vaddr = T2C_LLVM_GEN_ALU32_IMM(Add, val_sp, ir->imm);
    /* MMU 读：uint32_t fn(riscv_t *rv, uint32_t vaddr)。 */
    LLVMTypeRef param_types[] = {LLVMPointerType(LLVMVoidType(), 0),
                                 LLVMInt32Type()};
    LLVMTypeRef mmu_fn_type =
        LLVMFunctionType(LLVMInt32Type(), param_types, 2, 0);
    LLVMValueRef rv_param = LLVMGetParam(start, 0);
    LLVMValueRef fn_offset = LLVMConstInt(
        LLVMInt64Type(),
        offsetof(riscv_t, io) + offsetof(riscv_io_t, mmu_read_w), false);
    LLVMValueRef fn_ptr_loc = LLVMBuildInBoundsGEP2(
        *builder, LLVMInt8Type(), rv_param, &fn_offset, 1, "");
    LLVMValueRef mmu_fn_ptr = LLVMBuildLoad2(
        *builder, LLVMPointerType(mmu_fn_type, 0), fn_ptr_loc, "");
    LLVMValueRef params[] = {rv_param, vaddr};
    LLVMValueRef ret =
        LLVMBuildCall2(*builder, mmu_fn_type, mmu_fn_ptr, params, 2, "");
    LLVMBuildStore(*builder, ret, t2c_gen_rd_addr(start, builder, ir));
}

/* cswsp 的 MMU 包装器：通过 MMU 向 sp + imm 存储 32 位字。 */
static void t2c_mmu_wrapper_cswsp(LLVMBuilderRef *builder,
                                  LLVMValueRef start,
                                  rv_insn_t *ir)
{
    /* 加载 sp（x2）并加上立即数偏移。 */
    LLVMValueRef val_sp = LLVMBuildLoad2(
        *builder, LLVMInt32Type(), t2c_gen_sp_addr(start, builder, ir), "");
    LLVMValueRef vaddr = T2C_LLVM_GEN_ALU32_IMM(Add, val_sp, ir->imm);
    /* MMU 写：void fn(riscv_t *rv, uint32_t vaddr, uint32_t val)。 */
    LLVMTypeRef param_types[] = {LLVMPointerType(LLVMVoidType(), 0),
                                 LLVMInt32Type(), LLVMInt32Type()};
    LLVMTypeRef mmu_fn_type =
        LLVMFunctionType(LLVMVoidType(), param_types, 3, 0);
    LLVMValueRef rv_param = LLVMGetParam(start, 0);
    LLVMValueRef fn_offset = LLVMConstInt(
        LLVMInt64Type(),
        offsetof(riscv_t, io) + offsetof(riscv_io_t, mmu_write_w), false);
    LLVMValueRef fn_ptr_loc = LLVMBuildInBoundsGEP2(
        *builder, LLVMInt8Type(), rv_param, &fn_offset, 1, "");
    LLVMValueRef mmu_fn_ptr = LLVMBuildLoad2(
        *builder, LLVMPointerType(mmu_fn_type, 0), fn_ptr_loc, "");
    T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, t2c_gen_rs2_addr(start, builder, ir));
    LLVMValueRef params[] = {rv_param, vaddr, val_rs2};
    LLVMBuildCall2(*builder, mmu_fn_type, mmu_fn_ptr, params, 3, "");
}

/* fuse9 的 MMU 包装器：LUI+LW 绝对地址加载。
 * addr = ir->imm + ir->imm2，目的寄存器为 ir->rs2（不是 rd）。
 */
static void t2c_mmu_wrapper_fuse9(LLVMBuilderRef *builder,
                                  LLVMValueRef start,
                                  rv_insn_t *ir)
{
    uint32_t addr_imm = (uint32_t) ir->imm + (uint32_t) ir->imm2;
    LLVMValueRef vaddr = LLVMConstInt(LLVMInt32Type(), addr_imm, false);
    LLVMTypeRef param_types[] = {LLVMPointerType(LLVMVoidType(), 0),
                                 LLVMInt32Type()};
    LLVMTypeRef mmu_fn_type =
        LLVMFunctionType(LLVMInt32Type(), param_types, 2, 0);
    LLVMValueRef rv_param = LLVMGetParam(start, 0);
    LLVMValueRef fn_offset = LLVMConstInt(
        LLVMInt64Type(),
        offsetof(riscv_t, io) + offsetof(riscv_io_t, mmu_read_w), false);
    LLVMValueRef fn_ptr_loc = LLVMBuildInBoundsGEP2(
        *builder, LLVMInt8Type(), rv_param, &fn_offset, 1, "");
    LLVMValueRef mmu_fn_ptr = LLVMBuildLoad2(
        *builder, LLVMPointerType(mmu_fn_type, 0), fn_ptr_loc, "");
    LLVMValueRef params[] = {rv_param, vaddr};
    LLVMValueRef ret =
        LLVMBuildCall2(*builder, mmu_fn_type, mmu_fn_ptr, params, 2, "");
    /* fuse9 使用 rs2 作为目的寄存器，而不是 rd。 */
    LLVMBuildStore(*builder, ret, t2c_gen_rs2_addr(start, builder, ir));
}

/* fuse10 的 MMU 包装器：LUI+SW 绝对地址存储。
 * addr = ir->imm + ir->imm2，源寄存器为 ir->rs1。
 */
static void t2c_mmu_wrapper_fuse10(LLVMBuilderRef *builder,
                                   LLVMValueRef start,
                                   rv_insn_t *ir)
{
    uint32_t addr_imm = (uint32_t) ir->imm + (uint32_t) ir->imm2;
    LLVMValueRef vaddr = LLVMConstInt(LLVMInt32Type(), addr_imm, false);
    LLVMTypeRef param_types[] = {LLVMPointerType(LLVMVoidType(), 0),
                                 LLVMInt32Type(), LLVMInt32Type()};
    LLVMTypeRef mmu_fn_type =
        LLVMFunctionType(LLVMVoidType(), param_types, 3, 0);
    LLVMValueRef rv_param = LLVMGetParam(start, 0);
    LLVMValueRef fn_offset = LLVMConstInt(
        LLVMInt64Type(),
        offsetof(riscv_t, io) + offsetof(riscv_io_t, mmu_write_w), false);
    LLVMValueRef fn_ptr_loc = LLVMBuildInBoundsGEP2(
        *builder, LLVMInt8Type(), rv_param, &fn_offset, 1, "");
    LLVMValueRef mmu_fn_ptr = LLVMBuildLoad2(
        *builder, LLVMPointerType(mmu_fn_type, 0), fn_ptr_loc, "");
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    LLVMValueRef params[] = {rv_param, vaddr, val_rs1};
    LLVMBuildCall2(*builder, mmu_fn_type, mmu_fn_ptr, params, 3, "");
}

/* fuse11 的 MMU 包装器：LW+ADDI 后递增加载。
 * addr = X[rs1] + imm，目的寄存器为 rd，然后 X[rs1] += imm2。
 */
static void t2c_mmu_wrapper_fuse11(LLVMBuilderRef *builder,
                                   LLVMValueRef start,
                                   rv_insn_t *ir)
{
    LLVMValueRef addr_rs1 = t2c_gen_rs1_addr(start, builder, ir);
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, addr_rs1);
    LLVMValueRef vaddr = T2C_LLVM_GEN_ALU32_IMM(Add, val_rs1, ir->imm);
    /* MMU 读。 */
    LLVMTypeRef param_types[] = {LLVMPointerType(LLVMVoidType(), 0),
                                 LLVMInt32Type()};
    LLVMTypeRef mmu_fn_type =
        LLVMFunctionType(LLVMInt32Type(), param_types, 2, 0);
    LLVMValueRef rv_param = LLVMGetParam(start, 0);
    LLVMValueRef fn_offset = LLVMConstInt(
        LLVMInt64Type(),
        offsetof(riscv_t, io) + offsetof(riscv_io_t, mmu_read_w), false);
    LLVMValueRef fn_ptr_loc = LLVMBuildInBoundsGEP2(
        *builder, LLVMInt8Type(), rv_param, &fn_offset, 1, "");
    LLVMValueRef mmu_fn_ptr = LLVMBuildLoad2(
        *builder, LLVMPointerType(mmu_fn_type, 0), fn_ptr_loc, "");
    LLVMValueRef params[] = {rv_param, vaddr};
    LLVMValueRef ret =
        LLVMBuildCall2(*builder, mmu_fn_type, mmu_fn_ptr, params, 2, "");
    LLVMBuildStore(*builder, ret, t2c_gen_rd_addr(start, builder, ir));
    /* rs1 按 imm2 后递增。 */
    LLVMValueRef inc = T2C_LLVM_GEN_ALU32_IMM(Add, val_rs1, ir->imm2);
    LLVMBuildStore(*builder, inc, addr_rs1);
}

#endif

T2C_OP(lb, {
    IIF(RV32_HAS(SYSTEM))(
        { t2c_mmu_wrapper(lb)(builder, start, ir); },
        {
            LLVMValueRef mem_loc =
                t2c_gen_mem_loc(start, builder, ir, mem_base);
            LLVMValueRef res = LLVMBuildSExt(
                *builder,
                LLVMBuildLoad2(*builder, LLVMInt8Type(), mem_loc, "res"),
                LLVMInt32Type(), "sext8to32");
            LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
        });
})

T2C_OP(lh, {
    IIF(RV32_HAS(SYSTEM))(
        { t2c_mmu_wrapper(lh)(builder, start, ir); },
        {
            LLVMValueRef mem_loc =
                t2c_gen_mem_loc(start, builder, ir, mem_base);
            LLVMValueRef res = LLVMBuildSExt(
                *builder,
                LLVMBuildLoad2(*builder, LLVMInt16Type(), mem_loc, "res"),
                LLVMInt32Type(), "sext16to32");
            LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
        });
})


T2C_OP(lw, {
    IIF(RV32_HAS(SYSTEM))(
        { t2c_mmu_wrapper(lw)(builder, start, ir); },
        {
            LLVMValueRef mem_loc =
                t2c_gen_mem_loc(start, builder, ir, mem_base);
            LLVMValueRef res =
                LLVMBuildLoad2(*builder, LLVMInt32Type(), mem_loc, "res");
            LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
        });
})

T2C_OP(lbu, {
    IIF(RV32_HAS(SYSTEM))(
        { t2c_mmu_wrapper(lbu)(builder, start, ir); },
        {
            LLVMValueRef mem_loc =
                t2c_gen_mem_loc(start, builder, ir, mem_base);
            LLVMValueRef res = LLVMBuildZExt(
                *builder,
                LLVMBuildLoad2(*builder, LLVMInt8Type(), mem_loc, "res"),
                LLVMInt32Type(), "zext8to32");
            LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
        });
})

T2C_OP(lhu, {
    IIF(RV32_HAS(SYSTEM))(
        { t2c_mmu_wrapper(lhu)(builder, start, ir); },
        {
            LLVMValueRef mem_loc =
                t2c_gen_mem_loc(start, builder, ir, mem_base);
            LLVMValueRef res = LLVMBuildZExt(
                *builder,
                LLVMBuildLoad2(*builder, LLVMInt16Type(), mem_loc, "res"),
                LLVMInt32Type(), "zext16to32");
            LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
        });
})

T2C_OP(sb, {
    IIF(RV32_HAS(SYSTEM))(
        { t2c_mmu_wrapper(sb)(builder, start, ir); },
        {
            LLVMValueRef mem_loc =
                t2c_gen_mem_loc(start, builder, ir, mem_base);
            T2C_LLVM_GEN_LOAD_VMREG(rs2, 8,
                                    t2c_gen_rs2_addr(start, builder, ir));
            LLVMBuildStore(*builder, val_rs2, mem_loc);
        });
})

T2C_OP(sh, {
    IIF(RV32_HAS(SYSTEM))(
        { t2c_mmu_wrapper(sh)(builder, start, ir); },
        {
            LLVMValueRef mem_loc =
                t2c_gen_mem_loc(start, builder, ir, mem_base);
            T2C_LLVM_GEN_LOAD_VMREG(rs2, 16,
                                    t2c_gen_rs2_addr(start, builder, ir));
            LLVMBuildStore(*builder, val_rs2, mem_loc);
        });
})

T2C_OP(sw, {
    IIF(RV32_HAS(SYSTEM))(
        { t2c_mmu_wrapper(sw)(builder, start, ir); },
        {
            LLVMValueRef mem_loc =
                t2c_gen_mem_loc(start, builder, ir, mem_base);
            T2C_LLVM_GEN_LOAD_VMREG(rs2, 32,
                                    t2c_gen_rs2_addr(start, builder, ir));
            LLVMBuildStore(*builder, val_rs2, mem_loc);
        });
})

T2C_OP(addi, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    LLVMValueRef res = T2C_LLVM_GEN_ALU32_IMM(Add, val_rs1, ir->imm);
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(slti, {
    LLVMValueRef addr_rd = t2c_gen_rd_addr(start, builder, ir);
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_CMP_IMM32(SLT, val_rs1, ir->imm);
    LLVMValueRef res =
        LLVMBuildSelect(*builder, cmp, LLVMConstInt(LLVMInt32Type(), 1, true),
                        LLVMConstInt(LLVMInt32Type(), 0, true), "");
    LLVMBuildStore(*builder, res, addr_rd);
})

T2C_OP(sltiu, {
    LLVMValueRef addr_rd = t2c_gen_rd_addr(start, builder, ir);
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_CMP_IMM32(ULT, val_rs1, ir->imm);
    LLVMValueRef res =
        LLVMBuildSelect(*builder, cmp, LLVMConstInt(LLVMInt32Type(), 1, true),
                        LLVMConstInt(LLVMInt32Type(), 0, true), "");
    LLVMBuildStore(*builder, res, addr_rd);
})

T2C_OP(xori, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    LLVMValueRef res = T2C_LLVM_GEN_ALU32_IMM(Xor, val_rs1, ir->imm);
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(ori, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    LLVMValueRef res = T2C_LLVM_GEN_ALU32_IMM(Or, val_rs1, ir->imm);
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(andi, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    LLVMValueRef res = T2C_LLVM_GEN_ALU32_IMM(And, val_rs1, ir->imm);
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(slli, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    LLVMValueRef res = T2C_LLVM_GEN_ALU32_IMM(Shl, val_rs1, ir->imm & 0x1f);
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(srli, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    LLVMValueRef res = T2C_LLVM_GEN_ALU32_IMM(LShr, val_rs1, ir->imm & 0x1f);
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(srai, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    LLVMValueRef res = T2C_LLVM_GEN_ALU32_IMM(AShr, val_rs1, ir->imm & 0x1f);
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(add, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, t2c_gen_rs2_addr(start, builder, ir));
    LLVMValueRef res = LLVMBuildAdd(*builder, val_rs1, val_rs2, "add");
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(sub, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, t2c_gen_rs2_addr(start, builder, ir));
    LLVMValueRef res = LLVMBuildSub(*builder, val_rs1, val_rs2, "sub");
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(sll, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, t2c_gen_rs2_addr(start, builder, ir));
    val_rs2 = T2C_LLVM_GEN_ALU32_IMM(And, val_rs2, 0x1f);
    LLVMValueRef res = LLVMBuildShl(*builder, val_rs1, val_rs2, "sll");
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(slt, {
    LLVMValueRef addr_rd = t2c_gen_rd_addr(start, builder, ir);
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, t2c_gen_rs2_addr(start, builder, ir));
    T2C_LLVM_GEN_CMP(SLT, val_rs1, val_rs2);
    LLVMValueRef res =
        LLVMBuildSelect(*builder, cmp, LLVMConstInt(LLVMInt32Type(), 1, true),
                        LLVMConstInt(LLVMInt32Type(), 0, true), "");
    LLVMBuildStore(*builder, res, addr_rd);
})

T2C_OP(sltu, {
    LLVMValueRef addr_rd = t2c_gen_rd_addr(start, builder, ir);
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, t2c_gen_rs2_addr(start, builder, ir));
    T2C_LLVM_GEN_CMP(ULT, val_rs1, val_rs2);
    LLVMValueRef res =
        LLVMBuildSelect(*builder, cmp, LLVMConstInt(LLVMInt32Type(), 1, true),
                        LLVMConstInt(LLVMInt32Type(), 0, true), "");
    LLVMBuildStore(*builder, res, addr_rd);
})

T2C_OP(xor, {
  T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
  T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, t2c_gen_rs2_addr(start, builder, ir));
  LLVMValueRef res = LLVMBuildXor(*builder, val_rs1, val_rs2, "xor");
  LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(srl, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, t2c_gen_rs2_addr(start, builder, ir));
    val_rs2 = T2C_LLVM_GEN_ALU32_IMM(And, val_rs2, 0x1f);
    LLVMValueRef res = LLVMBuildLShr(*builder, val_rs1, val_rs2, "sll");
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(sra, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, t2c_gen_rs2_addr(start, builder, ir));
    val_rs2 = T2C_LLVM_GEN_ALU32_IMM(And, val_rs2, 0x1f);
    LLVMValueRef res = LLVMBuildAShr(*builder, val_rs1, val_rs2, "sll");
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(or, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, t2c_gen_rs2_addr(start, builder, ir));
    LLVMValueRef res = LLVMBuildOr(*builder, val_rs1, val_rs2, "xor");
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(and, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, t2c_gen_rs2_addr(start, builder, ir));
    LLVMValueRef res = LLVMBuildAnd(*builder, val_rs1, val_rs2, "xor");
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(fence, { __UNREACHABLE; })

T2C_OP(ecall, {
    T2C_LLVM_GEN_STORE_IMM32(*builder, ir->pc,
                             t2c_gen_PC_addr(start, builder, ir));
    /* 使用 offsetof() 计算 on_ecall 的正确字节偏移。无论 SYSTEM 模式是否添加
     * MMU 指针字段，这种方式都能保持正确。
     */
    t2c_gen_call_io_func(
        start, builder, param_types,
        offsetof(riscv_t, io) + offsetof(riscv_io_t, on_ecall));
    T2C_STORE_TIMER(*builder, start, insn_counter);
    LLVMBuildRetVoid(*builder);
})

T2C_OP(ebreak, {
    T2C_LLVM_GEN_STORE_IMM32(*builder, ir->pc,
                             t2c_gen_PC_addr(start, builder, ir));
    /* 使用 offsetof() 计算 on_ebreak 的正确字节偏移。无论 SYSTEM 模式是否添加
     * MMU 指针字段，这种方式都能保持正确。
     */
    t2c_gen_call_io_func(
        start, builder, param_types,
        offsetof(riscv_t, io) + offsetof(riscv_io_t, on_ebreak));
    T2C_STORE_TIMER(*builder, start, insn_counter);
    LLVMBuildRetVoid(*builder);
})

T2C_OP(wfi, { __UNREACHABLE; })

T2C_OP(uret, { __UNREACHABLE; })

#if RV32_HAS(SYSTEM)
T2C_OP(sret, { __UNREACHABLE; })
#endif

T2C_OP(hret, { __UNREACHABLE; })

T2C_OP(mret, { __UNREACHABLE; })

T2C_OP(sfencevma, { __UNREACHABLE; })

#if RV32_HAS(Zifencei)
T2C_OP(fencei, { __UNREACHABLE; })
#endif

#if RV32_HAS(Zicsr)
T2C_OP(csrrw, { __UNREACHABLE; })

T2C_OP(csrrs, { __UNREACHABLE; })

T2C_OP(csrrc, { __UNREACHABLE; })

T2C_OP(csrrwi, { __UNREACHABLE; })

T2C_OP(csrrsi, { __UNREACHABLE; })

T2C_OP(csrrci, { __UNREACHABLE; })
#endif

#if RV32_HAS(EXT_M)
T2C_OP(mul, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, t2c_gen_rs2_addr(start, builder, ir));
    val_rs1 = LLVMBuildSExt(*builder, val_rs1, LLVMInt64Type(), "sextrs1to64");
    val_rs2 = LLVMBuildSExt(*builder, val_rs2, LLVMInt64Type(), "sextrs2to64");
    LLVMValueRef res = LLVMBuildMul(*builder, val_rs1, val_rs2, "mul");
    res = T2C_LLVM_GEN_ALU64_IMM(And, res, 0xFFFFFFFF);
    res = LLVMBuildTrunc(*builder, res, LLVMInt32Type(), "sextresto32");
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(mulh, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, t2c_gen_rs2_addr(start, builder, ir));
    val_rs1 = LLVMBuildSExt(*builder, val_rs1, LLVMInt64Type(), "sextrs1to64");
    val_rs2 = LLVMBuildSExt(*builder, val_rs2, LLVMInt64Type(), "sextrs2to64");
    LLVMValueRef res = LLVMBuildMul(*builder, val_rs1, val_rs2, "mul");
    res = T2C_LLVM_GEN_ALU64_IMM(LShr, res, 32);
    res = LLVMBuildTrunc(*builder, res, LLVMInt32Type(), "sextresto32");
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(mulhsu, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, t2c_gen_rs2_addr(start, builder, ir));
    val_rs1 = LLVMBuildSExt(*builder, val_rs1, LLVMInt64Type(), "sextrs1to64");
    val_rs2 = LLVMBuildZExt(*builder, val_rs2, LLVMInt64Type(), "zextrs2to64");
    LLVMValueRef res = LLVMBuildMul(*builder, val_rs1, val_rs2, "mul");
    res = T2C_LLVM_GEN_ALU64_IMM(LShr, res, 32);
    res = LLVMBuildTrunc(*builder, res, LLVMInt32Type(), "sextresto32");
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(mulhu, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, t2c_gen_rs2_addr(start, builder, ir));
    val_rs1 = LLVMBuildZExt(*builder, val_rs1, LLVMInt64Type(), "sextrs1to64");
    val_rs2 = LLVMBuildZExt(*builder, val_rs2, LLVMInt64Type(), "zextrs2to64");
    LLVMValueRef res = LLVMBuildMul(*builder, val_rs1, val_rs2, "mul");
    res = T2C_LLVM_GEN_ALU64_IMM(LShr, res, 32);
    res = LLVMBuildTrunc(*builder, res, LLVMInt32Type(), "sextresto32");
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(div, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, t2c_gen_rs2_addr(start, builder, ir));
    LLVMValueRef res = LLVMBuildSDiv(*builder, val_rs1, val_rs2, "sdiv");
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(divu, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, t2c_gen_rs2_addr(start, builder, ir));
    LLVMValueRef res = LLVMBuildUDiv(*builder, val_rs1, val_rs2, "udiv");
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(rem, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, t2c_gen_rs2_addr(start, builder, ir));
    LLVMValueRef res = LLVMBuildSRem(*builder, val_rs1, val_rs2, "srem");
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(remu, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, t2c_gen_rs2_addr(start, builder, ir));
    LLVMValueRef res = LLVMBuildURem(*builder, val_rs1, val_rs2, "urem");
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})
#endif

#if RV32_HAS(EXT_A)
T2C_OP(lrw, { __UNREACHABLE; })

T2C_OP(scw, { __UNREACHABLE; })

T2C_OP(amoswapw, { __UNREACHABLE; })

T2C_OP(amoaddw, { __UNREACHABLE; })

T2C_OP(amoxorw, { __UNREACHABLE; })

T2C_OP(amoandw, { __UNREACHABLE; })

T2C_OP(amoorw, { __UNREACHABLE; })

T2C_OP(amominw, { __UNREACHABLE; })

T2C_OP(amomaxw, { __UNREACHABLE; })

T2C_OP(amominuw, { __UNREACHABLE; })

T2C_OP(amomaxuw, { __UNREACHABLE; })
#endif

#if RV32_HAS(EXT_F)
T2C_OP(flw, { __UNREACHABLE; })

T2C_OP(fsw, { __UNREACHABLE; })

T2C_OP(fmadds, { __UNREACHABLE; })

T2C_OP(fmsubs, { __UNREACHABLE; })

T2C_OP(fnmsubs, { __UNREACHABLE; })

T2C_OP(fnmadds, { __UNREACHABLE; })

T2C_OP(fadds, { __UNREACHABLE; })

T2C_OP(fsubs, { __UNREACHABLE; })

T2C_OP(fmuls, { __UNREACHABLE; })

T2C_OP(fdivs, { __UNREACHABLE; })

T2C_OP(fsqrts, { __UNREACHABLE; })

T2C_OP(fsgnjs, { __UNREACHABLE; })

T2C_OP(fsgnjns, { __UNREACHABLE; })

T2C_OP(fsgnjxs, { __UNREACHABLE; })

T2C_OP(fmins, { __UNREACHABLE; })

T2C_OP(fmaxs, { __UNREACHABLE; })

T2C_OP(fcvtws, { __UNREACHABLE; })

T2C_OP(fcvtwus, { __UNREACHABLE; })

T2C_OP(fmvxw, { __UNREACHABLE; })

T2C_OP(feqs, { __UNREACHABLE; })

T2C_OP(flts, { __UNREACHABLE; })

T2C_OP(fles, { __UNREACHABLE; })

T2C_OP(fclasss, { __UNREACHABLE; })

T2C_OP(fcvtsw, { __UNREACHABLE; })

T2C_OP(fcvtswu, { __UNREACHABLE; })

T2C_OP(fmvwx, { __UNREACHABLE; })
#endif

#if RV32_HAS(EXT_C)
T2C_OP(caddi4spn, {
    T2C_LLVM_GEN_LOAD_VMREG(sp, 32, t2c_gen_sp_addr(start, builder, ir));
    LLVMValueRef res = T2C_LLVM_GEN_ALU32_IMM(Add, val_sp, (int16_t) ir->imm);
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})
T2C_OP(clw, {
    IIF(RV32_HAS(SYSTEM))(
        { t2c_mmu_wrapper(lw)(builder, start, ir); },
        {
            LLVMValueRef mem_loc =
                t2c_gen_mem_loc(start, builder, ir, mem_base);
            LLVMValueRef res =
                LLVMBuildLoad2(*builder, LLVMInt32Type(), mem_loc, "res");
            LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
        });
})

T2C_OP(csw, {
    IIF(RV32_HAS(SYSTEM))(
        { t2c_mmu_wrapper(sw)(builder, start, ir); },
        {
            LLVMValueRef mem_loc =
                t2c_gen_mem_loc(start, builder, ir, mem_base);
            T2C_LLVM_GEN_LOAD_VMREG(rs2, 32,
                                    t2c_gen_rs2_addr(start, builder, ir));
            LLVMBuildStore(*builder, val_rs2, mem_loc);
        });
})

T2C_OP(cnop, { return; })

T2C_OP(caddi, {
    LLVMValueRef addr_rd = t2c_gen_rd_addr(start, builder, ir);
    T2C_LLVM_GEN_LOAD_VMREG(rd, 32, addr_rd);
    LLVMValueRef res = T2C_LLVM_GEN_ALU32_IMM(Add, val_rd, (int16_t) ir->imm);
    LLVMBuildStore(*builder, res, addr_rd);
})

T2C_OP(cjal, {
    T2C_LLVM_GEN_STORE_IMM32(*builder, ir->pc + 2,
                             t2c_gen_ra_addr(start, builder, ir));
    if (ir->branch_taken)
        *taken_builder = *builder;
    else {
        T2C_LLVM_GEN_STORE_IMM32(*builder, ir->pc + ir->imm,
                                 t2c_gen_PC_addr(start, builder, ir));
        T2C_STORE_TIMER(*builder, start, insn_counter);
        LLVMBuildRetVoid(*builder);
    }
})

T2C_OP(cli, {
    T2C_LLVM_GEN_STORE_IMM32(*builder, ir->imm,
                             t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(caddi16sp, {
    LLVMValueRef addr_rd = t2c_gen_rd_addr(start, builder, ir);
    T2C_LLVM_GEN_LOAD_VMREG(rd, 32, addr_rd);
    LLVMValueRef res = T2C_LLVM_GEN_ALU32_IMM(Add, val_rd, ir->imm);
    LLVMBuildStore(*builder, res, addr_rd);
})

T2C_OP(clui, {
    T2C_LLVM_GEN_STORE_IMM32(*builder, ir->imm,
                             t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(csrli, {
    LLVMValueRef addr_rs1 = t2c_gen_rs1_addr(start, builder, ir);
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, addr_rs1);
    LLVMValueRef res = T2C_LLVM_GEN_ALU32_IMM(LShr, val_rs1, ir->shamt);
    LLVMBuildStore(*builder, res, addr_rs1);
})

T2C_OP(csrai, {
    LLVMValueRef addr_rs1 = t2c_gen_rs1_addr(start, builder, ir);
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, addr_rs1);
    LLVMValueRef res = T2C_LLVM_GEN_ALU32_IMM(AShr, val_rs1, ir->shamt);
    LLVMBuildStore(*builder, res, addr_rs1);
})

T2C_OP(candi, {
    LLVMValueRef addr_rs1 = t2c_gen_rs1_addr(start, builder, ir);
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, addr_rs1);
    LLVMValueRef res = T2C_LLVM_GEN_ALU32_IMM(And, val_rs1, ir->imm);
    LLVMBuildStore(*builder, res, addr_rs1);
})

T2C_OP(csub, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, t2c_gen_rs2_addr(start, builder, ir));
    LLVMValueRef res = LLVMBuildSub(*builder, val_rs1, val_rs2, "sub");
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(cxor, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, t2c_gen_rs2_addr(start, builder, ir));
    LLVMValueRef res = LLVMBuildXor(*builder, val_rs1, val_rs2, "xor");
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(cor, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, t2c_gen_rs2_addr(start, builder, ir));
    LLVMValueRef res = LLVMBuildOr(*builder, val_rs1, val_rs2, "xor");
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(cand, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, t2c_gen_rs2_addr(start, builder, ir));
    LLVMValueRef res = LLVMBuildAnd(*builder, val_rs1, val_rs2, "xor");
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(cj, {
    if (ir->branch_taken)
        *taken_builder = *builder;
    else {
        T2C_LLVM_GEN_STORE_IMM32(*builder, ir->pc + ir->imm,
                                 t2c_gen_PC_addr(start, builder, ir));
        T2C_STORE_TIMER(*builder, start, insn_counter);
        LLVMBuildRetVoid(*builder);
    }
})

T2C_OP(cbeqz, {
    LLVMValueRef addr_PC = t2c_gen_PC_addr(start, builder, ir);
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_CMP_IMM32(EQ, val_rs1, 0);
    LLVMBasicBlockRef taken = LLVMAppendBasicBlock(start, "taken");
    LLVMBuilderRef builder2 = LLVMCreateBuilder();
    LLVMPositionBuilderAtEnd(builder2, taken);
    if (ir->branch_taken)
        *taken_builder = builder2;
    else {
        T2C_LLVM_GEN_STORE_IMM32(builder2, ir->pc + ir->imm, addr_PC);
        T2C_STORE_TIMER(builder2, start, insn_counter);
        LLVMBuildRetVoid(builder2);
    }

    LLVMBasicBlockRef untaken = LLVMAppendBasicBlock(start, "untaken");
    LLVMBuilderRef builder3 = LLVMCreateBuilder();
    LLVMPositionBuilderAtEnd(builder3, untaken);
    if (ir->branch_untaken)
        *untaken_builder = builder3;
    else {
        T2C_LLVM_GEN_STORE_IMM32(builder3, ir->pc + 2, addr_PC);
        T2C_STORE_TIMER(builder3, start, insn_counter);
        LLVMBuildRetVoid(builder3);
    }
    LLVMBuildCondBr(*builder, cmp, taken, untaken);
})

T2C_OP(cbnez, {
    LLVMValueRef addr_PC = t2c_gen_PC_addr(start, builder, ir);
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_CMP_IMM32(NE, val_rs1, 0);
    LLVMBasicBlockRef taken = LLVMAppendBasicBlock(start, "taken");
    LLVMBuilderRef builder2 = LLVMCreateBuilder();
    LLVMPositionBuilderAtEnd(builder2, taken);
    if (ir->branch_taken)
        *taken_builder = builder2;
    else {
        T2C_LLVM_GEN_STORE_IMM32(builder2, ir->pc + ir->imm, addr_PC);
        T2C_STORE_TIMER(builder2, start, insn_counter);
        LLVMBuildRetVoid(builder2);
    }

    LLVMBasicBlockRef untaken = LLVMAppendBasicBlock(start, "untaken");
    LLVMBuilderRef builder3 = LLVMCreateBuilder();
    LLVMPositionBuilderAtEnd(builder3, untaken);
    if (ir->branch_untaken)
        *untaken_builder = builder3;
    else {
        T2C_LLVM_GEN_STORE_IMM32(builder3, ir->pc + 2, addr_PC);
        T2C_STORE_TIMER(builder3, start, insn_counter);
        LLVMBuildRetVoid(builder3);
    }
    LLVMBuildCondBr(*builder, cmp, taken, untaken);
})

T2C_OP(cslli, {
    LLVMValueRef addr_rd = t2c_gen_rd_addr(start, builder, ir);
    T2C_LLVM_GEN_LOAD_VMREG(rd, 32, addr_rd);
    LLVMValueRef res = T2C_LLVM_GEN_ALU32_IMM(Shl, val_rd, (uint8_t) ir->imm);
    LLVMBuildStore(*builder, res, addr_rd);
})

T2C_OP(clwsp, {
    IIF(RV32_HAS(SYSTEM))(
        { t2c_mmu_wrapper_clwsp(builder, start, ir); },
        {
            LLVMValueRef val_sp = LLVMBuildZExt(
                *builder,
                LLVMBuildLoad2(*builder, LLVMInt32Type(),
                               t2c_gen_sp_addr(start, builder, ir), "val_sp"),
                LLVMInt64Type(), "zext32to64");
            LLVMValueRef addr = LLVMBuildAdd(
                *builder, val_sp,
                LLVMConstInt(LLVMInt64Type(), ir->imm + mem_base, true),
                "addr");
            LLVMValueRef cast_addr = LLVMBuildIntToPtr(
                *builder, addr, LLVMPointerType(LLVMInt32Type(), 0), "cast");
            LLVMValueRef res =
                LLVMBuildLoad2(*builder, LLVMInt32Type(), cast_addr, "res");
            LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
        });
})

T2C_OP(cjr, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    t2c_jit_cache_helper(builder, start, val_rs1, rv, block, ir, insn_counter);
})

T2C_OP(cmv, {
    T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, t2c_gen_rs2_addr(start, builder, ir));
    LLVMBuildStore(*builder, val_rs2, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(cebreak, {
    T2C_LLVM_GEN_STORE_IMM32(*builder, ir->pc,
                             t2c_gen_PC_addr(start, builder, ir));
    /* 使用 offsetof() 计算 on_ebreak 的正确字节偏移。无论 SYSTEM 模式是否添加
     * MMU 指针字段，这种方式都能保持正确。
     */
    t2c_gen_call_io_func(
        start, builder, param_types,
        offsetof(riscv_t, io) + offsetof(riscv_io_t, on_ebreak));
    T2C_STORE_TIMER(*builder, start, insn_counter);
    LLVMBuildRetVoid(*builder);
})

T2C_OP(cjalr, {
    /* 保存间接地址的寄存器必须先加载，避免后续操作覆盖其值。
     */
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_STORE_IMM32(*builder, ir->pc + 2,
                             t2c_gen_ra_addr(start, builder, ir));
    t2c_jit_cache_helper(builder, start, val_rs1, rv, block, ir, insn_counter);
})

T2C_OP(cadd, {
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, t2c_gen_rs2_addr(start, builder, ir));
    LLVMValueRef res = LLVMBuildAdd(*builder, val_rs1, val_rs2, "add");
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
})

T2C_OP(cswsp, {
    IIF(RV32_HAS(SYSTEM))(
        { t2c_mmu_wrapper_cswsp(builder, start, ir); },
        {
            LLVMValueRef addr_rs2 = t2c_gen_rs2_addr(start, builder, ir);
            LLVMValueRef val_sp = LLVMBuildZExt(
                *builder,
                LLVMBuildLoad2(*builder, LLVMInt32Type(),
                               t2c_gen_sp_addr(start, builder, ir), "val_sp"),
                LLVMInt64Type(), "zext32to64");
            T2C_LLVM_GEN_LOAD_VMREG(rs2, 32, addr_rs2);
            LLVMValueRef addr = LLVMBuildAdd(
                *builder, val_sp,
                LLVMConstInt(LLVMInt64Type(), ir->imm + mem_base, true),
                "addr");
            LLVMValueRef cast_addr = LLVMBuildIntToPtr(
                *builder, addr, LLVMPointerType(LLVMInt32Type(), 0), "cast");
            LLVMBuildStore(*builder, val_rs2, cast_addr);
        });
})
#endif

#if RV32_HAS(EXT_C) && RV32_HAS(EXT_F)
T2C_OP(cflwsp, { __UNREACHABLE; })

T2C_OP(cfswsp, { __UNREACHABLE; })

T2C_OP(cflw, { __UNREACHABLE; })

T2C_OP(cfsw, { __UNREACHABLE; })
#endif

#if RV32_HAS(Zba)
T2C_OP(sh1add, { __UNREACHABLE; })

T2C_OP(sh2add, { __UNREACHABLE; })

T2C_OP(sh3add, { __UNREACHABLE; })
#endif

#if RV32_HAS(Zbb)
T2C_OP(andn, { __UNREACHABLE; })

T2C_OP(orn, { __UNREACHABLE; })

T2C_OP(xnor, { __UNREACHABLE; })

T2C_OP(clz, { __UNREACHABLE; })

T2C_OP(ctz, { __UNREACHABLE; })

T2C_OP(cpop, { __UNREACHABLE; })

T2C_OP(max, { __UNREACHABLE; })

T2C_OP(maxu, { __UNREACHABLE; })

T2C_OP(min, { __UNREACHABLE; })

T2C_OP(minu, { __UNREACHABLE; })

T2C_OP(sextb, { __UNREACHABLE; })

T2C_OP(sexth, { __UNREACHABLE; })

T2C_OP(zexth, { __UNREACHABLE; })

T2C_OP(rol, { __UNREACHABLE; })

T2C_OP(ror, { __UNREACHABLE; })

T2C_OP(rori, { __UNREACHABLE; })

T2C_OP(orcb, { __UNREACHABLE; })

T2C_OP(rev8, { __UNREACHABLE; })
#endif

#if RV32_HAS(Zbc)
T2C_OP(clmul, { __UNREACHABLE; })

T2C_OP(clmulh, { __UNREACHABLE; })

T2C_OP(clmulr, { __UNREACHABLE; })
#endif

#if RV32_HAS(Zbs)
T2C_OP(bclr, { __UNREACHABLE; })

T2C_OP(bclri, { __UNREACHABLE; })

T2C_OP(bext, { __UNREACHABLE; })

T2C_OP(bexti, { __UNREACHABLE; })

T2C_OP(binv, { __UNREACHABLE; })

T2C_OP(binvi, { __UNREACHABLE; })

T2C_OP(bset, { __UNREACHABLE; })

T2C_OP(bseti, { __UNREACHABLE; })
#endif

T2C_OP(fuse1, {
    opcode_fuse_t *fuse = ir->fuse;
    for (int i = 0; i < ir->imm2; i++) {
        LLVMValueRef rd_offset =
            LLVMConstInt(LLVMInt32Type(),
                         offsetof(riscv_t, X) / sizeof(int) + fuse[i].rd, true);
        LLVMValueRef addr_rd = LLVMBuildInBoundsGEP2(*builder, LLVMInt32Type(),
                                                     LLVMGetParam(start, 0),
                                                     &rd_offset, 1, "addr_rd");
        LLVMBuildStore(*builder,
                       LLVMConstInt(LLVMInt32Type(), fuse[i].imm, true),
                       addr_rd);
    }
})

T2C_OP(fuse2, {
    LLVMValueRef addr_rd = t2c_gen_rd_addr(start, builder, ir);
    T2C_LLVM_GEN_STORE_IMM32(*builder, ir->imm, addr_rd);
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    T2C_LLVM_GEN_LOAD_VMREG(rd, 32, addr_rd);
    LLVMValueRef res = LLVMBuildAdd(*builder, val_rs1, val_rd, "add");
    LLVMBuildStore(*builder, res, t2c_gen_rs2_addr(start, builder, ir));
})

T2C_OP(fuse3, {
    opcode_fuse_t *fuse = ir->fuse;
    for (int i = 0; i < ir->imm2; i++) {
        IIF(RV32_HAS(SYSTEM))(
            { t2c_mmu_wrapper(sw)(builder, start, (rv_insn_t *) (&fuse[i])); },
            {
                LLVMValueRef mem_loc = t2c_gen_mem_loc(
                    start, builder, (rv_insn_t *) (&fuse[i]), mem_base);
                T2C_LLVM_GEN_LOAD_VMREG(
                    rs2, 32,
                    t2c_gen_rs2_addr(start, builder, (rv_insn_t *) (&fuse[i])));
                LLVMBuildStore(*builder, val_rs2, mem_loc);
            });
    }
})

T2C_OP(fuse4, {
    opcode_fuse_t *fuse = ir->fuse;
    for (int i = 0; i < ir->imm2; i++) {
        IIF(RV32_HAS(SYSTEM))(
            { t2c_mmu_wrapper(lw)(builder, start, (rv_insn_t *) (&fuse[i])); },
            {
                LLVMValueRef mem_loc = t2c_gen_mem_loc(
                    start, builder, (rv_insn_t *) (&fuse[i]), mem_base);
                LLVMValueRef res =
                    LLVMBuildLoad2(*builder, LLVMInt32Type(), mem_loc, "res");
                LLVMBuildStore(
                    *builder, res,
                    t2c_gen_rd_addr(start, builder, (rv_insn_t *) (&fuse[i])));
            });
    }
})

T2C_OP(fuse5, {
    opcode_fuse_t *fuse = ir->fuse;
    for (int i = 0; i < ir->imm2; i++) {
        switch (fuse[i].opcode) {
        case rv_insn_slli:
            t2c_slli(builder, param_types, start, entry, taken_builder,
                     untaken_builder, rv, mem_base, block,
                     (rv_insn_t *) (&fuse[i]), insn_counter);
            break;
        case rv_insn_srli:
            t2c_srli(builder, param_types, start, entry, taken_builder,
                     untaken_builder, rv, mem_base, block,
                     (rv_insn_t *) (&fuse[i]), insn_counter);
            break;
        case rv_insn_srai:
            t2c_srai(builder, param_types, start, entry, taken_builder,
                     untaken_builder, rv, mem_base, block,
                     (rv_insn_t *) (&fuse[i]), insn_counter);
            break;
        default:
            __UNREACHABLE;
            break;
        }
    }
})

/* 融合 LI a7, imm + ECALL。
 * 该融合只适用于标准 RV32I/M/A/F/C；RV32E 使用不同系统调用约定（t0 而非 a7）。
 */
#if !RV32_HAS(RV32E)
T2C_OP(fuse6, {
    /* 把系统调用号 imm 写入 a7 寄存器。 */
    LLVMValueRef a7_offset = LLVMConstInt(
        LLVMInt32Type(), offsetof(riscv_t, X) / sizeof(int) + rv_reg_a7, true);
    LLVMValueRef addr_a7 =
        LLVMBuildInBoundsGEP2(*builder, LLVMInt32Type(), LLVMGetParam(start, 0),
                              &a7_offset, 1, "addr_a7");
    LLVMBuildStore(*builder, LLVMConstInt(LLVMInt32Type(), ir->imm, true),
                   addr_a7);
    /* 保存 PC 并调用 ecall 处理器。
     * ECALL 位于 ir->pc + 4，即融合对中的第二条指令。
     * 使用 offsetof() 计算 on_ecall 的正确字节偏移。
     */
    T2C_LLVM_GEN_STORE_IMM32(*builder, ir->pc + 4,
                             t2c_gen_PC_addr(start, builder, ir));
    t2c_gen_call_io_func(
        start, builder, param_types,
        offsetof(riscv_t, io) + offsetof(riscv_io_t, on_ecall));
    T2C_STORE_TIMER(*builder, start, insn_counter);
    LLVMBuildRetVoid(*builder);
})
#else
/* RV32E stub：RV32E 下不会生成 fuse6 模式。
 * 防御性回退：如果意外到达，直接返回 void。
 */
T2C_OP(fuse6, {
    assert(!"RV32E 模式不应调用 fuse6");
    T2C_STORE_TIMER(*builder, start, insn_counter);
    LLVMBuildRetVoid(*builder);
})
#endif

/* 融合多条 ADDI。 */
T2C_OP(fuse7, {
    opcode_fuse_t *fuse = ir->fuse;
    for (int i = 0; i < ir->imm2; i++) {
        LLVMValueRef rs1_offset = LLVMConstInt(
            LLVMInt32Type(), offsetof(riscv_t, X) / sizeof(int) + fuse[i].rs1,
            true);
        LLVMValueRef addr_rs1 = LLVMBuildInBoundsGEP2(
            *builder, LLVMInt32Type(), LLVMGetParam(start, 0), &rs1_offset, 1,
            "addr_rs1");
        LLVMValueRef val_rs1 =
            LLVMBuildLoad2(*builder, LLVMInt32Type(), addr_rs1, "val_rs1");
        LLVMValueRef res = LLVMBuildAdd(
            *builder, val_rs1, LLVMConstInt(LLVMInt32Type(), fuse[i].imm, true),
            "add");
        LLVMValueRef rd_offset =
            LLVMConstInt(LLVMInt32Type(),
                         offsetof(riscv_t, X) / sizeof(int) + fuse[i].rd, true);
        LLVMValueRef addr_rd = LLVMBuildInBoundsGEP2(*builder, LLVMInt32Type(),
                                                     LLVMGetParam(start, 0),
                                                     &rd_offset, 1, "addr_rd");
        LLVMBuildStore(*builder, res, addr_rd);
    }
})

/* 融合 LUI + ADDI：加载 32 位常量（li 伪指令）。
 * rd = (lui_imm << 12) + addi_imm = ir->imm + ir->imm2。
 */
T2C_OP(fuse8, {
    /* 计算合并后的立即数并写入 rd。转为 uint32_t 可避免有符号溢出 UB。
     */
    uint32_t combined_imm = (uint32_t) ir->imm + (uint32_t) ir->imm2;
    LLVMValueRef rd_offset = LLVMConstInt(
        LLVMInt32Type(), offsetof(riscv_t, X) / sizeof(int) + ir->rd, true);
    LLVMValueRef addr_rd =
        LLVMBuildInBoundsGEP2(*builder, LLVMInt32Type(), LLVMGetParam(start, 0),
                              &rd_offset, 1, "addr_rd");
    LLVMBuildStore(*builder, LLVMConstInt(LLVMInt32Type(), combined_imm, true),
                   addr_rd);
})

/* 融合 LUI + LW：绝对地址加载。
 * addr = ir->imm（lui << 12）+ ir->imm2（lw 偏移）。
 * ir->rs2 是加载目的寄存器。
 */
T2C_OP(fuse9, {
    IIF(RV32_HAS(SYSTEM))(
        { t2c_mmu_wrapper_fuse9(builder, start, ir); },
        {
            uint32_t addr_imm = (uint32_t) ir->imm + (uint32_t) ir->imm2;
            LLVMValueRef addr = LLVMConstInt(
                LLVMInt64Type(), (uint64_t) addr_imm + mem_base, false);
            LLVMValueRef cast_addr = LLVMBuildIntToPtr(
                *builder, addr, LLVMPointerType(LLVMInt32Type(), 0), "cast");
            LLVMValueRef res =
                LLVMBuildLoad2(*builder, LLVMInt32Type(), cast_addr, "res");
            LLVMBuildStore(*builder, res, t2c_gen_rs2_addr(start, builder, ir));
        });
})

/* 融合 LUI + SW：绝对地址存储。
 * addr = ir->imm（lui << 12）+ ir->imm2（sw 偏移）。
 * ir->rs1 是存储源寄存器。
 */
T2C_OP(fuse10, {
    IIF(RV32_HAS(SYSTEM))(
        { t2c_mmu_wrapper_fuse10(builder, start, ir); },
        {
            uint32_t addr_imm = (uint32_t) ir->imm + (uint32_t) ir->imm2;
            LLVMValueRef addr = LLVMConstInt(
                LLVMInt64Type(), (uint64_t) addr_imm + mem_base, false);
            LLVMValueRef cast_addr = LLVMBuildIntToPtr(
                *builder, addr, LLVMPointerType(LLVMInt32Type(), 0), "cast");
            T2C_LLVM_GEN_LOAD_VMREG(rs1, 32,
                                    t2c_gen_rs1_addr(start, builder, ir));
            LLVMBuildStore(*builder, val_rs1, cast_addr);
        });
})

/* 融合 LW + ADDI（后递增加载）。
 * addr = rv->X[ir->rs1] + ir->imm。
 * ir->rd = 加载目的寄存器。
 * ir->rs1 += ir->imm2（递增）。
 *
 * 注意：match_pattern() 要求 rd != rs1，以免在递增前覆盖基址寄存器。这样就能安全
 * 使用原始 rs1 值执行后递增。
 */
T2C_OP(fuse11, {
    IIF(RV32_HAS(SYSTEM))(
        { t2c_mmu_wrapper_fuse11(builder, start, ir); },
        {
            LLVMValueRef addr_rs1 = t2c_gen_rs1_addr(start, builder, ir);
            T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, addr_rs1);
            /* 计算地址并加载。 */
            LLVMValueRef mem_loc =
                t2c_gen_mem_loc(start, builder, ir, mem_base);
            LLVMValueRef res =
                LLVMBuildLoad2(*builder, LLVMInt32Type(), mem_loc, "res");
            LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
            /* rs1 按 imm2 递增，融合约束保证 rd != rs1。 */
            LLVMValueRef inc_val =
                T2C_LLVM_GEN_ALU32_IMM(Add, val_rs1, ir->imm2);
            LLVMBuildStore(*builder, inc_val, addr_rs1);
        });
})

/* 融合 ADDI + BNE（循环计数递减并分支）。
 * rd = rs1 + imm。
 * 如果 rd != 0，则跳转到 PC + 4 + imm2。
 */
T2C_OP(fuse12, {
    LLVMValueRef addr_PC = t2c_gen_PC_addr(start, builder, ir);
    /* 计算 rd = rs1 + imm。 */
    T2C_LLVM_GEN_LOAD_VMREG(rs1, 32, t2c_gen_rs1_addr(start, builder, ir));
    LLVMValueRef res = T2C_LLVM_GEN_ALU32_IMM(Add, val_rs1, ir->imm);
    LLVMBuildStore(*builder, res, t2c_gen_rd_addr(start, builder, ir));
    /* 比较 rd 与 0。 */
    T2C_LLVM_GEN_CMP_IMM32(NE, res, 0);
    /* 创建 taken 和 untaken 分支。 */
    LLVMBasicBlockRef taken = LLVMAppendBasicBlock(start, "taken");
    LLVMBuilderRef builder2 = LLVMCreateBuilder();
    LLVMPositionBuilderAtEnd(builder2, taken);
    if (ir->branch_taken &&
        t2c_check_valid_blk(rv, block, ir->branch_taken->pc)) {
        *taken_builder = builder2;
    } else {
        /* PC = ir->pc + 4 + ir->imm2；ADDI 4 字节，随后加分支偏移。 */
        T2C_LLVM_GEN_STORE_IMM32(builder2, ir->pc + 4 + ir->imm2, addr_PC);
        T2C_STORE_TIMER(builder2, start, insn_counter);
        LLVMBuildRetVoid(builder2);
    }
    LLVMBasicBlockRef untaken = LLVMAppendBasicBlock(start, "untaken");
    LLVMBuilderRef builder3 = LLVMCreateBuilder();
    LLVMPositionBuilderAtEnd(builder3, untaken);
    if (ir->branch_untaken &&
        t2c_check_valid_blk(rv, block, ir->branch_untaken->pc)) {
        *untaken_builder = builder3;
    } else {
        /* PC = ir->pc + 8；跳过 ADDI 和 BNE，两条各 4 字节。 */
        T2C_LLVM_GEN_STORE_IMM32(builder3, ir->pc + 8, addr_PC);
        T2C_STORE_TIMER(builder3, start, insn_counter);
        LLVMBuildRetVoid(builder3);
    }
    LLVMBuildCondBr(*builder, cmp, taken, untaken);
})
