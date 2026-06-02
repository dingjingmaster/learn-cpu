/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/*
 * 主执行引擎。
 *
 * 本文件组织解释器调度、基本块翻译、宏操作融合、CSR/trap 处理、运行时 profile
 * 和 JIT 热点提升。它连接 decode 生成的 IR、rv32_template.c 中的指令语义、
 * cache.c 中的基本块缓存，以及系统模式的中断/MMU 处理。
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if RV32_HAS(EXT_F)
#include <math.h>
#include "softfp.h"
#endif /* RV32_HAS(EXT_F) */

#if RV32_HAS(GDBSTUB)
extern struct target_ops gdbstub_ops;
#endif

#if defined(__EMSCRIPTEN__)
#include "em_runtime.h"
#endif

#include "decode.h"
#include "io.h"
#include "mpool.h"
#include "riscv.h"
#include "riscv_private.h"
#include "utils.h"

#if RV32_HAS(JIT)
#include "cache.h"
#include "jit.h"
#endif

/* 比较指定 RISC-V 指令字段的快捷宏。 */
#define IF_insn(i, o) (i->opcode == rv_insn_##o)
#define IF_rd(i, r) (i->rd == rv_reg_##r)
#define IF_rs1(i, r) (i->rs1 == rv_reg_##r)
#define IF_rs2(i, r) (i->rs2 == rv_reg_##r)
#define IF_imm(i, v) (i->imm == v)

#if RV32_HAS(SYSTEM)
#if !RV32_HAS(JIT)
static bool need_clear_block_map = false;
#endif
static uint32_t reloc_enable_mmu_jalr_addr;
static bool reloc_enable_mmu = false;
bool need_retranslate = false;
bool need_handle_signal = false;
#endif

/* 模拟非对齐 load/store。
 * 仅用于非 SYSTEM 构建中的用户态非对齐访存模拟。
 * SYSTEM 模式下，非对齐访存 trap 由客体 OS 处理。
 */
#if !RV32_HAS(SYSTEM)
/* 模拟非对齐 load。
 * 快速路径：2 字节对齐的 word 访问使用 halfword 读取。
 * 慢速路径：奇数地址回退为逐字节读取。
 */
static bool emulate_misaligned_load(riscv_t *rv,
                                    const rv_insn_t *ir,
                                    uint32_t addr)
{
    uint32_t value = 0;

    switch (ir->opcode) {
    case rv_insn_lw:
#if RV32_HAS(EXT_C)
    case rv_insn_clw:
    case rv_insn_clwsp:
#endif
        /* 读取 word：2 字节对齐走快速路径，奇数地址走慢速路径。 */
        if ((addr & 1) == 0) {
            /* 2 字节对齐：使用两次 halfword 读取。 */
            value = (uint32_t) rv->io.mem_read_s(rv, addr);
            value |= ((uint32_t) rv->io.mem_read_s(rv, addr + 2)) << 16;
        } else {
            /* 奇数地址：使用四次字节读取。 */
            for (int i = 0; i < 4; i++)
                value |= ((uint32_t) rv->io.mem_read_b(rv, addr + i))
                         << (i * 8);
        }
        rv->X[ir->rd] = value;
        break;

    case rv_insn_lh:
        /* 有符号读取 halfword：奇数地址始终使用逐字节读取。 */
        for (int i = 0; i < 2; i++)
            value |= ((uint32_t) rv->io.mem_read_b(rv, addr + i)) << (i * 8);
        rv->X[ir->rd] = sign_extend_h(value);
        break;

    case rv_insn_lhu:
        /* 无符号读取 halfword：奇数地址始终使用逐字节读取。 */
        for (int i = 0; i < 2; i++)
            value |= ((uint32_t) rv->io.mem_read_b(rv, addr + i)) << (i * 8);
        rv->X[ir->rd] = value;
        break;

    default:
        return false;
    }

    return true;
}

/* 模拟非对齐 store。
 * 快速路径：2 字节对齐的 word 访问使用 halfword 写入。
 * 慢速路径：奇数地址回退为逐字节写入。
 */
static bool emulate_misaligned_store(riscv_t *rv,
                                     const rv_insn_t *ir,
                                     uint32_t addr)
{
    uint32_t value;

    switch (ir->opcode) {
    case rv_insn_sw:
#if RV32_HAS(EXT_C)
    case rv_insn_csw:
    case rv_insn_cswsp:
#endif
        /* 写入 word：2 字节对齐走快速路径，奇数地址走慢速路径。 */
        value = rv->X[ir->rs2];
        if ((addr & 1) == 0) {
            /* 2 字节对齐：使用两次 halfword 写入。 */
            rv->io.mem_write_s(rv, addr, value & 0xFFFF);
            rv->io.mem_write_s(rv, addr + 2, (value >> 16) & 0xFFFF);
        } else {
            /* 奇数地址：使用四次字节写入。 */
            for (int i = 0; i < 4; i++)
                rv->io.mem_write_b(rv, addr + i, (value >> (i * 8)) & 0xFF);
        }
        break;

    case rv_insn_sh:
        /* 写入 halfword：奇数地址始终使用逐字节写入。 */
        value = rv->X[ir->rs2];
        for (int i = 0; i < 2; i++)
            rv->io.mem_write_b(rv, addr + i, (value >> (i * 8)) & 0xFF);
        break;

    default:
        return false;
    }

    return true;
}

/* 用户态模拟在没有配置 trap vector 时使用的默认 trap handler。
 * 发生非对齐访存时，该 handler 会用逐字节访问模拟原操作，而不是简单跳过指令。
 * 注意：SYSTEM 模式会通过恢复 PC 并清除 is_trapped 处理未配置的 trap vector，
 * 因此本函数只用于非 SYSTEM 构建。
 */
static void rv_trap_default_handler(riscv_t *rv)
{
    uint32_t cause = rv->csr_mcause;
    uint32_t tval = rv->csr_mtval; /* 保存非对齐地址。 */
    uint32_t insn_addr = rv->csr_mepc;

    /* 对非对齐 load/store 使用逐字节操作进行模拟。 */
    if (cause == LOAD_MISALIGNED || cause == STORE_MISALIGNED) {
        /* 取出触发异常的指令。 */
        uint32_t insn = rv->io.mem_ifetch(rv, insn_addr);
        if (!insn)
            goto skip_insn;

        /* 解码指令以确定操作类型和寄存器。 */
        rv_insn_t ir;
        memset(&ir, 0, sizeof(rv_insn_t));
        if (!rv_decode(&ir, insn))
            goto skip_insn;

        /* 执行非对齐操作的模拟。 */
        bool handled = false;
        if (cause == LOAD_MISALIGNED)
            handled = emulate_misaligned_load(rv, &ir, tval);
        else
            handled = emulate_misaligned_store(rv, &ir, tval);

        if (handled) {
            /* 已处理成功，PC 前进到下一条指令。 */
            rv->csr_mepc += rv->compressed ? 2 : 4;
            rv->PC = rv->csr_mepc;
            return;
        }
    }

skip_insn:
    /* 其他异常或模拟失败时，跳过当前指令。 */
    rv->csr_mepc += rv->compressed ? 2 : 4;
    rv->PC = rv->csr_mepc; /* mret */
}
#endif /* !RV32_HAS(SYSTEM) */

#if RV32_HAS(SYSTEM)
static void __trap_handler(riscv_t *rv);
#endif /* RV32_HAS(SYSTEM) */

#if RV32_HAS(ARCH_TEST)
/* 检查写入目标是否为 tohost；若是，则停止模拟。
 * 架构一致性测试会写 tohost 后进入无限循环，模拟器需要检测该写入并正常退出。
 */
static inline void check_tohost_write(riscv_t *rv,
                                      uint32_t addr,
                                      uint32_t value)
{
    if (rv->tohost_addr && addr == rv->tohost_addr && value != 0) {
        /* 向 tohost 写入非零值表示测试请求退出。 */
        rv->halt = true;
        /* 从 tohost 值中提取退出码（value >> 1）。 */
        vm_attr_t *attr = PRIV(rv);
        attr->exit_code = (value >> 1);
    }
}
#endif /* RV32_HAS(ARCH_TEST) */

/* 封装 load/store 和取指非对齐处理。
 * @mask_or_pc：load/store 使用对齐 mask，取指非对齐处理使用 pc。
 * @type：非对齐异常类型。
 * @compress：当前是否为压缩指令。
 * @IO：是否为 load/store 非对齐处理；否则为取指非对齐处理。
 */
#define RV_EXC_MISALIGN_HANDLER(mask_or_pc, type, compress, IO)              \
    IIF(IO)(if (!PRIV(rv)->allow_misalign && unlikely(addr & (mask_or_pc))), \
            if (unlikely(insn_is_misaligned(PC))))                           \
    {                                                                        \
        rv->compressed = compress;                                           \
        rv->csr_cycle = cycle;                                               \
        rv->PC = PC;                                                         \
        SET_CAUSE_AND_TVAL_THEN_TRAP(rv, type##_MISALIGNED,                  \
                                     IIF(IO)(addr, mask_or_pc));             \
        return false;                                                        \
    }

/* FIXME：使用更精确的时间更新方式，例如 RTC。 */
#if RV32_HAS(Zicsr)
static inline void update_time(riscv_t *rv)
{
#if !RV32_HAS(SYSTEM)
    struct timeval tv;

    rv_gettimeofday(&tv);
    rv->timer = (uint64_t) tv.tv_sec * 1e6 + (uint32_t) tv.tv_usec;
#else
    /* SYSTEM 模式：从 cycle counter 推导 timer。
     * timer 按需计算，而不是每条指令执行后递增。
     */
    rv->timer = rv->csr_cycle + rv->timer_offset;
#endif
    rv->csr_time[0] = rv->timer & 0xFFFFFFFF;
    rv->csr_time[1] = rv->timer >> 32;
}

/* 获取 CSR 对应的存储指针。 */
static uint32_t *csr_get_ptr(riscv_t *rv, uint32_t csr)
{
    /* csr & 0xFFF 可避免解码阶段的符号扩展影响索引。 */
    switch (csr & 0xFFF) {
    case CSR_MSTATUS: /* Machine 状态寄存器。 */
        return (uint32_t *) (&rv->csr_mstatus);
    case CSR_MTVEC: /* Machine trap handler 基址。 */
        return (uint32_t *) (&rv->csr_mtvec);
    case CSR_MISA: /* Machine ISA 和扩展位。 */
        return (uint32_t *) (&rv->csr_misa);

    /* Machine trap 处理相关 CSR。 */
    case CSR_MEDELEG: /* Machine 异常委托寄存器。 */
        return (uint32_t *) (&rv->csr_medeleg);
    case CSR_MIDELEG: /* Machine 中断委托寄存器。 */
        return (uint32_t *) (&rv->csr_mideleg);
    case CSR_MSCRATCH: /* Machine scratch 寄存器。 */
        return (uint32_t *) (&rv->csr_mscratch);
    case CSR_MEPC: /* Machine 异常 PC。 */
        return (uint32_t *) (&rv->csr_mepc);
    case CSR_MCAUSE: /* Machine 异常原因。 */
        return (uint32_t *) (&rv->csr_mcause);
    case CSR_MTVAL: /* Machine trap 附加值。 */
        return (uint32_t *) (&rv->csr_mtval);
    case CSR_MIP: /* Machine pending 中断。 */
        return (uint32_t *) (&rv->csr_mip);

    /* Machine 计数器和定时器。 */
    case CSR_CYCLE: /* RDCYCLE 指令读取的 cycle counter。 */
        return (uint32_t *) &rv->csr_cycle;
    case CSR_CYCLEH: /* cycle 的高 32 位。 */
        return &((uint32_t *) &rv->csr_cycle)[1];

    /* TIME/TIMEH：非常粗略地按约 1 ms 每 tick 处理。 */
    case CSR_TIME: /* RDTIME 指令读取的 timer。 */
        update_time(rv);
        return &rv->csr_time[0];
    case CSR_TIMEH: /* time 的高 32 位。 */
        update_time(rv);
        return &rv->csr_time[1];
    case CSR_INSTRET: /* 已退休指令数计数器。 */
        return (uint32_t *) (&rv->csr_cycle);
    case CSR_INSTRETH: /* 已退休指令数的高 32 位。 */
        return &((uint32_t *) &rv->csr_cycle)[1];
#if RV32_HAS(EXT_F)
    case CSR_FFLAGS:
        return (uint32_t *) (&rv->csr_fcsr);
    case CSR_FCSR:
        return (uint32_t *) (&rv->csr_fcsr);
#endif
    case CSR_SSTATUS:
        return (uint32_t *) (&rv->csr_sstatus);
    case CSR_SIE:
        return (uint32_t *) (&rv->csr_sie);
    case CSR_STVEC:
        return (uint32_t *) (&rv->csr_stvec);
    case CSR_SCOUNTEREN:
        return (uint32_t *) (&rv->csr_scounteren);
    case CSR_SSCRATCH:
        return (uint32_t *) (&rv->csr_sscratch);
    case CSR_SEPC:
        return (uint32_t *) (&rv->csr_sepc);
    case CSR_SCAUSE:
        return (uint32_t *) (&rv->csr_scause);
    case CSR_STVAL:
        return (uint32_t *) (&rv->csr_stval);
    case CSR_SIP:
        return (uint32_t *) (&rv->csr_sip);
    case CSR_SATP:
        return (uint32_t *) (&rv->csr_satp);
    default:
        return NULL;
    }
}

/* 为 cycle/time CSR 读取同步 cycle counter。
 * TIME CSR 需要先同步 cycle，update_time() 才能推导正确 timer。
 */
static inline void csr_sync_cycle(riscv_t *rv, uint32_t csr, uint64_t cycle)
{
    switch (csr & 0xFFF) {
    case CSR_CYCLE:
    case CSR_CYCLEH:
    case CSR_INSTRET:
    case CSR_INSTRETH:
    case CSR_TIME:
    case CSR_TIMEH:
        if (rv->csr_cycle != cycle)
            rv->csr_cycle = cycle;
        break;
    }
}

/* CSRRW（Atomic Read/Write CSR）在 CSR 和整数寄存器之间原子交换值。
 * CSRRW 读取 CSR 旧值，将其零扩展到 XLEN 位后写入 rd，同时把 rs1 的初始值
 * 写入 CSR。若 rd == x0，则指令不读取 CSR，也不会触发 CSR 读取的副作用。
 */
static uint32_t csr_csrrw(riscv_t *rv,
                          uint32_t csr,
                          uint32_t val,
                          uint64_t cycle)
{
    csr_sync_cycle(rv, csr, cycle);
    uint32_t *c = csr_get_ptr(rv, csr);
    if (!c)
        return 0;

    uint32_t out = *c;
#if RV32_HAS(EXT_F)
    if (csr == CSR_FFLAGS)
        out &= FFLAG_MASK;
#endif

#if RV32_HAS(SYSTEM)
    uint32_t old_satp = *c;
#endif
    *c = val;

#if RV32_HAS(SYSTEM)
    /* SATP 实际变化表示地址空间变化，需要刷新 TLB。 */
    if (c == &rv->csr_satp && *c != old_satp)
        mmu_tlb_flush_all(rv);
#if !RV32_HAS(JIT)
    /*
     * guestOS 的不同进程可能拥有相同 VA，因此 block map 不能跨地址空间复用。
     *
     * 这里不直接调用 block_map_clear()，而是设置标志，等对应 RVOP 的 code
     * 执行完后再清理。这样可避免 RVOP 的 code 后续访问到 NULL ir。
     */
    if (c == &rv->csr_satp)
        need_clear_block_map = true;
#endif
#endif

    return out;
}

/* 执行 CSRRS（atomic read and set）。 */
static uint32_t csr_csrrs(riscv_t *rv,
                          uint32_t csr,
                          uint32_t val,
                          uint64_t cycle)
{
    csr_sync_cycle(rv, csr, cycle);
    uint32_t *c = csr_get_ptr(rv, csr);
    if (!c)
        return 0;

    uint32_t out = *c;
#if RV32_HAS(EXT_F)
    if (csr == CSR_FFLAGS)
        out &= FFLAG_MASK;
#endif

#if RV32_HAS(SYSTEM)
    uint32_t old_satp = *c;
#endif
    *c |= val;

#if RV32_HAS(SYSTEM)
    /* SATP 实际变化时刷新 TLB。 */
    if (c == &rv->csr_satp && *c != old_satp)
        mmu_tlb_flush_all(rv);
#endif

    return out;
}

/* 执行 CSRRC（atomic read and clear）。
 * 读取 CSR 旧值，零扩展到 XLEN 位后写入 rd。
 * 读取 rs1 的值，将其作为位掩码清除 CSR 中的对应位。
 */
static uint32_t csr_csrrc(riscv_t *rv,
                          uint32_t csr,
                          uint32_t val,
                          uint64_t cycle)
{
    csr_sync_cycle(rv, csr, cycle);
    uint32_t *c = csr_get_ptr(rv, csr);
    if (!c)
        return 0;

    uint32_t out = *c;
#if RV32_HAS(EXT_F)
    if (csr == CSR_FFLAGS)
        out &= FFLAG_MASK;
#endif

#if RV32_HAS(SYSTEM)
    uint32_t old_satp = *c;
#endif
    *c &= ~val;

#if RV32_HAS(SYSTEM)
    /* SATP 实际变化时刷新 TLB。 */
    if (c == &rv->csr_satp && *c != old_satp)
        mmu_tlb_flush_all(rv);
#endif

    return out;
}
#endif

#if RV32_HAS(GDBSTUB)
void rv_debug(riscv_t *rv)
{
    if (!gdbstub_init(&rv->gdbstub, &gdbstub_ops,
                      (arch_info_t) {
                          .reg_num = 33,
                          .target_desc = TARGET_RV32,
                      },
                      GDBSTUB_COMM)) {
        return;
    }

    rv->debug_mode = true;
    rv->breakpoint_map = breakpoint_map_new();
    rv->is_interrupted = false;

    if (!gdbstub_run(&rv->gdbstub, (void *) rv))
        return;

    breakpoint_map_destroy(rv->breakpoint_map);
    gdbstub_close(&rv->gdbstub);
}
#endif /* RV32_HAS(GDBSTUB) */

#if !RV32_HAS(JIT)
/* block map 使用的哈希函数。 */
HASH_FUNC_IMPL(map_hash, BLOCK_MAP_CAPACITY_BITS, 1 << BLOCK_MAP_CAPACITY_BITS)
#endif

/* 分配并初始化一个基本块。 */
static block_t *block_alloc(riscv_t *rv)
{
    block_t *block = mpool_alloc(rv->block_mp);
    if (unlikely(!block))
        return NULL;
    assert(block);
    block->n_insn = 0;
#if RV32_HAS(SYSTEM_MMIO) && RV32_HAS(MOP_FUSION)
    block->n_lazy_candidates = 0;
    block->lazy_fusion_done = false;
#endif
#if RV32_HAS(JIT)
    block->translatable = true;
    block->hot = false;
    block->hot2 = false;
    block->has_loops = false;
    block->n_invoke = 0;
    block->func = NULL;
    INIT_LIST_HEAD(&block->list);
#if RV32_HAS(T2C)
    block->compiled = false;
    block->is_compiling = false;
    block->should_free = false;
    block->llvm_engine = NULL;
#endif
#endif
    return block;
}

#if !RV32_HAS(JIT)
/* 更新 L1 direct-mapped 基本块缓存。
 * 插入基本块后调用，使热点路径能快速查找。
 */
static inline void block_l1_update(riscv_t *rv, block_t *block)
{
    uint32_t idx = (block->pc_start >> BLOCK_L1_INDEX_SHIFT) & BLOCK_L1_MASK;
    rv->block_l1.tags[idx] = block->pc_start;
    rv->block_l1.ptrs[idx] = block;
}

/* 把基本块插入 block map。 */
static void block_insert(block_map_t *map, riscv_t *rv, const block_t *block)
{
    assert(map && block);
    const uint32_t mask = map->block_capacity - 1;
    uint32_t index = map_hash(block->pc_start);

    /* 线性探测插入 block map。 */
    for (;; index++) {
        if (!map->map[index & mask]) {
            map->map[index & mask] = (block_t *) block;
            break;
        }
    }
    map->size++;

    /* 更新 L1 缓存，便于后续快速命中。 */
    block_l1_update(rv, (block_t *) block);
}

/* 尝试在 block map 中查找已翻译的基本块。 */
static block_t *block_find(const block_map_t *map, const uint32_t addr)
{
    assert(map);
    uint32_t index = map_hash(addr);
    const uint32_t mask = map->block_capacity - 1;

    /* 在线性探测链上查找基本块。 */
    for (;; index++) {
        block_t *block = map->map[index & mask];
        if (!block)
            return NULL;

        if (block->pc_start == addr)
            return block;
    }
    return NULL;
}

/* 使用 L1 direct-mapped 缓存快速查找基本块。
 * L1 未命中时回退到哈希表。
 * 这是热点路径，针对紧凑循环做了优化。
 *
 * 使用分离数组：先检查 tag 数组（1KB），命中后再加载指针。
 * 在 x86-64 上实测比分散交织存储更快。
 */
static inline block_t *block_lookup_or_find(riscv_t *rv, uint32_t pc)
{
    /* L1 缓存查找：先检查 tag，未命中时避免加载指针。 */
    uint32_t idx = (pc >> BLOCK_L1_INDEX_SHIFT) & BLOCK_L1_MASK;
    if (likely(rv->block_l1.tags[idx] == pc))
        return rv->block_l1.ptrs[idx];

    /* L1 未命中，回退到哈希表查找。 */
    block_t *block = block_find(&rv->block_map, pc);

    /* 哈希表命中后回填 L1 缓存，提升后续查找速度。 */
    if (block) {
        rv->block_l1.tags[idx] = pc;
        rv->block_l1.ptrs[idx] = block;
    }

    return block;
}
#endif

#if !RV32_HAS(EXT_C)
FORCE_INLINE bool insn_is_misaligned(uint32_t pc)
{
    return pc & 0x3;
}
#endif

/* 每条 RISC-V 指令的长度信息。 */
enum {
#define _(inst, can_branch, insn_len, translatable, reg_mask) \
    __rv_insn_##inst##_len = insn_len,
    RV_INSN_LIST
#undef _
};

/* 每条 RISC-V 指令是否可能分支的信息。 */
enum {
#define _(inst, can_branch, insn_len, translatable, reg_mask) \
    __rv_insn_##inst##_canbranch = can_branch,
    RV_INSN_LIST
#undef _
};

#if RV32_HAS(GDBSTUB)
#define RVOP_NO_NEXT(ir) \
    (!ir->next | rv->debug_mode IIF(RV32_HAS(SYSTEM))(| rv->is_trapped, ))
#else
#define RVOP_NO_NEXT(ir) (!ir->next IIF(RV32_HAS(SYSTEM))(| rv->is_trapped, ))
#endif

/* 记录模拟执行时分支是否被采用。 */
static bool is_branch_taken = false;

/* 记录上一基本块的程序计数器。 */
static uint32_t last_pc = 0;

#if RV32_HAS(JIT)
static set_t pc_set;
static bool has_loops = false;
#endif

#if RV32_HAS(SYSTEM_MMIO)
extern void emu_update_uart_interrupts(riscv_t *rv);
extern void emu_update_rtc_interrupts(riscv_t *rv);
static uint32_t peripheral_update_ctr = 64;
#endif

/* 基于解释器的执行路径。
 * 采用基本块级 cycle 计数：进入基本块时预先递增 cycle，因此去掉逐指令 cycle++。
 * timer 在中断检查点（rv_check_interrupt）由 cycle 推导，而不是逐指令维护。
 */
#if RV32_HAS(SYSTEM)
#define RVOP_SYNC_PC(rv, PC) \
    do {                     \
        (rv)->PC = (PC);     \
    } while (0)
#else
#define RVOP_SYNC_PC(rv, PC) \
    do {                     \
    } while (0)
#endif

FORCE_INLINE bool insn_is_branch(uint8_t opcode)
{
    switch (opcode) {
#define _(inst, can_branch, insn_len, translatable, reg_mask) \
    IIF(can_branch)(case rv_insn_##inst:, )
        RV_INSN_LIST
#undef _
        return true;
    }
    return false;
}

#define RVOP(inst, code)                                                       \
    static PRESERVE_NONE bool do_##inst(riscv_t *rv, const rv_insn_t *ir,      \
                                        uint64_t cycle, uint32_t PC)           \
    {                                                                          \
        RVOP_SYNC_PC(rv, PC);                                                  \
        cycle++;                                                               \
        code;                                                                  \
        IIF(RV32_HAS(SYSTEM))(                                                 \
            if (need_handle_signal) {                                          \
                need_handle_signal = false;                                    \
                return true;                                                   \
            }, ) nextop : PC += __rv_insn_##inst##_len;                        \
        IIF(RV32_HAS(SYSTEM))(IIF(RV32_HAS(JIT))(                              \
                                  , if (unlikely(need_clear_block_map)) {      \
                                      block_map_clear(rv);                     \
                                      need_clear_block_map = false;            \
                                      rv->csr_cycle = cycle;                   \
                                      rv->PC = PC;                             \
                                      return false;                            \
                                  }), );                                       \
        if (unlikely(RVOP_NO_NEXT(ir)))                                        \
            goto end_op;                                                       \
        const rv_insn_t *next = ir->next;                                      \
        MUST_TAIL return next->impl(rv, next, cycle, PC);                      \
    end_op:                                                                    \
        IIF(RV32_HAS(BLOCK_CHAINING))(                                         \
            {                                                                  \
                /* 页边界终止基本块的 fallthrough：若 branch_taken 已设置，     \
                 * 且当前不是分支指令，则尾调用下一基本块。分支指令使用        \
                 * branch_taken 表示 taken 路径，不代表 fallthrough。           \
                 */                                                            \
                if (!insn_is_branch(ir->opcode)) {                             \
                    struct rv_insn *taken = ir->branch_taken;                  \
                    if (taken) {                                               \
                        IIF(RV32_HAS(SYSTEM))(                                 \
                            if (!rv->is_trapped) {                             \
                                last_pc = PC;                                  \
                                MUST_TAIL return taken->impl(rv, taken, cycle, \
                                                             PC);              \
                            },                                                 \
                            {                                                  \
                                last_pc = PC;                                  \
                                MUST_TAIL return taken->impl(rv, taken, cycle, \
                                                             PC);              \
                            });                                                \
                    }                                                          \
                }                                                              \
            }, );                                                              \
        rv->csr_cycle = cycle;                                                 \
        rv->PC = PC;                                                           \
        return true;                                                           \
    }

#include "rv32_template.c"
#undef RVOP

/* 融合指令尾部辅助函数：继续执行下一条 IR 或停止当前块。
 * 与 RVOP 宏的信号处理和 block map 清理逻辑保持一致。
 * 注意：RVOP 处理信号时不会保存 cycle/PC，这里也保持同样行为。
 */
static inline bool fuse_next_or_stop(riscv_t *rv,
                                     const rv_insn_t *ir,
                                     uint64_t cycle,
                                     uint32_t PC)
{
#if RV32_HAS(SYSTEM)
    if (need_handle_signal) {
        need_handle_signal = false;
        /* 与 RVOP 保持一致：不保存 cycle/PC 直接返回。信号处理器会从未修改的
         * rv->PC 判断合适的 PC。
         */
        return true;
    }
#if !RV32_HAS(JIT)
    if (unlikely(need_clear_block_map)) {
        block_map_clear(rv);
        need_clear_block_map = false;
        rv->csr_cycle = cycle;
        rv->PC = PC;
        return false;
    }
#endif
#endif
    if (unlikely(RVOP_NO_NEXT(ir))) {
        rv->csr_cycle = cycle;
        rv->PC = PC;
        return true;
    }
    const rv_insn_t *next = ir->next;
    MUST_TAIL return next->impl(rv, next, cycle, PC);
}

/* 多条连续 LUI 融合。 */
static PRESERVE_NONE bool do_fuse1(riscv_t *rv,
                                   const rv_insn_t *ir,
                                   uint64_t cycle,
                                   uint32_t PC)
{
    RVOP_SYNC_PC(rv, PC);
    cycle += ir->imm2;
    opcode_fuse_t *fuse = ir->fuse;
    for (int i = 0; i < ir->imm2; i++)
        rv->X[fuse[i].rd] = fuse[i].imm;
    PC += ir->imm2 * 4;
    return fuse_next_or_stop(rv, ir, cycle, PC);
}

/* LUI + ADD 融合。 */
static PRESERVE_NONE bool do_fuse2(riscv_t *rv,
                                   const rv_insn_t *ir,
                                   uint64_t cycle,
                                   uint32_t PC)
{
    RVOP_SYNC_PC(rv, PC);
    cycle += 2;
    rv->X[ir->rd] = ir->imm;
    rv->X[ir->rs2] = rv->X[ir->rd] + rv->X[ir->rs1];
    PC += 8;
    return fuse_next_or_stop(rv, ir, cycle, PC);
}

/* 多条连续 SW 融合。 */
static PRESERVE_NONE bool do_fuse3(riscv_t *rv,
                                   const rv_insn_t *ir,
                                   uint64_t cycle,
                                   uint32_t PC)
{
    RVOP_SYNC_PC(rv, PC);
    cycle += ir->imm2;
    opcode_fuse_t *fuse = ir->fuse;
    for (int i = 0; i < ir->imm2; i++) {
        uint32_t addr = rv->X[fuse[i].rs1] + fuse[i].imm;
        RV_EXC_MISALIGN_HANDLER(3, STORE, false, 1);
        uint32_t value = rv->X[fuse[i].rs2];
        MEM_WRITE_W(rv, addr, value);
#if RV32_HAS(ARCH_TEST)
        check_tohost_write(rv, addr, value);
#endif
    }
    PC += ir->imm2 * 4;
    return fuse_next_or_stop(rv, ir, cycle, PC);
}

/* 多条连续 LW 融合。 */
static PRESERVE_NONE bool do_fuse4(riscv_t *rv,
                                   const rv_insn_t *ir,
                                   uint64_t cycle,
                                   uint32_t PC)
{
    RVOP_SYNC_PC(rv, PC);
    cycle += ir->imm2;
    opcode_fuse_t *fuse = ir->fuse;
    for (int i = 0; i < ir->imm2; i++) {
        uint32_t addr = rv->X[fuse[i].rs1] + fuse[i].imm;
        RV_EXC_MISALIGN_HANDLER(3, LOAD, false, 1);
        rv->X[fuse[i].rd] = MEM_READ_W(rv, addr);
    }
    PC += ir->imm2 * 4;
    return fuse_next_or_stop(rv, ir, cycle, PC);
}

/* 根据融合指令数据执行移位操作。
 * 这样可避免把 opcode_fuse_t* 不安全地强转为 rv_insn_t*。
 */
static inline void fuse_shift_exec(riscv_t *rv, const opcode_fuse_t *f)
{
    switch (f->opcode) {
    case rv_insn_slli:
        rv->X[f->rd] = rv->X[f->rs1] << (f->imm & 0x1f);
        break;
    case rv_insn_srli:
        rv->X[f->rd] = rv->X[f->rs1] >> (f->imm & 0x1f);
        break;
    case rv_insn_srai:
        rv->X[f->rd] = ((int32_t) rv->X[f->rs1]) >> (f->imm & 0x1f);
        break;
    default:
        __UNREACHABLE;
        break;
    }
}

/* 多条连续立即数移位指令融合。 */
static PRESERVE_NONE bool do_fuse5(riscv_t *rv,
                                   const rv_insn_t *ir,
                                   uint64_t cycle,
                                   uint32_t PC)
{
    RVOP_SYNC_PC(rv, PC);
    cycle += ir->imm2;
    opcode_fuse_t *fuse = ir->fuse;
    for (int i = 0; i < ir->imm2; i++)
        fuse_shift_exec(rv, &fuse[i]);
    PC += ir->imm2 * 4;
    return fuse_next_or_stop(rv, ir, cycle, PC);
}

/* LI + ECALL 融合：li a7, imm; ecall。
 * 该融合只适用于标准 RV32I/M/A/F/C，因为 RV32E 使用不同 syscall 约定
 * （使用 t0 而不是 a7）。
 */
#if !RV32_HAS(RV32E)
static PRESERVE_NONE bool do_fuse6(riscv_t *rv,
                                   const rv_insn_t *ir,
                                   uint64_t cycle,
                                   uint32_t PC)
{
    RVOP_SYNC_PC(rv, PC);
    cycle += 2;
    rv->X[rv_reg_a7] = ir->imm;
    rv->compressed = false;
    rv->csr_cycle = cycle;
    /* ECALL 位于 PC+4（融合对中的第二条指令）。
     * on_ecall 期望 rv->PC 指向 ECALL 地址以便处理 trap。
     */
    rv->PC = PC + 4;
    rv->io.on_ecall(rv);
    return true;
}
#else
/* RV32E 桩函数：RV32E 不会生成 fuse6 模式。
 * 若意外分派到这里，作为防御性回退。
 */
static PRESERVE_NONE bool do_fuse6(riscv_t *rv UNUSED,
                                   const rv_insn_t *ir UNUSED,
                                   uint64_t cycle UNUSED,
                                   uint32_t PC UNUSED)
{
    assert(!"fuse6 should not be called in RV32E mode");
    return false;
}
#endif

/* 多条连续 ADDI 融合。 */
static PRESERVE_NONE bool do_fuse7(riscv_t *rv,
                                   const rv_insn_t *ir,
                                   uint64_t cycle,
                                   uint32_t PC)
{
    RVOP_SYNC_PC(rv, PC);
    cycle += ir->imm2;
    opcode_fuse_t *fuse = ir->fuse;
    for (int i = 0; i < ir->imm2; i++)
        /* 使用无符号算术避免有符号溢出 UB。
         * imm 转为 uint32_t 后仍保留二进制补码语义。
         */
        rv->X[fuse[i].rd] =
            (uint32_t) rv->X[fuse[i].rs1] + (uint32_t) fuse[i].imm;
    PC += ir->imm2 * 4;
    return fuse_next_or_stop(rv, ir, cycle, PC);
}

/* LUI + ADDI 融合：lui rd, imm20; addi rd, rd, imm12。
 * 这是加载 32 位常量（li 伪指令）的标准模式。
 * ir->imm = lui 立即数（已左移 12 位）。
 * ir->imm2 = addi 立即数（已按 12 位符号扩展）。
 * ir->rd = 目标寄存器。
 */
static PRESERVE_NONE bool do_fuse8(riscv_t *rv,
                                   const rv_insn_t *ir,
                                   uint64_t cycle,
                                   uint32_t PC)
{
    RVOP_SYNC_PC(rv, PC);
    cycle += 2;
    /* 转为 uint32_t 以避免有符号溢出 UB。 */
    rv->X[ir->rd] = (uint32_t) ir->imm + (uint32_t) ir->imm2;
    PC += 8;
    return fuse_next_or_stop(rv, ir, cycle, PC);
}

/* LUI + LW 融合：lui rd, imm20; lw rd2, imm12(rd)。
 * 这是绝对地址或 PC 相对 load 的常见模式（AUIPC->LUI 常量优化后）。
 * ir->imm = lui 立即数（已左移 12 位）。
 * ir->imm2 = lw 偏移（已按 12 位符号扩展）。
 * ir->rd = lui 目标寄存器，也作为 load 基址。
 * ir->rs2 = lw 目标寄存器。
 */
static PRESERVE_NONE bool do_fuse9(riscv_t *rv,
                                   const rv_insn_t *ir,
                                   uint64_t cycle,
                                   uint32_t PC)
{
    RVOP_SYNC_PC(rv, PC);
    cycle += 2;
    /* 先把 LUI 结果写入 rd；当 rd != LW 目标寄存器时这是必须的。
     * LUI 在 LW 前完成，因此即使 LW fault，该写入也已经发生。
     */
    rv->X[ir->rd] = ir->imm;
    /* 转为 uint32_t 以避免有符号溢出 UB。 */
    uint32_t addr = (uint32_t) ir->imm + (uint32_t) ir->imm2;
    RV_EXC_MISALIGN_HANDLER(3, LOAD, false, 1);
    rv->X[ir->rs2] = MEM_READ_W(rv, addr);
    PC += 8;
    return fuse_next_or_stop(rv, ir, cycle, PC);
}

/* LUI + SW 融合：lui rd, imm20; sw rs2, imm12(rd)。
 * 这是绝对地址或 PC 相对 store 的常见模式。
 * ir->imm = lui 立即数（已左移 12 位）。
 * ir->imm2 = sw 偏移（已按 12 位符号扩展）。
 * ir->rd = lui 目标寄存器，也作为 store 基址。
 * ir->rs1 = sw 源寄存器，也就是待写入数据。
 */
static PRESERVE_NONE bool do_fuse10(riscv_t *rv,
                                    const rv_insn_t *ir,
                                    uint64_t cycle,
                                    uint32_t PC)
{
    RVOP_SYNC_PC(rv, PC);
    cycle += 2;
    /* 先把 LUI 结果写入 rd；SW 不写寄存器，因此 rd 后续仍可能被使用。
     * LUI 在 SW 前完成，因此即使 SW fault，该写入也已经发生。
     */
    rv->X[ir->rd] = ir->imm;
    /* 转为 uint32_t 以避免有符号溢出 UB。 */
    uint32_t addr = (uint32_t) ir->imm + (uint32_t) ir->imm2;
    RV_EXC_MISALIGN_HANDLER(3, STORE, false, 1);
    uint32_t value = rv->X[ir->rs1];
    MEM_WRITE_W(rv, addr, value);
#if RV32_HAS(ARCH_TEST)
    check_tohost_write(rv, addr, value);
#endif
    PC += 8;
    return fuse_next_or_stop(rv, ir, cycle, PC);
}

/* LW + ADDI 后递增融合：lw rd, 0(rs1); addi rs1, rs1, step。
 * 这是指针遍历循环（memcpy、字符串操作等）的常见模式。
 * ir->rd = load 目标寄存器。
 * ir->rs1 = 基址寄存器，同时也是会递增的寄存器。
 * ir->imm = load 偏移。
 * ir->imm2 = 递增步长。
 */
static PRESERVE_NONE bool do_fuse11(riscv_t *rv,
                                    const rv_insn_t *ir,
                                    uint64_t cycle,
                                    uint32_t PC)
{
    RVOP_SYNC_PC(rv, PC);
    cycle += 2;
    uint32_t addr = rv->X[ir->rs1] + ir->imm;
    RV_EXC_MISALIGN_HANDLER(3, LOAD, false, 1);
    rv->X[ir->rd] = MEM_READ_W(rv, addr);
    /* 只有 load 成功时才递增 rs1（SYSTEM 模式下不能发生 trap）。
     * 非 SYSTEM 模式中 RAM 访问不会 fault，因此总会执行。
     */
#if RV32_HAS(SYSTEM)
    if (!rv->is_trapped)
#endif
        rv->X[ir->rs1] = rv->X[ir->rs1] + ir->imm2;
    PC += 8;
    return fuse_next_or_stop(rv, ir, cycle, PC);
}

/* ADDI + BNE 融合：addi rd, rs1, imm; bne rd, x0, offset。
 * 这是循环计数器（倒计时循环）的常见模式。
 * ir->rd = 计数器寄存器，由 addi 写入、由 bne 测试。
 * ir->rs1 = addi 源寄存器。
 * ir->imm = addi 立即数，倒计时场景通常为 -1。
 * ir->imm2 = 分支偏移。
 */
static PRESERVE_NONE bool do_fuse12(riscv_t *rv,
                                    const rv_insn_t *ir,
                                    uint64_t cycle,
                                    uint32_t PC)
{
    RVOP_SYNC_PC(rv, PC);
    cycle += 2;
    rv->X[ir->rd] = rv->X[ir->rs1] + ir->imm;

    if (rv->X[ir->rd] != 0) {
        /* 分支命中。 */
        is_branch_taken = true;
        PC += 4 + ir->imm2; /* ADDI 长度 + 分支偏移。 */
        struct rv_insn *taken = ir->branch_taken;
        if (taken) {
#if RV32_HAS(SYSTEM)
            if (!rv->is_trapped) {
                last_pc = PC;
                MUST_TAIL return taken->impl(rv, taken, cycle, PC);
            }
#else
            last_pc = PC;
            MUST_TAIL return taken->impl(rv, taken, cycle, PC);
#endif
        }
    } else {
        /* 分支未命中。 */
        is_branch_taken = false;
        PC += 8; /* 跳过 ADDI 和 BNE 两条指令。 */
        struct rv_insn *untaken = ir->branch_untaken;
        if (untaken) {
#if RV32_HAS(SYSTEM)
            if (!rv->is_trapped) {
                last_pc = PC;
                MUST_TAIL return untaken->impl(rv, untaken, cycle, PC);
            }
#else
            last_pc = PC;
            MUST_TAIL return untaken->impl(rv, untaken, cycle, PC);
#endif
        }
    }

    rv->csr_cycle = cycle;
    rv->PC = PC;
    return true;
}

/* clang-format off */
static const void *dispatch_table[] = {
    /* RV32 指令。 */
#define _(inst, can_branch, insn_len, translatable, reg_mask) [rv_insn_##inst] = do_##inst,
    RV_INSN_LIST
#undef _
    /* 宏操作融合指令。 */
#define _(inst) [rv_insn_##inst] = do_##inst,
    FUSE_INSN_LIST
#undef _
};
/* clang-format on */

#if RV32_HAS(JIT)
FORCE_INLINE bool insn_is_translatable(uint8_t opcode)
{
    switch (opcode) {
#define _(inst, can_branch, insn_len, translatable, reg_mask) \
    IIF(translatable)(case rv_insn_##inst:, )
        RV_INSN_LIST
#undef _
        return true;
    }
    return false;
}
#endif

#if RV32_HAS(BLOCK_CHAINING)
FORCE_INLINE bool insn_is_unconditional_branch(uint8_t opcode)
{
    switch (opcode) {
    case rv_insn_ecall:
    case rv_insn_ebreak:
    case rv_insn_jal:
    case rv_insn_jalr:
    case rv_insn_mret:
#if RV32_HAS(Zicsr)
    case rv_insn_csrrw:
#endif
#if RV32_HAS(SYSTEM)
    case rv_insn_sret:
#endif
#if RV32_HAS(EXT_C)
    case rv_insn_cj:
    case rv_insn_cjalr:
    case rv_insn_cjal:
    case rv_insn_cjr:
    case rv_insn_cebreak:
#endif
        return true;
    default:
        return false;
    }
}

FORCE_INLINE bool insn_is_direct_branch(uint8_t opcode)
{
    switch (opcode) {
    case rv_insn_jal:
#if RV32_HAS(EXT_C)
    case rv_insn_cjal:
    case rv_insn_cj:
#endif
        return true;
    default:
        return false;
    }
}
#endif

FORCE_INLINE bool insn_is_indirect_branch(uint8_t opcode)
{
    switch (opcode) {
    case rv_insn_jalr:
#if RV32_HAS(EXT_C)
    case rv_insn_cjalr:
    case rv_insn_cjr:
#endif
        return true;
    default:
        return false;
    }
}

static bool block_translate(riscv_t *rv, block_t *block)
{
retranslate:
    block->pc_start = block->pc_end = rv->PC;
#if RV32_HAS(BLOCK_CHAINING)
    block->page_terminated = false;
#endif

    rv_insn_t *prev_ir = NULL;
    rv_insn_t *ir = mpool_calloc(rv->block_ir_mp);
    if (unlikely(!ir))
        return false;
    block->ir_head = ir;

    /* 翻译当前基本块。 */
    while (true) {
        if (prev_ir)
            prev_ir->next = ir;

        /* 取下一条指令。 */
        uint32_t insn = rv->io.mem_ifetch(rv, block->pc_end);

#if RV32_HAS(SYSTEM)
        if (!insn && need_retranslate) {
            memset(block, 0, sizeof(block_t));
            need_retranslate = false;
            goto retranslate;
        }
#endif

        /* 若取指因 trap（页故障等）失败，则结束翻译。
         * 调用方会检查 rv->is_trapped 并调用 trap handler。
         * 注意：仅凭 insn==0 存在歧义，因此需要显式检查 trap 状态。
         */
        if (!insn)
            break;

        /* 解码指令。 */
        if (!rv_decode(ir, insn)) {
            rv->compressed = is_compressed(insn);
            SET_CAUSE_AND_TVAL_THEN_TRAP(rv, ILLEGAL_INSN, insn);
            break;
        }
        ir->impl = dispatch_table[ir->opcode];
        ir->pc = block->pc_end; /* 记录当前 IR 对应的 PC。 */
        block->pc_end += is_compressed(insn) ? 2 : 4;
        block->n_insn++;
        prev_ir = ir;
#if RV32_HAS(JIT)
        if (!insn_is_translatable(ir->opcode))
            block->translatable = false;
#endif
        /* 遇到分支指令时终止当前基本块。 */
        if (insn_is_branch(ir->opcode)) {
            if (insn_is_indirect_branch(ir->opcode)) {
                ir->branch_table = calloc(1, sizeof(branch_history_table_t));
                if (unlikely(!ir->branch_table))
                    return false;
                assert(ir->branch_table);
                memset(ir->branch_table->PC, -1,
                       sizeof(uint32_t) * HISTORY_SIZE);
            }
            break;
        }

#if RV32_HAS(BLOCK_CHAINING)
        /* 在页边界终止基本块，以支持 O(1) 缓存失效。
         * 每个基本块完全位于一个 4KB 页内，SFENCE.VMA 即可按页地址失效基本块，
         * 无需扫描整个缓存。
         */
        {
            const uint32_t page_end =
                (block->pc_start & ~(RV_PG_SIZE - 1)) + RV_PG_SIZE;
            if (block->pc_end >= page_end) {
                block->page_terminated = true;
                break;
            }
        }
#endif

        ir = mpool_calloc(rv->block_ir_mp);
        if (unlikely(!ir))
            return false;
    }

    /* 若没有任何指令成功解码（例如第一条就是非法指令），释放已分配 IR 并返回失败。
     */
    if (unlikely(!prev_ir)) {
        mpool_free(rv->block_ir_mp, block->ir_head);
        return false;
    }

    /* 释放上一轮末尾预分配但因提前退出而未使用的孤儿 IR（取指失败、解码失败等）。
     * 该 IR 已链接到 prev_ir->next，但从未成为 prev_ir。
     */
    if (prev_ir->next)
        mpool_free(rv->block_ir_mp, prev_ir->next);

    block->ir_tail = prev_ir;
    block->ir_tail->next = NULL;
    /* 在宏操作融合前设置 cycle cost。这里刻意按原始指令计数以保持计时准确，
     * 融合操作仍代表与未融合序列相同的逻辑工作量。
     */
    block->cycle_cost = block->n_insn;
    return true;
}

#if RV32_HAS(MOP_FUSION)
static inline void remove_next_nth_ir(const riscv_t *rv,
                                      rv_insn_t *ir,
                                      block_t *block,
                                      uint8_t n)
{
    for (uint8_t i = 0; i < n; i++) {
        rv_insn_t *next = ir->next;
        ir->next = ir->next->next;
        mpool_free(rv->block_ir_mp, next);
    }
    if (!ir->next)
        block->ir_tail = ir;
    block->n_insn -= n;
}

/* 统计同 opcode 的连续指令数量。 */
static inline int count_consecutive_insn(rv_insn_t *ir, uint8_t opcode)
{
    int count = 1;
    rv_insn_t *next = ir->next;
    while (next && next->opcode == opcode) {
        count++;
        if (!next->next)
            break;
        next = next->next;
    }
    return count;
}

/* 判断指令是否为立即数移位。 */
static inline bool is_shift_imm(const rv_insn_t *ir)
{
    return IF_insn(ir, slli) || IF_insn(ir, srli) || IF_insn(ir, srai);
}

/* 统计连续立即数移位指令数量。 */
static inline int count_consecutive_shift(rv_insn_t *ir)
{
    int count = 1;
    rv_insn_t *next = ir->next;
    while (next && is_shift_imm(next)) {
        count++;
        if (!next->next)
            break;
        next = next->next;
    }
    return count;
}

/* 分配并重写一段可融合指令序列。
 * 成功返回 true；分配失败时返回 false，以便优雅退化为未融合执行。
 * 长度不超过 FUSE_MAX_ENTRIES 的 fuse 数组使用内存池分配。
 */
static inline bool try_fuse_sequence(riscv_t *rv,
                                     block_t *block,
                                     rv_insn_t *ir,
                                     int count,
                                     uint8_t fuse_opcode)
{
    if (count <= 1)
        return false;

    /* 拒绝超过内存池槽位大小的序列；这种情况少见且收益递减。
     * 该检查同时隐含处理了溢出风险。
     */
    if (unlikely(count > FUSE_MAX_ENTRIES))
        return false;

    opcode_fuse_t *fuse_data = mpool_alloc(rv->fuse_mp);
    if (unlikely(!fuse_data))
        return false;

    ir->fuse = fuse_data;
    /* 修改 opcode 前先复制原始指令，保留 fuse[0] 中的原 opcode，
     * 供 shift_func 等 handler 使用。
     */
    memcpy(ir->fuse, ir, sizeof(opcode_fuse_t));
    ir->opcode = fuse_opcode;
    ir->imm2 = count;
    ir->impl = dispatch_table[ir->opcode];

    rv_insn_t *next_ir = ir->next;
    for (int j = 1; j < count; j++, next_ir = next_ir->next)
        memcpy(ir->fuse + j, next_ir, sizeof(opcode_fuse_t));

    remove_next_nth_ir(rv, ir, block, count - 1);
    return true;
}

#if RV32_HAS(SYSTEM_MMIO)
/* 函数参数寄存器（a0-a7）通常会在调用间变化。
 * 惰性融合只验证一次；如果这些寄存器在后续调用中指向 MMIO，验证结果会失效。
 */
#define ARG_REG_MASK                                                  \
    ((1u << 10) | (1u << 11) | (1u << 12) | (1u << 13) | (1u << 14) | \
     (1u << 15) | (1u << 16) | (1u << 17))

/* 检查 LW/SW 序列是否适合惰性融合验证。
 *
 * 安全要求：
 * 1. 当前块中位于序列前的指令不能写入该序列使用的任何 rs1。
 *    这里通过 modified_regs_before 做 O(1) 查询，避免 O(N) 扫描。
 * 2. 序列内部不能存在依赖：任意 j > i 时，指令 i 的 rd 不能等于指令 j 的 rs1。
 *    这可避免指针追逐模式，例如 lw x10, 0(x11); lw x12, 0(x10)，其中 x10
 *    会在序列中途变化。
 * 3. 基址不能使用函数参数寄存器（a0-a7），这些寄存器在不同函数调用中可能指向
 *    不同内存区域（RAM 或 MMIO）。
 *
 * 安全则返回 true，否则返回 false。
 */
static bool lazy_fusion_safe_base_regs(uint32_t modified_regs_before,
                                       rv_insn_t *seq_start,
                                       int count,
                                       bool is_load)
{
    /* 收集该序列使用的所有 rs1 寄存器。 */
    uint32_t rs1_mask = 0;
    rv_insn_t *ir = seq_start;
    for (int i = 0; i < count && ir; i++, ir = ir->next) {
        if (ir->rs1 < 32)
            rs1_mask |= (1u << ir->rs1);
    }

    /* x0 永远不会被写入，从掩码中移除。 */
    rs1_mask &= ~1u;

    if (rs1_mask == 0)
        return true; /* 只使用 x0，始终安全。 */

    /* 拒绝使用函数参数寄存器（a0-a7）的序列。
     * 它们可能在某次调用中指向 RAM、另一次调用中指向 MMIO，使缓存的验证结果
     * 无法跨调用复用。
     */
    if (rs1_mask & ARG_REG_MASK)
        return false;

    /* O(1) 检查：序列前是否有指令写过任意 rs1？ */
    if (rs1_mask & modified_regs_before)
        return false;

    /* 检查序列内部依赖（仅 LW；SW 不写 rd）。
     * 若指令 i 写 rd，且之后的指令 j（j > i）用该 rd 作为 rs1，验证阶段会算出
     * 错误地址（指针追逐）。
     */
    if (is_load) {
        uint32_t written_mask = 0;
        ir = seq_start;
        for (int i = 0; i < count && ir; i++, ir = ir->next) {
            /* 检查当前指令的 rs1 是否已被前面的指令写入。 */
            if (ir->rs1 != 0 && (written_mask & (1u << ir->rs1))) {
                return false; /* 检测到序列内部依赖。 */
            }
            /* 记录当前指令的写入。 */
            if (ir->rd != 0)
                written_mask |= (1u << ir->rd);
        }
    }

    return true;
}
#endif

/* 检查基本块中的指令是否匹配特定模式；若匹配，则重写为融合指令。
 *
 * 后续可通过更多策略提高模式命中率，包括可能的指令重排。
 */
static void match_pattern(riscv_t *rv, block_t *block)
{
    uint32_t i;
    rv_insn_t *ir;
#if RV32_HAS(SYSTEM_MMIO)
    /* 记录到当前位置为止已修改的寄存器，供惰性融合做 O(1) 安全检查。 */
    uint32_t modified_regs = 0;
#endif
    for (i = 0, ir = block->ir_head; i < block->n_insn - 1;
         i++, ir = ir->next) {
        assert(ir);
        rv_insn_t *next_ir = NULL;
        int32_t count = 0;
        switch (ir->opcode) {
        case rv_insn_lui:
            next_ir = ir->next;
            if (!next_ir)
                break;
            switch (next_ir->opcode) {
            case rv_insn_add:
                /* LUI + ADD 融合（fuse2）。 */
                if (ir->rd == next_ir->rs2 || ir->rd == next_ir->rs1) {
#if RV32_HAS(SYSTEM_MMIO)
                    /* 融合前记录 LUI 的 rd 和 ADD 的 rd。 */
                    if (ir->rd != 0)
                        modified_regs |= (1u << ir->rd);
                    if (next_ir->rd != 0)
                        modified_regs |= (1u << next_ir->rd);
#endif
                    ir->opcode = rv_insn_fuse2;
                    ir->rs2 = next_ir->rd;
                    ir->rs1 =
                        (ir->rd == next_ir->rs2) ? next_ir->rs1 : next_ir->rs2;
                    ir->impl = dispatch_table[ir->opcode];
                    remove_next_nth_ir(rv, ir, block, 1);
                }
                break;
            case rv_insn_addi:
                /* LUI + ADDI 融合（fuse8）：lui rd, imm; addi rd, rd, imm。
                 * 这是标准 32 位常量加载（li 伪指令）模式。
                 * rd == x0 时跳过：LUI x0 结果是 0，而不是 imm << 12。
                 */
                if (ir->rd != rv_reg_zero && ir->rd == next_ir->rs1 &&
                    ir->rd == next_ir->rd) {
                    /* ir->imm 已保存 lui 的高位立即数（已移位）。
                     * 将 addi 立即数存入 imm2，供 handler 使用。
                     */
                    ir->imm2 = next_ir->imm;
                    ir->opcode = rv_insn_fuse8;
                    ir->impl = dispatch_table[ir->opcode];
                    remove_next_nth_ir(rv, ir, block, 1);
                }
                break;
            case rv_insn_lw:
                /* LUI + LW 融合（fuse9）：lui rd, imm20; lw rd2, imm12(rd)。
                 * 这是绝对地址或 PC 相对 load 的常见模式。
                 * lui 结果作为 lw 的基址。
                 * rd == x0 时跳过：LUI x0 结果是 0，而不是 imm << 12。
                 *
                 * SYSTEM 模式下，JIT 通过 MMU handler 做地址转换。
                 */
                /* LUI + LW 融合（fuse9）。 */
                if (ir->rd != rv_reg_zero && ir->rd == next_ir->rs1) {
                    ir->imm2 = next_ir->imm; /* lw 偏移。 */
                    ir->rs2 = next_ir->rd;   /* lw 目标寄存器。 */
                    ir->opcode = rv_insn_fuse9;
                    ir->impl = dispatch_table[ir->opcode];
                    remove_next_nth_ir(rv, ir, block, 1);
#if RV32_HAS(SYSTEM_MMIO)
                    /* 记录 rs2（lw 目标寄存器），供惰性融合安全检查使用。 */
                    if (ir->rs2 != 0)
                        modified_regs |= (1u << ir->rs2);
#endif
                }
                break;
            case rv_insn_sw:
                /* LUI + SW 融合（fuse10）：lui rd, imm20; sw rs2, imm12(rd)。
                 * 这是绝对地址或 PC 相对 store 的常见模式。
                 * lui 结果作为 sw 的基址。
                 * rd == x0 时跳过：LUI x0 结果是 0，而不是 imm << 12。
                 * rd == rs2 时跳过：JIT 会用 rd 作为地址计算暂存寄存器，这会覆盖
                 * 待写入的值。
                 *
                 * SYSTEM 模式下，JIT 通过 MMU handler 做地址转换。
                 */
                /* LUI + SW 融合（fuse10）。 */
                if (ir->rd != rv_reg_zero && ir->rd == next_ir->rs1 &&
                    ir->rd != next_ir->rs2) {
                    ir->imm2 = next_ir->imm; /* sw 偏移。 */
                    ir->rs1 = next_ir->rs2;  /* sw 源寄存器（待写入数据）。 */
                    ir->opcode = rv_insn_fuse10;
                    ir->impl = dispatch_table[ir->opcode];
                    remove_next_nth_ir(rv, ir, block, 1);
                }
                break;
            case rv_insn_lui:
                /* 多条 LUI 融合（fuse1）。 */
                count = count_consecutive_insn(ir, rv_insn_lui);
#if RV32_HAS(SYSTEM_MMIO)
                /* 融合删除指令前，记录所有 rd。 */
                {
                    rv_insn_t *tmp = ir;
                    for (int j = 0; j < count && tmp; j++, tmp = tmp->next) {
                        if (tmp->rd != 0)
                            modified_regs |= (1u << tmp->rd);
                    }
                }
#endif
                try_fuse_sequence(rv, block, ir, count, rv_insn_fuse1);
                break;
            }
            break;
            /* 融合连续 SW 或 LW 以降低分派开销。内存地址不要求连续；
             * 每条融合指令的地址都会在运行时独立计算。
             */
        case rv_insn_sw:
            /* 多条 SW 融合（fuse3）。 */
            count = count_consecutive_insn(ir, rv_insn_sw);
#if RV32_HAS(SYSTEM_MMIO)
            /* SYSTEM_MMIO 模式下先标记为惰性融合候选。
             * 只有验证所有地址都是 RAM 后才真正融合。
             * 若基址寄存器在序列前被修改过，则跳过。
             */
            if (count > 1 && block->n_lazy_candidates < MAX_LAZY_CANDIDATES &&
                lazy_fusion_safe_base_regs(modified_regs, ir, count, false)) {
                lazy_fusion_candidate_t *cand =
                    &block->lazy_candidates[block->n_lazy_candidates++];
                cand->ir = ir;
                cand->count = (uint8_t) count;
                cand->opcode = rv_insn_sw;
                cand->verified = false;
                cand->failed = false;
                /* 跳过该序列，避免产生重叠候选。
                 * SW 不写 rd，因此无需更新 modified_regs。
                 */
                for (int skip = 1; skip < count && ir->next; skip++) {
                    ir = ir->next;
                    i++;
                }
            }
#else
            try_fuse_sequence(rv, block, ir, count, rv_insn_fuse3);
#endif
            break;
        case rv_insn_lw:
            /* 先检查 LW + ADDI 后递增融合（fuse11）。
             * SYSTEM 模式下，JIT 通过 MMU handler 做地址转换。
             */
            next_ir = ir->next;
            /* fuse11：LW + ADDI 后递增融合。 */
            if (next_ir && IF_insn(next_ir, addi) && ir->rs1 == next_ir->rs1 &&
                next_ir->rs1 == next_ir->rd && ir->rd != ir->rs1) {
                /* 模式：lw rd, imm(rs1); addi rs1, rs1, step。
                 * 约束：rd != rs1，避免使用前覆盖基址。
                 */
                ir->imm2 = next_ir->imm; /* 递增步长。 */
                ir->opcode = rv_insn_fuse11;
                ir->impl = dispatch_table[ir->opcode];
                remove_next_nth_ir(rv, ir, block, 1);
#if RV32_HAS(SYSTEM_MMIO)
                /* 记录 rd（lw 目标）和 rs1（后递增寄存器）。 */
                if (ir->rd != 0)
                    modified_regs |= (1u << ir->rd);
                if (ir->rs1 != 0)
                    modified_regs |= (1u << ir->rs1);
#endif
                break;
            }
            /* 多条 LW 融合（fuse4）。 */
            count = count_consecutive_insn(ir, rv_insn_lw);
#if RV32_HAS(SYSTEM_MMIO)
            /* SYSTEM_MMIO 模式下先标记为惰性融合候选。
             * 只有验证所有地址都是 RAM 后才真正融合。
             * 若基址寄存器在序列前或序列内被修改过，则跳过。
             */
            if (count > 1 && block->n_lazy_candidates < MAX_LAZY_CANDIDATES &&
                lazy_fusion_safe_base_regs(modified_regs, ir, count, true)) {
                lazy_fusion_candidate_t *cand =
                    &block->lazy_candidates[block->n_lazy_candidates++];
                cand->ir = ir;
                cand->count = (uint8_t) count;
                cand->opcode = rv_insn_lw;
                cand->verified = false;
                cand->failed = false;
                /* 跳过该序列，避免产生重叠候选。
                 * 同时记录所有 rd 写入，供后续安全检查使用。
                 */
                for (int skip = 1; skip < count && ir->next; skip++) {
                    if (ir->rd != 0)
                        modified_regs |= (1u << ir->rd);
                    ir = ir->next;
                    i++;
                }
            }
#else
            try_fuse_sequence(rv, block, ir, count, rv_insn_fuse4);
#endif
            break;
            /* TODO：支持 SW 和 LW 混合序列。 */
            /* TODO：通过指令重排提高模式命中率。 */
        case rv_insn_slli:
        case rv_insn_srli:
        case rv_insn_srai:
            /* 多条立即数移位指令融合（fuse5）。 */
            count = count_consecutive_shift(ir);
#if RV32_HAS(SYSTEM_MMIO)
            /* 融合删除指令前，记录所有 rd。 */
            {
                rv_insn_t *tmp = ir;
                for (int j = 0; j < count && tmp; j++, tmp = tmp->next) {
                    if (tmp->rd != 0)
                        modified_regs |= (1u << tmp->rd);
                }
            }
#endif
            try_fuse_sequence(rv, block, ir, count, rv_insn_fuse5);
            break;
        case rv_insn_addi:
            next_ir = ir->next;
#if !RV32_HAS(RV32E)
            /* LI a7 + ECALL 融合（fuse6）：li a7, imm; ecall。 */
            if (ir->rd == rv_reg_a7 && ir->rs1 == rv_reg_zero && next_ir &&
                IF_insn(next_ir, ecall)) {
                ir->opcode = rv_insn_fuse6;
                ir->impl = dispatch_table[ir->opcode];
                remove_next_nth_ir(rv, ir, block, 1);
                break;
            }
#endif
            /* ADDI + BNE 循环计数器融合（fuse12）：
             * addi rd, rs1, imm; bne rd, x0, offset。
             * 这是倒计时循环的常见模式。
             * rd == x0 时跳过：ADDI x0 结果为 0，会破坏分支逻辑。
             */
            if (next_ir && IF_insn(next_ir, bne) && ir->rd != rv_reg_zero &&
                ir->rd == next_ir->rs1 && next_ir->rs2 == rv_reg_zero) {
                ir->imm2 = next_ir->imm; /* 分支偏移。 */
                ir->opcode = rv_insn_fuse12;
                ir->impl = dispatch_table[ir->opcode];
                /* 复制分支目标，供基本块链接使用。 */
                ir->branch_taken = next_ir->branch_taken;
                ir->branch_untaken = next_ir->branch_untaken;
                remove_next_nth_ir(rv, ir, block, 1);
                break;
            }
            /* 多条 ADDI 融合（fuse7）。 */
            count = count_consecutive_insn(ir, rv_insn_addi);
#if RV32_HAS(SYSTEM_MMIO)
            /* 融合删除指令前，记录所有 rd。 */
            {
                rv_insn_t *tmp = ir;
                for (int j = 0; j < count && tmp; j++, tmp = tmp->next) {
                    if (tmp->rd != 0)
                        modified_regs |= (1u << tmp->rd);
                }
            }
#endif
            try_fuse_sequence(rv, block, ir, count, rv_insn_fuse7);
            break;
        }
#if RV32_HAS(SYSTEM_MMIO)
        /* 记录非融合指令的寄存器修改。
         * 融合指令的写入在上方已经显式记录。
         */
        if (ir->rd != 0)
            modified_regs |= (1u << ir->rd);
#endif
    }
}

#if RV32_HAS(SYSTEM_MMIO)
/* 尝试惰性融合前的最小基本块执行次数。
 * 用于避免在冷基本块上产生验证开销。
 */
#define LAZY_FUSION_HOTNESS_THRESHOLD 8

/* 验证惰性融合候选的地址；若全部是 RAM，则执行融合。
 * 在 cache hit 且 lazy_fusion_done 为 false 时调用。
 * 使用当前寄存器值计算地址。
 *
 * 安全保护：
 * - MMU 启用时禁用，因为虚拟地址无法可靠对应物理 RAM 区域。
 * - 只在基本块变热（执行多次）后运行。
 * - 基址寄存器修改情况在标记候选时检查。
 */
static void try_lazy_fusion(riscv_t *rv, block_t *block)
{
    if (block->lazy_fusion_done || block->n_lazy_candidates == 0)
        return;

#if RV32_HAS(SYSTEM)
    /* MMU 启用时禁用惰性融合。
     * 虚拟地址可能无法对应物理 RAM 区域，而这里不能做无副作用地址转换。
     */
    if (rv->csr_satp != 0) {
        block->lazy_fusion_done = true; /* MMU 开启时不再重试。 */
        return;
    }
#endif

#if RV32_HAS(JIT)
    /* 只对热点基本块尝试融合，避免冷块开销。 */
    if (block->n_invoke < LAZY_FUSION_HOTNESS_THRESHOLD)
        return;
#endif

    /* 检查所有候选是否都已进入终态。 */
    bool all_finalized = true;

    for (uint8_t i = 0; i < block->n_lazy_candidates; i++) {
        lazy_fusion_candidate_t *cand = &block->lazy_candidates[i];
        if (cand->failed || cand->verified)
            continue;

        /* 验证该候选内所有地址都是 RAM，而不是 MMIO。 */
        bool all_ram = true;
        rv_insn_t *ir = cand->ir;
        for (int j = 0; j < cand->count && ir; j++, ir = ir->next) {
            /* 计算地址：base + offset。 */
            uint32_t addr = rv->X[ir->rs1] + (uint32_t) ir->imm;

            /* 检查地址是否位于 RAM 边界内。 */
            if (!GUEST_RAM_CONTAINS(PRIV(rv)->mem, addr, 4)) {
                all_ram = false;
                cand->failed = true;
                break;
            }
        }

        if (all_ram) {
            /* 尝试融合；可能因分配失败而失败。 */
            uint8_t fuse_opcode =
                (cand->opcode == rv_insn_lw) ? rv_insn_fuse4 : rv_insn_fuse3;
            if (try_fuse_sequence(rv, block, cand->ir, cand->count,
                                  fuse_opcode)) {
                cand->verified = true;
            } else {
                /* 分配失败，之后可重试。 */
                all_finalized = false;
            }
        }
    }

    /* 只有所有候选都进入终态后才标记完成。 */
    if (all_finalized)
        block->lazy_fusion_done = true;
}
#endif /* RV32_HAS(SYSTEM_MMIO) */
#endif /* RV32_HAS(MOP_FUSION) */

typedef struct {
    bool is_constant[N_RV_REGS];
    uint32_t const_val[N_RV_REGS];
} constopt_info_t;

#define CONSTOPT(inst, code)                                  \
    static void constopt_##inst(rv_insn_t *ir UNUSED,         \
                                constopt_info_t *info UNUSED) \
    {                                                         \
        code;                                                 \
    }

#include "rv32_constopt.c"
static const void *constopt_table[] = {
#define _(inst, can_branch, insn_len, translatable, reg_mask) \
    [rv_insn_##inst] = constopt_##inst,
    RV_INSN_LIST
#undef _
};
#undef CONSTOPT

typedef void (*constopt_func_t)(rv_insn_t *, constopt_info_t *);
static void optimize_constant(riscv_t *rv UNUSED, block_t *block)
{
    constopt_info_t info = {.is_constant[0] = true};
    assert(rv->X[0] == 0);

    uint32_t i;
    rv_insn_t *ir;
    for (i = 0, ir = block->ir_head; i < block->n_insn; i++, ir = ir->next)
        ((constopt_func_t) constopt_table[ir->opcode])(ir, &info);
}

static block_t *prev = NULL;
static block_t *block_find_or_translate(riscv_t *rv)
{
#if !RV32_HAS(JIT)
    block_map_t *map = &rv->block_map;
    /* 使用 L1 缓存查找下一基本块，未命中时回退到哈希表。 */
    block_t *next_blk = block_lookup_or_find(rv, rv->PC);
#else
    /* 在 block cache 中查找下一基本块。 */
    block_t *next_blk = (block_t *) cache_get(rv->block_cache, rv->PC, true);
#if RV32_HAS(SYSTEM)
    /* satp 不匹配或基本块被 SFENCE.VMA 失效时，丢弃该缓存命中。 */
    if (next_blk && (next_blk->satp != rv->csr_satp || next_blk->invalidated))
        next_blk = NULL;
#endif
#endif

    if (next_blk) {
#if RV32_HAS(SYSTEM_MMIO) && RV32_HAS(MOP_FUSION)
        /* cache hit（第二次及后续执行）时，对 LW/SW 序列尝试惰性融合；
         * 融合前需验证地址都是 RAM。
         */
        try_lazy_fusion(rv, next_blk);
#endif
        return next_blk;
    }

#if !RV32_HAS(JIT)
    /* block map 接近填满时清空，避免探测链过长。 */
    if (map->size * 1.25 > map->block_capacity) {
        block_map_clear(rv);
        prev = NULL;
    }
#endif
    /* 分配新基本块。 */
    next_blk = block_alloc(rv);
    if (unlikely(!next_blk))
        return NULL;

    if (unlikely(!block_translate(rv, next_blk)))
        return NULL;

#if RV32_HAS(JIT) && RV32_HAS(SYSTEM)
    /*
     * 取指 fault 可能修改 satp，因此不要在 block_alloc() 中设置该值。
     */
    next_blk->satp = rv->csr_satp;
    next_blk->invalidated = false;
#endif

    optimize_constant(rv, next_blk);
#if RV32_HAS(MOP_FUSION)
    /* 执行宏操作融合。 */
    match_pattern(rv, next_blk);
#endif

#if !RV32_HAS(JIT)
    /* 将基本块插入 block map 和 L1 缓存。 */
    block_insert(&rv->block_map, rv, next_blk);
#else
    list_add(&next_blk->list, &rv->block_list);

#if RV32_HAS(T2C)
    pthread_mutex_lock(&rv->cache_lock);
#endif

    /* 将基本块插入 block cache。 */
    block_t *replaced_blk = cache_put(rv->block_cache, rv->PC, next_blk);

    if (!replaced_blk) {
#if RV32_HAS(T2C)
        pthread_mutex_unlock(&rv->cache_lock);
#endif
        return next_blk;
    }

    if (prev == replaced_blk)
        prev = NULL;

    /* 移除父基本块到被替换基本块的连接。 */
    rv_insn_t *replaced_blk_entry = replaced_blk->ir_head;

    /* TODO：记录每个基本块的父节点，避免遍历所有基本块。 */
    block_t *entry;
    list_for_each_entry (entry, &rv->block_list, list) {
        rv_insn_t *taken = entry->ir_tail->branch_taken,
                  *untaken = entry->ir_tail->branch_untaken;

        if (taken == replaced_blk_entry) {
            entry->ir_tail->branch_taken = NULL;
        }
        if (untaken == replaced_blk_entry) {
            entry->ir_tail->branch_untaken = NULL;
        }

        /* 更新 JALR LUT。 */
        if (!entry->ir_tail->branch_table) {
            continue;
        }

        /**
         * TODO：更新所有以该基本块为目标的 JALR 指令引用。
         */
    }

#if RV32_HAS(T2C)
    /* 检查 T2C 线程是否正在使用被替换基本块。
     * 若正在使用，则标记为延迟释放并跳过立即销毁。
     * T2C 线程完成后会负责释放它。
     */
    if (replaced_blk->is_compiling) {
        replaced_blk->should_free = true;

        /* 清空 jit_cache 以阻止新的执行进入该基本块，但暂不释放 engine 或内存。
         * 此时 T2C 线程拥有 engine 和基本块内存。
         */
#if RV32_HAS(SYSTEM)
        uint64_t key = (uint64_t) replaced_blk->pc_start |
                       ((uint64_t) replaced_blk->satp << 32);
#else
        uint64_t key = (uint64_t) replaced_blk->pc_start;
#endif
        if (replaced_blk->func) {
            jit_cache_update(rv->jit_cache, key, NULL);
        }
        inline_cache_clear_key(rv->inline_cache, key);

        /* 从全局基本块链表移除，避免后续查找或遍历命中。 */
        list_del_init(&replaced_blk->list);

        pthread_mutex_unlock(&rv->cache_lock);
        return next_blk;
    }
#endif

    /* 释放被替换基本块中的 IR。 */
    for (rv_insn_t *ir = replaced_blk->ir_head, *next_ir; ir != NULL;
         ir = next_ir) {
        next_ir = ir->next;

        if (ir->fuse)
            mpool_free(rv->fuse_mp, ir->fuse);

        mpool_free(rv->block_ir_mp, ir);
    }

#if RV32_HAS(T2C)
    /* 销毁 LLVM engine 前先清理 jit_cache 条目，避免留下陈旧函数指针。
     * SYSTEM 模式下 jit_cache key 包含 SATP。
     * 调用方此时已经持有 cache_lock。
     */
#if RV32_HAS(SYSTEM)
    uint64_t key = (uint64_t) replaced_blk->pc_start |
                   ((uint64_t) replaced_blk->satp << 32);
#else
    uint64_t key = (uint64_t) replaced_blk->pc_start;
#endif
    if (replaced_blk->func) {
        jit_cache_update(rv->jit_cache, key, NULL);
    }
    inline_cache_clear_key(rv->inline_cache, key);
    /* 释放基本块前先销毁 LLVM execution engine。
     * block->func 指向的代码内存由该 engine 持有。
     */
    t2c_dispose_engine(replaced_blk->llvm_engine);
#endif

    list_del_init(&replaced_blk->list);
    mpool_free(rv->block_mp, replaced_blk);
#if RV32_HAS(T2C)
    pthread_mutex_unlock(&rv->cache_lock);
#endif
#endif

    assert(next_blk);
    return next_blk;
}

/* 架构测试中禁用 profiler，确保每条客体指令都由 JIT 编译器翻译。
 */
#if RV32_HAS(JIT) && !RV32_HAS(ARCH_TEST)
static bool runtime_profiler(riscv_t *rv, block_t *block)
{
#if RV32_HAS(SYSTEM)
    if (block->satp != rv->csr_satp)
        return false;
#endif
    /* 观察表明，很多真实热点具有执行频率高且包含循环的特征。
     * 因此 profiler 通过若干指标识别热点基本块。
     */
    uint32_t freq = cache_freq(rv->block_cache, block->pc_start);
    /* 基本块链接后仍需先实际执行，才能进行 profile。 */
    if (unlikely(freq >= 2 && block->has_loops))
        return true;
    /* 执行频率超过预设阈值。 */
    if (unlikely(freq >= THRESHOLD))
        return true;
    return false;
}
#endif

#if RV32_HAS(SYSTEM_MMIO)
static bool rv_has_plic_trap(riscv_t *rv)
{
    return ((rv->csr_sstatus & SSTATUS_SIE || !rv->priv_mode) &&
            (rv->csr_sip & rv->csr_sie));
}

static void rv_check_interrupt(riscv_t *rv)
{
    vm_attr_t *attr = PRIV(rv);
    if (peripheral_update_ctr-- == 0) {
        peripheral_update_ctr = 64;

#if defined(__EMSCRIPTEN__)
    escape_seq:
#endif
        u8250_check_ready(PRIV(rv)->uart);
        if (PRIV(rv)->uart->in_ready)
            emu_update_uart_interrupts(rv);

#if RV32_HAS(GOLDFISH_RTC)
        if (PRIV(rv)->rtc->irq_enabled) {
            uint64_t now_nsec = rtc_get_now_nsec(PRIV(rv)->rtc);
            if (rtc_alarm_fire(PRIV(rv)->rtc, now_nsec)) {
                PRIV(rv)->rtc->alarm_status = 1;
                PRIV(rv)->rtc->interrupt_status = 1;
                emu_update_rtc_interrupts(rv);
            }
        }
#endif /* RV32_HAS(GOLDFISH_RTC) */
    }

    /* 从 cycle counter 推导当前 timer，用于中断比较。
     * timer 不再逐指令递增，而是在这里按需计算。
     */
    uint64_t current_timer = rv->csr_cycle + rv->timer_offset;
    if (current_timer > attr->timer)
        rv->csr_sip |= RV_INT_STI;
    else
        rv->csr_sip &= ~RV_INT_STI;

    if (rv_has_plic_trap(rv)) {
        uint32_t intr_applicable = rv->csr_sip & rv->csr_sie;
        uint8_t intr_idx = ilog2(intr_applicable);
        switch (intr_idx) {
        case (SUPERVISOR_SW_INTR & 0xf):
            SET_CAUSE_AND_TVAL_THEN_TRAP(rv, SUPERVISOR_SW_INTR, 0);
            break;
        case (SUPERVISOR_TIMER_INTR & 0xf):
            SET_CAUSE_AND_TVAL_THEN_TRAP(rv, SUPERVISOR_TIMER_INTR, 0);
            break;
        case (SUPERVISOR_EXTERNAL_INTR & 0xf):
            SET_CAUSE_AND_TVAL_THEN_TRAP(rv, SUPERVISOR_EXTERNAL_INTR, 0);
#if defined(__EMSCRIPTEN__)
            /* escape 序列可能超过 1 个字节。 */
            if (input_buf_size)
                goto escape_seq;
#endif
            break;
        default:
            break;
        }
    }
}
#endif

void rv_step(void *arg)
{
    assert(arg);
    riscv_t *rv = arg;

    vm_attr_t *attr = PRIV(rv);
    uint32_t cycles = attr->cycle_per_step;

    /* 根据起始 PC 查找或翻译基本块。 */
    const uint64_t cycles_target = rv->csr_cycle + cycles;

    /* 循环执行，直到达到本次 step 的 cycle 目标。 */
    while (rv->csr_cycle < cycles_target && !rv->halt) {
#if RV32_HAS(SYSTEM_MMIO)
        /* 每个基本块执行后检查一次中断。 */
        rv_check_interrupt(rv);
#endif

        if (prev && prev->pc_start != last_pc) {
            /* 更新上一基本块引用。 */
#if !RV32_HAS(JIT)
            prev = block_lookup_or_find(rv, last_pc);
#else
            prev = cache_get(rv->block_cache, last_pc, false);
#endif
        }
        /* 在 block map 中查找下一基本块；若不存在则翻译新基本块，
         * 然后推进到该基本块。
         */
        block_t *block = block_find_or_translate(rv);
        /* 此时应已有可执行基本块。 */
        if (unlikely(!block)) {
#if RV32_HAS(SYSTEM)
            /* 检查是否有待处理 trap（例如翻译期间页故障）。
             * 若有，则调用 trap handler 并继续执行，而不是直接停机。
             */
            if (rv->is_trapped) {
                trap_handler(rv);
                prev = NULL;
                continue;
            }
#endif
            rv_log_fatal("分配或翻译基本块失败，PC=0x%08x",
                         rv->PC);
            rv->halt = true;
            return;
        }
        assert(block);

#if RV32_HAS(JIT) && RV32_HAS(SYSTEM)
        assert(block->satp == rv->csr_satp && !block->invalidated);
#endif

#if !RV32_HAS(SYSTEM)
        /* 命中退出地址。 */
        if (unlikely(block->ir_head->pc == PRIV(rv)->exit_addr))
            PRIV(rv)->on_exit = true;
#endif

        /* 上一基本块执行完成后即可确定分支是否被采用。
         * 当前基本块的 IR 入口会被挂到上一基本块的 branch_taken 或
         * branch_untaken 指针上。
         */

#if RV32_HAS(BLOCK_CHAINING)
        if (prev
#if RV32_HAS(JIT) && RV32_HAS(SYSTEM)
            && prev->satp == rv->csr_satp && !prev->invalidated
#endif
        ) {
            rv_insn_t *last_ir = prev->ir_tail;
            /* 链接基本块。 */
            if (prev->page_terminated) {
                /* 页边界终止的基本块总是 fallthrough 到下一地址。
                 * 这里使用 branch_taken 表示 fallthrough，类似无条件跳转。
                 */
                if (!last_ir->branch_taken)
                    last_ir->branch_taken = block->ir_head;
            } else if (!insn_is_unconditional_branch(last_ir->opcode)) {
                /* 条件分支：根据 taken/untaken 路径建立链接。 */
                if (is_branch_taken && !last_ir->branch_taken) {
                    last_ir->branch_taken = block->ir_head;
                } else if (!is_branch_taken && !last_ir->branch_untaken) {
                    last_ir->branch_untaken = block->ir_head;
                }
            } else if (insn_is_direct_branch(last_ir->opcode)) {
                /* 无条件直接分支：始终使用 branch_taken。 */
                if (!last_ir->branch_taken) {
                    last_ir->branch_taken = block->ir_head;
                }
            }
        }
#endif
        last_pc = rv->PC;
#if RV32_HAS(JIT)
#if RV32_HAS(T2C)
        /* 通过 tier-2 JIT 编译器执行。 */
        if (ATOMIC_LOAD(&block->hot2, ATOMIC_ACQUIRE)) {
            /* atomic load-acquire 与 t2c_compile() 中的 store-release 配对。
             * 观察到 hot2=true 后，可保证看到更新后的 block->func。
             */
#if defined(__aarch64__)
            /* 执行 T2C 代码前确保 instruction cache 一致性。 */
            __asm__ volatile("isb" ::: "memory");
#endif
            /* 防御性 NULL 检查；seqlock 正常工作时不应发生，但可防护基本块失效
             * 期间的竞态。
             */
            if (unlikely(!block->func)) {
                /* 基本块已失效，回退到解释器路径。 */
                prev = NULL;
                continue;
            }
            ((exec_t2c_func_t) block->func)(rv);
            prev = NULL;
            continue;
        } /* 检查 tier-1 生成代码的调用次数是否超过阈值。 */
        else if (!ATOMIC_LOAD(&block->compiled, ATOMIC_RELAXED) &&
                 ATOMIC_LOAD(&block->n_invoke, ATOMIC_RELAXED) >= THRESHOLD) {
            ATOMIC_STORE(&block->compiled, true, ATOMIC_RELAXED);
            queue_entry_t *entry = malloc(sizeof(queue_entry_t));
            if (unlikely(!entry)) {
                /* malloc 失败，重置 compiled 标志以便后续重试。 */
                ATOMIC_STORE(&block->compiled, false, ATOMIC_RELAXED);
                continue;
            }
            /* 存储 cache key 而不是指针，避免 use-after-free。 */
#if RV32_HAS(SYSTEM)
            entry->key =
                (uint64_t) block->pc_start | ((uint64_t) block->satp << 32);
#else
            entry->key = (uint64_t) block->pc_start;
#endif
            pthread_mutex_lock(&rv->wait_queue_lock);
            list_add(&entry->list, &rv->wait_queue);
            pthread_cond_signal(&rv->wait_queue_cond);
            pthread_mutex_unlock(&rv->wait_queue_lock);
        }
#endif
        /* 通过 tier-1 JIT 编译器执行。 */
        struct jit_state *state = rv->jit_state;
        /*
         * TODO：这里不一定需要已翻译基本块本身，只需要用程序计数器作为 key，
         *       在已编译二进制缓冲区中查找对应条目。
         */
        if (block->hot) {
#if RV32_HAS(T2C)
            ATOMIC_FETCH_ADD(&block->n_invoke, 1, ATOMIC_RELAXED);
#else
            block->n_invoke++;
#endif
#if defined(__aarch64__)
            /* 执行 JIT 代码前确保 instruction cache 一致性。 */
            __asm__ volatile("isb" ::: "memory");
#endif
            ((exec_block_func_t) state->buf)(
                rv, (uintptr_t) (state->buf + block->offset));
            rv->csr_cycle += block->cycle_cost;
#if RV32_HAS(SYSTEM)
            /* 若 JIT 基本块执行期间发生 trap，则在这里处理。 */
            if (rv->is_trapped) {
                trap_handler(rv);
                prev = NULL;
                continue;
            }
#endif
            prev = NULL;
            continue;
        } /* 检查当前执行路径是否为潜在热点。 */
        if (block->translatable
#if !RV32_HAS(ARCH_TEST)
            && runtime_profiler(rv, block)
#endif
        ) {
            jit_translate(rv, block);
#if defined(__aarch64__)
            /* 执行 JIT 代码前确保 instruction cache 一致性。 */
            __asm__ volatile("isb" ::: "memory");
#endif
            ((exec_block_func_t) state->buf)(
                rv, (uintptr_t) (state->buf + block->offset));
            rv->csr_cycle += block->cycle_cost;
#if RV32_HAS(SYSTEM)
            /* 若 JIT 基本块执行期间发生 trap，则在这里处理。 */
            if (rv->is_trapped) {
                trap_handler(rv);
                prev = NULL;
                continue;
            }
#endif
            prev = NULL;
            continue;
        }
        set_reset(&pc_set);
        has_loops = false;
#endif
        /* 通过解释器执行基本块。
         * 这里使用逐指令 cycle 计数以支持基本块链接；已链接基本块会绕过外层循环，
         * 并通过每个指令 handler 中的 cycle++ 累积 cycle。
         */
        const rv_insn_t *ir = block->ir_head;
        uint64_t cycle = rv->csr_cycle;
        if (unlikely(!ir->impl(rv, ir, cycle, rv->PC))) {
            /* 调用异常处理器后不应继续扩展基本块链接。 */
            prev = NULL;
            break;
        }
#if RV32_HAS(JIT)
        if (has_loops && !block->has_loops)
            block->has_loops = true;
#endif
        prev = block;
    }

    /* 增量内存维护：周期性回收未使用页面。
     * 使用 16 位计数器，因此每 65536 次 rv_step() 调用执行一次。
     */
    static uint16_t gc_counter = 0;
    if (unlikely(++gc_counter == 0))
        memory_gc();

#ifdef __EMSCRIPTEN__
    if (rv_has_halted(rv)) {
        emscripten_cancel_main_loop();
        rv_delete(rv); /* 清理并复用内存。 */
        rv_log_info("RISC-V 模拟器已销毁");
        enable_run_button();
    }
#endif
}

void rv_step_debug(void *arg)
{
    assert(arg);
    riscv_t *rv = arg;

#if RV32_HAS(SYSTEM_MMIO)
    rv_check_interrupt(rv);
#endif

#if !RV32_HAS(SYSTEM)
    /* 命中退出地址。 */
    if (unlikely(rv->PC == PRIV(rv)->exit_addr))
        PRIV(rv)->on_exit = true;
#endif

    rv_insn_t ir;

retranslate:
    memset(&ir, 0, sizeof(rv_insn_t));

    /* 取下一条指令。 */
    uint32_t insn = rv->io.mem_ifetch(rv, rv->PC);
#if RV32_HAS(SYSTEM)
    if (!insn && need_retranslate) {
        need_retranslate = false;
        goto retranslate;
    }
#endif

    /* 若取指失败（页故障等），调用 trap handler。
     * SYSTEM 模式下，insn==0 应始终伴随 trap 状态。
     */
    if (!insn) {
#if RV32_HAS(SYSTEM)
        assert(rv->is_trapped &&
               "insn fetch returned 0 without setting trap state");
        trap_handler(rv);
#endif
        return;
    }

    /* 解码指令。 */
    if (!rv_decode(&ir, insn)) {
        rv->compressed = is_compressed(insn);
        SET_CAUSE_AND_TVAL_THEN_TRAP(rv, ILLEGAL_INSN, insn);
        return;
    }

    ir.impl = dispatch_table[ir.opcode];
    ir.pc = rv->PC;
    ir.next = NULL;
    ir.impl(rv, &ir, rv->csr_cycle, rv->PC);
    return;
}

#if RV32_HAS(SYSTEM)
static void __trap_handler(riscv_t *rv)
{
    rv_insn_t *ir = mpool_calloc(rv->block_ir_mp);
    assert(ir);

    /* sret 实现会将其置为 false。 */
    while (rv->is_trapped && !rv_has_halted(rv)) {
        uint32_t insn;
    retry_fetch:
        insn = rv->io.mem_ifetch(rv, rv->PC);

        /* 若取指返回 0 且 need_retranslate 已设置，表示执行期间启用了 MMU。
         * 清除该标志并重试取指；这与 block_translate 和 rv_step 处理中途启用
         * MMU 的模式保持一致。
         */
        if (!insn && need_retranslate) {
            need_retranslate = false;
            goto retry_fetch;
        }

        /* 若取指失败（insn==0），表示以下情况之一：
         * 1. ifetch 期间发生页故障（need_handle_signal 已设置）。
         * 2. PC 无效或其他错误。
         * 两种情况都退出循环，交由主循环处理。
         */
        if (!insn)
            break;

        rv_decode(ir, insn);
        reloc_enable_mmu_jalr_addr = rv->PC;

        ir->impl = dispatch_table[ir->opcode];
        rv->compressed = is_compressed(insn);
        ir->impl(rv, ir, rv->csr_cycle, rv->PC);
    }

    mpool_free(rv->block_ir_mp, ir);
    prev = NULL;
}
#endif /* RV32_HAS(SYSTEM) */

/* M-mode/S-mode 发生 trap 时，m/stval 要么初始化为 0，要么填入与异常相关的
 * 细节，帮助软件处理该 trap。其他情况下实现不会修改 m/stval，但软件仍可显式
 * 写入它。硬件平台会定义哪些异常必须提供有效 mtval 信息，哪些异常可以始终把
 * mtval 置为 0。
 *
 * 当硬件断点被触发，或取指、load、store 期间发生地址非对齐、访问故障、页故障等
 * 异常时，m/stval 会更新为导致故障的虚拟地址。非法指令 trap 中，m/stval 可能
 * 更新为 offending instruction 的前 XLEN 或 ILEN 位。其他 trap 通常把 m/stval
 * 置为 0。不过，未来标准仍可能重新定义不同 trap 类型下 m/stval 的处理方式。
 *
 */
static void _trap_handler(riscv_t *rv)
{
    /* m/stvec（Machine/Supervisor Trap-Vector Base Address Register）
     * m/stvec[MXLEN-1:2]：vector base address。
     * m/stvec[1:0]：vector mode。
     * m/sepc：Machine/Supervisor Exception Program Counter。
     * m/stval：Machine/Supervisor Trap Value Register。
     * m/scause：Machine/Supervisor Cause Register，保存异常码。
     * m/sstatus：Machine/Supervisor Status Register，记录并控制 hart 当前运行状态。
     *
     * m/stval 和 m/scause 在 SET_CAUSE_AND_TVAL_THEN_TRAP 中设置。
     */
    uint32_t base;
    uint32_t mode;
    uint32_t cause;
    /* 当前处于 user 或 supervisor 模式。 */
    if (RV_PRIV_IS_U_OR_S_MODE()) {
        const uint32_t sstatus_sie =
            (rv->csr_sstatus & SSTATUS_SIE) >> SSTATUS_SIE_SHIFT;
        rv->csr_sstatus |= (sstatus_sie << SSTATUS_SPIE_SHIFT);
        rv->csr_sstatus &= ~(SSTATUS_SIE);
        rv->csr_sstatus |= (rv->priv_mode << SSTATUS_SPP_SHIFT);
        rv->priv_mode = RV_PRIV_S_MODE;
        base = rv->csr_stvec & ~0x3;
        mode = rv->csr_stvec & 0x3;
        cause = rv->csr_scause;
        rv->csr_sepc = rv->PC;
#if RV32_HAS(SYSTEM)
        rv->last_csr_sepc = rv->csr_sepc;
        if (!rv->csr_stvec) { /* CSR 尚未配置时。 */
            /* SYSTEM 模式没有 trap vector 时，从 sepc 恢复 PC 并清除 is_trapped，
             * 继续执行。这用于处理早期启动阶段 handler 尚未设置时的杂散中断。
             */
            rv->PC = rv->csr_sepc;
            rv->is_trapped = false;
            return;
        }
#endif
    } else { /* machine 模式。 */
        const uint32_t mstatus_mie =
            (rv->csr_mstatus & MSTATUS_MIE) >> MSTATUS_MIE_SHIFT;
        rv->csr_mstatus |= (mstatus_mie << MSTATUS_MPIE_SHIFT);
        rv->csr_mstatus &= ~(MSTATUS_MIE);
        rv->csr_mstatus |= (rv->priv_mode << MSTATUS_MPP_SHIFT);
        rv->priv_mode = RV_PRIV_M_MODE;
        base = rv->csr_mtvec & ~0x3;
        mode = rv->csr_mtvec & 0x3;
        cause = rv->csr_mcause;
        rv->csr_mepc = rv->PC;
        if (!rv->csr_mtvec) { /* CSR 尚未配置时。 */
#if RV32_HAS(SYSTEM)
            /* SYSTEM 模式没有 trap vector 时，从 mepc 恢复 PC 并清除 is_trapped，
             * 继续执行。这用于处理早期启动阶段 handler 尚未设置时的杂散中断。
             */
            rv->PC = rv->csr_mepc;
            rv->is_trapped = false;
#else
            rv_trap_default_handler(rv);
#endif
            return;
        }
    }
    switch (mode) {
    /* DIRECT：所有 trap 都把 PC 设置为 base。 */
    case 0:
        rv->PC = base;
        break;
    /* VECTORED：异步 trap 把 PC 设置为 base + 4 * code。 */
    case 1:
        /* code 的 MSB 用于标记 trap 是中断还是异常，因此不属于真正的 code。 */
        rv->PC = base + 4 * (cause & MASK(31));
        break;
    }
    IIF(RV32_HAS(SYSTEM))(if (rv->is_trapped) __trap_handler(rv);, )
}

void trap_handler(riscv_t *rv)
{
    assert(rv);
    _trap_handler(rv);
}

void ebreak_handler(riscv_t *rv)
{
    assert(rv);
    SET_CAUSE_AND_TVAL_THEN_TRAP(rv, BREAKPOINT, rv->PC);
}

void ecall_handler(riscv_t *rv)
{
    assert(rv);

#if RV32_HAS(ELF_LOADER)
    rv->PC += 4;
    syscall_handler(rv);
#elif RV32_HAS(SYSTEM)
    if (rv->priv_mode == RV_PRIV_U_MODE) {
        uint32_t reg_a7 = rv_get_reg(rv, rv_reg_a7);
        switch (reg_a7) { /* 捕获 guestOS 中面向 SDL 应用的 syscall。 */
        case 0xBEEF:
        case 0xC0DE:
        case 0xFEED:
        case 0xBABE:
        case 0xD00D:
            syscall_handler(rv);
            rv->PC += 4;
            break;
        default:
#if RV32_HAS(SDL) && RV32_HAS(SYSTEM_MMIO)
            /*
             * guestOS 可能反复打开和关闭 SDL 窗口，用户也可能通过应用内置退出
             * 功能关闭程序。这里需要捕获内置退出，确保 SDL 窗口和 SDL mixer
             * 被正确销毁。
             */
            {
                extern void sdl_video_audio_cleanup();
                if (unlikely(PRIV(rv)->running_sdl && reg_a7 == 93)) {
                    sdl_video_audio_cleanup();
                    PRIV(rv)->running_sdl = false;
                }
            }
#endif
            SET_CAUSE_AND_TVAL_THEN_TRAP(rv, ECALL_U, 0);
            break;
        }
    } else if (rv->priv_mode ==
               RV_PRIV_S_MODE) { /* 转入 SBI syscall handler。 */
        rv->PC += 4;
        syscall_handler(rv);
    }
#else
    SET_CAUSE_AND_TVAL_THEN_TRAP(rv, ECALL_M, 0);
    syscall_handler(rv);
#endif
}

void memset_handler(riscv_t *rv)
{
    memory_t *m = PRIV(rv)->mem;
    uint32_t dest = rv->X[rv_reg_a0];
    uint32_t value = rv->X[rv_reg_a1];
    uint32_t count = rv->X[rv_reg_a2];

    /* 边界检查，避免缓冲区溢出。 */
    if (dest >= m->mem_size || count > m->mem_size - dest) {
        SET_CAUSE_AND_TVAL_THEN_TRAP(rv, STORE_MISALIGNED, dest);
        return;
    }

    memset((char *) m->mem_base + dest, value, count);
    rv->PC = rv->X[rv_reg_ra] & ~1U;
}

void memcpy_handler(riscv_t *rv)
{
    memory_t *m = PRIV(rv)->mem;
    uint32_t dest = rv->X[rv_reg_a0];
    uint32_t src = rv->X[rv_reg_a1];
    uint32_t count = rv->X[rv_reg_a2];

    /* 边界检查，避免缓冲区溢出。 */
    if (dest >= m->mem_size || count > m->mem_size - dest) {
        SET_CAUSE_AND_TVAL_THEN_TRAP(rv, STORE_MISALIGNED, dest);
        return;
    }
    if (src >= m->mem_size || count > m->mem_size - src) {
        SET_CAUSE_AND_TVAL_THEN_TRAP(rv, LOAD_MISALIGNED, src);
        return;
    }

    memcpy((char *) m->mem_base + dest, (char *) m->mem_base + src, count);
    rv->PC = rv->X[rv_reg_ra] & ~1U;
}

void dump_registers(riscv_t *rv, char *out_file_path)
{
    FILE *f = out_file_path[0] == '-' ? stdout : fopen(out_file_path, "w");
    if (!f) {
        rv_log_error("无法打开寄存器输出文件");
        return;
    }

    fprintf(f, "{\n");
    for (unsigned i = 0; i < N_RV_REGS; i++) {
        char *comma = i < N_RV_REGS - 1 ? "," : "";
        fprintf(f, "  \"x%d\": %u%s\n", i, rv->X[i], comma);
    }
    fprintf(f, "}\n");

    if (out_file_path[0] != '-')
        fclose(f);
}
