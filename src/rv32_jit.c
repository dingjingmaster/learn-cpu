/*
 * RISC-V 指令一级 JIT 代码生成器。
 *
 * 本文件包含 RISC-V 指令到宿主机器码的生成处理器，支持 x86-64 和 Arm64。
 *
 * 架构概览：
 * - GEN(name, { body })：定义某条 RISC-V 指令的宿主机器码生成器。
 * - 寄存器分配：把 RISC-V 寄存器 X[rd] 映射到 vm_reg[0..2] 等宿主寄存器，
 *   淘汰策略基于后续活跃距离。
 * - 手工维护：处理器为宿主性能单独优化，但语义仍需和解释器保持一致。
 *
 * 关键寄存器：
 * - vm_reg[0..2]：分配给 VM 寄存器操作的宿主寄存器
 * - temp_reg：中间计算使用的临时寄存器
 * - parameter_reg[0]：指向 riscv_t 结构
 *
 * 代码生成（emit_*）API：
 * ---------------------------------------------------------------------------
 * 函数                    | 说明
 * ---------------------------------------------------------------------------
 * emit_alu32/64           | 发射算术/逻辑操作（ADD、SUB、XOR、OR、AND）。
 * emit_alu32_imm32/8      | 发射带立即数的 ALU 操作。
 * emit_load/store         | 发射内存访问，并支持 MMIO/系统模式。
 * emit_load_sext          | 发射带符号扩展的内存加载（LB、LH）。
 * emit_cmp32/imm32        | 发射分支/SLT 使用的比较逻辑。
 * emit_jcc_offset         | 发射条件跳转，使用 JCC_* 标识符。
 * emit_jmp                | 发射跳转到目标 PC 的无条件跳转。
 * emit_exit               | 发射从 JIT 执行返回的尾声代码。
 * ---------------------------------------------------------------------------
 *
 * 代码生成宏：
 * 辅助宏用于减少常见指令形态的重复实现：
 * - GEN_BRANCH：条件分支指令（beq、bne、blt 等）
 * - GEN_CBRANCH：压缩分支指令（cbeqz、cbnez）
 * - GEN_ALU_IMM：带立即数的 ALU 指令（addi、xori、ori、andi）
 * - GEN_ALU_REG：寄存器 ALU 指令（add、sub、xor、or、and）
 * - GEN_SHIFT_IMM：立即数移位（slli、srli、srai）
 * - GEN_SHIFT_REG：寄存器移位（sll、srl、sra）
 * - GEN_SLT_IMM：立即数 set-less-than（slti、sltiu）
 * - GEN_SLT_REG：寄存器 set-less-than（slt、sltu）
 * - GEN_LOAD：带 MMIO 支持的内存加载（lb、lh、lw、lbu、lhu）
 * - GEN_STORE：带 MMIO 支持的内存存储（sb、sh、sw）
 *
 * 宿主抽象层：
 * emit_* API 用 x86-64 位模式（例如 JCC_JE=0x84、ALU_OP_ADD=0x01）作为跨宿主
 * 符号标识，src/jit.c 中的后端再把这些标识映射为 Arm64 或 x86 原生指令。
 *
 * 内存访问模式：
 * 处理器通过 IIF(RV32_HAS(SYSTEM_MMIO)) 在直接 RAM 访问（用户模式）和 JIT MMU
 * handler 路径（系统模式）之间切换。
 *
 * 对应解释器语义见 rv32_template.c。
 */

/* 分支尾声辅助宏：发射 fall-through 和 taken 两条路径。
 * 普通 4 字节分支和压缩 2 字节分支都使用它。
 */
#define EMIT_BRANCH_EPILOGUE(inst_size)                            \
    do {                                                           \
        if (ir->branch_untaken) {                                  \
            emit_jmp(state, ir->pc + (inst_size), rv->csr_satp);   \
        }                                                          \
        emit_load_imm(state, temp_reg, ir->pc + (inst_size));      \
        emit_store(state, S32, temp_reg, parameter_reg[0],         \
                   offsetof(riscv_t, PC));                         \
        emit_exit(state);                                          \
        emit_jump_target_offset(state, JUMP_LOC_0, state->offset); \
        if (ir->branch_taken) {                                    \
            emit_jmp(state, ir->pc + ir->imm, rv->csr_satp);       \
        }                                                          \
        emit_load_imm(state, temp_reg, ir->pc + ir->imm);          \
        emit_store(state, S32, temp_reg, parameter_reg[0],         \
                   offsetof(riscv_t, PC));                         \
        emit_exit(state);                                          \
    } while (0)

/* 分支指令处理器宏：所有分支指令形态相同，只是条件码不同。
 */
#define GEN_BRANCH(inst, cond)                            \
    GEN(inst, {                                           \
        ra_load2(state, ir->rs1, ir->rs2);                \
        emit_cmp32(state, vm_reg[1], vm_reg[0]);          \
        store_back(state);                                \
        uint32_t jump_loc_0 = state->offset;              \
        emit_jcc_offset(state, cond);                     \
        EMIT_BRANCH_EPILOGUE(4); /* 4 字节指令。 */       \
    })

/* 压缩分支指令处理器宏：把 rs1 与 0 比较。
 * 压缩指令长度为 2 字节，因此使用 pc+2 而不是 pc+4。
 */
#define GEN_CBRANCH(inst, cond)                           \
    GEN(inst, {                                           \
        vm_reg[0] = ra_load(state, ir->rs1);              \
        emit_cmp_imm32(state, vm_reg[0], 0);              \
        store_back(state);                                \
        uint32_t jump_loc_0 = state->offset;              \
        emit_jcc_offset(state, cond);                     \
        EMIT_BRANCH_EPILOGUE(2); /* 2 字节指令。 */       \
    })

/* 立即数操作数使用的 Group 1 ALU opcode（x86-64 编码）。
 * 在 Arm64 上该 opcode 会被忽略，具体操作由 ALU_* 选择器决定。
 */
#define ALU_GRP1_OPCODE 0x81

/* Group 1 操作使用的 ALU 操作选择器。
 * 在 x86-64 上，它们是 ModR/M reg 字段值；在 Arm64 上，它们是
 * emit_alu32_imm32() 中映射到原生指令的 switch case 选择器。
 */
#define ALU_ADD 0
#define ALU_OR 1
#define ALU_AND 4
#define ALU_XOR 6

/* ALU 立即数指令处理器宏。 */
#define GEN_ALU_IMM(inst, op)                                             \
    GEN(inst, {                                                           \
        vm_reg[0] = ra_load(state, ir->rs1);                              \
        vm_reg[1] = map_vm_reg_reserved(state, ir->rd, vm_reg[0]);        \
        if (vm_reg[0] != vm_reg[1]) {                                     \
            emit_mov(state, vm_reg[0], vm_reg[1]);                        \
        }                                                                 \
        emit_alu32_imm32(state, ALU_GRP1_OPCODE, op, vm_reg[1], ir->imm); \
    })

/* 移位操作标识符。
 * 取值匹配 x86-64 ModR/M reg 字段，并在两种宿主架构上复用。
 */
#define SHIFT_SHL 4
#define SHIFT_SHR 5
#define SHIFT_SAR 7

/* 移位 opcode（x86-64 编码）。
 * 在 Arm64 上，emit_alu32_imm8() 和 emit_alu32() 会把 SHIFT_* 值映射为原生
 * 指令。
 */
#define SHIFT_IMM_OPCODE 0xc1 /* 按立即数移位。 */
#define SHIFT_REG_OPCODE 0xd3 /* 按寄存器移位。 */

/* RV32 移位量掩码：只使用低 5 位。 */
#define RV32_SHIFT_MASK 0x1f

/* 立即数移位指令处理器宏。 */
#define GEN_SHIFT_IMM(inst, op)                                    \
    GEN(inst, {                                                    \
        vm_reg[0] = ra_load(state, ir->rs1);                       \
        vm_reg[1] = map_vm_reg_reserved(state, ir->rd, vm_reg[0]); \
        if (vm_reg[0] != vm_reg[1]) {                              \
            emit_mov(state, vm_reg[0], vm_reg[1]);                 \
        }                                                          \
        emit_alu32_imm8(state, SHIFT_IMM_OPCODE, op, vm_reg[1],    \
                        ir->imm & RV32_SHIFT_MASK);                \
    })

/* 寄存器到寄存器 ALU 操作的 opcode（x86-64 编码）。
 * 在 Arm64 上，emit_alu32() 会把它们映射为等价原生指令。
 */
#define ALU_OP_ADD 0x01
#define ALU_OP_SUB 0x29
#define ALU_OP_XOR 0x31
#define ALU_OP_OR 0x09
#define ALU_OP_AND 0x21

/* ALU 寄存器指令处理器宏。 */
#define GEN_ALU_REG(inst, op)                                                  \
    GEN(inst, {                                                                \
        ra_load2(state, ir->rs1, ir->rs2);                                     \
        vm_reg[2] = map_vm_reg_reserved2(state, ir->rd, vm_reg[0], vm_reg[1]); \
        emit_mov(state, vm_reg[1], temp_reg);                                  \
        emit_mov(state, vm_reg[0], vm_reg[2]);                                 \
        emit_alu32(state, op, temp_reg, vm_reg[2]);                            \
    })

/* 寄存器移位指令处理器宏。 */
#define GEN_SHIFT_REG(inst, op)                                                \
    GEN(inst, {                                                                \
        ra_load2(state, ir->rs1, ir->rs2);                                     \
        vm_reg[2] = map_vm_reg_reserved2(state, ir->rd, vm_reg[0], vm_reg[1]); \
        emit_mov(state, vm_reg[1], temp_reg);                                  \
        emit_mov(state, vm_reg[0], vm_reg[2]);                                 \
        emit_alu32_imm32(state, ALU_GRP1_OPCODE, ALU_AND, temp_reg,            \
                         RV32_SHIFT_MASK);                                     \
        emit_alu32(state, SHIFT_REG_OPCODE, op, vm_reg[2]);                    \
    })

/* 立即数 set-less-than 指令处理器宏（slti/sltiu）。 */
#define GEN_SLT_IMM(inst, cond)                                    \
    GEN(inst, {                                                    \
        vm_reg[0] = ra_load(state, ir->rs1);                       \
        emit_cmp_imm32(state, vm_reg[0], ir->imm);                 \
        vm_reg[1] = map_vm_reg_reserved(state, ir->rd, vm_reg[0]); \
        emit_load_imm(state, vm_reg[1], 1);                        \
        uint32_t jump_loc_0 = state->offset;                       \
        emit_jcc_offset(state, cond);                              \
        emit_load_imm(state, vm_reg[1], 0);                        \
        emit_jump_target_offset(state, JUMP_LOC_0, state->offset); \
    })

/* 寄存器 set-less-than 指令处理器宏（slt/sltu）。 */
#define GEN_SLT_REG(inst, cond)                                                \
    GEN(inst, {                                                                \
        ra_load2(state, ir->rs1, ir->rs2);                                     \
        vm_reg[2] = map_vm_reg_reserved2(state, ir->rd, vm_reg[0], vm_reg[1]); \
        emit_cmp32(state, vm_reg[1], vm_reg[0]);                               \
        emit_load_imm(state, vm_reg[2], 1);                                    \
        uint32_t jump_loc_0 = state->offset;                                   \
        emit_jcc_offset(state, cond);                                          \
        emit_load_imm(state, vm_reg[2], 0);                                    \
        emit_jump_target_offset(state, JUMP_LOC_0, state->offset);             \
    })

/* 加载指令处理器宏：启用 SYSTEM_MMIO 时处理 MMIO 路径。
 * 参数：
 *   inst：指令名（lb、lh、lw、lbu、lhu）
 *   insn_type：传给 MMIO handler 的 rv_insn_* 常量
 *   size：内存访问大小（S8、S16、S32）
 *   load_fn：emit_load 或 emit_load_sext
 */
#define GEN_LOAD(inst, insn_type, size, load_fn)                              \
    GEN(inst, {                                                               \
        memory_t *m = PRIV(rv)->mem;                                          \
        vm_reg[0] = ra_load(state, ir->rs1);                                  \
        IIF(RV32_HAS(SYSTEM_MMIO))(                                           \
            {                                                                 \
                emit_load_imm_sext(state, temp_reg, ir->imm);                 \
                emit_alu32(state, ALU_OP_ADD, vm_reg[0], temp_reg);           \
                emit_store(state, S32, temp_reg, parameter_reg[0],            \
                           offsetof(riscv_t, jit_mmu.vaddr));                 \
                emit_load_imm(state, temp_reg, insn_type);                    \
                emit_store(state, S32, temp_reg, parameter_reg[0],            \
                           offsetof(riscv_t, jit_mmu.type));                  \
                /* 保存指令 PC，作为 trap 返回地址。 */                       \
                emit_load_imm(state, temp_reg, ir->pc);                       \
                emit_store(state, S32, temp_reg, parameter_reg[0],            \
                           offsetof(riscv_t, jit_mmu.pc));                    \
                                                                              \
                store_back(state);                                            \
                emit_jit_mmu_handler(state, ir->rd);                          \
                reset_reg();                                                  \
                                                                              \
                /* 检查是否发生 trap；若已陷入，则跳过加载。 */              \
                emit_load(state, S8, parameter_reg[0], temp_reg,              \
                          offsetof(riscv_t, is_trapped));                     \
                emit_cmp_imm32(state, temp_reg, 0);                           \
                uint32_t jump_trap = state->offset;                           \
                emit_jcc_offset(state, JCC_JNE);                              \
                                                                              \
                /* 若为 MMIO，从 X[rd] 取值；否则从内存加载。 */             \
                emit_load(state, S8, parameter_reg[0], temp_reg,              \
                          offsetof(riscv_t, jit_mmu.is_mmio));                \
                emit_cmp_imm32(state, temp_reg, 0);                           \
                vm_reg[1] = map_vm_reg(state, ir->rd);                        \
                uint32_t jump_loc_0 = state->offset;                          \
                emit_jcc_offset(state, JCC_JE);                               \
                                                                              \
                emit_load(state, S32, parameter_reg[0], vm_reg[1],            \
                          offsetof(riscv_t, X) + 4 * ir->rd);                 \
                uint32_t jump_loc_1 = state->offset;                          \
                emit_jcc_offset(state, JCC_JMP);                              \
                                                                              \
                emit_jump_target_offset(state, JUMP_LOC_0, state->offset);    \
                emit_load(state, S32, parameter_reg[0], temp_reg,             \
                          offsetof(riscv_t, jit_mmu.paddr));                  \
                emit_load_imm_sext(state, vm_reg[1], (intptr_t) m->mem_base); \
                emit_alu64(state, ALU_OP_ADD, temp_reg, vm_reg[1]);           \
                load_fn(state, size, vm_reg[1], vm_reg[1], 0);                \
                emit_jump_target_offset(state, JUMP_LOC_1, state->offset);    \
                /* 跳过 trap 退出点，继续正常路径。 */                       \
                uint32_t jump_normal = state->offset;                         \
                emit_jcc_offset(state, JCC_JMP);                              \
                /* trap 退出点：离开 JIT 基本块，交给 trap 处理。 */          \
                emit_jump_target_offset(state, JUMP_TRAP, state->offset);     \
                emit_exit(state);                                             \
                /* 正常续执行点。 */                                         \
                emit_jump_target_offset(state, JUMP_NORMAL, state->offset);   \
            },                                                                \
            {                                                                 \
                emit_load_imm_sext(state, temp_reg,                           \
                                   (intptr_t) (m->mem_base + ir->imm));       \
                emit_alu64(state, ALU_OP_ADD, vm_reg[0], temp_reg);           \
                vm_reg[1] = map_vm_reg(state, ir->rd);                        \
                load_fn(state, size, temp_reg, vm_reg[1], 0);                 \
            })                                                                \
    })

/* 存储指令处理器宏：启用 SYSTEM_MMIO 时处理 MMIO 路径。
 * 参数：
 *   inst：指令名（sb、sh、sw）
 *   insn_type：传给 MMIO handler 的 rv_insn_* 常量
 *   size：内存访问大小（S8、S16、S32）
 */
#define GEN_STORE(inst, insn_type, size)                                      \
    GEN(inst, {                                                               \
        memory_t *m = PRIV(rv)->mem;                                          \
        vm_reg[0] = ra_load(state, ir->rs1);                                  \
        IIF(RV32_HAS(SYSTEM_MMIO))(                                           \
            {                                                                 \
                emit_load_imm_sext(state, temp_reg, ir->imm);                 \
                emit_alu32(state, ALU_OP_ADD, vm_reg[0], temp_reg);           \
                emit_store(state, S32, temp_reg, parameter_reg[0],            \
                           offsetof(riscv_t, jit_mmu.vaddr));                 \
                emit_load_imm(state, temp_reg, insn_type);                    \
                emit_store(state, S32, temp_reg, parameter_reg[0],            \
                           offsetof(riscv_t, jit_mmu.type));                  \
                /* 保存指令 PC，作为 trap 返回地址。 */                       \
                emit_load_imm(state, temp_reg, ir->pc);                       \
                emit_store(state, S32, temp_reg, parameter_reg[0],            \
                           offsetof(riscv_t, jit_mmu.pc));                    \
                store_back(state);                                            \
                emit_jit_mmu_handler(state, ir->rs2);                         \
                reset_reg();                                                  \
                                                                              \
                /* 检查是否发生 trap；若已陷入，则跳过存储。 */              \
                emit_load(state, S8, parameter_reg[0], temp_reg,              \
                          offsetof(riscv_t, is_trapped));                     \
                emit_cmp_imm32(state, temp_reg, 0);                           \
                uint32_t jump_trap = state->offset;                           \
                emit_jcc_offset(state, JCC_JNE);                              \
                                                                              \
                /* 若为 MMIO，跳过这里的存储，已经由 MMIO handler 处理。 */   \
                emit_load(state, S8, parameter_reg[0], temp_reg,              \
                          offsetof(riscv_t, jit_mmu.is_mmio));                \
                emit_cmp_imm32(state, temp_reg, 1);                           \
                uint32_t jump_loc_0 = state->offset;                          \
                emit_jcc_offset(state, JCC_JE);                               \
                                                                              \
                /* 先加载 rs2 值，再计算地址，避免寄存器分配冲突。           \
                 * ra_load 可能淘汰任意已分配寄存器，因此先加载 rs2，之后    \
                 * 使用 temp_reg 保存地址；temp_reg 是保留寄存器，不会被淘汰。\
                 */                                                           \
                vm_reg[1] = ra_load(state, ir->rs2);                          \
                emit_load(state, S32, parameter_reg[0], temp_reg,             \
                          offsetof(riscv_t, jit_mmu.paddr));                  \
                vm_reg[0] = map_vm_reg(state, rv_reg_zero);                   \
                emit_load_imm_sext(state, vm_reg[0], (intptr_t) m->mem_base); \
                emit_alu64(state, ALU_OP_ADD, vm_reg[0], temp_reg);           \
                emit_store(state, size, vm_reg[1], temp_reg, 0);              \
                emit_jump_target_offset(state, JUMP_LOC_0, state->offset);    \
                /* 跳过 trap 退出点，继续正常路径。 */                       \
                uint32_t jump_normal = state->offset;                         \
                emit_jcc_offset(state, JCC_JMP);                              \
                /* trap 退出点：离开 JIT 基本块，交给 trap 处理。 */          \
                emit_jump_target_offset(state, JUMP_TRAP, state->offset);     \
                emit_exit(state);                                             \
                /* 正常续执行点。 */                                         \
                emit_jump_target_offset(state, JUMP_NORMAL, state->offset);   \
                reset_reg();                                                  \
            },                                                                \
            {                                                                 \
                emit_load_imm_sext(state, temp_reg,                           \
                                   (intptr_t) (m->mem_base + ir->imm));       \
                emit_alu64(state, ALU_OP_ADD, vm_reg[0], temp_reg);           \
                vm_reg[1] = ra_load(state, ir->rs2);                          \
                emit_store(state, size, vm_reg[1], temp_reg, 0);              \
            })                                                                \
    })

GEN(nop, {})
GEN(lui, {
    vm_reg[0] = map_vm_reg(state, ir->rd);
    emit_load_imm(state, vm_reg[0], ir->imm);
})
GEN(auipc, {
    vm_reg[0] = map_vm_reg(state, ir->rd);
    emit_load_imm(state, vm_reg[0], ir->pc + ir->imm);
})
GEN(jal, {
    if (ir->rd) {
        vm_reg[0] = map_vm_reg(state, ir->rd);
        emit_load_imm(state, vm_reg[0], ir->pc + 4);
    }
    store_back(state);
    emit_jmp(state, ir->pc + ir->imm, rv->csr_satp);
    emit_load_imm(state, temp_reg, ir->pc + ir->imm);
    emit_store(state, S32, temp_reg, parameter_reg[0], offsetof(riscv_t, PC));
    emit_exit(state);
})
GEN(jalr, {
    vm_reg[0] = ra_load(state, ir->rs1);
    emit_mov(state, vm_reg[0], temp_reg);
    emit_alu32_imm32(state, ALU_GRP1_OPCODE, ALU_ADD, temp_reg, ir->imm);
    /* RISC-V 规范要求：目标地址最低位总是清零。 */
    emit_alu32_imm32(state, ALU_GRP1_OPCODE, ALU_AND, temp_reg, ~1U);
    if (ir->rd) {
        vm_reg[1] = map_vm_reg(state, ir->rd);
        emit_load_imm(state, vm_reg[1], ir->pc + 4);
    }
    store_back(state);
    parse_branch_history_table(state, rv, ir);
    emit_store(state, S32, temp_reg, parameter_reg[0], offsetof(riscv_t, PC));
    emit_exit(state);
})
/* RV32I 分支指令。 */
GEN_BRANCH(beq, JCC_JE)
GEN_BRANCH(bne, JCC_JNE)
GEN_BRANCH(blt, JCC_JL)
GEN_BRANCH(bge, JCC_JGE)
GEN_BRANCH(bltu, JCC_JB)
GEN_BRANCH(bgeu, JCC_JAE)
/* RV32I 加载指令。 */
GEN_LOAD(lb, rv_insn_lb, S8, emit_load_sext)
GEN_LOAD(lh, rv_insn_lh, S16, emit_load_sext)
GEN_LOAD(lw, rv_insn_lw, S32, emit_load)
GEN_LOAD(lbu, rv_insn_lbu, S8, emit_load)
GEN_LOAD(lhu, rv_insn_lhu, S16, emit_load)
/* RV32I 存储指令。 */
GEN_STORE(sb, rv_insn_sb, S8)
GEN_STORE(sh, rv_insn_sh, S16)
GEN_STORE(sw, rv_insn_sw, S32)
/* RV32I ALU 立即数指令。 */
GEN_ALU_IMM(addi, ALU_ADD)
GEN_SLT_IMM(slti, JCC_JL)
GEN_SLT_IMM(sltiu, JCC_JB)
GEN_ALU_IMM(xori, ALU_XOR)
GEN_ALU_IMM(ori, ALU_OR)
GEN_ALU_IMM(andi, ALU_AND)
/* RV32I 立即数移位指令。 */
GEN_SHIFT_IMM(slli, SHIFT_SHL)
GEN_SHIFT_IMM(srli, SHIFT_SHR)
GEN_SHIFT_IMM(srai, SHIFT_SAR)
/* RV32I ALU 寄存器指令。 */
GEN_ALU_REG(add, ALU_OP_ADD)
GEN_ALU_REG(sub, ALU_OP_SUB)
/* RV32I 寄存器移位指令。 */
GEN_SHIFT_REG(sll, SHIFT_SHL)
GEN_SLT_REG(slt, JCC_JL)
GEN_SLT_REG(sltu, JCC_JB)
GEN_ALU_REG(xor, ALU_OP_XOR)
GEN_SHIFT_REG(srl, SHIFT_SHR)
GEN_SHIFT_REG(sra, SHIFT_SAR)
GEN_ALU_REG(or, ALU_OP_OR)
GEN_ALU_REG(and, ALU_OP_AND)
GEN(fence, { assert(NULL); })
GEN(ecall, {
    store_back(state);
    emit_load_imm(state, temp_reg, ir->pc);
    emit_store(state, S32, temp_reg, parameter_reg[0], offsetof(riscv_t, PC));
    emit_call(state, (intptr_t) rv->io.on_ecall);
    emit_exit(state);
})
GEN(ebreak, {
    store_back(state);
    emit_load_imm(state, temp_reg, ir->pc);
    emit_store(state, S32, temp_reg, parameter_reg[0], offsetof(riscv_t, PC));
    emit_call(state, (intptr_t) rv->io.on_ebreak);
    emit_exit(state);
})
GEN(wfi, { assert(NULL); })
GEN(uret, { assert(NULL); })
#if RV32_HAS(SYSTEM)
GEN(sret, { assert(NULL); })
#endif
GEN(hret, { assert(NULL); })
GEN(mret, { assert(NULL); })
GEN(sfencevma, { assert(NULL); })
#if RV32_HAS(Zifencei) /* RV32 Zifencei 标准扩展。 */
GEN(fencei, { assert(NULL); })
#endif
#if RV32_HAS(Zicsr) /* RV32 Zicsr 标准扩展。 */
GEN(csrrw, { assert(NULL); })
GEN(csrrs, { assert(NULL); })
GEN(csrrc, { assert(NULL); })
GEN(csrrwi, { assert(NULL); })
GEN(csrrsi, { assert(NULL); })
GEN(csrrci, { assert(NULL); })
#endif
#if RV32_HAS(EXT_M)
GEN(mul, {
    ra_load2(state, ir->rs1, ir->rs2);
    vm_reg[2] = map_vm_reg_reserved2(state, ir->rd, vm_reg[0], vm_reg[1]);
    emit_mov(state, vm_reg[1], temp_reg);
    emit_mov(state, vm_reg[0], vm_reg[2]);
    muldivmod(state, 0x28, temp_reg, vm_reg[2], 0);
})
GEN(mulh, {
    ra_load2_sext(state, ir->rs1, ir->rs2, true, true);
    vm_reg[2] = map_vm_reg_reserved2(state, ir->rd, vm_reg[0], vm_reg[1]);
    emit_mov(state, vm_reg[1], temp_reg);
    emit_mov(state, vm_reg[0], vm_reg[2]);
    muldivmod(state, 0x2f, temp_reg, vm_reg[2], 0);
    emit_alu64_imm8(state, SHIFT_IMM_OPCODE, SHIFT_SHR, vm_reg[2], 32);
})
GEN(mulhsu, {
    ra_load2_sext(state, ir->rs1, ir->rs2, true, false);
    vm_reg[2] = map_vm_reg_reserved2(state, ir->rd, vm_reg[0], vm_reg[1]);
    emit_mov(state, vm_reg[1], temp_reg);
    emit_mov(state, vm_reg[0], vm_reg[2]);
    muldivmod(state, 0x2f, temp_reg, vm_reg[2], 0);
    emit_alu64_imm8(state, SHIFT_IMM_OPCODE, SHIFT_SHR, vm_reg[2], 32);
})
GEN(mulhu, {
    ra_load2(state, ir->rs1, ir->rs2);
    vm_reg[2] = map_vm_reg_reserved2(state, ir->rd, vm_reg[0], vm_reg[1]);
    emit_mov(state, vm_reg[1], temp_reg);
    emit_mov(state, vm_reg[0], vm_reg[2]);
    muldivmod(state, 0x2f, temp_reg, vm_reg[2], 0);
    emit_alu64_imm8(state, SHIFT_IMM_OPCODE, SHIFT_SHR, vm_reg[2], 32);
})
GEN(div, {
    ra_load2_sext(state, ir->rs1, ir->rs2, true, true);
    vm_reg[2] = map_vm_reg_reserved2(state, ir->rd, vm_reg[0], vm_reg[1]);
    emit_mov(state, vm_reg[1], temp_reg);
    emit_mov(state, vm_reg[0], vm_reg[2]);
    muldivmod(state, 0x38, temp_reg, vm_reg[2], 1);
})
GEN(divu, {
    ra_load2(state, ir->rs1, ir->rs2);
    vm_reg[2] = map_vm_reg_reserved2(state, ir->rd, vm_reg[0], vm_reg[1]);
    emit_mov(state, vm_reg[1], temp_reg);
    emit_mov(state, vm_reg[0], vm_reg[2]);
    muldivmod(state, 0x38, temp_reg, vm_reg[2], 0);
})
GEN(rem, {
    ra_load2_sext(state, ir->rs1, ir->rs2, true, true);
    vm_reg[2] = map_vm_reg_reserved2(state, ir->rd, vm_reg[0], vm_reg[1]);
    emit_mov(state, vm_reg[1], temp_reg);
    emit_mov(state, vm_reg[0], vm_reg[2]);
    muldivmod(state, 0x98, temp_reg, vm_reg[2], 1);
})
GEN(remu, {
    ra_load2(state, ir->rs1, ir->rs2);
    vm_reg[2] = map_vm_reg_reserved2(state, ir->rd, vm_reg[0], vm_reg[1]);
    emit_mov(state, vm_reg[1], temp_reg);
    emit_mov(state, vm_reg[0], vm_reg[2]);
    muldivmod(state, 0x98, temp_reg, vm_reg[2], 0);
})
#endif
#if RV32_HAS(EXT_A)
GEN(lrw, { assert(NULL); })
GEN(scw, { assert(NULL); })
GEN(amoswapw, { assert(NULL); })
GEN(amoaddw, { assert(NULL); })
GEN(amoxorw, { assert(NULL); })
GEN(amoandw, { assert(NULL); })
GEN(amoorw, { assert(NULL); })
GEN(amominw, { assert(NULL); })
GEN(amomaxw, { assert(NULL); })
GEN(amominuw, { assert(NULL); })
GEN(amomaxuw, { assert(NULL); })
#endif
#if RV32_HAS(EXT_F)
GEN(flw, { assert(NULL); })
GEN(fsw, { assert(NULL); })
GEN(fmadds, { assert(NULL); })
GEN(fmsubs, { assert(NULL); })
GEN(fnmsubs, { assert(NULL); })
GEN(fnmadds, { assert(NULL); })
GEN(fadds, { assert(NULL); })
GEN(fsubs, { assert(NULL); })
GEN(fmuls, { assert(NULL); })
GEN(fdivs, { assert(NULL); })
GEN(fsqrts, { assert(NULL); })
GEN(fsgnjs, { assert(NULL); })
GEN(fsgnjns, { assert(NULL); })
GEN(fsgnjxs, { assert(NULL); })
GEN(fmins, { assert(NULL); })
GEN(fmaxs, { assert(NULL); })
GEN(fcvtws, { assert(NULL); })
GEN(fcvtwus, { assert(NULL); })
GEN(fmvxw, { assert(NULL); })
GEN(feqs, { assert(NULL); })
GEN(flts, { assert(NULL); })
GEN(fles, { assert(NULL); })
GEN(fclasss, { assert(NULL); })
GEN(fcvtsw, { assert(NULL); })
GEN(fcvtswu, { assert(NULL); })
GEN(fmvwx, { assert(NULL); })
#endif
#if RV32_HAS(EXT_C)
GEN(caddi4spn, {
    vm_reg[0] = ra_load(state, rv_reg_sp);
    vm_reg[1] = map_vm_reg_reserved(state, ir->rd, vm_reg[0]);
    if (vm_reg[0] != vm_reg[1]) {
        emit_mov(state, vm_reg[0], vm_reg[1]);
    }
    emit_alu32_imm32(state, ALU_GRP1_OPCODE, ALU_ADD, vm_reg[1],
                     (uint16_t) ir->imm);
})
GEN(clw, {
    memory_t *m = PRIV(rv)->mem;
    vm_reg[0] = ra_load(state, ir->rs1);
    emit_load_imm_sext(state, temp_reg, (intptr_t) (m->mem_base + ir->imm));
    emit_alu64(state, 0x01, vm_reg[0], temp_reg);
    vm_reg[1] = map_vm_reg(state, ir->rd);
    emit_load(state, S32, temp_reg, vm_reg[1], 0);
})
GEN(csw, {
    memory_t *m = PRIV(rv)->mem;
    vm_reg[0] = ra_load(state, ir->rs1);
    emit_load_imm_sext(state, temp_reg, (intptr_t) (m->mem_base + ir->imm));
    emit_alu64(state, 0x01, vm_reg[0], temp_reg);
    vm_reg[1] = ra_load(state, ir->rs2);
    emit_store(state, S32, vm_reg[1], temp_reg, 0);
})
GEN(cnop, {})
GEN(caddi, {
    vm_reg[0] = ra_load(state, ir->rd);
    emit_alu32_imm32(state, ALU_GRP1_OPCODE, ALU_ADD, vm_reg[0],
                     (int16_t) ir->imm);
})
GEN(cjal, {
    vm_reg[0] = map_vm_reg(state, rv_reg_ra);
    emit_load_imm(state, vm_reg[0], ir->pc + 2);
    store_back(state);
    emit_jmp(state, ir->pc + ir->imm, rv->csr_satp);
    emit_load_imm(state, temp_reg, ir->pc + ir->imm);
    emit_store(state, S32, temp_reg, parameter_reg[0], offsetof(riscv_t, PC));
    emit_exit(state);
})
GEN(cli, {
    vm_reg[0] = map_vm_reg(state, ir->rd);
    emit_load_imm(state, vm_reg[0], ir->imm);
})
GEN(caddi16sp, {
    vm_reg[0] = ra_load(state, ir->rd);
    emit_alu32_imm32(state, ALU_GRP1_OPCODE, ALU_ADD, vm_reg[0], ir->imm);
})
GEN(clui, {
    vm_reg[0] = map_vm_reg(state, ir->rd);
    emit_load_imm(state, vm_reg[0], ir->imm);
})
GEN(csrli, {
    vm_reg[0] = ra_load(state, ir->rs1);
    emit_alu32_imm8(state, SHIFT_IMM_OPCODE, SHIFT_SHR, vm_reg[0], ir->shamt);
})
GEN(csrai, {
    vm_reg[0] = ra_load(state, ir->rs1);
    emit_alu32_imm8(state, SHIFT_IMM_OPCODE, SHIFT_SAR, vm_reg[0], ir->shamt);
})
GEN(candi, {
    vm_reg[0] = ra_load(state, ir->rs1);
    emit_alu32_imm32(state, ALU_GRP1_OPCODE, ALU_AND, vm_reg[0], ir->imm);
})
GEN(csub, {
    ra_load2(state, ir->rs1, ir->rs2);
    vm_reg[2] = map_vm_reg_reserved2(state, ir->rd, vm_reg[0], vm_reg[1]);
    emit_mov(state, vm_reg[1], temp_reg);
    emit_mov(state, vm_reg[0], vm_reg[2]);
    emit_alu32(state, 0x29, temp_reg, vm_reg[2]);
})
GEN(cxor, {
    ra_load2(state, ir->rs1, ir->rs2);
    vm_reg[2] = map_vm_reg_reserved2(state, ir->rd, vm_reg[0], vm_reg[1]);
    emit_mov(state, vm_reg[1], temp_reg);
    emit_mov(state, vm_reg[0], vm_reg[2]);
    emit_alu32(state, 0x31, temp_reg, vm_reg[2]);
})
GEN(cor, {
    ra_load2(state, ir->rs1, ir->rs2);
    vm_reg[2] = map_vm_reg_reserved2(state, ir->rd, vm_reg[0], vm_reg[1]);
    emit_mov(state, vm_reg[1], temp_reg);
    emit_mov(state, vm_reg[0], vm_reg[2]);
    emit_alu32(state, 0x09, temp_reg, vm_reg[2]);
})
GEN(cand, {
    ra_load2(state, ir->rs1, ir->rs2);
    vm_reg[2] = map_vm_reg_reserved2(state, ir->rd, vm_reg[0], vm_reg[1]);
    emit_mov(state, vm_reg[1], temp_reg);
    emit_mov(state, vm_reg[0], vm_reg[2]);
    emit_alu32(state, 0x21, temp_reg, vm_reg[2]);
})
GEN(cj, {
    store_back(state);
    emit_jmp(state, ir->pc + ir->imm, rv->csr_satp);
    emit_load_imm(state, temp_reg, ir->pc + ir->imm);
    emit_store(state, S32, temp_reg, parameter_reg[0], offsetof(riscv_t, PC));
    emit_exit(state);
})
/* RV32C 压缩分支指令。 */
GEN_CBRANCH(cbeqz, JCC_JE)
GEN_CBRANCH(cbnez, JCC_JNE)
GEN(cslli, {
    vm_reg[0] = ra_load(state, ir->rd);
    emit_alu32_imm8(state, SHIFT_IMM_OPCODE, SHIFT_SHL, vm_reg[0],
                    (uint8_t) ir->imm);
})
GEN(clwsp, {
    memory_t *m = PRIV(rv)->mem;
    vm_reg[0] = ra_load(state, rv_reg_sp);
    emit_load_imm_sext(state, temp_reg, (intptr_t) (m->mem_base + ir->imm));
    emit_alu64(state, 0x01, vm_reg[0], temp_reg);
    vm_reg[1] = map_vm_reg(state, ir->rd);
    emit_load(state, S32, temp_reg, vm_reg[1], 0);
})
GEN(cjr, {
    vm_reg[0] = ra_load(state, ir->rs1);
    emit_mov(state, vm_reg[0], temp_reg);
    store_back(state);
    parse_branch_history_table(state, rv, ir);
    emit_store(state, S32, temp_reg, parameter_reg[0], offsetof(riscv_t, PC));
    emit_exit(state);
})
GEN(cmv, {
    vm_reg[0] = ra_load(state, ir->rs2);
    vm_reg[1] = map_vm_reg_reserved(state, ir->rd, vm_reg[0]);
    if (vm_reg[0] != vm_reg[1]) {
        emit_mov(state, vm_reg[0], vm_reg[1]);
    } else {
        set_dirty(vm_reg[1], true);
    }
})
GEN(cebreak, {
    store_back(state);
    emit_load_imm(state, temp_reg, ir->pc);
    emit_store(state, S32, temp_reg, parameter_reg[0], offsetof(riscv_t, PC));
    emit_call(state, (intptr_t) rv->io.on_ebreak);
    emit_exit(state);
})
GEN(cjalr, {
    vm_reg[0] = ra_load(state, ir->rs1);
    emit_mov(state, vm_reg[0], temp_reg);
    vm_reg[1] = map_vm_reg(state, rv_reg_ra);
    emit_load_imm(state, vm_reg[1], ir->pc + 2);
    store_back(state);
    parse_branch_history_table(state, rv, ir);
    emit_store(state, S32, temp_reg, parameter_reg[0], offsetof(riscv_t, PC));
    emit_exit(state);
})
GEN(cadd, {
    ra_load2(state, ir->rs1, ir->rs2);
    vm_reg[2] = map_vm_reg_reserved2(state, ir->rd, vm_reg[0], vm_reg[1]);
    emit_mov(state, vm_reg[1], temp_reg);
    emit_mov(state, vm_reg[0], vm_reg[2]);
    emit_alu32(state, 0x01, temp_reg, vm_reg[2]);
})
GEN(cswsp, {
    memory_t *m = PRIV(rv)->mem;
    vm_reg[0] = ra_load(state, rv_reg_sp);
    emit_load_imm_sext(state, temp_reg, (intptr_t) (m->mem_base + ir->imm));
    emit_alu64(state, 0x01, vm_reg[0], temp_reg);
    vm_reg[1] = ra_load(state, ir->rs2);
    emit_store(state, S32, vm_reg[1], temp_reg, 0);
})
#endif
#if RV32_HAS(EXT_C) && RV32_HAS(EXT_F)
GEN(cflwsp, { assert(NULL); })
GEN(cfswsp, { assert(NULL); })
GEN(cflw, { assert(NULL); })
GEN(cfsw, { assert(NULL); })
#endif
#if RV32_HAS(Zba)
GEN(sh1add, { assert(NULL); })
GEN(sh2add, { assert(NULL); })
GEN(sh3add, { assert(NULL); })
#endif
#if RV32_HAS(Zbb)
GEN(andn, { assert(NULL); })
GEN(orn, { assert(NULL); })
GEN(xnor, { assert(NULL); })
GEN(clz, { assert(NULL); })
GEN(ctz, { assert(NULL); })
GEN(cpop, { assert(NULL); })
GEN(max, { assert(NULL); })
GEN(min, { assert(NULL); })
GEN(maxu, { assert(NULL); })
GEN(minu, { assert(NULL); })
GEN(sextb, { assert(NULL); })
GEN(sexth, { assert(NULL); })
GEN(zexth, { assert(NULL); })
GEN(rol, { assert(NULL); })
GEN(ror, { assert(NULL); })
GEN(rori, { assert(NULL); })
GEN(orcb, { assert(NULL); })
GEN(rev8, { assert(NULL); })
#endif
#if RV32_HAS(Zbc)
GEN(clmul, { assert(NULL); })
GEN(clmulh, { assert(NULL); })
GEN(clmulr, { assert(NULL); })
#endif
#if RV32_HAS(Zbs)
GEN(bclr, { assert(NULL); })
GEN(bclri, { assert(NULL); })
GEN(bext, { assert(NULL); })
GEN(bexti, { assert(NULL); })
GEN(binv, { assert(NULL); })
GEN(binvi, { assert(NULL); })
GEN(bset, { assert(NULL); })
GEN(bseti, { assert(NULL); })
#endif
