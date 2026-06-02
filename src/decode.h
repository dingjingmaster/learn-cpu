/*
 * rv32emu 可依据 MIT 许可证自由再分发。使用和再分发规则见 LICENSE 文件。
 */

/*
 * 指令解码接口和 IR 定义。
 *
 * rv_insn_t 是解释器、JIT 和优化阶段共享的指令内部表示。这里定义 opcode 枚举、
 * 字段掩码、压缩指令格式常量以及 rv_decode 入口。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "riscv.h"

enum op_field {
    F_none = 0,
    F_rs1 = 1,
    F_rs2 = 2,
    F_rs3 = 4,
    F_rd = 8,
};

#define ENC4(a, b, c, d, ...) F_##a | F_##b | F_##c | F_##d
#define ENC3(a, b, c, ...) F_##a | F_##b | F_##c
#define ENC2(a, b, ...) F_##a | F_##b
#define ENC1(a, ...) F_##a
#define ENC0(...) F_none

#define ENCN(X, A) X##A
#define ENC_GEN(X, A) ENCN(X, A)
#define ENC(...) ENC_GEN(ENC, COUNT_VARARGS(__VA_ARGS__))(__VA_ARGS__)

/* RISC-V 指令清单，格式为：
 * _(指令名, 是否可能分支, 指令长度, 是否可翻译为快速路径, 使用的寄存器掩码)
 */
/* clang-format off */
#define RV_INSN_LIST                                   \
    _(nop, 0, 4, 1, ENC(rs1, rd))                      \
    /* RV32I 基础指令集。 */                           \
    _(lui, 0, 4, 1, ENC(rd))                           \
    _(auipc, 0, 4, 1, ENC(rd))                         \
    _(jal, 1, 4, 1, ENC(rd))                           \
    _(jalr, 1, 4, 1, ENC(rs1, rd))                     \
    _(beq, 1, 4, 1, ENC(rs1, rs2))                     \
    _(bne, 1, 4, 1, ENC(rs1, rs2))                     \
    _(blt, 1, 4, 1, ENC(rs1, rs2))                     \
    _(bge, 1, 4, 1, ENC(rs1, rs2))                     \
    _(bltu, 1, 4, 1, ENC(rs1, rs2))                    \
    _(bgeu, 1, 4, 1, ENC(rs1, rs2))                    \
    _(lb, 0, 4, 1, ENC(rs1, rd))                       \
    _(lh, 0, 4, 1, ENC(rs1, rd))                       \
    _(lw, 0, 4, 1, ENC(rs1, rd))                       \
    _(lbu, 0, 4, 1, ENC(rs1, rd))                      \
    _(lhu, 0, 4, 1, ENC(rs1, rd))                      \
    _(sb, 0, 4, 1, ENC(rs1, rs2))                      \
    _(sh, 0, 4, 1, ENC(rs1, rs2))                      \
    _(sw, 0, 4, 1, ENC(rs1, rs2))                      \
    _(addi, 0, 4, 1, ENC(rs1, rd))                     \
    _(slti, 0, 4, 1, ENC(rs1, rd))                     \
    _(sltiu, 0, 4, 1, ENC(rs1, rd))                    \
    _(xori, 0, 4, 1, ENC(rs1, rd))                     \
    _(ori, 0, 4, 1, ENC(rs1, rd))                      \
    _(andi, 0, 4, 1, ENC(rs1, rd))                     \
    _(slli, 0, 4, 1, ENC(rs1, rd))                     \
    _(srli, 0, 4, 1, ENC(rs1, rd))                     \
    _(srai, 0, 4, 1, ENC(rs1, rd))                     \
    _(add, 0, 4, 1, ENC(rs1, rs2, rd))                 \
    _(sub, 0, 4, 1, ENC(rs1, rs2, rd))                 \
    _(sll, 0, 4, 1, ENC(rs1, rs2, rd))                 \
    _(slt, 0, 4, 1, ENC(rs1, rs2, rd))                 \
    _(sltu, 0, 4, 1, ENC(rs1, rs2, rd))                \
    _(xor, 0, 4, 1, ENC(rs1, rs2, rd))                 \
    _(srl, 0, 4, 1, ENC(rs1, rs2, rd))                 \
    _(sra, 0, 4, 1, ENC(rs1, rs2, rd))                 \
    _(or, 0, 4, 1, ENC(rs1, rs2, rd))                  \
    _(and, 0, 4, 1, ENC(rs1, rs2, rd))                 \
    _(fence, 1, 4, 0, ENC(rs1, rd))                    \
    _(ecall, 1, 4, 1, ENC(rs1, rd))                    \
    _(ebreak, 1, 4, 1, ENC(rs1, rd))                   \
    /* RISC-V 特权指令。 */                            \
    _(wfi, 0, 4, 0, ENC(rs1, rd))                      \
    _(uret, 0, 4, 0, ENC(rs1, rd))                     \
    IIF(RV32_HAS(SYSTEM))(                             \
        _(sret, 1, 4, 0, ENC(rs1, rd))                 \
    )                                                  \
    _(hret, 0, 4, 0, ENC(rs1, rd))                     \
    _(mret, 1, 4, 0, ENC(rs1, rd))                     \
    _(sfencevma, 1, 4, 0, ENC(rs1, rs2, rd))           \
    /* RV32 Zifencei 标准扩展。 */                     \
    IIF(RV32_HAS(Zifencei))(                           \
        _(fencei, 1, 4, 0, ENC(rs1, rd))               \
    )                                                  \
    /* RV32 Zicsr 标准扩展。 */                        \
    IIF(RV32_HAS(Zicsr))(                              \
        _(csrrw, 1, 4, 0, ENC(rs1, rd))                \
        _(csrrs, 0, 4, 0, ENC(rs1, rd))                \
        _(csrrc, 0, 4, 0, ENC(rs1, rd))                \
        _(csrrwi, 0, 4, 0, ENC(rs1, rd))               \
        _(csrrsi, 0, 4, 0, ENC(rs1, rd))               \
        _(csrrci, 0, 4, 0, ENC(rs1, rd))               \
    )                                                  \
    /* RV32 Zba 标准扩展。 */                          \
    IIF(RV32_HAS(Zba))(                                \
        _(sh1add, 0, 4, 0, ENC(rs1, rs2, rd))          \
        _(sh2add, 0, 4, 0, ENC(rs1, rs2, rd))          \
        _(sh3add, 0, 4, 0, ENC(rs1, rs2, rd))          \
    )                                                  \
    /* RV32 Zbb 标准扩展。 */                          \
    IIF(RV32_HAS(Zbb))(                                \
        _(andn, 0, 4, 0, ENC(rs1, rs2, rd))            \
        _(orn, 0, 4, 0, ENC(rs1, rs2, rd))             \
        _(xnor, 0, 4, 0, ENC(rs1, rs2, rd))            \
        _(clz, 0, 4, 0, ENC(rs1, rd))                  \
        _(ctz, 0, 4, 0, ENC(rs1, rd))                  \
        _(cpop, 0, 4, 0, ENC(rs1, rd))                 \
        _(max, 0, 4, 0, ENC(rs1, rs2, rd))             \
        _(maxu, 0, 4, 0, ENC(rs1, rs2, rd))            \
        _(min, 0, 4, 0, ENC(rs1, rs2, rd))             \
        _(minu, 0, 4, 0, ENC(rs1, rs2, rd))            \
        _(sextb, 0, 4, 0, ENC(rs1, rd))                \
        _(sexth, 0, 4, 0, ENC(rs1, rd))                \
        _(zexth, 0, 4, 0, ENC(rs1, rd))                \
        _(rol, 0, 4, 0, ENC(rs1, rs2, rd))             \
        _(ror, 0, 4, 0, ENC(rs1, rs2, rd))             \
        _(rori, 0, 4, 0, ENC(rs1, rd))                 \
        _(orcb, 0, 4, 0, ENC(rs1, rd))                 \
        _(rev8, 0, 4, 0, ENC(rs1, rd))                 \
    )                                                  \
     /* RV32 Zbc 标准扩展。 */                         \
    IIF(RV32_HAS(Zbc))(                                \
        _(clmul, 0, 4, 0, ENC(rs1, rs2, rd))           \
        _(clmulh, 0, 4, 0, ENC(rs1, rs2, rd))          \
        _(clmulr, 0, 4, 0, ENC(rs1, rs2, rd))          \
    )                                                  \
    /* RV32 Zbs 标准扩展。 */                          \
    IIF(RV32_HAS(Zbs))(                                \
        _(bclr, 0, 4, 0, ENC(rs1, rs2, rd))            \
        _(bclri, 0, 4, 0, ENC(rs1, rs2, rd))           \
        _(bext, 0, 4, 0, ENC(rs1, rs2, rd))            \
        _(bexti, 0, 4, 0, ENC(rs1, rs2, rd))           \
        _(binv, 0, 4, 0, ENC(rs1, rs2, rd))            \
        _(binvi, 0, 4, 0, ENC(rs1, rs2, rd))           \
        _(bset, 0, 4, 0, ENC(rs1, rs2, rd))            \
        _(bseti, 0, 4, 0, ENC(rs1, rs2, rd))           \
    )                                                  \
    /* RV32M 标准扩展。 */                             \
    IIF(RV32_HAS(EXT_M))(                              \
        _(mul, 0, 4, 1, ENC(rs1, rs2, rd))             \
        _(mulh, 0, 4, 1, ENC(rs1, rs2, rd))            \
        _(mulhsu, 0, 4, 1, ENC(rs1, rs2, rd))          \
        _(mulhu, 0, 4, 1, ENC(rs1, rs2, rd))           \
        _(div, 0, 4, 1, ENC(rs1, rs2, rd))             \
        _(divu, 0, 4, 1, ENC(rs1, rs2, rd))            \
        _(rem, 0, 4, 1, ENC(rs1, rs2, rd))             \
        _(remu, 0, 4, 1, ENC(rs1, rs2, rd))            \
    )                                                  \
    /* RV32A 标准扩展。 */                             \
    IIF(RV32_HAS(EXT_A))(                              \
        _(lrw, 0, 4, 0, ENC(rs1, rs2, rd))             \
        _(scw, 0, 4, 0, ENC(rs1, rs2, rd))             \
        _(amoswapw, 0, 4, 0, ENC(rs1, rs2, rd))        \
        _(amoaddw, 0, 4, 0, ENC(rs1, rs2, rd))         \
        _(amoxorw, 0, 4, 0, ENC(rs1, rs2, rd))         \
        _(amoandw, 0, 4, 0, ENC(rs1, rs2, rd))         \
        _(amoorw, 0, 4, 0, ENC(rs1, rs2, rd))          \
        _(amominw, 0, 4, 0, ENC(rs1, rs2, rd))         \
        _(amomaxw, 0, 4, 0, ENC(rs1, rs2, rd))         \
        _(amominuw, 0, 4, 0, ENC(rs1, rs2, rd))        \
        _(amomaxuw, 0, 4, 0, ENC(rs1, rs2, rd))        \
    )                                                  \
    /* RV32F 标准扩展。 */                             \
    IIF(RV32_HAS(EXT_F))(                              \
        _(flw, 0, 4, 0, ENC(rs1, rd))                  \
        _(fsw, 0, 4, 0, ENC(rs1, rs2))                 \
        _(fmadds, 0, 4, 0, ENC(rs1, rs2, rs3, rd))     \
        _(fmsubs, 0, 4, 0, ENC(rs1, rs2, rs3, rd))     \
        _(fnmsubs, 0, 4, 0, ENC(rs1, rs2, rs3, rd))    \
        _(fnmadds, 0, 4, 0, ENC(rs1, rs2, rs3, rd))    \
        _(fadds, 0, 4, 0, ENC(rs1, rs2, rd))           \
        _(fsubs, 0, 4, 0, ENC(rs1, rs2, rd))           \
        _(fmuls, 0, 4, 0, ENC(rs1, rs2, rd))           \
        _(fdivs, 0, 4, 0, ENC(rs1, rs2, rd))           \
        _(fsqrts, 0, 4, 0, ENC(rs1, rs2, rd))          \
        _(fsgnjs, 0, 4, 0, ENC(rs1, rs2, rd))          \
        _(fsgnjns, 0, 4, 0, ENC(rs1, rs2, rd))         \
        _(fsgnjxs, 0, 4, 0, ENC(rs1, rs2, rd))         \
        _(fmins, 0, 4, 0, ENC(rs1, rs2, rd))           \
        _(fmaxs, 0, 4, 0, ENC(rs1, rs2, rd))           \
        _(fcvtws, 0, 4, 0, ENC(rs1, rs2, rd))          \
        _(fcvtwus, 0, 4, 0, ENC(rs1, rs2, rd))         \
        _(fmvxw, 0, 4, 0, ENC(rs1, rs2, rd))           \
        _(feqs, 0, 4, 0, ENC(rs1, rs2, rd))            \
        _(flts, 0, 4, 0, ENC(rs1, rs2, rd))            \
        _(fles, 0, 4, 0, ENC(rs1, rs2, rd))            \
        _(fclasss, 0, 4, 0, ENC(rs1, rs2, rd))         \
        _(fcvtsw, 0, 4, 0, ENC(rs1, rs2, rd))          \
        _(fcvtswu, 0, 4, 0, ENC(rs1, rs2, rd))         \
        _(fmvwx, 0, 4, 0, ENC(rs1, rs2, rd))           \
    )                                                  \
    /* RV32C 标准扩展。 */                             \
    IIF(RV32_HAS(EXT_C))(                              \
        _(caddi4spn, 0, 2, 1, ENC(rd))                 \
        _(clw, 0, 2, 1, ENC(rs1, rd))                  \
        _(csw, 0, 2, 1, ENC(rs1, rs2))                 \
        _(cnop, 0, 2, 1, ENC())                        \
        _(caddi, 0, 2, 1, ENC(rd))                     \
        _(cjal, 1, 2, 1, ENC())                        \
        _(cli, 0, 2, 1, ENC(rd))                       \
        _(caddi16sp, 0, 2, 1, ENC())                   \
        _(clui, 0, 2, 1, ENC(rd))                      \
        _(csrli, 0, 2, 1, ENC(rs1))                    \
        _(csrai, 0, 2, 1, ENC(rs1))                    \
        _(candi, 0, 2, 1, ENC(rs1))                    \
        _(csub, 0, 2, 1, ENC(rs1, rs2, rd))            \
        _(cxor, 0, 2, 1, ENC(rs1, rs2, rd))            \
        _(cor, 0, 2, 1, ENC(rs1, rs2, rd))             \
        _(cand, 0, 2, 1, ENC(rs1, rs2, rd))            \
        _(cj, 1, 2, 1, ENC())                          \
        _(cbeqz, 1, 2, 1, ENC(rs1))                    \
        _(cbnez, 1, 2, 1, ENC(rs1))                    \
        _(cslli, 0, 2, 1, ENC(rd))                     \
        _(clwsp, 0, 2, 1, ENC(rd))                     \
        _(cjr, 1, 2, 1, ENC(rs1, rs2, rd))             \
        _(cmv, 0, 2, 1, ENC(rs1, rs2, rd))             \
        _(cebreak, 1, 2, 1,ENC(rs1, rs2, rd))          \
        _(cjalr, 1, 2, 1, ENC(rs1, rs2, rd))           \
        _(cadd, 0, 2, 1, ENC(rs1, rs2, rd))            \
        _(cswsp, 0, 2, 1, ENC(rs2))                    \
        /* RV32FC 指令。 */                            \
        IIF(RV32_HAS(EXT_F))(                          \
            _(cflwsp, 0, 2, 1, ENC(rd))                \
            _(cfswsp, 0, 2, 1, ENC(rs2))               \
            _(cflw, 0, 2, 1, ENC(rs1, rd))             \
            _(cfsw, 0, 2, 1, ENC(rs1, rs2))            \
        )                                              \
    )
/* clang-format on */

/* 宏操作融合。 */

/* 宏操作融合会把特定 RISC-V 指令序列转换为语义等价但更快的内部操作。 */
#define FUSE_INSN_LIST \
    _(fuse1)           \
    _(fuse2)           \
    _(fuse3)           \
    _(fuse4)           \
    _(fuse5)           \
    _(fuse6)           \
    _(fuse7)           \
    _(fuse8)           \
    _(fuse9)           \
    _(fuse10)          \
    _(fuse11)          \
    _(fuse12)

/* 融合模式说明：
 * fuse1:  多条 LUI                  - 批量加载高位立即数。
 * fuse2:  LUI + ADD                 - 高位立即数加寄存器。
 * fuse3:  多条 SW                   - 批量存储。
 * fuse4:  多条 LW                   - 批量加载。
 * fuse5:  多条移位立即数指令        - 批量处理 SLLI/SRLI/SRAI。
 * fuse6:  LI a7 + ECALL             - 系统调用分派。
 * fuse7:  多条 ADDI                 - 批量立即数加法。
 * fuse8:  LUI + ADDI                - 32 位常量加载（li）。
 * fuse9:  LUI + LW                  - 绝对地址或 PC 相对加载。
 * fuse10: LUI + SW                  - 绝对地址或 PC 相对存储。
 * fuse11: LW + ADDI（post-inc）     - 加载后递增指针。
 * fuse12: ADDI + BNE                - 循环计数递减并分支。
 */

/* clang-format off */
/* IR（中间表示）仍以 RISC-V 指令形式表达，但执行成本更低。 */
enum {
#define _(inst, can_branch, insn_len, translatable, reg_mask) rv_insn_##inst,
    RV_INSN_LIST
#undef _
    N_RV_INSNS,
#define _(inst) rv_insn_##inst,
    FUSE_INSN_LIST
#undef _
};
/* clang-format on */

/* clang-format off */
/* 指令解码掩码。 */
enum {
    //               ....xxxx....xxxx....xxxx....xxxx
    INSN_6_2     = 0b00000000000000000000000001111100,
    //               ....xxxx....xxxx....xxxx....xxxx
    FR_OPCODE    = 0b00000000000000000000000001111111, // R 型
    FR_RD        = 0b00000000000000000000111110000000,
    FR_FUNCT3    = 0b00000000000000000111000000000000,
    FR_RS1       = 0b00000000000011111000000000000000,
    FR_RS2       = 0b00000001111100000000000000000000,
    FR_FUNCT7    = 0b11111110000000000000000000000000,
    //               ....xxxx....xxxx....xxxx....xxxx
    FI_IMM_11_0  = 0b11111111111100000000000000000000, // I 型
    //               ....xxxx....xxxx....xxxx....xxxx
    FS_IMM_4_0   = 0b00000000000000000000111110000000, // S 型
    FS_IMM_11_5  = 0b11111110000000000000000000000000,
    //               ....xxxx....xxxx....xxxx....xxxx
    FB_IMM_11    = 0b00000000000000000000000010000000, // B 型
    FB_IMM_4_1   = 0b00000000000000000000111100000000,
    FB_IMM_10_5  = 0b01111110000000000000000000000000,
    FB_IMM_12    = 0b10000000000000000000000000000000,
    //               ....xxxx....xxxx....xxxx....xxxx
    FU_IMM_31_12 = 0b11111111111111111111000000000000, // U 型
    //               ....xxxx....xxxx....xxxx....xxxx
    FJ_IMM_19_12 = 0b00000000000011111111000000000000, // J 型
    FJ_IMM_11    = 0b00000000000100000000000000000000,
    FJ_IMM_10_1  = 0b01111111111000000000000000000000,
    FJ_IMM_20    = 0b10000000000000000000000000000000,
    //               ....xxxx....xxxx....xxxx....xxxx
    FR4_FMT      = 0b00000110000000000000000000000000, // R4 型
    FR4_RS3      = 0b11111000000000000000000000000000,
    //               ....xxxx....xxxx....xxxx....xxxx
    FC_OPCODE    = 0b00000000000000000000000000000011, // 压缩指令
    FC_FUNC3     = 0b00000000000000001110000000000000,
    //               ....xxxx....xxxx....xxxx....xxxx
    FC_RS1C      = 0b00000000000000000000001110000000,
    FC_RS2C      = 0b00000000000000000000000000011100,
    FC_RS1       = 0b00000000000000000000111110000000,
    FC_RS2       = 0b00000000000000000000000001111100,
    //               ....xxxx....xxxx....xxxx....xxxx
    FC_RDC       = 0b00000000000000000000000000011100,
    FC_RD        = 0b00000000000000000000111110000000,
    //               ....xxxx....xxxx....xxxx....xxxx
    FC_IMM_12_10 = 0b00000000000000000001110000000000, // CL,CS,CB
    FC_IMM_6_5   = 0b00000000000000000000000001100000,
    //               ....xxxx....xxxx....xxxx....xxxx
    FCI_IMM_12   = 0b00000000000000000001000000000000,
    FCI_IMM_6_2  = 0b00000000000000000000000001111100,
    //               ....xxxx....xxxx....xxxx....xxxx
    FCSS_IMM     = 0b00000000000000000001111110000000,
    //               ....xxxx....xxxx....xxxx....xxxx
    FCJ_IMM      = 0b00000000000000000001111111111100,
    //               ....xxxx....xxxx....xxxx....xxxx
};
/* clang-format on */

/* 融合指令数据必须与 rv_insn_t 布局的前 8 字节完全一致。
 * fuse 数组和处理器会直接访问这些字段。
 * 警告：不要把 opcode_fuse_t* 强转为 rv_insn_t*，应使用专门函数。
 */
typedef struct {
    int32_t imm;
    uint8_t rd, rs1, rs2;
    uint8_t opcode;
} opcode_fuse_t;

/* 编译期布局校验。 */
_Static_assert(sizeof(opcode_fuse_t) == 8,
               "opcode_fuse_t 必须正好为 8 字节");
_Static_assert(offsetof(opcode_fuse_t, imm) == 0,
               "opcode_fuse_t.imm 必须位于偏移 0");
_Static_assert(offsetof(opcode_fuse_t, rd) == 4,
               "opcode_fuse_t.rd 必须位于偏移 4");
_Static_assert(offsetof(opcode_fuse_t, rs1) == 5,
               "opcode_fuse_t.rs1 必须位于偏移 5");
_Static_assert(offsetof(opcode_fuse_t, rs2) == 6,
               "opcode_fuse_t.rs2 必须位于偏移 6");
_Static_assert(offsetof(opcode_fuse_t, opcode) == 7,
               "opcode_fuse_t.opcode 必须位于偏移 7");

#define HISTORY_SIZE 16
/* 直接映射 BHT 需要 2 的幂大小，便于用掩码计算索引。 */
_Static_assert((HISTORY_SIZE & (HISTORY_SIZE - 1)) == 0,
               "HISTORY_SIZE 必须是 2 的幂");

typedef struct {
    uint32_t PC[HISTORY_SIZE]; /**< 直接映射查找使用的 PC 标签。 */
#if !RV32_HAS(JIT)
    struct rv_insn *target[HISTORY_SIZE]; /**< 目标 IR 指针。 */
#else
    uint32_t times[HISTORY_SIZE]; /**< 用于 JIT 热度判断的访问次数。 */
#if RV32_HAS(SYSTEM)
    uint32_t satp[HISTORY_SIZE]; /**< 用于地址空间匹配的 SATP。 */
#endif
#endif
} branch_history_table_t;

#if RV32_HAS(JIT)
/* 在分支历史表中找到 times 最大的索引。
 * JIT 使用它识别最常被采用的间接跳转目标。直接映射 BHT 中任意索引都可能为 0，
 * 因此必须扫描所有条目，不能在遇到第一个 0 时提前停止。
 */
static inline int bht_find_max_idx(const branch_history_table_t *bt)
{
    int max_idx = 0;
    for (int i = 1; i < HISTORY_SIZE; i++) {
        if (bt->times[i] > bt->times[max_idx])
            max_idx = i;
    }
    return max_idx;
}

#endif

typedef struct rv_insn {
    union {
        int32_t imm;
        uint8_t rs3;
    };
    uint8_t rd, rs1, rs2;
    /* 保存 IR 操作码。 */
    uint8_t opcode;

#if RV32_HAS(EXT_C)
    uint8_t shamt;
#endif

#if RV32_HAS(EXT_F)
    /* 浮点操作可以使用指令编码中的静态舍入模式，也可以使用 frm 中的动态舍入
     * 模式。指令 rm 字段为 111 时选择 frm；如果 frm 为非法值（101-111），后续
     * 任何使用动态舍入模式的浮点操作都会触发非法指令异常。有些带 rm 字段的
     * 指令实际不受舍入模式影响，它们应把 rm 设为 RNE（000）。
     */
    uint8_t rm;
#endif

    /* 融合操作附加数据。 */
    int32_t imm2;
    opcode_fuse_t *fuse;

    uint32_t pc;

    /* 尾调用优化（TCO）允许 C 函数把“调用另一个函数或自身并立即返回其结果”的
     * 模式替换为直接跳转到目标函数。这样自递归函数就能复用同一栈帧。
     *
     * @next 指向下一条 IR；如果当前指令是基本块最后一条则为 NULL。@impl 保存
     * 下一条指令的模拟函数入口，避免运行时再计算跳转地址。借助这两个成员，指令
     * 模拟函数可以写成自递归形式，让编译器利用 TCO。
     */
    struct rv_insn *next;
    PRESERVE_NONE bool (*impl)(riscv_t *,
                               const struct rv_insn *,
                               uint64_t,
                               uint32_t);

    /* branch_taken 和 branch_untaken 用来避免频繁复制整段 IR 数组。它们分别指向
     * 分支命中路径和未命中路径中第一个基本块的第一条 IR，使解释器/JIT 可以直接
     * 跳到对应 IR 数组。
     */
    struct rv_insn *branch_taken, *branch_untaken;
    branch_history_table_t *branch_table;
} rv_insn_t;

/* 解码一条 RISC-V 指令。 */
bool rv_decode(rv_insn_t *ir, const uint32_t insn);
