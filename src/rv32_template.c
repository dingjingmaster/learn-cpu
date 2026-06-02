/* RV32I 基础指令集。
 *
 * 语义遵循 RISC-V 非特权规范 20191213 版本第 2 章。
 */

/* 解释器指令语义实现。
 *
 * 本文件用 RVOP 宏定义每条 RISC-V 指令如何直接修改模拟器状态，是解释器的语义
 * 来源。JIT 和 T2C 需要与这里保持一致，以避免不同执行层出现行为差异。
 *
 * 架构：
 * - RVOP(name, { body })：定义解释器处理函数。
 * - 参数：rv（模拟器状态）、ir（已解码指令）、cycle（周期计数）、
 *   PC（程序计数器）。
 * - 返回值：bool，表示是否继续执行。
 *
 * 示例：
 *   RVOP(addi, { rv->X[ir->rd] = rv->X[ir->rs1] + ir->imm; })
 *
 * 实现说明：
 * - 指令语义变更应先改这里；JIT 专用优化放在 src/rv32_jit.c。
 */

/* 内部伪操作。 */
RVOP(nop, { rv->X[rv_reg_zero] = 0; })

/* LUI 用 U-type 格式构造 32 位常量：U-immediate 放入目标寄存器高 20 位，
 * 低 12 位填 0，结果按指令语义写回 rd。
 */
RVOP(lui, { rv->X[ir->rd] = ir->imm; })

/* AUIPC 用 U-type 格式构造 PC 相对地址：20 位 U-immediate 形成 32 位偏移，
 * 低 12 位填 0，再加上当前指令 PC 并写入 rd。
 */
RVOP(auipc, { rv->X[ir->rd] = ir->imm + PC; })

/* JAL：跳转并链接。
 * 把下一条指令地址写入 rd，并把 J-type 立即数偏移加到 PC。
 */
RVOP(jal, {
    const uint32_t pc = PC;
    /* 执行跳转。 */
    PC += ir->imm;
    /* 写入返回地址。 */
    if (ir->rd)
        rv->X[ir->rd] = pc + 4;
    /* 检查目标地址是否满足指令对齐要求。 */
#if !RV32_HAS(EXT_C)
    RV_EXC_MISALIGN_HANDLER(pc, INSN, false, 0);
#endif
    struct rv_insn *taken = ir->branch_taken;
    if (taken) {
#if RV32_HAS(JIT)
        IIF(RV32_HAS(SYSTEM)(if (!rv->is_trapped && !reloc_enable_mmu), ))
        {
            IIF(RV32_HAS(SYSTEM))(block_t *next =, )
                cache_get(rv->block_cache, PC, true);
            IIF(RV32_HAS(SYSTEM))(
                if (next->satp == rv->csr_satp && !next->invalidated), )
            {
                if (!set_add(&pc_set, PC))
                    has_loops = true;
                if (cache_hot(rv->block_cache, PC))
                    goto end_op;
            }
        }
#endif
#if RV32_HAS(SYSTEM)
        if (!rv->is_trapped)
#endif
        {
            /* last_pc 只能在非 trap 路径上更新。
             * 如果在 trap 路径中更新，rv_step() 中的基本块链接可能出错。
             * 具体来说，中断可能在通过 last_pc 查找上一个基本块之前发生；而
             * __trap_handler() 也复用同一个 RVOP，因此 trap 路径可能错误更新
             * last_pc。
             *
             * 本文件中其他相同语句也遵循这个规则。
             */
            last_pc = PC;

            MUST_TAIL return taken->impl(rv, taken, cycle, PC);
        }
    }
    goto end_op;
})

/* 分支历史表记录间接跳转目标的历史数据。
 * 它可以减少 block_find() 调用；只有间接跳转目标此前未记录时才会产生额外开销。
 * 同时，该表让解释器可以快速处理间接跳转，而无需反复调用 block_find()。
 */
#if !RV32_HAS(JIT)
#define LOOKUP_OR_UPDATE_BRANCH_HISTORY_TABLE()                                \
    /*                                                                         \
     * 直接映射分支历史表查找。                                                \
     *                                                                         \
     * 处理 trap 时不应查找分支历史表，否则可能从 trap_handler 错误返回。       \
     *                                                                         \
     * 此外，在 relocate_enable_mmu 完成前，基本块可能会重新翻译，因此也不应   \
     * 更新分支历史表。                                                        \
     */                                                                        \
    IIF(RV32_HAS(GDBSTUB)(if (!rv->debug_mode), ))                             \
    {                                                                          \
        IIF(RV32_HAS(SYSTEM)(if (!rv->is_trapped && !reloc_enable_mmu), ))     \
        {                                                                      \
            /* 直接映射查找：O(1)，避免 O(n) 线性搜索。 */                    \
            const uint32_t bht_idx = (PC >> 2) & (HISTORY_SIZE - 1);           \
            if (ir->branch_table->PC[bht_idx] == PC &&                         \
                ir->branch_table->target[bht_idx]) {                           \
                MUST_TAIL return ir->branch_table->target[bht_idx]->impl(      \
                    rv, ir->branch_table->target[bht_idx], cycle, PC);         \
            }                                                                  \
            block_t *block = block_find(&rv->block_map, PC);                   \
            if (block) {                                                       \
                /* 在计算出的索引处直接替换。 */                              \
                ir->branch_table->PC[bht_idx] = PC;                            \
                ir->branch_table->target[bht_idx] = block->ir_head;            \
                MUST_TAIL return block->ir_head->impl(rv, block->ir_head,      \
                                                      cycle, PC);              \
            }                                                                  \
        }                                                                      \
    }
#else
#define LOOKUP_OR_UPDATE_BRANCH_HISTORY_TABLE()                              \
    IIF(RV32_HAS(SYSTEM))(if (!rv->is_trapped && !reloc_enable_mmu), )       \
    {                                                                        \
        block_t *block = cache_get(rv->block_cache, PC, true);               \
        if (block) {                                                         \
            /* 直接映射查找：O(1)，避免 O(n) 线性搜索。 */                  \
            const uint32_t bht_idx = (PC >> 2) & (HISTORY_SIZE - 1);         \
            if (ir->branch_table->PC[bht_idx] == PC) {                       \
                IIF(RV32_HAS(SYSTEM))(                                       \
                    if (ir->branch_table->satp[bht_idx] == rv->csr_satp), )  \
                {                                                            \
                    ir->branch_table->times[bht_idx]++;                      \
                    if (cache_hot(rv->block_cache, PC))                      \
                        goto end_op;                                         \
                }                                                            \
            }                                                                \
            /* 在计算出的索引处直接替换。 */                                \
            ir->branch_table->times[bht_idx] = 1;                            \
            ir->branch_table->PC[bht_idx] = PC;                              \
            IIF(RV32_HAS(SYSTEM))(                                           \
                ir->branch_table->satp[bht_idx] = rv->csr_satp, );           \
            if (cache_hot(rv->block_cache, PC))                              \
                goto end_op;                                                 \
            MUST_TAIL return block->ir_head->impl(rv, block->ir_head, cycle, \
                                                  PC);                       \
        }                                                                    \
    }
#endif

/* 间接跳转指令 JALR 使用 I-type 编码。
 * 目标地址由寄存器 rs1 加上符号扩展后的 12 位 I-immediate 得到，然后把结果最低位
 * 清零。跳转后一条指令地址（pc+4）写入 rd。如果不需要返回地址，可以把 x0 作为
 * 目标寄存器。
 */
RVOP(jalr, {
    const uint32_t pc = PC;
    /* 跳转。 */
    PC = (rv->X[ir->rs1] + ir->imm) & ~1U;
    /* 链接返回地址。 */
    if (ir->rd)
        rv->X[ir->rd] = pc + 4;
    /* 检查指令地址是否未对齐。 */
#if !RV32_HAS(EXT_C)
    RV_EXC_MISALIGN_HANDLER(pc, INSN, false, 0);
#endif
    LOOKUP_OR_UPDATE_BRANCH_HISTORY_TABLE();

#if RV32_HAS(SYSTEM)
    /*
     * relocate_enable_mmu 是设置 MMU 时调用的第一个函数。
     * 该函数内部在地址 0x98 会访问无效 PTE，导致取指页错误并陷入 trap_handler，
     * 且不会通过 sret 返回。
     *
     * 执行物理地址 0xc00000b4 处的 jalr 指令后（relocate_enable_mmu 的最后一条
     * 指令），MMU 变为可用。
     *
     * 因此，该 jalr 执行完成后需要手动从 trap_handler 中退出。
     */
    if (!reloc_enable_mmu && reloc_enable_mmu_jalr_addr == 0xc00000b4) {
        reloc_enable_mmu = true;
        need_retranslate = true;
        rv->is_trapped = false;
    }

#endif /* RV32_HAS(SYSTEM) */

    goto end_op;
})

/* clang-format off */
#define BRANCH_COND(type, x, y, cond) \
    (type) x cond (type) y
/* clang-format on */

#define BRANCH_FUNC(type, cond)                                                \
    IIF(RV32_HAS(EXT_C))(, const uint32_t pc = PC;);                           \
    if (BRANCH_COND(type, rv->X[ir->rs1], rv->X[ir->rs2], cond)) {             \
        IIF(RV32_HAS(SYSTEM))(                                                 \
            {                                                                  \
                if (!rv->is_trapped) {                                         \
                    is_branch_taken = false;                                   \
                }                                                              \
            },                                                                 \
            is_branch_taken = false;);                                         \
        struct rv_insn *untaken = ir->branch_untaken;                          \
        if (!untaken)                                                          \
            goto nextop;                                                       \
        IIF(RV32_HAS(JIT))(                                                    \
            {                                                                  \
                block_t *next = cache_get(rv->block_cache, PC + 4, true);      \
                if (next IIF(RV32_HAS(SYSTEM))(&&next->satp == rv->csr_satp && \
                                                   !next->invalidated, )) {    \
                    if (!set_add(&pc_set, PC + 4))                             \
                        has_loops = true;                                      \
                    if (cache_hot(rv->block_cache, PC + 4))                    \
                        goto nextop;                                           \
                }                                                              \
            }, );                                                              \
        PC += 4;                                                               \
        IIF(RV32_HAS(SYSTEM))(                                                 \
            {                                                                  \
                if (!rv->is_trapped) {                                         \
                    last_pc = PC;                                              \
                    MUST_TAIL return untaken->impl(rv, untaken, cycle, PC);    \
                }                                                              \
            }, );                                                              \
        goto end_op;                                                           \
    }                                                                          \
    IIF(RV32_HAS(SYSTEM))(                                                     \
        {                                                                      \
            if (!rv->is_trapped) {                                             \
                is_branch_taken = true;                                        \
            }                                                                  \
        },                                                                     \
        is_branch_taken = true;);                                              \
    PC += ir->imm;                                                             \
    /* 检查指令地址是否未对齐。 */                                             \
    IIF(RV32_HAS(EXT_C))(, RV_EXC_MISALIGN_HANDLER(pc, INSN, false, 0););      \
    struct rv_insn *taken = ir->branch_taken;                                  \
    if (taken) {                                                               \
        IIF(RV32_HAS(JIT))(                                                    \
            {                                                                  \
                block_t *next = cache_get(rv->block_cache, PC, true);          \
                if (next IIF(RV32_HAS(SYSTEM))(&&next->satp == rv->csr_satp && \
                                                   !next->invalidated, )) {    \
                    if (!set_add(&pc_set, PC))                                 \
                        has_loops = true;                                      \
                    if (cache_hot(rv->block_cache, PC))                        \
                        goto end_op;                                           \
                }                                                              \
            }, );                                                              \
        IIF(RV32_HAS(SYSTEM))(                                                 \
            {                                                                  \
                if (!rv->is_trapped) {                                         \
                    last_pc = PC;                                              \
                    MUST_TAIL return taken->impl(rv, taken, cycle, PC);        \
                }                                                              \
            }, );                                                              \
    }                                                                          \
    goto end_op;

/* 在 RV32I 和 RV64I 中，若分支成立，则设置 pc = pc + offset；offset 是 2 的
 * 倍数，长度为 13 位。若分支不成立，则不修改 PC。
 *
 * 分支成立条件由助记符决定：
 * - "beq"：src1 == src2
 * - "bne"：src1 != src2
 * - "blt"：按有符号整数比较，src1 < src2
 * - "bge"：按有符号整数比较，src1 >= src2
 * - "bltu"：按无符号整数比较，src1 < src2
 * - "bgeu"：按无符号整数比较，src1 >= src2
 *
 * 分支成立时，如果目标 PC 不是 4 字节对齐，则产生指令地址未对齐异常。
 */

/* BEQ：相等则分支。 */
RVOP(beq, { BRANCH_FUNC(uint32_t, !=); })

/* BNE：不相等则分支。 */
RVOP(bne, { BRANCH_FUNC(uint32_t, ==); })

/* BLT：有符号小于则分支。 */
RVOP(blt, { BRANCH_FUNC(int32_t, >=); })

/* BGE：有符号大于等于则分支。 */
RVOP(bge, { BRANCH_FUNC(int32_t, <); })

/* BLTU：无符号小于则分支。 */
RVOP(bltu, { BRANCH_FUNC(uint32_t, >=); })

/* BGEU：无符号大于等于则分支。 */
RVOP(bgeu, { BRANCH_FUNC(uint32_t, <); })

/* 加载指令共有 5 种：字节和半字各有两种，字有一种。
 * 字节和半字加载需要分别支持零扩展和符号扩展；字加载会从内存读取完整寄存器宽度的
 * 数据，因此不需要扩展。
 */

/* RAM 快路径内存访问宏。
 *
 * 非 SYSTEM 模式下，绕过 io 回调间接层，直接访问 RAM，消除每次内存操作的函数
 * 指针分派开销。SYSTEM 模式下则继续使用 io 回调处理 MMU/TLB。
 */
#if !RV32_HAS(SYSTEM)
#define MEM_READ_W(rv, addr) ram_read_w(rv, addr)
#define MEM_READ_S(rv, addr) ram_read_s(rv, addr)
#define MEM_READ_B(rv, addr) ram_read_b(rv, addr)
#define MEM_WRITE_W(rv, addr, val) ram_write_w(rv, addr, val)
#define MEM_WRITE_S(rv, addr, val) ram_write_s(rv, addr, val)
#define MEM_WRITE_B(rv, addr, val) ram_write_b(rv, addr, val)
#else
#define MEM_READ_W(rv, addr) (rv)->io.mem_read_w(rv, addr)
#define MEM_READ_S(rv, addr) (rv)->io.mem_read_s(rv, addr)
#define MEM_READ_B(rv, addr) (rv)->io.mem_read_b(rv, addr)
#define MEM_WRITE_W(rv, addr, val) (rv)->io.mem_write_w(rv, addr, val)
#define MEM_WRITE_S(rv, addr, val) (rv)->io.mem_write_s(rv, addr, val)
#define MEM_WRITE_B(rv, addr, val) (rv)->io.mem_write_b(rv, addr, val)
#endif

/* LB：加载字节并符号扩展。 */
RVOP(lb, {
    uint32_t addr = rv->X[ir->rs1] + ir->imm;
    rv->X[ir->rd] = sign_extend_b(MEM_READ_B(rv, addr));
})

/* LH：加载半字并符号扩展。 */
RVOP(lh, {
    const uint32_t addr = rv->X[ir->rs1] + ir->imm;
    RV_EXC_MISALIGN_HANDLER(1, LOAD, false, 1);
    rv->X[ir->rd] = sign_extend_h(MEM_READ_S(rv, addr));
})

/* LW：加载字。 */
RVOP(lw, {
    const uint32_t addr = rv->X[ir->rs1] + ir->imm;
    RV_EXC_MISALIGN_HANDLER(3, LOAD, false, 1);
    rv->X[ir->rd] = MEM_READ_W(rv, addr);
})

/* LBU：加载字节并零扩展。 */
RVOP(lbu, {
    uint32_t addr = rv->X[ir->rs1] + ir->imm;
    rv->X[ir->rd] = MEM_READ_B(rv, addr);
})

/* LHU：加载半字并零扩展。 */
RVOP(lhu, {
    const uint32_t addr = rv->X[ir->rs1] + ir->imm;
    RV_EXC_MISALIGN_HANDLER(1, LOAD, false, 1);
    rv->X[ir->rd] = MEM_READ_S(rv, addr);
})

/* 存储指令共有 3 种：字节、半字和字。
 * 与加载不同，存储没有有符号/无符号变体，因为写入内存时只写指定字节数，不涉及
 * 符号扩展或零扩展。
 */

/* SB：存储字节。 */
RVOP(sb, {
    const uint32_t addr = rv->X[ir->rs1] + ir->imm;
    const uint32_t value = rv->X[ir->rs2];
    MEM_WRITE_B(rv, addr, value);
#if RV32_HAS(ARCH_TEST)
    check_tohost_write(rv, addr, value);
#endif
})

/* SH：存储半字。 */
RVOP(sh, {
    const uint32_t addr = rv->X[ir->rs1] + ir->imm;
    RV_EXC_MISALIGN_HANDLER(1, STORE, false, 1);
    const uint32_t value = rv->X[ir->rs2];
    MEM_WRITE_S(rv, addr, value);
#if RV32_HAS(ARCH_TEST)
    check_tohost_write(rv, addr, value);
#endif
})

/* SW：存储字。 */
RVOP(sw, {
    const uint32_t addr = rv->X[ir->rs1] + ir->imm;
    RV_EXC_MISALIGN_HANDLER(3, STORE, false, 1);
    const uint32_t value = rv->X[ir->rs2];
    MEM_WRITE_W(rv, addr, value);
#if RV32_HAS(ARCH_TEST)
    check_tohost_write(rv, addr, value);
#endif
})

/* ADDI 把符号扩展后的 12 位立即数加到寄存器 rs1。
 * 算术溢出会被忽略，结果取低 XLEN 位。ADDI rd, rs1, 0 常用于实现汇编伪指令
 * MV rd, rs1。
 */
RVOP(addi, { rv->X[ir->rd] = rv->X[ir->rs1] + ir->imm; })

/* SLTI 把 rs1 和符号扩展立即数都当作有符号数比较；若 rs1 小于立即数，则 rd=1，
 * 否则 rd=0。
 */
RVOP(slti, { rv->X[ir->rd] = ((int32_t) (rv->X[ir->rs1]) < ir->imm) ? 1 : 0; })

/* SLTIU 把 rs1 和立即数都当作无符号数比较；若 rs1 小于立即数，则 rd=1，
 * 否则 rd=0。
 */
RVOP(sltiu, { rv->X[ir->rd] = (rv->X[ir->rs1] < (uint32_t) ir->imm) ? 1 : 0; })

/* XORI：立即数异或。 */
RVOP(xori, { rv->X[ir->rd] = rv->X[ir->rs1] ^ ir->imm; })

/* ORI：立即数或。 */
RVOP(ori, { rv->X[ir->rd] = rv->X[ir->rs1] | ir->imm; })

/* ANDI 对寄存器 rs1 和符号扩展后的 12 位立即数执行按位与，并把结果写入 rd。
 */
RVOP(andi, { rv->X[ir->rd] = rv->X[ir->rs1] & ir->imm; })

FORCE_INLINE void shift_func(riscv_t *rv, const rv_insn_t *ir)
{
    switch (ir->opcode) {
    case rv_insn_slli:
        rv->X[ir->rd] = rv->X[ir->rs1] << (ir->imm & 0x1f);
        break;
    case rv_insn_srli:
        rv->X[ir->rd] = rv->X[ir->rs1] >> (ir->imm & 0x1f);
        break;
    case rv_insn_srai:
        rv->X[ir->rd] = ((int32_t) rv->X[ir->rs1]) >> (ir->imm & 0x1f);
        break;
    default:
        __UNREACHABLE;
        break;
    }
};

/* SLLI 对 rs1 的值执行逻辑左移，移位量来自立即数低 5 位。
 */
RVOP(slli, { shift_func(rv, ir); })

/* SRLI 对 rs1 的值执行逻辑右移，移位量来自立即数低 5 位。
 */
RVOP(srli, { shift_func(rv, ir); })

/* SRAI 对 rs1 的值执行算术右移，移位量来自立即数低 5 位。
 */
RVOP(srai, { shift_func(rv, ir); })

/* ADD */
RVOP(add, { rv->X[ir->rd] = rv->X[ir->rs1] + rv->X[ir->rs2]; })

/* SUB：减法。 */
RVOP(sub, { rv->X[ir->rd] = rv->X[ir->rs1] - rv->X[ir->rs2]; })

/* SLL：逻辑左移。 */
RVOP(sll, { rv->X[ir->rd] = rv->X[ir->rs1] << (rv->X[ir->rs2] & 0x1f); })

/* SLT：有符号小于则置位。 */
RVOP(slt, {
    rv->X[ir->rd] =
        ((int32_t) (rv->X[ir->rs1]) < (int32_t) (rv->X[ir->rs2])) ? 1 : 0;
})

/* SLTU：无符号小于则置位。 */
RVOP(sltu, { rv->X[ir->rd] = (rv->X[ir->rs1] < rv->X[ir->rs2]) ? 1 : 0; })

/* XOR：按位异或。 */
RVOP(xor, {
  rv->X[ir->rd] = rv->X[ir->rs1] ^ rv->X[ir->rs2];
})

/* SRL：逻辑右移。 */
RVOP(srl, { rv->X[ir->rd] = rv->X[ir->rs1] >> (rv->X[ir->rs2] & 0x1f); })

/* SRA：算术右移。 */
RVOP(sra,
     { rv->X[ir->rd] = ((int32_t) rv->X[ir->rs1]) >> (rv->X[ir->rs2] & 0x1f); })

/* OR */
RVOP(or, { rv->X[ir->rd] = rv->X[ir->rs1] | rv->X[ir->rs2]; })

/* AND */
/* clang-format off */
RVOP(
     and,
     { rv->X[ir->rd] = rv->X[ir->rs1] & rv->X[ir->rs2]; })
/* clang-format on */

/*
 * FENCE：约束其他 RISC-V hart、外部设备或协处理器观察到的设备 I/O 和内存访问顺序。
 */
RVOP(fence, {
    PC += 4;
    /* FIXME：补充真实实现。 */
    goto end_op;
})

/* ECALL：环境调用。 */
RVOP(ecall, {
    rv->compressed = false;
    rv->csr_cycle = cycle;
    rv->PC = PC;
    rv->io.on_ecall(rv);
    return true;
})

/* EBREAK：环境断点。 */
RVOP(ebreak, {
    rv->compressed = false;
    rv->csr_cycle = cycle;
    rv->PC = PC;
    rv->io.on_ebreak(rv);
    return true;
})

/* WFI：等待中断。 */
RVOP(wfi, {
    PC += 4;
    /* FIXME：补充实现。 */
    goto end_op;
})

/* URET：从 U-mode trap 返回。 */
RVOP(uret, {
    /* FIXME：补充实现。 */
    return false;
})

/* SRET：从 S-mode trap 返回。 */
#if RV32_HAS(SYSTEM)
RVOP(sret, {
    rv->is_trapped = false;
    rv->priv_mode = (rv->csr_sstatus & SSTATUS_SPP) >> SSTATUS_SPP_SHIFT;
    rv->csr_sstatus &= ~(SSTATUS_SPP);

    const uint32_t sstatus_spie =
        (rv->csr_sstatus & SSTATUS_SPIE) >> SSTATUS_SPIE_SHIFT;
    rv->csr_sstatus |= (sstatus_spie << SSTATUS_SIE_SHIFT);
    rv->csr_sstatus |= SSTATUS_SPIE;

    rv->PC = rv->csr_sepc;

    return true;
})
#endif

/* HRET：从 H-mode trap 返回。 */
RVOP(hret, {
    /* FIXME：补充实现。 */
    return false;
})

/* MRET：从 M-mode trap 返回。 */
RVOP(mret, {
    rv->priv_mode = (rv->csr_mstatus & MSTATUS_MPP) >> MSTATUS_MPP_SHIFT;
    rv->csr_mstatus &= ~(MSTATUS_MPP);

    const uint32_t mstatus_mpie =
        (rv->csr_mstatus & MSTATUS_MPIE) >> MSTATUS_MPIE_SHIFT;
    rv->csr_mstatus |= (mstatus_mpie << MSTATUS_MIE_SHIFT);
    rv->csr_mstatus |= MSTATUS_MPIE;

    rv->PC = rv->csr_mepc;
    return true;
})

/* SFENCE.VMA：同步内存中内存管理数据结构的更新与当前执行流。
 * 该指令会失效 TLB 条目：
 * - rs1 = 0：所有 TLB 条目（全局刷新）
 * - rs1 != 0：仅刷新 rs1 中虚拟地址对应的条目
 * rs2 字段指定 ASID（当前未实现，按全局处理）。
 *
 * 在 JIT 模式下，还需要失效可能包含过期 VA 到 PA 映射的已编译基本块。
 * 当 PTE 被修改但 SATP 未改变时（例如 munmap 后 mmap 到不同 PA，或 mprotect
 * 修改权限），这一步是必要的。
 */
RVOP(sfencevma, {
    PC += 4;
#if RV32_HAS(SYSTEM)
    if (ir->rs1 == 0) {
        /* 全局刷新：失效所有 TLB 条目。 */
        mmu_tlb_flush_all(rv);
#if RV32_HAS(JIT)
#if RV32_HAS(T2C)
        /* 失效期间持有 cache_lock，避免与 T2C 编译线程竞争。
         * 这能保证 T2C 线程以原子方式观察到 invalidated 标志和 hot2 重置。
         */
        pthread_mutex_lock(&rv->cache_lock);
#endif
        /* 失效当前 SATP 下的 JIT 基本块。 */
        cache_invalidate_satp(rv->block_cache, rv->csr_satp);
#if RV32_HAS(T2C)
        jit_cache_clear(rv->jit_cache);
        inline_cache_clear(rv->inline_cache);
        pthread_mutex_unlock(&rv->cache_lock);
#endif
#endif
    } else {
        /* 选择性刷新：失效指定 VA 对应的 TLB 条目。 */
        uint32_t va = rv->X[ir->rs1];
        mmu_tlb_flush(rv, va);
#if RV32_HAS(JIT)
#if RV32_HAS(T2C)
        /* 失效期间持有 cache_lock，避免与 T2C 编译线程竞争。
         */
        pthread_mutex_lock(&rv->cache_lock);
#endif
        /* 失效目标 VA 页内的 JIT 基本块。 */
        cache_invalidate_va(rv->block_cache, va, rv->csr_satp);
#if RV32_HAS(T2C)
        /* 只选择性清理匹配该 VA 页的 jit_cache 条目。 */
        jit_cache_clear_page(rv->jit_cache, va, rv->csr_satp);
        inline_cache_clear_page(rv->inline_cache, va, rv->csr_satp);
        pthread_mutex_unlock(&rv->cache_lock);
#endif
#endif
    }
#endif
    goto end_op;
})

#if RV32_HAS(Zifencei) /* RV32 Zifencei 标准扩展。 */
/* FENCE.I：用于自修改代码同步的指令栅栏。
 * 保证写入指令内存的 store 对后续取指可见。由于指令流可能已改变，必须失效所有
 * 缓存/JIT 后的代码。
 *
 * SFENCE.VMA 处理虚拟内存变化，而 FENCE.I 处理指令缓存一致性。客体代码自修改或
 * 加载新代码（例如动态链接器、客体内部运行的 JIT 编译器）时需要它。
 */
RVOP(fencei, {
    PC += 4;
#if RV32_HAS(JIT) && RV32_HAS(SYSTEM)
#if RV32_HAS(T2C)
    /* 失效期间持有 cache_lock，避免与 T2C 编译线程竞争。
     * 锁协议与 SFENCE.VMA 相同。
     */
    pthread_mutex_lock(&rv->cache_lock);
#endif
    /* 失效当前地址空间中的全部 JIT 基本块。
     * FENCE.I 是全局指令缓存屏障；由于不知道哪些地址被修改，必须清除所有缓存代码。
     * 这里使用与全局 SFENCE.VMA（rs1=0）相同的失效方式。
     */
    cache_invalidate_satp(rv->block_cache, rv->csr_satp);
#if RV32_HAS(T2C)
    jit_cache_clear(rv->jit_cache);
    inline_cache_clear(rv->inline_cache);
    pthread_mutex_unlock(&rv->cache_lock);
#endif
#endif
    /* 注意：非系统 JIT 模式中，自修改代码较少见，基本块会自然淘汰。
     * 该场景未实现全缓存失效，因为这需要额外基础设施。
     */
    rv->csr_cycle = cycle;
    rv->PC = PC;
    return true;
})
#endif

#if RV32_HAS(Zicsr) /* RV32 Zicsr 标准扩展。 */
/* CSRRW：原子读写 CSR。 */
RVOP(csrrw, {
    uint32_t tmp = csr_csrrw(rv, ir->imm, rv->X[ir->rs1], cycle);
    rv->X[ir->rd] = ir->rd ? tmp : rv->X[ir->rd];
})

/* CSRRS：原子读取 CSR 并置位。 */
/* 整数寄存器 rs1 的初始值被视为位掩码，用于指定 CSR 中要置位的比特位置。
 * 如果 rs1 中某位为 1，且对应 CSR 位可写，则该 CSR 位会被置位。CSR 中其他位保持
 * 不变，不过某些 CSR 在写入时可能有副作用。
 *
 * 见 RISC-V 非特权规范第 56 页。
 */
RVOP(csrrs, {
    uint32_t tmp = csr_csrrs(
        rv, ir->imm, (ir->rs1 == rv_reg_zero) ? 0U : rv->X[ir->rs1], cycle);
    rv->X[ir->rd] = ir->rd ? tmp : rv->X[ir->rd];
})

/* CSRRC：原子读取 CSR 并清位。 */
RVOP(csrrc, {
    uint32_t tmp = csr_csrrc(
        rv, ir->imm, (ir->rs1 == rv_reg_zero) ? 0U : rv->X[ir->rs1], cycle);
    rv->X[ir->rd] = ir->rd ? tmp : rv->X[ir->rd];
})

/* CSRRWI */
RVOP(csrrwi, {
    uint32_t tmp = csr_csrrw(rv, ir->imm, ir->rs1, cycle);
    rv->X[ir->rd] = ir->rd ? tmp : rv->X[ir->rd];
})

/* CSRRSI */
RVOP(csrrsi, {
    uint32_t tmp = csr_csrrs(rv, ir->imm, ir->rs1, cycle);
    rv->X[ir->rd] = ir->rd ? tmp : rv->X[ir->rd];
})

/* CSRRCI */
RVOP(csrrci, {
    uint32_t tmp = csr_csrrc(rv, ir->imm, ir->rs1, cycle);
    rv->X[ir->rd] = ir->rd ? tmp : rv->X[ir->rd];
})
#endif

/* RV32M 标准扩展。 */

#if RV32_HAS(EXT_M)
/* MUL：乘法，返回低 32 位。 */
RVOP(mul, {
    const int64_t multiplicand = (int32_t) rv->X[ir->rs1];
    const int64_t multiplier = (int32_t) rv->X[ir->rs2];
    rv->X[ir->rd] =
        ((uint64_t) (multiplicand * multiplier)) & ((1ULL << 32) - 1);
})

/* MULH：有符号 x 有符号乘法高位。 */
/* 需要先把 rs1 和 rs2 转为 i32，这样后续转为 i64 时会对寄存器值做符号扩展。
 */
RVOP(mulh, {
    const int64_t multiplicand = (int32_t) rv->X[ir->rs1];
    const int64_t multiplier = (int32_t) rv->X[ir->rs2];
    rv->X[ir->rd] = ((uint64_t) (multiplicand * multiplier)) >> 32;
})

/* MULHSU：有符号 x 无符号乘法高位。 */
/* 必须先把 rs1 转为 i32，确保后续转为 i64 时对寄存器值符号扩展。
 * rs2 则不应进行符号扩展。
 */
RVOP(mulhsu, {
    const int64_t multiplicand = (int32_t) rv->X[ir->rs1];
    const uint64_t umultiplier = rv->X[ir->rs2];
    rv->X[ir->rd] = ((uint64_t) (multiplicand * umultiplier)) >> 32;
})

/* MULHU：无符号 x 无符号乘法高位。 */
RVOP(mulhu, {
    rv->X[ir->rd] =
        ((uint64_t) rv->X[ir->rs1] * (uint64_t) rv->X[ir->rs2]) >> 32;
})

/* DIV：有符号除法。 */
/* +------------------------+-----------+----------+-----------+
 * |        条件            |   被除数  |   除数   |   DIV[W]  |
 * +------------------------+-----------+----------+-----------+
 * | 除零                   |  x        |  0       |  −1       |
 * | 溢出（仅有符号）       |  −2^{L−1} |  −1      |  −2^{L−1} |
 * +------------------------+-----------+----------+-----------+
 */
RVOP(div, {
    const int32_t dividend = (int32_t) rv->X[ir->rs1];
    const int32_t divisor = (int32_t) rv->X[ir->rs2];
    rv->X[ir->rd] = !divisor ? ~0U
                    : (divisor == -1 && rv->X[ir->rs1] == 0x80000000U)
                        ? rv->X[ir->rs1] /* 溢出。 */
                        : (unsigned int) (dividend / divisor);
})

/* DIVU：无符号除法。 */
/* +------------------------+-----------+----------+----------+
 * |        条件            |   被除数  |   除数   |  DIVU[W] |
 * +------------------------+-----------+----------+----------+
 * | 除零                   |  x        |  0       |  2^L − 1 |
 * +------------------------+-----------+----------+----------+
 */
RVOP(divu, {
    const uint32_t udividend = rv->X[ir->rs1];
    const uint32_t udivisor = rv->X[ir->rs2];
    rv->X[ir->rd] = !udivisor ? ~0U : udividend / udivisor;
})

/* clang-format off */
/* REM：有符号取余。 */
/* +------------------------+-----------+----------+---------+
 * |        条件            |   被除数  |   除数   |  REM[W] |
 * +------------------------+-----------+----------+---------+
 * | 除零                   |  x        |  0       |  x      |
 * | 溢出（仅有符号）       |  −2^{L−1} |  −1      |  0      |
 * +------------------------+-----------+----------+---------+
 */
RVOP(rem, {
    const int32_t dividend = rv->X[ir->rs1];
    const int32_t divisor = rv->X[ir->rs2];
    rv->X[ir->rd] = !divisor ? dividend
                    : (divisor == -1 && rv->X[ir->rs1] == 0x80000000U)
                        ? 0  : (dividend
                        % divisor);
})

/* REMU：无符号取余。 */
/* +------------------------+-----------+----------+----------+
 * |        条件            |   被除数  |   除数   |  REMU[W] |
 * +------------------------+-----------+----------+----------+
 * | 除零                   |  x        |  0       |  x       |
 * +------------------------+-----------+----------+----------+
 */
RVOP(remu, {
    const uint32_t udividend = rv->X[ir->rs1];
    const uint32_t udivisor = rv->X[ir->rs2];
    rv->X[ir->rd] = !udivisor ? udividend : udividend
    % udivisor;
})
/* clang-format on */
#endif

/* RV32A 标准扩展。 */

#if RV32_HAS(EXT_A)
/* AMO（Atomic Memory Operation）指令执行读-改-写操作，用于同步多个处理器；
 * 它们采用 R-type 指令格式编码。
 *
 * 这些 AMO 指令保证从 rs1 指向的内存地址加载数据值时的原子性。加载出的原值会写入
 * rd，同时与 rs2 中的原值执行某个二元运算；最终运算结果再写回 rs1 指向的内存地址，
 * 从而保证整个过程原子。
 *
 * RV64 中 AMO 支持 64 位字操作；其他系统中还支持 32 位字操作。在 RV64 中执行
 * 32 位 AMO 时，写入 rd 的值总是符号扩展后的结果。
 *
 * 当前 AMO 没有真正做原子实现，因为模拟的 RISC-V 核只在单线程中运行，也不会发生
 * 乱序执行。另外，rl/aq 位目前未处理。
 */

/* LR.W：保留加载。 */
RVOP(lrw, {
    const uint32_t addr = rv->X[ir->rs1];
    RV_EXC_MISALIGN_HANDLER(3, LOAD, false, 1);
    if (ir->rd)
        rv->X[ir->rd] = MEM_READ_W(rv, addr);
    /* 跳过 reservation set 注册。
     * FIXME：尚未实现。
     */
})

/* SC.W：条件存储。 */
RVOP(scw, {
    /* 暂时假设 reservation set 有效。
     * FIXME：尚未实现。
     */
    const uint32_t addr = rv->X[ir->rs1];
    RV_EXC_MISALIGN_HANDLER(3, STORE, false, 1);
    const uint32_t value = rv->X[ir->rs2];
    MEM_WRITE_W(rv, addr, value);
    rv->X[ir->rd] = 0;
#if RV32_HAS(ARCH_TEST)
    check_tohost_write(rv, addr, value);
#endif
})

/* AMOSWAP.W：原子交换。 */
RVOP(amoswapw, {
    const uint32_t addr = rv->X[ir->rs1];
    RV_EXC_MISALIGN_HANDLER(3, LOAD, false, 1);
    const uint32_t value1 = MEM_READ_W(rv, addr);
    const uint32_t value2 = rv->X[ir->rs2];
    if (ir->rd)
        rv->X[ir->rd] = value1;
    MEM_WRITE_W(rv, addr, value2);
#if RV32_HAS(ARCH_TEST)
    check_tohost_write(rv, addr, value2);
#endif
})

/* AMOADD.W：原子加。 */
RVOP(amoaddw, {
    const uint32_t addr = rv->X[ir->rs1];
    RV_EXC_MISALIGN_HANDLER(3, LOAD, false, 1);
    const uint32_t value1 = MEM_READ_W(rv, addr);
    const uint32_t value2 = rv->X[ir->rs2];
    if (ir->rd)
        rv->X[ir->rd] = value1;
    const uint32_t res = value1 + value2;
    MEM_WRITE_W(rv, addr, res);
#if RV32_HAS(ARCH_TEST)
    check_tohost_write(rv, addr, res);
#endif
})

/* AMOXOR.W：原子异或。 */
RVOP(amoxorw, {
    const uint32_t addr = rv->X[ir->rs1];
    RV_EXC_MISALIGN_HANDLER(3, LOAD, false, 1);
    const uint32_t value1 = MEM_READ_W(rv, addr);
    const uint32_t value2 = rv->X[ir->rs2];
    if (ir->rd)
        rv->X[ir->rd] = value1;
    const uint32_t res = value1 ^ value2;
    MEM_WRITE_W(rv, addr, res);
#if RV32_HAS(ARCH_TEST)
    check_tohost_write(rv, addr, res);
#endif
})

/* AMOAND.W：原子与。 */
RVOP(amoandw, {
    const uint32_t addr = rv->X[ir->rs1];
    RV_EXC_MISALIGN_HANDLER(3, LOAD, false, 1);
    const uint32_t value1 = MEM_READ_W(rv, addr);
    const uint32_t value2 = rv->X[ir->rs2];
    if (ir->rd)
        rv->X[ir->rd] = value1;
    const uint32_t res = value1 & value2;
    MEM_WRITE_W(rv, addr, res);
#if RV32_HAS(ARCH_TEST)
    check_tohost_write(rv, addr, res);
#endif
})

/* AMOOR.W：原子或。 */
RVOP(amoorw, {
    const uint32_t addr = rv->X[ir->rs1];
    RV_EXC_MISALIGN_HANDLER(3, LOAD, false, 1);
    const uint32_t value1 = MEM_READ_W(rv, addr);
    const uint32_t value2 = rv->X[ir->rs2];
    if (ir->rd)
        rv->X[ir->rd] = value1;
    const uint32_t res = value1 | value2;
    MEM_WRITE_W(rv, addr, res);
#if RV32_HAS(ARCH_TEST)
    check_tohost_write(rv, addr, res);
#endif
})

/* AMOMIN.W：原子有符号最小值。 */
RVOP(amominw, {
    const uint32_t addr = rv->X[ir->rs1];
    RV_EXC_MISALIGN_HANDLER(3, LOAD, false, 1);
    const uint32_t value1 = MEM_READ_W(rv, addr);
    const uint32_t value2 = rv->X[ir->rs2];
    if (ir->rd)
        rv->X[ir->rd] = value1;
    const int32_t a = value1;
    const int32_t b = value2;
    const uint32_t res = a < b ? value1 : value2;
    MEM_WRITE_W(rv, addr, res);
#if RV32_HAS(ARCH_TEST)
    check_tohost_write(rv, addr, res);
#endif
})

/* AMOMAX.W：原子有符号最大值。 */
RVOP(amomaxw, {
    const uint32_t addr = rv->X[ir->rs1];
    RV_EXC_MISALIGN_HANDLER(3, LOAD, false, 1);
    const uint32_t value1 = MEM_READ_W(rv, addr);
    const uint32_t value2 = rv->X[ir->rs2];
    if (ir->rd)
        rv->X[ir->rd] = value1;
    const int32_t a = value1;
    const int32_t b = value2;
    const uint32_t res = a > b ? value1 : value2;
    MEM_WRITE_W(rv, addr, res);
#if RV32_HAS(ARCH_TEST)
    check_tohost_write(rv, addr, res);
#endif
})

/* AMOMINU.W */
RVOP(amominuw, {
    const uint32_t addr = rv->X[ir->rs1];
    RV_EXC_MISALIGN_HANDLER(3, LOAD, false, 1);
    const uint32_t value1 = MEM_READ_W(rv, addr);
    const uint32_t value2 = rv->X[ir->rs2];
    if (ir->rd)
        rv->X[ir->rd] = value1;
    const uint32_t ures = value1 < value2 ? value1 : value2;
    MEM_WRITE_W(rv, addr, ures);
#if RV32_HAS(ARCH_TEST)
    check_tohost_write(rv, addr, ures);
#endif
})

/* AMOMAXU.W */
RVOP(amomaxuw, {
    const uint32_t addr = rv->X[ir->rs1];
    RV_EXC_MISALIGN_HANDLER(3, LOAD, false, 1);
    const uint32_t value1 = MEM_READ_W(rv, addr);
    const uint32_t value2 = rv->X[ir->rs2];
    if (ir->rd)
        rv->X[ir->rd] = value1;
    const uint32_t ures = value1 > value2 ? value1 : value2;
    MEM_WRITE_W(rv, addr, ures);
#if RV32_HAS(ARCH_TEST)
    check_tohost_write(rv, addr, ures);
#endif
})
#endif /* RV32_HAS(EXT_A) */

/* RV32F 标准扩展。 */

#if RV32_HAS(EXT_F)
/* FLW */
RVOP(flw, {
    /* 拷贝到浮点寄存器。 */
    const uint32_t addr = rv->X[ir->rs1] + ir->imm;
    RV_EXC_MISALIGN_HANDLER(3, LOAD, false, 1);
    rv->F[ir->rd].v = MEM_READ_W(rv, addr);
})

/* FSW */
RVOP(fsw, {
    /* 从浮点寄存器拷贝。 */
    const uint32_t addr = rv->X[ir->rs1] + ir->imm;
    RV_EXC_MISALIGN_HANDLER(3, STORE, false, 1);
    const uint32_t value = rv->F[ir->rs2].v;
    MEM_WRITE_W(rv, addr, value);
#if RV32_HAS(ARCH_TEST)
    check_tohost_write(rv, addr, value);
#endif
})

/* FMADD.S */
RVOP(fmadds, {
    set_rounding_mode(rv, ir->rm);
    rv->F[ir->rd] = f32_mulAdd(rv->F[ir->rs1], rv->F[ir->rs2], rv->F[ir->rs3]);
    set_fflag(rv);
})

/* FMSUB.S */
RVOP(fmsubs, {
    set_rounding_mode(rv, ir->rm);
    riscv_float_t tmp = rv->F[ir->rs3];
    tmp.v ^= FMASK_SIGN;
    rv->F[ir->rd] = f32_mulAdd(rv->F[ir->rs1], rv->F[ir->rs2], tmp);
    set_fflag(rv);
})

/* FNMSUB.S */
RVOP(fnmsubs, {
    set_rounding_mode(rv, ir->rm);
    riscv_float_t tmp = rv->F[ir->rs1];
    tmp.v ^= FMASK_SIGN;
    rv->F[ir->rd] = f32_mulAdd(tmp, rv->F[ir->rs2], rv->F[ir->rs3]);
    set_fflag(rv);
})

/* FNMADD.S */
RVOP(fnmadds, {
    set_rounding_mode(rv, ir->rm);
    riscv_float_t tmp1 = rv->F[ir->rs1];
    riscv_float_t tmp2 = rv->F[ir->rs3];
    tmp1.v ^= FMASK_SIGN;
    tmp2.v ^= FMASK_SIGN;
    rv->F[ir->rd] = f32_mulAdd(tmp1, rv->F[ir->rs2], tmp2);
    set_fflag(rv);
})

/* FADD.S */
RVOP(fadds, {
    set_rounding_mode(rv, ir->rm);
    rv->F[ir->rd] = f32_add(rv->F[ir->rs1], rv->F[ir->rs2]);
    set_fflag(rv);
})

/* FSUB.S */
RVOP(fsubs, {
    set_rounding_mode(rv, ir->rm);
    rv->F[ir->rd] = f32_sub(rv->F[ir->rs1], rv->F[ir->rs2]);
    set_fflag(rv);
})

/* FMUL.S */
RVOP(fmuls, {
    set_rounding_mode(rv, ir->rm);
    rv->F[ir->rd] = f32_mul(rv->F[ir->rs1], rv->F[ir->rs2]);
    set_fflag(rv);
})

/* FDIV.S */
RVOP(fdivs, {
    set_rounding_mode(rv, ir->rm);
    rv->F[ir->rd] = f32_div(rv->F[ir->rs1], rv->F[ir->rs2]);
    set_fflag(rv);
})

/* FSQRT.S */
RVOP(fsqrts, {
    set_rounding_mode(rv, ir->rm);
    rv->F[ir->rd] = f32_sqrt(rv->F[ir->rs1]);
    set_fflag(rv);
})

/* FSGNJ.S */
RVOP(fsgnjs, {
    rv->F[ir->rd].v =
        (rv->F[ir->rs1].v & ~FMASK_SIGN) | (rv->F[ir->rs2].v & FMASK_SIGN);
})

/* FSGNJN.S */
RVOP(fsgnjns, {
    rv->F[ir->rd].v =
        (rv->F[ir->rs1].v & ~FMASK_SIGN) | (~rv->F[ir->rs2].v & FMASK_SIGN);
})

/* FSGNJX.S */
RVOP(fsgnjxs,
     { rv->F[ir->rd].v = rv->F[ir->rs1].v ^ (rv->F[ir->rs2].v & FMASK_SIGN); })

/* FMIN.S
 * 在 IEEE754-201x 中，fmin(x, y) 返回：
 * - 若两者都不是 NaN，则返回 min(x, y)
 * - 若一个是 NaN、另一个是数值，则返回该数值
 * - 若两者都是 NaN，则返回 NaN
 * 若输入为 signaling NaN，则置 invalid operation 异常标志。
 */
RVOP(fmins, {
    if (f32_isSignalingNaN(rv->F[ir->rs1]) ||
        f32_isSignalingNaN(rv->F[ir->rs2]))
        rv->csr_fcsr |= FFLAG_INVALID_OP;
    bool less = f32_lt_quiet(rv->F[ir->rs1], rv->F[ir->rs2]) ||
                (f32_eq(rv->F[ir->rs1], rv->F[ir->rs2]) &&
                 (rv->F[ir->rs1].v & FMASK_SIGN));
    if (is_nan(rv->F[ir->rs1].v) && is_nan(rv->F[ir->rs2].v))
        rv->F[ir->rd].v = RV_NAN;
    else
        rv->F[ir->rd] = (less || is_nan(rv->F[ir->rs2].v) ? rv->F[ir->rs1]
                                                          : rv->F[ir->rs2]);
})

/* FMAX.S */
RVOP(fmaxs, {
    if (f32_isSignalingNaN(rv->F[ir->rs1]) ||
        f32_isSignalingNaN(rv->F[ir->rs2]))
        rv->csr_fcsr |= FFLAG_INVALID_OP;
    bool greater = f32_lt_quiet(rv->F[ir->rs2], rv->F[ir->rs1]) ||
                   (f32_eq(rv->F[ir->rs1], rv->F[ir->rs2]) &&
                    (rv->F[ir->rs2].v & FMASK_SIGN));
    if (is_nan(rv->F[ir->rs1].v) && is_nan(rv->F[ir->rs2].v))
        rv->F[ir->rd].v = RV_NAN;
    else
        rv->F[ir->rd] = (greater || is_nan(rv->F[ir->rs2].v) ? rv->F[ir->rs1]
                                                             : rv->F[ir->rs2]);
})

/* FCVT.W.S 和 FCVT.WU.S 把浮点数转换为整数，舍入模式由 rm 字段指定。
 */

/* FCVT.W.S */
RVOP(fcvtws, {
    set_rounding_mode(rv, ir->rm);
    uint32_t ret = f32_to_i32(rv->F[ir->rs1], softfloat_roundingMode, true);
    if (ir->rd)
        rv->X[ir->rd] = ret;
    set_fflag(rv);
})

/* FCVT.WU.S */
RVOP(fcvtwus, {
    set_rounding_mode(rv, ir->rm);
    uint32_t ret = f32_to_ui32(rv->F[ir->rs1], softfloat_roundingMode, true);
    if (ir->rd)
        rv->X[ir->rd] = ret;
    set_fflag(rv);
})

/* FMV.X.W */
RVOP(fmvxw, {
    if (ir->rd)
        rv->X[ir->rd] = rv->F[ir->rs1].v;
})

/* FEQ.S 执行静默比较：仅当任一输入为 signaling NaN 时才设置 invalid operation
 * 异常标志。
 */
RVOP(feqs, {
    uint32_t ret = f32_eq(rv->F[ir->rs1], rv->F[ir->rs2]);
    if (ir->rd)
        rv->X[ir->rd] = ret;
    set_fflag(rv);
})

/* FLT.S 和 FLE.S 执行 IEEE 754-2008 所称的 signaling comparisons：
 * 只要任一输入为 NaN，就会设置 invalid operation 异常标志。
 */
RVOP(flts, {
    uint32_t ret = f32_lt(rv->F[ir->rs1], rv->F[ir->rs2]);
    if (ir->rd)
        rv->X[ir->rd] = ret;
    set_fflag(rv);
})

RVOP(fles, {
    uint32_t ret = f32_le(rv->F[ir->rs1], rv->F[ir->rs2]);
    if (ir->rd)
        rv->X[ir->rd] = ret;
    set_fflag(rv);
})

/* FCLASS.S */
RVOP(fclasss, {
    if (ir->rd)
        rv->X[ir->rd] = calc_fclass(rv->F[ir->rs1].v);
})

/* FCVT.S.W */
RVOP(fcvtsw, {
    set_rounding_mode(rv, ir->rm);
    rv->F[ir->rd] = i32_to_f32(rv->X[ir->rs1]);
    set_fflag(rv);
})

/* FCVT.S.WU */
RVOP(fcvtswu, {
    set_rounding_mode(rv, ir->rm);
    rv->F[ir->rd] = ui32_to_f32(rv->X[ir->rs1]);
    set_fflag(rv);
})

/* FMV.W.X */
RVOP(fmvwx, { rv->F[ir->rd].v = rv->X[ir->rs1]; })
#endif

/* RV32C 标准扩展。 */

#if RV32_HAS(EXT_C)
/* C.ADDI4SPN 是 CIW 格式指令。
 * 它把零扩展且非零的立即数（按 4 缩放）加到栈指针 x2，并把结果写入 rd'。
 * 该指令用于生成指向栈分配变量的指针，可展开为 addi rd', x2, nzuimm[9:2]。
 */
RVOP(caddi4spn, { rv->X[ir->rd] = rv->X[rv_reg_sp] + (uint16_t) ir->imm; })

/* C.LW 从内存加载 32 位值到 rd'。
 * 有效地址由 rs1' 中的基地址加上零扩展且按 4 缩放的偏移得到。它可展开为
 * lw rd', offset[6:2](rs1')。
 */
RVOP(clw, {
    const uint32_t addr = rv->X[ir->rs1] + (uint32_t) ir->imm;
    RV_EXC_MISALIGN_HANDLER(3, LOAD, true, 1);
    rv->X[ir->rd] = MEM_READ_W(rv, addr);
})

/* C.SW 把寄存器 rs2' 中的 32 位值存入内存。
 * 有效地址由 rs1' 中的基地址加上零扩展且按 4 缩放的偏移得到。
 * 它可展开为 sw rs2', offset[6:2](rs1')。
 */
RVOP(csw, {
    const uint32_t addr = rv->X[ir->rs1] + (uint32_t) ir->imm;
    RV_EXC_MISALIGN_HANDLER(3, STORE, true, 1);
    const uint32_t value = rv->X[ir->rs2];
    MEM_WRITE_W(rv, addr, value);
#if RV32_HAS(ARCH_TEST)
    check_tohost_write(rv, addr, value);
#endif
})

/* C.NOP：空操作。 */
RVOP(cnop, {/* 空操作。 */})

/* C.ADDI 把非零且符号扩展后的 6 位立即数加到寄存器 rd，并把结果写回 rd。
 * C.ADDI 可展开为 addi rd, rd, nzimm[5:0]。C.ADDI 仅在 rd' != x0 时有效。
 * rd=x0 且 nzimm=0 的编码点表示 C.NOP；rd=x0 或 nzimm=0 的其他编码点表示 HINT。
 */
RVOP(caddi, { rv->X[ir->rd] += (int16_t) ir->imm; })

/* C.JAL */
RVOP(cjal, {
    rv->X[rv_reg_ra] = PC + 2;
    PC += ir->imm;
    struct rv_insn *taken = ir->branch_taken;
    if (taken) {
#if RV32_HAS(JIT)
        IIF(RV32_HAS(SYSTEM))(block_t *next =, )
            cache_get(rv->block_cache, PC, true);
        IIF(RV32_HAS(SYSTEM))(
            if (next->satp == rv->csr_satp && !next->invalidated), )
        {
            if (!set_add(&pc_set, PC))
                has_loops = true;
            if (cache_hot(rv->block_cache, PC))
                goto end_op;
        }
#endif

#if RV32_HAS(SYSTEM)
        if (!rv->is_trapped)
#endif
        {
            last_pc = PC;
            MUST_TAIL return taken->impl(rv, taken, cycle, PC);
        }
    }
    goto end_op;
})

/* C.LI 把符号扩展后的 6 位立即数 imm 加载到寄存器 rd。
 * C.LI 可展开为 addi rd, x0, imm[5:0]。
 * C.LI 仅在 rd != x0 时有效；rd=x0 的编码点表示 HINT。
 */
RVOP(cli, { rv->X[ir->rd] = ir->imm; })

/* C.ADDI16SP 用于在过程序言和尾声中调整栈指针。
 * 它可展开为 addi x2, x2, nzimm[9:4]。
 * C.ADDI16SP 仅在 nzimm != 0 时有效；nzimm=0 的编码点为保留编码。
 */
RVOP(caddi16sp, { rv->X[ir->rd] += ir->imm; })

/* C.LUI 把非零 6 位立即数字段加载到目标寄存器的 17-12 位，清零低 12 位，并把
 * 第 17 位符号扩展到目标寄存器更高位。
 * C.LUI 可展开为 lui rd, nzimm[17:12]。
 * C.LUI 仅在 rd 不为 x0/x2 且立即数非零时有效。
 */
RVOP(clui, { rv->X[ir->rd] = ir->imm; })

/* C.SRLI 是 CB 格式指令。
 * 它对寄存器 rd' 的值执行逻辑右移，并把结果写回 rd'。移位量编码在 shamt 字段。
 * C.SRLI 可展开为 srli rd', rd', shamt[5:0]。
 */
RVOP(csrli, { rv->X[ir->rs1] >>= ir->shamt; })

/* C.SRAI 与 C.SRLI 类似，但执行算术右移。
 * C.SRAI 可展开为 srai rd', rd', shamt[5:0]。
 */
RVOP(csrai, {
    const uint32_t mask = 0x80000000 & rv->X[ir->rs1];
    rv->X[ir->rs1] >>= ir->shamt;
    for (unsigned int i = 0; i < ir->shamt; ++i)
        rv->X[ir->rs1] |= mask >> i;
})

/* C.ANDI 是 CB 格式指令。
 * 它对寄存器 rd' 的值和符号扩展后的 6 位立即数执行按位与，并把结果写回 rd'。
 * C.ANDI 可展开为 andi rd', rd', imm[5:0]。
 */
RVOP(candi, { rv->X[ir->rs1] &= ir->imm; })

/* C.SUB */
RVOP(csub, { rv->X[ir->rd] = rv->X[ir->rs1] - rv->X[ir->rs2]; })

/* C.XOR */
RVOP(cxor, { rv->X[ir->rd] = rv->X[ir->rs1] ^ rv->X[ir->rs2]; })

RVOP(cor, { rv->X[ir->rd] = rv->X[ir->rs1] | rv->X[ir->rs2]; })

RVOP(cand, { rv->X[ir->rd] = rv->X[ir->rs1] & rv->X[ir->rs2]; })

/* C.J 执行无条件控制转移。
 * 偏移经过符号扩展后加到 PC，形成跳转目标地址。因此 C.J 可跳转到 ±2 KiB 范围。
 * C.J 可展开为 jal x0, offset[11:1]。
 */
RVOP(cj, {
    PC += ir->imm;
    struct rv_insn *taken = ir->branch_taken;
    if (taken) {
#if RV32_HAS(JIT)
        IIF(RV32_HAS(SYSTEM))(block_t *next =, )
            cache_get(rv->block_cache, PC, true);
        IIF(RV32_HAS(SYSTEM))(
            if (next->satp == rv->csr_satp && !next->invalidated), )
        {
            if (!set_add(&pc_set, PC))
                has_loops = true;
            if (cache_hot(rv->block_cache, PC))
                goto end_op;
        }
#endif
#if RV32_HAS(SYSTEM)
        if (!rv->is_trapped)
#endif
        {
            last_pc = PC;
            MUST_TAIL return taken->impl(rv, taken, cycle, PC);
        }
    }
    goto end_op;
})

/* C.BEQZ 执行条件控制转移。
 * 偏移经过符号扩展后加到 PC，形成分支目标地址，因此可跳转到 ±256 B 范围。
 * 若寄存器 rs1' 的值为零，C.BEQZ 选择分支。它可展开为
 * beq rs1', x0, offset[8:1]。
 */
RVOP(cbeqz, {
    if (rv->X[ir->rs1]) {
        is_branch_taken = false;
        struct rv_insn *untaken = ir->branch_untaken;
        if (!untaken)
            goto nextop;
#if RV32_HAS(JIT)
        IIF(RV32_HAS(SYSTEM))(block_t *next =, )
            cache_get(rv->block_cache, PC + 2, true);
        IIF(RV32_HAS(SYSTEM))(
            if (next->satp == rv->csr_satp && !next->invalidated), )
        {
            if (!set_add(&pc_set, PC + 2))
                has_loops = true;
            if (cache_hot(rv->block_cache, PC + 2))
                goto nextop;
        }
#endif
        PC += 2;
#if RV32_HAS(SYSTEM)
        if (!rv->is_trapped)
#endif
        {
            last_pc = PC;
            MUST_TAIL return untaken->impl(rv, untaken, cycle, PC);
        }

        goto end_op;
    }
    is_branch_taken = true;
    PC += ir->imm;
    struct rv_insn *taken = ir->branch_taken;
    if (taken) {
#if RV32_HAS(JIT)
        IIF(RV32_HAS(SYSTEM))(block_t *next =, )
            cache_get(rv->block_cache, PC, true);
        IIF(RV32_HAS(SYSTEM))(
            if (next->satp == rv->csr_satp && !next->invalidated), )
        {
            if (!set_add(&pc_set, PC))
                has_loops = true;
            if (cache_hot(rv->block_cache, PC))
                goto end_op;
        }
#endif
#if RV32_HAS(SYSTEM)
        if (!rv->is_trapped)
#endif
        {
            last_pc = PC;
            MUST_TAIL return taken->impl(rv, taken, cycle, PC);
        }
    }
    goto end_op;
})

/* C.BNEZ */
RVOP(cbnez, {
    if (!rv->X[ir->rs1]) {
        is_branch_taken = false;
        struct rv_insn *untaken = ir->branch_untaken;
        if (!untaken)
            goto nextop;
#if RV32_HAS(JIT)
        IIF(RV32_HAS(SYSTEM))(block_t *next =, )
            cache_get(rv->block_cache, PC + 2, true);
        IIF(RV32_HAS(SYSTEM))(
            if (next->satp == rv->csr_satp && !next->invalidated), )
        {
            if (!set_add(&pc_set, PC + 2))
                has_loops = true;
            if (cache_hot(rv->block_cache, PC + 2))
                goto nextop;
        }
#endif
        PC += 2;
#if RV32_HAS(SYSTEM)
        if (!rv->is_trapped)
#endif
        {
            last_pc = PC;
            MUST_TAIL return untaken->impl(rv, untaken, cycle, PC);
        }

        goto end_op;
    }
    is_branch_taken = true;
    PC += ir->imm;
    struct rv_insn *taken = ir->branch_taken;
    if (taken) {
#if RV32_HAS(JIT)
        IIF(RV32_HAS(SYSTEM))(block_t *next =, )
            cache_get(rv->block_cache, PC, true);
        IIF(RV32_HAS(SYSTEM))(
            if (next->satp == rv->csr_satp && !next->invalidated), )
        {
            if (!set_add(&pc_set, PC))
                has_loops = true;
            if (cache_hot(rv->block_cache, PC))
                goto end_op;
        }
#endif
#if RV32_HAS(SYSTEM)
        if (!rv->is_trapped)
#endif
        {
            last_pc = PC;
            MUST_TAIL return taken->impl(rv, taken, cycle, PC);
        }
    }
    goto end_op;
})

/* C.SLLI 是 CI 格式指令。
 * 它对寄存器 rd 的值执行逻辑左移，并把结果写回 rd。移位量编码在 shamt 字段。
 * C.SLLI 可展开为 slli rd, rd, shamt[5:0]。
 */
RVOP(cslli, { rv->X[ir->rd] <<= (uint8_t) ir->imm; })

/* C.LWSP */
RVOP(clwsp, {
    const uint32_t addr = rv->X[rv_reg_sp] + ir->imm;
    RV_EXC_MISALIGN_HANDLER(3, LOAD, true, 1);
    rv->X[ir->rd] = MEM_READ_W(rv, addr);
})

/* C.JR */
RVOP(cjr, {
    PC = rv->X[ir->rs1];
    LOOKUP_OR_UPDATE_BRANCH_HISTORY_TABLE();
    goto end_op;
})

/* C.MV */
RVOP(cmv, { rv->X[ir->rd] = rv->X[ir->rs2]; })

/* C.EBREAK */
RVOP(cebreak, {
    rv->compressed = true;
    rv->csr_cycle = cycle;
    rv->PC = PC;
    rv->io.on_ebreak(rv);
    return true;
})

/* C.JALR */
RVOP(cjalr, {
    /* 无条件跳转，并把 PC+2 保存到 ra。 */
    const int32_t jump_to = rv->X[ir->rs1];
    rv->X[rv_reg_ra] = PC + 2;
    PC = jump_to;
    LOOKUP_OR_UPDATE_BRANCH_HISTORY_TABLE();
    goto end_op;
})

/* C.ADD 把寄存器 rd 和 rs2 的值相加，并把结果写回 rd。
 * C.ADD 可展开为 add rd, rd, rs2。
 * C.ADD 仅在 rs2 != x0 时有效；rs2=x0 的编码点对应 C.JALR 和 C.EBREAK。
 * rs2=x0 且 rd=x0 的编码点表示 HINT。
 */
RVOP(cadd, { rv->X[ir->rd] = rv->X[ir->rs1] + rv->X[ir->rs2]; })

/* C.SWSP */
RVOP(cswsp, {
    const uint32_t addr = rv->X[rv_reg_sp] + ir->imm;
    RV_EXC_MISALIGN_HANDLER(3, STORE, true, 1);
    const uint32_t value = rv->X[ir->rs2];
    MEM_WRITE_W(rv, addr, value);
#if RV32_HAS(ARCH_TEST)
    check_tohost_write(rv, addr, value);
#endif
})
#endif

#if RV32_HAS(EXT_C) && RV32_HAS(EXT_F)
/* C.FLWSP */
RVOP(cflwsp, {
    const uint32_t addr = rv->X[rv_reg_sp] + ir->imm;
    RV_EXC_MISALIGN_HANDLER(3, LOAD, false, 1);
    rv->F[ir->rd].v = MEM_READ_W(rv, addr);
})

/* C.FSWSP */
RVOP(cfswsp, {
    const uint32_t addr = rv->X[rv_reg_sp] + ir->imm;
    RV_EXC_MISALIGN_HANDLER(3, STORE, false, 1);
    const uint32_t value = rv->F[ir->rs2].v;
    MEM_WRITE_W(rv, addr, value);
#if RV32_HAS(ARCH_TEST)
    check_tohost_write(rv, addr, value);
#endif
})

/* C.FLW */
RVOP(cflw, {
    const uint32_t addr = rv->X[ir->rs1] + (uint32_t) ir->imm;
    RV_EXC_MISALIGN_HANDLER(3, LOAD, false, 1);
    rv->F[ir->rd].v = MEM_READ_W(rv, addr);
})

/* C.FSW */
RVOP(cfsw, {
    const uint32_t addr = rv->X[ir->rs1] + (uint32_t) ir->imm;
    RV_EXC_MISALIGN_HANDLER(3, STORE, false, 1);
    const uint32_t value = rv->F[ir->rs2].v;
    MEM_WRITE_W(rv, addr, value);
#if RV32_HAS(ARCH_TEST)
    check_tohost_write(rv, addr, value);
#endif
})
#endif

/* RV32Zba 标准扩展。 */

#if RV32_HAS(Zba)

/* SH1ADD */
RVOP(sh1add, { rv->X[ir->rd] = (rv->X[ir->rs1] << 1) + rv->X[ir->rs2]; })

/* SH2ADD */
RVOP(sh2add, { rv->X[ir->rd] = (rv->X[ir->rs1] << 2) + rv->X[ir->rs2]; })

/* SH3ADD */
RVOP(sh3add, { rv->X[ir->rd] = (rv->X[ir->rs1] << 3) + rv->X[ir->rs2]; })

#endif

/* RV32Zbb 标准扩展。 */

#if RV32_HAS(Zbb)

/* ANDN */
RVOP(andn, { rv->X[ir->rd] = rv->X[ir->rs1] & (~rv->X[ir->rs2]); })

/* ORN */
RVOP(orn, { rv->X[ir->rd] = rv->X[ir->rs1] | (~rv->X[ir->rs2]); })

/* XNOR */
RVOP(xnor, { rv->X[ir->rd] = ~(rv->X[ir->rs1] ^ rv->X[ir->rs2]); })

/* CLZ */
RVOP(clz, {
    if (rv->X[ir->rs1])
        rv->X[ir->rd] = rv_clz(rv->X[ir->rs1]);
    else
        rv->X[ir->rd] = 32;
})

/* CTZ */
RVOP(ctz, {
    if (rv->X[ir->rs1])
        rv->X[ir->rd] = rv_ctz(rv->X[ir->rs1]);
    else
        rv->X[ir->rd] = 32;
})

/* CPOP */
RVOP(cpop, { rv->X[ir->rd] = rv_popcount(rv->X[ir->rs1]); })

/* MAX */
RVOP(max, {
    const int32_t x = rv->X[ir->rs1];
    const int32_t y = rv->X[ir->rs2];
    rv->X[ir->rd] = x > y ? rv->X[ir->rs1] : rv->X[ir->rs2];
})

/* MIN */
RVOP(min, {
    const int32_t x = rv->X[ir->rs1];
    const int32_t y = rv->X[ir->rs2];
    rv->X[ir->rd] = x < y ? rv->X[ir->rs1] : rv->X[ir->rs2];
})

/* MAXU */
RVOP(maxu, {
    const uint32_t x = rv->X[ir->rs1];
    const uint32_t y = rv->X[ir->rs2];
    rv->X[ir->rd] = x > y ? rv->X[ir->rs1] : rv->X[ir->rs2];
})

/* MINU */
RVOP(minu, {
    const uint32_t x = rv->X[ir->rs1];
    const uint32_t y = rv->X[ir->rs2];
    rv->X[ir->rd] = x < y ? rv->X[ir->rs1] : rv->X[ir->rs2];
})

/* SEXT.B */
RVOP(sextb, {
    rv->X[ir->rd] = rv->X[ir->rs1] & 0xff;
    if (rv->X[ir->rs1] & (1U << 7))
        rv->X[ir->rd] |= 0xffffff00;
})

/* SEXT.H */
RVOP(sexth, {
    rv->X[ir->rd] = rv->X[ir->rs1] & 0xffff;
    if (rv->X[ir->rs1] & (1U << 15))
        rv->X[ir->rd] |= 0xffff0000;
})

/* ZEXT.H */
RVOP(zexth, { rv->X[ir->rd] = rv->X[ir->rs1] & 0x0000ffff; })

/* ROL */
RVOP(rol, {
    const unsigned int shamt = rv->X[ir->rs2] & 0b11111;
    rv->X[ir->rd] =
        (rv->X[ir->rs1] << shamt) | (rv->X[ir->rs1] >> (32 - shamt));
})

/* ROR */
RVOP(ror, {
    const unsigned int shamt = rv->X[ir->rs2] & 0b11111;
    rv->X[ir->rd] =
        (rv->X[ir->rs1] >> shamt) | (rv->X[ir->rs1] << (32 - shamt));
})

/* RORI */
RVOP(rori, {
    const unsigned int shamt = ir->imm & 0b11111;
    rv->X[ir->rd] =
        (rv->X[ir->rs1] >> shamt) | (rv->X[ir->rs1] << (32 - shamt));
})

/* ORCB */
RVOP(orcb, {
    const uint32_t x = rv->X[ir->rs1];
    rv->X[ir->rd] = 0;
    for (int i = 0; i < 4; i++)
        if (x & (0xffu << (i * 8)))
            rv->X[ir->rd] |= 0xffu << (i * 8);
})

/* REV8 */
RVOP(rev8, {
    rv->X[ir->rd] =
        (((rv->X[ir->rs1] & 0xffU) << 24) | ((rv->X[ir->rs1] & 0xff00U) << 8) |
         ((rv->X[ir->rs1] & 0xff0000U) >> 8) |
         ((rv->X[ir->rs1] & 0xff000000U) >> 24));
})

#endif

/* RV32Zbc 标准扩展。 */

#if RV32_HAS(Zbc)

/* CLMUL */
RVOP(clmul, {
    uint32_t output = 0;
    for (int i = 0; i < 32; i++)
        if ((rv->X[ir->rs2] >> i) & 1)
            output ^= rv->X[ir->rs1] << i;
    rv->X[ir->rd] = output;
})

/* CLMULH */
RVOP(clmulh, {
    uint32_t output = 0;
    for (int i = 1; i < 32; i++)
        if ((rv->X[ir->rs2] >> i) & 1)
            output ^= rv->X[ir->rs1] >> (32 - i);
    rv->X[ir->rd] = output;
})

/* CLMULR */
RVOP(clmulr, {
    uint32_t output = 0;
    for (int i = 0; i < 32; i++)
        if ((rv->X[ir->rs2] >> i) & 1)
            output ^= rv->X[ir->rs1] >> (32 - i - 1);
    rv->X[ir->rd] = output;
})

#endif

/* RV32Zbs 标准扩展。 */

#if RV32_HAS(Zbs)

/* BCLR */
RVOP(bclr, {
    const unsigned int index = rv->X[ir->rs2] & (32 - 1);
    rv->X[ir->rd] = rv->X[ir->rs1] & (~(1U << index));
})

/* BCLRI */
RVOP(bclri, {
    const unsigned int index = ir->imm & (32 - 1);
    rv->X[ir->rd] = rv->X[ir->rs1] & (~(1U << index));
})

/* BEXT */
RVOP(bext, {
    const unsigned int index = rv->X[ir->rs2] & (32 - 1);
    rv->X[ir->rd] = (rv->X[ir->rs1] >> index) & 1;
})

/* BEXTI */
RVOP(bexti, {
    const unsigned int index = ir->imm & (32 - 1);
    rv->X[ir->rd] = (rv->X[ir->rs1] >> index) & 1;
})

/* BINV */
RVOP(binv, {
    const unsigned int index = rv->X[ir->rs2] & (32 - 1);
    rv->X[ir->rd] = rv->X[ir->rs1] ^ (1U << index);
})

/* BINVI */
RVOP(binvi, {
    const unsigned int index = ir->imm & (32 - 1);
    rv->X[ir->rd] = rv->X[ir->rs1] ^ (1U << index);
})

/* BSET */
RVOP(bset, {
    const unsigned int index = rv->X[ir->rs2] & (32 - 1);
    rv->X[ir->rd] = rv->X[ir->rs1] | (1U << index);
})

/* BSETI */
RVOP(bseti, {
    const unsigned int index = ir->imm & (32 - 1);
    rv->X[ir->rd] = rv->X[ir->rs1] | (1U << index);
})

#endif
