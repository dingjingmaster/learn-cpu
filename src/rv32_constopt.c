/*
 * RV32 常量传播和折叠规则。
 *
 * 本文件由 emulate.c 以模板方式包含，使用 CONSTOPT 宏为每条 IR 定义常量状态
 * 如何传播。它不会执行指令，而是标记寄存器是否为常量、常量值是多少，以及遇到
 * 分支、访存、CSR、浮点等不易静态证明的操作时如何保守失效。
 */

/* RV32I 基础指令集。 */

/* 内部伪操作。 */
CONSTOPT(nop, {})

/* LUI 用 U-type 格式构造 32 位常量：U-immediate 放入目标寄存器高 20 位，
 * 低 12 位填 0，结果按指令语义写回 rd。
 */
CONSTOPT(lui, {
    info->is_constant[ir->rd] = true;
    info->const_val[ir->rd] = ir->imm;
})

/* AUIPC 用于构造 PC 相对地址，也采用 U-type 格式。它把 20 位 U-immediate 放到
 * 32 位偏移的高位，低 12 位填 0，再加上当前 AUIPC 指令地址，结果写入 rd。
 */
CONSTOPT(auipc, {
    ir->imm += ir->pc;
    info->is_constant[ir->rd] = true;
    info->const_val[ir->rd] = ir->imm;
    ir->opcode = rv_insn_lui;
    ir->impl = dispatch_table[ir->opcode];
})

/* JAL：跳转并链接。
 * 将后继指令地址写入 rd，并把 J 型立即数偏移加到 PC 上。
 */
CONSTOPT(jal, {
    if (ir->rd) {
        info->is_constant[ir->rd] = true;
        info->const_val[ir->rd] = ir->pc + 4;
    }
})

/* JALR 是 I-type 间接跳转指令。目标地址为 rs1 加符号扩展后的 12 位立即数，
 * 再清零最低位。跳转后继地址 pc+4 写入 rd；若不需要返回地址，可用 x0 作为 rd。
 */
CONSTOPT(jalr, {
    if (ir->rd) {
        info->is_constant[ir->rd] = true;
        info->const_val[ir->rd] = ir->pc + 4;
    }
})

/* clang-format off */
#define OPT_BRANCH_FUNC(type, cond)                                 \
    if (info->is_constant[ir->rs1] && info->is_constant[ir->rs2]) { \
        if ((type) info->const_val[ir->rs1] cond                    \
            (type) info->const_val[ir->rs2])                        \
            ir->imm = 4;                                            \
        ir->opcode = rv_insn_jal;                                   \
        ir->impl = dispatch_table[ir->opcode];                      \
    }
/* clang-format on */

/* BEQ：相等则分支。 */
CONSTOPT(beq, { OPT_BRANCH_FUNC(uint32_t, !=); })

/* BNE：不相等则分支。 */
CONSTOPT(bne, { OPT_BRANCH_FUNC(uint32_t, ==); })

/* BLT：有符号小于则分支。 */
CONSTOPT(blt, { OPT_BRANCH_FUNC(int32_t, >=); })

/* BGE：有符号大于等于则分支。 */
CONSTOPT(bge, { OPT_BRANCH_FUNC(int32_t, <); })

/* BLTU：无符号小于则分支。 */
CONSTOPT(bltu, { OPT_BRANCH_FUNC(uint32_t, >=); })

/* BGEU：无符号大于等于则分支。 */
CONSTOPT(bgeu, { OPT_BRANCH_FUNC(uint32_t, <); })

/* LB：加载有符号字节，内存值不可静态确定，目标寄存器失效。 */
CONSTOPT(lb, { info->is_constant[ir->rd] = false; })

/* LH：加载有符号半字，目标寄存器失效。 */
CONSTOPT(lh, { info->is_constant[ir->rd] = false; })

/* LW：加载字，目标寄存器失效。 */
CONSTOPT(lw, { info->is_constant[ir->rd] = false; })

/* LBU：加载无符号字节，目标寄存器失效。 */
CONSTOPT(lbu, { info->is_constant[ir->rd] = false; })

/* LHU：加载无符号半字，目标寄存器失效。 */
CONSTOPT(lhu, { info->is_constant[ir->rd] = false; })

/* SB：存储字节，不改变寄存器常量状态。 */
CONSTOPT(sb, {})

/* SH：存储半字，不改变寄存器常量状态。 */
CONSTOPT(sh, {})

/* SW：存储字，不改变寄存器常量状态。 */
CONSTOPT(sw, {})

/* ADDI 将符号扩展后的 12 位立即数加到 rs1。算术溢出按 XLEN 低位截断处理。
 * ADDI rd, rs1, 0 也用于实现 MV rd, rs1 汇编伪指令。
 */
CONSTOPT(addi, {
    if (info->is_constant[ir->rs1]) {
        ir->imm += info->const_val[ir->rs1];
        info->is_constant[ir->rd] = true;
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* SLTI 按有符号数比较 rs1 与符号扩展立即数；rs1 更小时 rd=1，否则 rd=0。 */
CONSTOPT(slti, {
    if (info->is_constant[ir->rs1]) {
        ir->imm = (int32_t) info->const_val[ir->rs1] < ir->imm ? 1 : 0;
        info->is_constant[ir->rd] = true;
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* SLTIU 按无符号数比较 rs1 与立即数；rs1 更小时 rd=1，否则 rd=0。 */
CONSTOPT(sltiu, {
    if (info->is_constant[ir->rs1]) {
        ir->imm = info->const_val[ir->rs1] < (uint32_t) ir->imm ? 1 : 0;
        info->is_constant[ir->rd] = true;
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* XORI：立即数异或。 */
CONSTOPT(xori, {
    if (info->is_constant[ir->rs1]) {
        ir->imm ^= info->const_val[ir->rs1];
        info->is_constant[ir->rd] = true;
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* ORI：立即数或。 */
CONSTOPT(ori, {
    if (info->is_constant[ir->rs1]) {
        ir->imm |= info->const_val[ir->rs1];
        info->is_constant[ir->rd] = true;
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* ANDI 对 rs1 和符号扩展后的 12 位立即数做按位与，结果写入 rd。 */
CONSTOPT(andi, {
    if (info->is_constant[ir->rs1]) {
        ir->imm &= info->const_val[ir->rs1];
        info->is_constant[ir->rd] = true;
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* SLLI 使用立即数低 5 位作为移位量，对 rs1 做逻辑左移。 */
CONSTOPT(slli, {
    if (info->is_constant[ir->rs1]) {
        ir->imm = info->const_val[ir->rs1] << (ir->imm & 0x1f);
        info->is_constant[ir->rd] = true;
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* SRLI 使用立即数低 5 位作为移位量，对 rs1 做逻辑右移。 */
CONSTOPT(srli, {
    if (info->is_constant[ir->rs1]) {
        ir->imm = info->const_val[ir->rs1] >> (ir->imm & 0x1f);
        info->is_constant[ir->rd] = true;
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* SRAI 使用立即数低 5 位作为移位量，对 rs1 做算术右移。 */
CONSTOPT(srai, {
    if (info->is_constant[ir->rs1]) {
        ir->imm = (int32_t) info->const_val[ir->rs1] >> (ir->imm & 0x1f);
        info->is_constant[ir->rd] = true;
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* ADD */
CONSTOPT(add, {
    if (info->is_constant[ir->rs1] && info->is_constant[ir->rs2]) {
        info->is_constant[ir->rd] = true;
        ir->imm = info->const_val[ir->rs1] + info->const_val[ir->rs2];
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* SUB：减法。 */
CONSTOPT(sub, {
    if (info->is_constant[ir->rs1] && info->is_constant[ir->rs2]) {
        info->is_constant[ir->rd] = true;
        ir->imm = info->const_val[ir->rs1] - info->const_val[ir->rs2];
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* SLL：逻辑左移。 */
CONSTOPT(sll, {
    if (info->is_constant[ir->rs1] && info->is_constant[ir->rs2]) {
        info->is_constant[ir->rd] = true;
        ir->imm = info->const_val[ir->rs1] << (info->const_val[ir->rs2] & 0x1f);
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* SLT：有符号小于则置 1。 */
CONSTOPT(slt, {
    if (info->is_constant[ir->rs1] && info->is_constant[ir->rs2]) {
        info->is_constant[ir->rd] = true;
        ir->imm = (int32_t) info->const_val[ir->rs1] <
                          (int32_t) info->const_val[ir->rs2]
                      ? 1
                      : 0;
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* SLTU：无符号小于则置 1。 */
CONSTOPT(sltu, {
    if (info->is_constant[ir->rs1] && info->is_constant[ir->rs2]) {
        info->is_constant[ir->rd] = true;
        ir->imm = info->const_val[ir->rs1] < info->const_val[ir->rs2] ? 1 : 0;
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* XOR：按位异或。 */
CONSTOPT(xor, {
  if (info->is_constant[ir->rs1] && info->is_constant[ir->rs2]) {
      info->is_constant[ir->rd] = true;
      ir->imm = (int32_t) info->const_val[ir->rs1] ^
                (int32_t) info->const_val[ir->rs2];
      info->const_val[ir->rd] = ir->imm;
      ir->opcode = rv_insn_lui;
      ir->impl = dispatch_table[ir->opcode];
  } else
      info->is_constant[ir->rd] = false;
})

/* SRL：逻辑右移。 */
CONSTOPT(srl, {
    if (info->is_constant[ir->rs1] && info->is_constant[ir->rs2]) {
        info->is_constant[ir->rd] = true;
        ir->imm = info->const_val[ir->rs1] >> (info->const_val[ir->rs2] & 0x1f);
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* SRA：算术右移。 */
CONSTOPT(sra, {
    if (info->is_constant[ir->rs1] && info->is_constant[ir->rs2]) {
        info->is_constant[ir->rd] = true;
        ir->imm = (int32_t) info->const_val[ir->rs1] >>
                  (info->const_val[ir->rs2] & 0x1f);
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* OR */
CONSTOPT(or, {
    if (info->is_constant[ir->rs1] && info->is_constant[ir->rs2]) {
        info->is_constant[ir->rd] = true;
        ir->imm = (int32_t) info->const_val[ir->rs1] |
                  (int32_t) info->const_val[ir->rs2];
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* AND */
CONSTOPT(and, {
    if (info->is_constant[ir->rs1] && info->is_constant[ir->rs2]) {
        info->is_constant[ir->rd] = true;
        ir->imm = (int32_t) info->const_val[ir->rs1] &
                  (int32_t) info->const_val[ir->rs2];
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/*
 * FENCE：对其他 RISC-V hart、外部设备或协处理器可见的设备 I/O 和内存访问排序。
 */
CONSTOPT(fence, {})

/* ECALL：环境调用。 */
CONSTOPT(ecall, {})

/* EBREAK：环境断点。 */
CONSTOPT(ebreak, {})

/* WFI：等待中断。 */
CONSTOPT(wfi, {})

/* URET：从 U 模式 trap 返回。 */
CONSTOPT(uret, {})

#if RV32_HAS(SYSTEM)
/* SRET：从 S 模式 trap 返回。 */
CONSTOPT(sret, {})
#endif

/* HRET：从 H 模式 trap 返回。 */
CONSTOPT(hret, {})

/* MRET：从 M 模式 trap 返回。 */
CONSTOPT(mret, {})

/* SFENCE.VMA：同步内存中内存管理数据结构的更新与当前执行流。 */
CONSTOPT(sfencevma, {})

#if RV32_HAS(Zifencei) /* RV32 Zifencei 标准扩展。 */
CONSTOPT(fencei, {})
#endif

#if RV32_HAS(Zicsr) /* RV32 Zicsr 标准扩展。 */
/* CSRRW：原子读/写 CSR。 */
CONSTOPT(csrrw, { info->is_constant[ir->rd] = false; })

/* CSRRS：原子读取并置位 CSR 位。 */
CONSTOPT(csrrs, { info->is_constant[ir->rd] = false; })

/* CSRRC：原子读取并清除 CSR 位。 */
CONSTOPT(csrrc, { info->is_constant[ir->rd] = false; })

/* CSRRWI */
CONSTOPT(csrrwi, { info->is_constant[ir->rd] = false; })

/* CSRRSI */
CONSTOPT(csrrsi, { info->is_constant[ir->rd] = false; })

/* CSRRCI */
CONSTOPT(csrrci, { info->is_constant[ir->rd] = false; })
#endif

/* RV32M 标准扩展。 */

#if RV32_HAS(EXT_M)
/* MUL：乘法低 32 位。 */
CONSTOPT(mul, {
    if (info->is_constant[ir->rs1] && info->is_constant[ir->rs2]) {
        info->is_constant[ir->rd] = true;
        const int64_t multiplicand = (int32_t) info->const_val[ir->rs1];
        const int64_t multiplier = (int32_t) info->const_val[ir->rs2];
        ir->imm = ((uint64_t) (multiplicand * multiplier)) & ((1ULL << 32) - 1);
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* MULH：有符号乘有符号，取高 32 位。 */
CONSTOPT(mulh, {
    if (info->is_constant[ir->rs1] && info->is_constant[ir->rs2]) {
        info->is_constant[ir->rd] = true;
        const int64_t a = (int32_t) info->const_val[ir->rs1];
        const int64_t b = (int32_t) info->const_val[ir->rs2];
        ir->imm = ((uint64_t) (a * b)) >> 32;
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* MULHSU：有符号乘无符号，取高 32 位。 */
CONSTOPT(mulhsu, {
    if (info->is_constant[ir->rs1] && info->is_constant[ir->rs2]) {
        info->is_constant[ir->rd] = true;
        const int64_t a = (int32_t) info->const_val[ir->rs1];
        const int64_t b = info->const_val[ir->rs2];
        ir->imm = ((uint64_t) (a * b)) >> 32;
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* MULHU：无符号乘无符号，取高 32 位。 */
CONSTOPT(mulhu, {
    if (info->is_constant[ir->rs1] && info->is_constant[ir->rs2]) {
        info->is_constant[ir->rd] = true;
        ir->imm = ((int64_t) info->const_val[ir->rs1] *
                   (int64_t) info->const_val[ir->rs2]) >>
                  32;
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* DIV：有符号除法。 */
/* +------------------------+-----------+----------+-----------+
 * |       条件             |  被除数   |  除数    |   DIV[W]  |
 * +------------------------+-----------+----------+-----------+
 * | 除零                  |  x        |  0       |  -1       |
 * | 溢出（仅有符号）      |  -2^{L-1} |  -1      |  -2^{L-1} |
 * +------------------------+-----------+----------+-----------+
 */
CONSTOPT(div, {
    if (info->is_constant[ir->rs1] && info->is_constant[ir->rs2]) {
        info->is_constant[ir->rd] = true;
        const int32_t dividend = (int32_t) info->const_val[ir->rs1];
        const int32_t divisor = (int32_t) info->const_val[ir->rs2];
        ir->imm = !divisor ? ~0U
                  : (divisor == -1 && info->const_val[ir->rs1] == 0x80000000U)
                      ? info->const_val[ir->rs1] /* 溢出。 */
                      : (unsigned int) (dividend / divisor);
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* DIVU：无符号除法。 */
/* +------------------------+-----------+----------+----------+
 * |       条件             |  被除数   |  除数    |  DIVU[W] |
 * +------------------------+-----------+----------+----------+
 * | 除零                  |  x        |  0       |  2^L - 1 |
 * +------------------------+-----------+----------+----------+
 */
CONSTOPT(divu, {
    if (info->is_constant[ir->rs1] && info->is_constant[ir->rs2]) {
        info->is_constant[ir->rd] = true;
        const uint32_t dividend = info->const_val[ir->rs1];
        const uint32_t divisor = info->const_val[ir->rs2];
        ir->imm = !divisor ? ~0U : dividend / divisor;
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* REM：有符号余数。 */
/* +------------------------+-----------+----------+---------+
 * |       条件             |  被除数   |  除数    |  REM[W] |
 * +------------------------+-----------+----------+---------+
 * | 除零                  |  x        |  0       |  x      |
 * | 溢出（仅有符号）      |  -2^{L-1} |  -1      |  0      |
 * +------------------------+-----------+----------+---------+
 */
CONSTOPT(rem, {
    if (info->is_constant[ir->rs1] && info->is_constant[ir->rs2]) {
        info->is_constant[ir->rd] = true;
        const int32_t dividend = info->const_val[ir->rs1];
        const int32_t divisor = info->const_val[ir->rs2];
        ir->imm = !divisor ? dividend
                  : (divisor == -1 && info->const_val[ir->rs1] == 0x80000000U)
                      ? 0 /* 溢出。 */
                      : (dividend % divisor);
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* REMU：无符号余数。 */
/* +------------------------+-----------+----------+----------+
 * |       条件             |  被除数   |  除数    |  REMU[W] |
 * +------------------------+-----------+----------+----------+
 * | 除零                  |  x        |  0       |  x       |
 * +------------------------+-----------+----------+----------+
 */
CONSTOPT(remu, {
    if (info->is_constant[ir->rs1] && info->is_constant[ir->rs2]) {
        info->is_constant[ir->rd] = true;
        const uint32_t dividend = info->const_val[ir->rs1];
        const uint32_t divisor = info->const_val[ir->rs2];
        ir->imm = !divisor ? dividend : dividend % divisor;
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_lui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})
#endif

/* RV32A 标准扩展。 */
/* TODO：支持 A 和 F 扩展的常量优化。 */
#if RV32_HAS(EXT_A)

/* LR.W：保留加载。 */
CONSTOPT(lrw, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* SC.W：条件存储。 */
CONSTOPT(scw, {})

/* AMOSWAP.W：原子交换。 */
CONSTOPT(amoswapw, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* AMOADD.W：原子加法。 */
CONSTOPT(amoaddw, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* AMOXOR.W：原子异或。 */
CONSTOPT(amoxorw, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* AMOAND.W：原子与。 */
CONSTOPT(amoandw, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* AMOOR.W：原子或。 */
CONSTOPT(amoorw, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* AMOMIN.W：原子有符号最小值。 */
CONSTOPT(amominw, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* AMOMAX.W：原子有符号最大值。 */
CONSTOPT(amomaxw, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* AMOMINU.W */
CONSTOPT(amominuw, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* AMOMAXU.W */
CONSTOPT(amomaxuw, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})
#endif /* RV32_HAS(EXT_A) */

/* RV32F 标准扩展。 */

#if RV32_HAS(EXT_F)
/* FLW */
CONSTOPT(flw, {})

/* FSW */
CONSTOPT(fsw, {})

/* FMADD.S */
CONSTOPT(fmadds, {})

/* FMSUB.S */
CONSTOPT(fmsubs, {})

/* FNMSUB.S */
CONSTOPT(fnmsubs, {})

/* FNMADD.S */
CONSTOPT(fnmadds, {})

/* FADD.S */
CONSTOPT(fadds, {})

/* FSUB.S */
CONSTOPT(fsubs, {})

/* FMUL.S */
CONSTOPT(fmuls, {})

/* FDIV.S */
CONSTOPT(fdivs, {})

/* FSQRT.S */
CONSTOPT(fsqrts, {})

/* FSGNJ.S */
CONSTOPT(fsgnjs, {})

/* FSGNJN.S */
CONSTOPT(fsgnjns, {})

/* FSGNJX.S */
CONSTOPT(fsgnjxs, {})

/* FMIN.S。
 * IEEE754-201x 中，fmin(x, y) 的返回规则为：
 * - 两个输入都不是 NaN 时返回 min(x, y)。
 * - 一个是 NaN、另一个是数值时返回该数值。
 * - 两个都是 NaN 时返回 NaN。
 * 输入为 signaling NaN 时设置 invalid operation。
 */
CONSTOPT(fmins, {})

/* FMAX.S */
CONSTOPT(fmaxs, {})

/* FCVT.W.S 和 FCVT.WU.S 把浮点数转换为整数，舍入模式由 rm 字段指定。
 */

/* FCVT.W.S */
CONSTOPT(fcvtws, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* FCVT.WU.S */
CONSTOPT(fcvtwus, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* FMV.X.W */
CONSTOPT(fmvxw, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* FEQ.S 执行静默比较：只有任一输入为信号 NaN 时才设置 invalid operation 异常标志。
 */
CONSTOPT(feqs, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* FLT.S 和 FLE.S 执行 IEEE 754-2008 所说的信号比较：任一输入为 NaN 时设置
 * invalid operation 异常标志。
 */
CONSTOPT(flts, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

CONSTOPT(fles, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* FCLASS.S */
CONSTOPT(fclasss, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* FCVT.S.W */
CONSTOPT(fcvtsw, {})

/* FCVT.S.WU */
CONSTOPT(fcvtswu, {})

/* FMV.W.X */
CONSTOPT(fmvwx, {})
#endif

/* RV32C 标准扩展。 */

#if RV32_HAS(EXT_C)
/* C.ADDI4SPN 是 CIW 格式指令，把零扩展且非零、按 4 缩放的立即数加到栈指针
 * x2，并把结果写入 rd'。它常用于生成栈上变量指针，展开为
 * addi rd', x2, nzuimm[9:2]。
 */
CONSTOPT(caddi4spn, {
    if (info->is_constant[rv_reg_sp]) {
        info->is_constant[ir->rd] = true;
        ir->imm = info->const_val[rv_reg_sp] + (uint16_t) ir->imm;
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_clui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* C.LW 从内存加载 32 位值到 rd'。有效地址为 rs1' 加上零扩展且按 4 缩放的偏移，
 * 展开为 lw rd', offset[6:2](rs1')。
 */
CONSTOPT(clw, { info->is_constant[ir->rd] = false; })

/* C.SW 把 rs2' 中的 32 位值存入内存。有效地址为 rs1' 加上零扩展且按 4 缩放的
 * 偏移，展开为 sw rs2', offset[6:2](rs1')。
 */
CONSTOPT(csw, {})

/* C.NOP */
CONSTOPT(cnop, {})

/* C.ADDI 将非零、符号扩展的 6 位立即数加到 rd，并写回 rd；展开为
 * addi rd, rd, nzimm[5:0]。rd=x0 且 nzimm=0 编码为 C.NOP，其余 rd=x0 或
 * nzimm=0 的编码为 HINT。
 */
CONSTOPT(caddi, {
    if (info->is_constant[ir->rd]) {
        ir->imm = info->const_val[ir->rd] + (int16_t) ir->imm;
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_clui;
        ir->impl = dispatch_table[ir->opcode];
    }
})

/* C.JAL */
CONSTOPT(cjal, {
    info->is_constant[rv_reg_ra] = true;
    info->const_val[rv_reg_ra] = ir->pc + 2;
})

/* C.LI 将符号扩展的 6 位立即数加载到 rd，展开为 addi rd, x0, imm[5:0]。 */
 */
CONSTOPT(cli, {
    info->is_constant[ir->rd] = true;
    info->const_val[ir->rd] = ir->imm;
})

/* C.ADDI16SP 用于在过程序言/尾声中调整栈指针，展开为
 * addi x2, x2, nzimm[9:4]。nzimm=0 的编码保留。
 */
CONSTOPT(caddi16sp, {
    if (info->is_constant[ir->rd]) {
        ir->imm = info->const_val[ir->rd] + ir->imm;
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_clui;
        ir->impl = dispatch_table[ir->opcode];
    }
})

/* C.LUI 把非零 6 位立即数字段加载到目标寄存器 17-12 位，清零低 12 位，并将
 * 第 17 位符号扩展到高位；展开为 lui rd, nzimm[17:12]。
 */
CONSTOPT(clui, {
    info->is_constant[ir->rd] = true;
    info->const_val[ir->rd] = ir->imm;
})

/* C.SRLI 是 CB 格式指令，对 rd' 做逻辑右移并写回 rd'；移位量编码在 shamt 中，
 * 展开为 srli rd', rd', shamt[5:0]。
 */
CONSTOPT(csrli, {
    if (info->is_constant[ir->rs1]) {
        ir->imm = info->const_val[ir->rs1] >> ir->shamt;
        info->const_val[ir->rs1] = ir->imm;
        ir->rd = ir->rs1;
        ir->opcode = rv_insn_clui;
        ir->impl = dispatch_table[ir->opcode];
    }
})

/* C.SRAI 与 C.SRLI 类似，但执行算术右移；展开为 srai rd', rd', shamt[5:0]。
 */
CONSTOPT(csrai, {
    if (info->is_constant[ir->rs1]) {
        const uint32_t mask = 0x80000000 & info->const_val[ir->rs1];
        ir->imm = info->const_val[ir->rs1] >> ir->shamt;
        for (unsigned int i = 0; i < ir->shamt; ++i)
            ir->imm |= mask >> i;
        info->const_val[ir->rs1] = ir->imm;
        ir->rd = ir->rs1;
        ir->opcode = rv_insn_clui;
        ir->impl = dispatch_table[ir->opcode];
    }
})

/* C.ANDI 是 CB 格式指令，对 rd' 与符号扩展 6 位立即数做按位与并写回 rd'；
 * 展开为 andi rd', rd', imm[5:0]。
 */
CONSTOPT(candi, {
    if (info->is_constant[ir->rs1]) {
        ir->imm = info->const_val[ir->rs1] & ir->imm;
        info->const_val[ir->rs1] = ir->imm;
        ir->rd = ir->rs1;
        ir->opcode = rv_insn_clui;
        ir->impl = dispatch_table[ir->opcode];
    }
})

/* C.SUB */
CONSTOPT(csub, {
    if (info->is_constant[ir->rs1] && info->is_constant[ir->rs2]) {
        info->is_constant[ir->rd] = true;
        ir->imm = info->const_val[ir->rs1] - info->const_val[ir->rs2];
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_clui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* C.XOR */
CONSTOPT(cxor, {
    if (info->is_constant[ir->rs1] && info->is_constant[ir->rs2]) {
        info->is_constant[ir->rd] = true;
        ir->imm = info->const_val[ir->rs1] ^ info->const_val[ir->rs2];
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_clui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

CONSTOPT(cor, {
    if (info->is_constant[ir->rs1] && info->is_constant[ir->rs2]) {
        info->is_constant[ir->rd] = true;
        ir->imm = info->const_val[ir->rs1] | info->const_val[ir->rs2];
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_clui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

CONSTOPT(cand, {
    if (info->is_constant[ir->rs1] && info->is_constant[ir->rs2]) {
        info->is_constant[ir->rd] = true;
        ir->imm = info->const_val[ir->rs1] & info->const_val[ir->rs2];
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_clui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* C.J 执行无条件控制转移。偏移经符号扩展后加到 PC，形成跳转目标；范围为
 * +/-2 KiB，展开为 jal x0, offset[11:1]。
 */
CONSTOPT(cj, {})

/* C.BEQZ 执行条件转移。偏移经符号扩展后加到 PC，范围为 +/-256 B；当 rs1' 为 0
 * 时分支，展开为 beq rs1', x0, offset[8:1]。
 */
CONSTOPT(cbeqz, {
    if (info->is_constant[ir->rs1]) {
        if (info->const_val[ir->rs1])
            ir->imm = 2;
        ir->opcode = rv_insn_cj;
        ir->impl = dispatch_table[ir->opcode];
    }
})

/* C.BEQZ */
CONSTOPT(cbnez, {
    if (info->is_constant[ir->rs1]) {
        if (!info->const_val[ir->rs1])
            ir->imm = 2;
        ir->opcode = rv_insn_cj;
        ir->impl = dispatch_table[ir->opcode];
    }
})

/* C.SLLI 是 CI 格式指令，对 rd 做逻辑左移并写回 rd；移位量编码在 shamt 字段，
 * 展开为 slli rd, rd, shamt[5:0]。
 */
CONSTOPT(cslli, {
    if (info->is_constant[ir->rd]) {
        ir->imm = info->const_val[ir->rd] << (uint8_t) ir->imm;
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_clui;
        ir->impl = dispatch_table[ir->opcode];
    }
})

/* C.LWSP */
CONSTOPT(clwsp, { info->is_constant[ir->rd] = false; })

/* C.JR */
CONSTOPT(cjr, {})

/* C.MV */
CONSTOPT(cmv, {
    if (info->is_constant[ir->rs2]) {
        info->is_constant[ir->rd] = true;
        ir->imm = info->const_val[ir->rs2];
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_clui;
        ir->impl = dispatch_table[ir->opcode];
    } else {
        info->is_constant[ir->rd] = false;
    }
})

/* C.EBREAK */
CONSTOPT(cebreak, {})

/* C.JALR */
CONSTOPT(cjalr, {
    info->is_constant[rv_reg_ra] = true;
    info->const_val[ir->rd] = ir->pc + 2;
})

/* C.ADD 将 rd 和 rs2 相加并写回 rd，展开为 add rd, rd, rs2。 */
 */
CONSTOPT(cadd, {
    if (info->is_constant[ir->rs1] && info->is_constant[ir->rs2]) {
        info->is_constant[ir->rd] = true;
        ir->imm = info->const_val[ir->rs1] + info->const_val[ir->rs2];
        info->const_val[ir->rd] = ir->imm;
        ir->opcode = rv_insn_clui;
        ir->impl = dispatch_table[ir->opcode];
    } else
        info->is_constant[ir->rd] = false;
})

/* C.SWSP */
CONSTOPT(cswsp, {})
#endif

/* RV32FC 标准扩展。 */

#if RV32_HAS(EXT_F) && RV32_HAS(EXT_C)
/* C.FLWSP */
CONSTOPT(cflwsp, {})

/* C.FSWSP */
CONSTOPT(cfswsp, {})

/* C.FLW */
CONSTOPT(cflw, {})

/* C.FSW */
CONSTOPT(cfsw, {})
#endif

#if RV32_HAS(Zba)
/* SH1ADD */
CONSTOPT(sh1add, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* SH2ADD */
CONSTOPT(sh2add, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* SH3ADD */
CONSTOPT(sh3add, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})
#endif

#if RV32_HAS(Zbb)
/* ANDN */
CONSTOPT(andn, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* ORN */
CONSTOPT(orn, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* XNOR */
CONSTOPT(xnor, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* CLZ */
CONSTOPT(clz, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* CTZ */
CONSTOPT(ctz, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* CPOP */
CONSTOPT(cpop, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* MAX */
CONSTOPT(max, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* MAXU */
CONSTOPT(maxu, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* MIN */
CONSTOPT(min, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* MINU */
CONSTOPT(minu, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* SEXT.B */
CONSTOPT(sextb, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* SEXT.H */
CONSTOPT(sexth, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* ZEXT.H */
CONSTOPT(zexth, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* ROL */
CONSTOPT(rol, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* ROR */
CONSTOPT(ror, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* RORI */
CONSTOPT(rori, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* ORCB */
CONSTOPT(orcb, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* REV8 */
CONSTOPT(rev8, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})
#endif

#if RV32_HAS(Zbc)
/* CLMUL */
CONSTOPT(clmul, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* CLMULH */
CONSTOPT(clmulh, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* CLMULR */
CONSTOPT(clmulr, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})
#endif

#if RV32_HAS(Zbs)
/* BCLR */
CONSTOPT(bclr, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* BCLRI */
CONSTOPT(bclri, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* BEXT */
CONSTOPT(bext, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* BEXTI */
CONSTOPT(bexti, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* BINV */
CONSTOPT(binv, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* BINVI */
CONSTOPT(binvi, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* BSET */
CONSTOPT(bset, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

/* BSETI */
CONSTOPT(bseti, {
    if (ir->rd)
        info->is_constant[ir->rd] = false;
})

#endif
