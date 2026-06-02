/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/*
 * RISC-V 指令解码器。
 *
 * 本文件把 32 位基础/扩展指令和 16 位 RVC 压缩指令转换为统一的 rv_insn_t
 * 内部表示。解码阶段会完成字段抽取、立即数重排、符号扩展和 opcode 分类，
 * 让解释器和 JIT 不必重复理解原始指令编码。
 */

#include <assert.h>
#include <stdlib.h>

#include "decode.h"
#include "riscv_private.h"

/* 解码 rd 字段。
 * rd = insn[11:7]
 */
static inline uint32_t decode_rd(const uint32_t insn)
{
    return (insn & FR_RD) >> 7;
}

/* 解码 rs1 字段。
 * rs1 = insn[19:15]
 */
static inline uint32_t decode_rs1(const uint32_t insn)
{
    return (insn & FR_RS1) >> 15;
}

/* 解码 rs2 字段。
 * rs2 = insn[24:20]
 */
static inline uint32_t decode_rs2(const uint32_t insn)
{
    return (insn & FR_RS2) >> 20;
}

/* 解码 funct3 字段。
 * funct3 = insn[14:12]
 */
static inline uint32_t decode_funct3(const uint32_t insn)
{
    return (insn & FR_FUNCT3) >> 12;
}

/* 解码 funct7 字段。
 * funct7 = insn[31:25]
 */
static inline uint32_t decode_funct7(const uint32_t insn)
{
    return (insn & FR_FUNCT7) >> 25;
}

/* 解码 U-type 指令立即数。
 * imm[31:12] = insn[31:12]
 */
static inline uint32_t decode_utype_imm(const uint32_t insn)
{
    return insn & FU_IMM_31_12;
}

/* 解码 J-type 指令立即数。
 * imm[20|10:1|11|19:12] = insn[31|30:21|20|19:12]
 */
static inline int32_t decode_jtype_imm(const uint32_t insn)
{
    uint32_t dst = 0;
    dst |= (insn & FJ_IMM_20);
    dst |= (insn & FJ_IMM_19_12) << 11;
    dst |= (insn & FJ_IMM_11) << 2;
    dst |= (insn & FJ_IMM_10_1) >> 9;
    /* NOTE：这里移动到次低有效位，最低位按 RISC-V 规则固定为 0。 */
    return ((int32_t) dst) >> 11;
}

/* 解码 I-type 指令立即数。
 * imm[11:0] = insn[31:20]
 */
static inline int32_t decode_itype_imm(const uint32_t insn)
{
    return ((int32_t) (insn & FI_IMM_11_0)) >> 20;
}

/* 解码 B-type 指令立即数。
 * imm[12] = insn[31]
 * imm[11] = insn[7]
 * imm[10:5] = insn[30:25]
 * imm[4:1] = insn[11:8]
 */
static inline int32_t decode_btype_imm(const uint32_t insn)
{
    uint32_t dst = 0;
    dst |= (insn & FB_IMM_12);
    dst |= (insn & FB_IMM_11) << 23;
    dst |= (insn & FB_IMM_10_5) >> 1;
    dst |= (insn & FB_IMM_4_1) << 12;
    /* NOTE：这里移动到次低有效位，最低位按 RISC-V 规则固定为 0。 */
    return ((int32_t) dst) >> 19;
}

/* 解码 S-type 指令立即数。
 * imm[11:5] = insn[31:25]
 * imm[4:0] = insn[11:7]
 */
static inline int32_t decode_stype_imm(const uint32_t insn)
{
    uint32_t dst = 0;
    dst |= (insn & FS_IMM_11_5);
    dst |= (insn & FS_IMM_4_0) << 13;
    return ((int32_t) dst) >> 20;
}

#if RV32_HAS(EXT_F)
/* 解码 R4-type 的 rs3 字段。
 * rs3 = inst[31:27]
 */
static inline uint32_t decode_r4type_rs3(const uint32_t insn)
{
    return (insn & FR4_RS3) >> 27;
}
#endif

#if RV32_HAS(EXT_C)
enum {
    /* clang-format off */
    /*             ....xxxx....xxxx */
    CJ_IMM_11    = 0b0001000000000000,
    CJ_IMM_4     = 0b0000100000000000,
    CJ_IMM_9_8   = 0b0000011000000000,
    CJ_IMM_10    = 0b0000000100000000,
    CJ_IMM_6     = 0b0000000010000000,
    CJ_IMM_7     = 0b0000000001000000,
    CJ_IMM_3_1   = 0b0000000000111000,
    CJ_IMM_5     = 0b0000000000000100,
    /*             ....xxxx....xxxx */
    CB_SHAMT_5   = 0b0001000000000000,
    CB_SHAMT_4_0 = 0b0000000001111100,
    /* clang-format on */
};

/* 解码 rs1 字段。
 * rs1 = inst[11:7]
 */
static inline uint16_t c_decode_rs1(const uint16_t insn)
{
    return (uint16_t) ((insn & FC_RS1) >> 7U);
}

/* 解码 rs2 字段。
 * rs2 = inst[6:2]
 */
static inline uint16_t c_decode_rs2(const uint16_t insn)
{
    return (uint16_t) ((insn & FC_RS2) >> 2U);
}

/* 解码 rd 字段。
 * rd = inst[11:7]
 */
static inline uint16_t c_decode_rd(const uint16_t insn)
{
    return (uint16_t) ((insn & FC_RD) >> 7U);
}

/* 解码 rs1' 字段。
 * rs1' = inst[9:7]
 */
static inline uint16_t c_decode_rs1c(const uint16_t insn)
{
    return (uint16_t) ((insn & FC_RS1C) >> 7U);
}

/* 解码 rs2' 字段。
 * rs2' = inst[4:2]
 */
static inline uint16_t c_decode_rs2c(const uint16_t insn)
{
    return (uint16_t) ((insn & FC_RS2C) >> 2U);
}

/* 解码 rd' 字段。
 * rd' = inst[4:2]
 */
static inline uint16_t c_decode_rdc(const uint16_t insn)
{
    return (uint16_t) ((insn & FC_RDC) >> 2U);
}

/* 解码 C.ADDI4SPN 的 nzuimm 字段。
 * nzuimm[5:4|9:6|2|3] = inst[]
 */
static inline uint16_t c_decode_caddi4spn_nzuimm(const uint16_t insn)
{
    uint16_t tmp = 0;
    tmp |= (insn & 0x1800) >> 7;
    tmp |= (insn & 0x780) >> 1;
    tmp |= (insn & 0x40) >> 4;
    tmp |= (insn & 0x20) >> 2;
    return tmp;
}

/* 解码 C.ADDI16SP 的 nzimm 字段。
 * nzimm[9] = inst[12]
 * nzimm[4|6|8:7|5] = inst[6:2]
 */
static inline int32_t c_decode_caddi16sp_nzimm(const uint16_t insn)
{
    int32_t tmp = (insn & 0x1000) >> 3;
    tmp |= (insn & 0x40) >> 2;
    tmp |= (insn & 0x20) << 1;
    tmp |= (insn & 0x18) << 4;
    tmp |= (insn & 0x4) << 3;
    return (tmp & 0x200) ? (0xfffffc00 | tmp) : (uint32_t) tmp;
}

/* 解码 C.LUI 的 nzimm 字段。
 * nzimm[17] = inst[12]
 * nzimm[16:12] = inst[6:2]
 */
static inline uint32_t c_decode_clui_nzimm(const uint16_t insn)
{
    uint32_t tmp = (insn & 0x1000) << 5 | (insn & 0x7c) << 10;
    return (tmp & 0x20000) ? (0xfffc0000 | tmp) : tmp;
}

static inline int32_t c_decode_caddi_imm(const uint16_t insn)
{
    int32_t tmp = 0;
    uint16_t mask = (0x1000 & insn) << 3;
    for (int i = 0; i <= 10; ++i)
        tmp |= (mask >> i);
    tmp |= (insn & 0x007C) >> 2;
    return sign_extend_h(tmp);
}

/* 解码 CI 格式指令立即数。
 * imm[5] = inst[12]
 * imm[4:0] = inst[6:2]
 */
static inline int32_t c_decode_citype_imm(const uint16_t insn)
{
    uint32_t tmp = ((insn & FCI_IMM_12) >> 7) | ((insn & FCI_IMM_6_2) >> 2);
    return (tmp & 0x20) ? (int32_t) (0xffffffc0 | tmp) : (int32_t) tmp;
}

/* 解码 CJ 格式指令立即数。
 * imm[11] = inst[12]
 * imm[10] = inst[8]
 * imm[9:8] = inst[10:9]
 * imm[7] = inst[6]
 * imm[6] = inst[7]
 * imm[5] = inst[2]
 * imm[4] = inst[11]
 * imm[3:1] = inst[5:3]
 */
static inline int32_t c_decode_cjtype_imm(const uint16_t insn)
{
    uint16_t tmp = 0;
    tmp |= (insn & CJ_IMM_3_1) >> 2;
    tmp |= (insn & CJ_IMM_4) >> 7;
    tmp |= (insn & CJ_IMM_5) << 3;
    tmp |= (insn & CJ_IMM_6) >> 1;
    tmp |= (insn & CJ_IMM_7) << 1;
    tmp |= (insn & CJ_IMM_9_8) >> 1;
    tmp |= (insn & CJ_IMM_10) << 2;
    tmp |= (insn & CJ_IMM_11) >> 1;

    for (int i = 1; i <= 4; ++i)
        tmp |= (0x0800 & tmp) << i;

    /* 扩展到 16 位。 */
    return (int32_t) (int16_t) tmp;
}

/* 解码 CB 格式 shamt 字段。
 * shamt[5] = inst[12]
 * shamt[4:0] = inst[6:2]
 */
static inline uint8_t c_decode_cbtype_shamt(const uint16_t insn)
{
    uint8_t tmp = 0;
    tmp |= (insn & CB_SHAMT_5) >> 7;
    tmp |= (insn & CB_SHAMT_4_0) >> 2;
    return tmp;
}

/* 解码 CB 格式指令立即数。
 * imm[8] = inst[12]
 * imm[7:6] = inst[6:5]
 * imm[4:3] = inst[11:10]
 * imm[5] = inst[2]
 * imm[2:1] = inst[4:3]
 */
static inline uint16_t c_decode_cbtype_imm(const uint16_t insn)
{
    uint16_t tmp = 0;
    /*            ....xxxx....xxxx     */
    tmp |= (insn & 0b0000000000011000) >> 2;
    tmp |= (insn & 0b0000110000000000) >> 7;
    tmp |= (insn & 0b0000000000000100) << 3;
    tmp |= (insn & 0b0000000001100000) << 1;
    tmp |= (insn & 0b0001000000000000) >> 4;

    /* 扩展到 16 位。 */
    for (int i = 1; i <= 8; ++i)
        tmp |= (0x0100 & tmp) << i;
    return tmp;
}
#endif /* RV32_HAS(EXT_C) */

/* decode I-type
 *  31       20 19   15 14    12 11   7 6      0
 * | imm[11:0] |  rs1  | funct3 |  rd  | opcode |
 */
static inline void decode_itype(rv_insn_t *ir, const uint32_t insn)
{
    ir->imm = decode_itype_imm(insn);
    ir->rs1 = decode_rs1(insn);
    ir->rd = decode_rd(insn);
}

/* decode U-type
 *  31        12 11   7 6      0
 * | imm[31:12] |  rd  | opcode |
 */
static inline void decode_utype(rv_insn_t *ir, const uint32_t insn)
{
    ir->imm = decode_utype_imm(insn);
    ir->rd = decode_rd(insn);
}

/* decode S-type
 *  31       25 24   20 19   15 14    12 11       7 6      0
 * | imm[11:5] |  rs2  |  rs1  | funct3 | imm[4:0] | opcode |
 */
static inline void decode_stype(rv_insn_t *ir, const uint32_t insn)
{
    ir->imm = decode_stype_imm(insn);
    ir->rs2 = decode_rs2(insn);
    ir->rs1 = decode_rs1(insn);
}

/* decode R-type
 *  31    25 24   20 19   15 14    12 11   7 6      0
 * | funct7 |  rs2  |  rs1  | funct3 |  rd  | opcode |
 */
static inline void decode_rtype(rv_insn_t *ir, const uint32_t insn)
{
    ir->rs2 = decode_rs2(insn);
    ir->rs1 = decode_rs1(insn);
    ir->rd = decode_rd(insn);
}

/* decode B-type
 *     31     30     25   24 20 19 15 14    12 11       8     7     6      0
 * | imm[12] | imm[10:5] | rs2 | rs1 | funct3 | imm[4:1] | imm[11] | opcode |
 */
static inline void decode_btype(rv_insn_t *ir, const uint32_t insn)
{
    ir->imm = decode_btype_imm(insn);
    ir->rs2 = decode_rs2(insn);
    ir->rs1 = decode_rs1(insn);
}

/* decode J-type
 *     31     30       21     20    19        12 11   7 6      0
 * | imm[20] | imm[10:1] | imm[11] | imm[19:12] |  rd  | opcode |
 */
static inline void decode_jtype(rv_insn_t *ir, const uint32_t insn)
{
    ir->imm = decode_jtype_imm(insn);
    ir->rd = decode_rd(insn);
}

#if RV32_HAS(EXT_F)
/* decode R4-type
 *  31   27 26    25 24   20 19   15 14    12 11   7 6      0
 * |  rs3  | funct2 |  rs2  |  rs1  | funct3 |  rd  | opcode |
 */
static inline void decode_r4type(rv_insn_t *ir, const uint32_t insn)
{
    ir->rd = decode_rd(insn);
    ir->rs1 = decode_rs1(insn);
    ir->rs2 = decode_rs2(insn);
    ir->rs3 = decode_r4type_rs3(insn);
    ir->rm = decode_funct3(insn);
}
#endif

/* LOAD: I-type
 *  31       20 19   15 14    12 11   7 6      0
 * | imm[11:0] |  rs1  | funct3 |  rd  | opcode |
 */
static inline bool op_load(rv_insn_t *ir, const uint32_t insn)
{
    /* inst imm[11:0] rs1 funct3 rd opcode
     * ----+---------+---+------+--+-------
     * LB   imm[11:0] rs1 000    rd 0000011
     * LH   imm[11:0] rs1 001    rd 0000011
     * LW   imm[11:0] rs1 010    rd 0000011
     * LD   imm[11:0] rs1 011    rd 0000011
     * LBU  imm[11:0] rs1 100    rd 0000011
     * LHU  imm[11:0] rs1 101    rd 0000011
     * LWU  imm[11:0] rs1 110    rd 0000011
     */

    /* 解码 I-type。 */
    decode_itype(ir, insn);

    /* 根据 funct3 字段分派。 */
    switch (decode_funct3(insn)) {
    case 0: /* LB：加载字节。 */
        ir->opcode = rv_insn_lb;
        break;
    case 1: /* LH：加载半字。 */
        ir->opcode = rv_insn_lh;
        break;
    case 2: /* LW：加载字。 */
        ir->opcode = rv_insn_lw;
        break;
    case 4: /* LBU：无符号加载字节。 */
        ir->opcode = rv_insn_lbu;
        break;
    case 5: /* LHU：无符号加载半字。 */
        ir->opcode = rv_insn_lhu;
        break;
    default: /* 非法指令。 */
        return false;
    }
    return true;
}

/* OP-IMM: I-type
 *  31       20 19   15 14    12 11   7 6      0
 * | imm[11:0] |  rs1  | funct3 |  rd  | opcode |
 */
static inline bool op_op_imm(rv_insn_t *ir, const uint32_t insn)
{
    /* inst  imm[11:5] imm[4:0]   rs1 funct3 rd opcode
     * -----+---------+----------+---+------+--+-------
     * ADDI  imm[11:0]            rs1 000    rd 0010011
     * SLLI  0000000   shamt[4:0] rs1 001    rd 0010011
     * SLTI  imm[11:0]            rs1 010    rd 0010011
     * SLTIU imm[11:0]            rs1 011    rd 0010011
     * XORI  imm[11:0]            rs1 100    rd 0010011
     * SLRI  0000000   shamt[4:0] rs1 101    rd 0010011
     * SRAI  0100000   shamt[4:0] rs1 101    rd 0010011
     * ORI   imm[11:0]            rs1 110    rd 0010011
     * ANDI  imm[11:0]            rs1 111    rd 0010011
     */

    /* 解码 I-type。 */
    decode_itype(ir, insn);

    /* nop 可实现为 "addi x0, x0, 0"。
     * 任何写入 "x0" 的整数计算指令都等价于 NOP。
     */
    if (unlikely(ir->rd == rv_reg_zero)) {
        ir->opcode = rv_insn_nop;
        return true;
    }

    /* 根据 funct3 字段分派。 */
    switch (decode_funct3(insn)) {
    case 0: /* ADDI：立即数加法。 */
        ir->opcode = rv_insn_addi;
        break;
    case 1: /* SLLI：立即数逻辑左移。 */
#if RV32_HAS(Zbb)
        if (ir->imm == 0b011000000000) { /* clz */
            ir->opcode = rv_insn_clz;
            return true;
        }
        if (ir->imm == 0b011000000001) { /* ctz */
            ir->opcode = rv_insn_ctz;
            return true;
        }
        if (ir->imm == 0b011000000010) { /* cpop */
            ir->opcode = rv_insn_cpop;
            return true;
        }
        if (ir->imm == 0b011000000100) { /* sext.b */
            ir->opcode = rv_insn_sextb;
            return true;
        }
        if (ir->imm == 0b011000000101) { /* sext.h */
            ir->opcode = rv_insn_sexth;
            return true;
        }
#endif
#if RV32_HAS(Zbs)
        if (ir->imm >> 5 == 0b0100100) { /* bclri */
            ir->opcode = rv_insn_bclri;
            return true;
        }
        if (ir->imm >> 5 == 0b0110100) { /* binvi */
            ir->opcode = rv_insn_binvi;
            return true;
        }
        if (ir->imm >> 5 == 0b0010100) { /* bseti */
            ir->opcode = rv_insn_bseti;
            return true;
        }
#endif
        ir->opcode = rv_insn_slli;
        if (unlikely(ir->imm & (1 << 5)))
            return false;
        break;
    case 2: /* SLTI：有符号小于立即数则置位。 */
        ir->opcode = rv_insn_slti;
        break;
    case 3: /* SLTIU：无符号小于立即数则置位。 */
        ir->opcode = rv_insn_sltiu;
        break;
    case 4: /* XORI：立即数异或。 */
        ir->opcode = rv_insn_xori;
        break;
    case 5:
#if RV32_HAS(Zbb)
        if (ir->imm >> 5 == 0b0110000) { /* rori */
            ir->opcode = rv_insn_rori;
            return true;
        }
        if (ir->imm == 0b001010000111) { /* orc.b */
            ir->opcode = rv_insn_orcb;
            return true;
        }
        if (ir->imm == 0b011010011000) { /* rev8 */
            ir->opcode = rv_insn_rev8;
            return true;
        }
#endif
#if RV32_HAS(Zbs)
        if (ir->imm >> 5 == 0b0100100) { /* bexti */
            ir->opcode = rv_insn_bexti;
            return true;
        }
#endif
        /* SLL、SRL 和 SRA 分别对寄存器 rs1 的值执行逻辑左移、逻辑右移和算术右移。
         */
        ir->opcode = (ir->imm & ~0x1f)
                         ? rv_insn_srai  /* SRAI：算术右移。 */
                         : rv_insn_srli; /* SRLI：逻辑右移。 */
        if (unlikely(ir->imm & (1 << 5)))
            return false;
        break;
    case 6: /* ORI：立即数或。 */
        ir->opcode = rv_insn_ori;
        break;
    case 7: /* ANDI：立即数与。 */
        ir->opcode = rv_insn_andi;
        break;
    default: /* 非法指令。 */
        return false;
    }
    return true;
}

/* AUIPC: U-type
 *  31        12 11   7 6      0
 * | imm[31:12] |  rd  | opcode |
 */
static inline bool op_auipc(rv_insn_t *ir, const uint32_t insn)
{
    /* inst  imm[31:12] rd opcode
     * -----+----------+--+-------
     * AUPIC imm[31:12] rd 0010111
     */

    /* 解码 U-type。 */
    decode_utype(ir, insn);

    /* 任何写入 "x0" 的整数计算指令都等价于 NOP。 */
    if (unlikely(ir->rd == rv_reg_zero)) {
        ir->opcode = rv_insn_nop;
        return true;
    }

    ir->opcode = rv_insn_auipc;
    return true;
}

/* STORE: S-type
 *  31       25 24   20 19   15 14    12 11       7 6      0
 * | imm[11:5] |  rs2  |  rs1  | funct3 | imm[4:0] | opcode |
 */
static inline bool op_store(rv_insn_t *ir, const uint32_t insn)
{
    /* inst imm[11:5] rs2 rs1 funct3 imm[4:0] opcode
     * ----+---------+---+---+------+--------+-------
     * SB   imm[11:5] rs2 rs1 000    imm[4:0] 0100011
     * SH   imm[11:5] rs2 rs1 001    imm[4:0] 0100011
     * SW   imm[11:5] rs2 rs1 010    imm[4:0] 0100011
     * SD   imm[11:5] rs2 rs1 011    imm[4:0] 0100011
     */

    /* 解码 S-type。 */
    decode_stype(ir, insn);

    /* 根据 funct3 字段分派。 */
    switch (decode_funct3(insn)) {
    case 0: /* SB：存储字节。 */
        ir->opcode = rv_insn_sb;
        break;
    case 1: /* SH：存储半字。 */
        ir->opcode = rv_insn_sh;
        break;
    case 2: /* SW：存储字。 */
        ir->opcode = rv_insn_sw;
        break;
    default: /* 非法指令。 */
        return false;
    }
    return true;
}

/* OP: R-type
 *  31    25 24   20 19   15 14    12 11   7 6      0
 * | funct7 |  rs2  |  rs1  | funct3 |  rd  | opcode |
 */
static inline bool op_op(rv_insn_t *ir, const uint32_t insn)
{
    /* inst funct7  rs2 rs1 funct3 rd opcode
     * ----+-------+---+---+------+--+-------
     * ADD  0000000 rs2 rs1 000    rd 0110011
     * SUB  0100000 rs2 rs1 000    rd 0110011
     * SLL  0000000 rs2 rs1 001    rd 0110011
     * SLT  0000000 rs2 rs1 010    rd 0110011
     * SLTU 0000000 rs2 rs1 011    rd 0110011
     * XOR  0000000 rs2 rs1 100    rd 0110011
     * SRL  0000000 rs2 rs1 101    rd 0110011
     * SRA  0100000 rs2 rs1 101    rd 0110011
     * OR   0000000 rs2 rs1 110    rd 0110011
     * AND  0000000 rs2 rs1 111    rd 0110011
     */

    /* 解码 R-type。 */
    decode_rtype(ir, insn);

    /* nop 可实现为 "add x0, x1, x2"。 */
    if (unlikely(ir->rd == rv_reg_zero)) {
        ir->opcode = rv_insn_nop;
        return true;
    }

    uint8_t funct3 = decode_funct3(insn);

    /* 根据 funct7 字段分派。 */
    switch (decode_funct7(insn)) {
    case 0b0000000:
        switch (funct3) {
        case 0b000: /* ADD */
            ir->opcode = rv_insn_add;
            break;
        case 0b001: /* SLL：逻辑左移。 */
            ir->opcode = rv_insn_sll;
            break;
        case 0b010: /* SLT：有符号小于则置位。 */
            ir->opcode = rv_insn_slt;
            break;
        case 0b011: /* SLTU：无符号小于则置位。 */
            ir->opcode = rv_insn_sltu;
            break;
        case 0b100: /* XOR：按位异或。 */
            ir->opcode = rv_insn_xor;
            break;
        case 0b101: /* SRL：逻辑右移。 */
            ir->opcode = rv_insn_srl;
            break;
        case 0b110: /* OR */
            ir->opcode = rv_insn_or;
            break;
        case 0b111: /* AND */
            ir->opcode = rv_insn_and;
            break;
        default: /* 非法指令。 */
            return false;
        }
        break;

#if RV32_HAS(EXT_M)
    /* inst   funct7  rs2 rs1 funct3 rd opcode
     * ------+-------+---+---+------+--+-------
     * MUL    0000001 rs2 rs1 000    rd 0110011
     * MULH   0000001 rs2 rs1 001    rd 0110011
     * MULHSU 0000001 rs2 rs1 010    rd 0110011
     * MULHU  0000001 rs2 rs1 011    rd 0110011
     * DIV    0000001 rs2 rs1 100    rd 0110011
     * DIVU   0000001 rs2 rs1 101    rd 0110011
     * REM    0000001 rs2 rs1 110    rd 0110011
     * REMU   0000001 rs2 rs1 111    rd 0110011
     */
    case 0b0000001: /* RV32M 指令。 */
        switch (funct3) {
        case 0b000: /* MUL：乘法。 */
            ir->opcode = rv_insn_mul;
            break;
        case 0b001: /* MULH：有符号 x 有符号乘法高位。 */
            ir->opcode = rv_insn_mulh;
            break;
        case 0b010: /* MULHSU：有符号 x 无符号乘法高位。 */
            ir->opcode = rv_insn_mulhsu;
            break;
        case 0b011: /* MULHU：无符号 x 无符号乘法高位。 */
            ir->opcode = rv_insn_mulhu;
            break;
        case 0b100: /* DIV：有符号除法。 */
            ir->opcode = rv_insn_div;
            break;
        case 0b101: /* DIVU：无符号除法。 */
            ir->opcode = rv_insn_divu;
            break;
        case 0b110: /* REM：有符号取余。 */
            ir->opcode = rv_insn_rem;
            break;
        case 0b111: /* REMU：无符号取余。 */
            ir->opcode = rv_insn_remu;
            break;
        default: /* 非法指令。 */
            return false;
        }
        break;
#endif /* RV32_HAS(EXT_M) */

#if RV32_HAS(Zba)
    /* inst   funct7  rs2 rs1 funct3 rd opcode
     * ------+-------+---+---+------+--+-------
     * SH1ADD 0010000 rs2 rs1 010    rd 0110011
     * SH2ADD 0010000 rs2 rs1 100    rd 0110011
     * SH3ADD 0010000 rs2 rs1 110    rd 0110011
     */
    case 0b0010000:
        switch (funct3) {
        case 0b010: /* sh1add */
            ir->opcode = rv_insn_sh1add;
            break;
        case 0b100: /* sh2add */
            ir->opcode = rv_insn_sh2add;
            break;
        case 0b110: /* sh3add */
            ir->opcode = rv_insn_sh3add;
            break;
        default: /* 非法指令。 */
            return false;
        }
        break;
#endif /* RV32_HAS(Zba) */

#if RV32_HAS(Zbb) || RV32_HAS(Zbc)
    /* inst   funct7  rs2 rs1 funct3 rd opcode
     * ------+-------+---+---+------+--+-------
     * MAX    0000101 rs2 rs1 110    rd 0110011
     * MIN    0000101 rs2 rs1 100    rd 0110011
     * MAXU   0000101 rs2 rs1 111    rd 0110011
     * MINU   0000101 rs2 rs1 101    rd 0110011
     * ROL    0110000 rs2 rs1 001    rd 0110011
     * ROR    0110000 rs2 rs1 101    rd 0110011
     */
    case 0b0000101:
        switch (funct3) {
#if RV32_HAS(Zbb)
        case 0b110: /* max */
            ir->opcode = rv_insn_max;
            break;
        case 0b100: /* min */
            ir->opcode = rv_insn_min;
            break;
        case 0b111: /* maxu */
            ir->opcode = rv_insn_maxu;
            break;
        case 0b101: /* minu */
            ir->opcode = rv_insn_minu;
            break;
#endif
#if RV32_HAS(Zbc)
        case 0b001: /*clmul */
            ir->opcode = rv_insn_clmul;
            break;
        case 0b011: /*clmulh */
            ir->opcode = rv_insn_clmulh;
            break;
        case 0b010: /*clmulr */
            ir->opcode = rv_insn_clmulr;
            break;
#endif
        default: /* 非法指令。 */
            return false;
        }
        break;
#endif
#if RV32_HAS(Zbb)
    case 0b0110000:
        switch (funct3) {
        case 0b001: /* rol */
            ir->opcode = rv_insn_rol;
            break;
        case 0b101: /* ror */
            ir->opcode = rv_insn_ror;
            break;
        default: /* 非法指令。 */
            return false;
        }
        break;
    case 0b0000100:
        if (unlikely(ir->rs2))
            return false;
        ir->opcode = rv_insn_zexth;
        break;
#endif /* RV32_HAS(Zbb) */

#if RV32_HAS(Zbs)
    case 0b0100100:
        switch (funct3) {
        case 0b001: /* bclr */
            ir->opcode = rv_insn_bclr;
            break;
        case 0b101: /* bext */
            ir->opcode = rv_insn_bext;
            break;
        default: /* 非法指令。 */
            return false;
        }
        break;
    case 0b0110100:
        if (unlikely(funct3 != 0b001))
            return false;
        ir->opcode = rv_insn_binv;
        break;
    case 0b0010100:
        if (unlikely(funct3 != 0b001))
            return false;
        ir->opcode = rv_insn_bset;
        break;
#endif /* RV32_HAS(Zbs) */

    case 0b0100000:
        switch (funct3) {
        case 0b000: /* SUB：减法。 */
            ir->opcode = rv_insn_sub;
            break;
        case 0b101: /* SRA：算术右移。 */
            ir->opcode = rv_insn_sra;
            break;
#if RV32_HAS(Zbb)
        case 0b111: /* ANDN */
            ir->opcode = rv_insn_andn;
            break;
        case 0b110: /* ORN */
            ir->opcode = rv_insn_orn;
            break;
        case 0b100: /* XNOR */
            ir->opcode = rv_insn_xnor;
            break;
#endif /* RV32_HAS(Zbb) */

        default: /* 非法指令。 */
            return false;
        }
        break;
    default: /* 非法指令。 */
        return false;
    }
    return true;
}

/* LUI: U-type
 *  31        12 11   7 6      0
 * | imm[31:12] |  rd  | opcode |
 */
static inline bool op_lui(rv_insn_t *ir, const uint32_t insn)
{
    /* inst imm[31:12] rd opcode
     * ----+----------+--+-------
     * LUI  imm[31:12] rd 0110111
     */

    /* 解码 U-type。 */
    decode_utype(ir, insn);

    /* 任何写入 "x0" 的整数计算指令都等价于 NOP。 */
    if (unlikely(ir->rd == rv_reg_zero)) {
        ir->opcode = rv_insn_nop;
        return true;
    }

    ir->opcode = rv_insn_lui;
    return true;
}

/* Branch: B-type
 *     31     30     25   24 20 19 15 14    12 11       8     7     6      0
 * | imm[12] | imm[10:5] | rs2 | rs1 | funct3 | imm[4:1] | imm[11] | opcode |
 */
static inline bool op_branch(rv_insn_t *ir, const uint32_t insn)
{
    /* inst imm[12] imm[10:5] rs2 rs1 funct3 imm[4:1] imm[11] opcode
     * ----+-------+---------+---+---+------+--------+-------+-------
     * BEQ  imm[12  imm[10:5] rs2 rs1 000    imm[4:1  imm[11] 1100011
     * BNE  imm[12  imm[10:5] rs2 rs1 001    imm[4:1  imm[11] 1100011
     * BLT  imm[12  imm[10:5] rs2 rs1 100    imm[4:1  imm[11] 1100011
     * BGE  imm[12  imm[10:5] rs2 rs1 101    imm[4:1  imm[11] 1100011
     * BLTU imm[12  imm[10:5] rs2 rs1 110    imm[4:1  imm[11] 1100011
     * BGEU imm[12  imm[10:5] rs2 rs1 111    imm[4:1  imm[11] 1100011
     */

    /* 解码 B-type。 */
    decode_btype(ir, insn);

    /* 根据 funct3 字段分派。 */
    switch (decode_funct3(insn)) {
    case 0: /* BEQ：相等则分支。 */
        ir->opcode = rv_insn_beq;
        break;
    case 1: /* BNE：不相等则分支。 */
        ir->opcode = rv_insn_bne;
        break;
    case 4: /* BLT：有符号小于则分支。 */
        ir->opcode = rv_insn_blt;
        break;
    case 5: /* BGE：有符号大于等于则分支。 */
        ir->opcode = rv_insn_bge;
        break;
    case 6: /* BLTU：无符号小于则分支。 */
        ir->opcode = rv_insn_bltu;
        break;
    case 7: /* BGEU：无符号大于等于则分支。 */
        ir->opcode = rv_insn_bgeu;
        break;
    default: /* 非法指令。 */
        return false;
    }
    return true;
}

/* JALR: I-type
 *  31       20 19   15 14    12 11   7 6      0
 * | imm[11:0] |  rs1  | funct3 |  rd  | opcode |
 */
static inline bool op_jalr(rv_insn_t *ir, const uint32_t insn)
{
    /* inst imm[11:0] rs1 funct3 rd opcode
     * ----+---------+---+------+--+-------
     * JALR imm[11:0] rs1 000    rd 1100111
     */

    /* decode I-type */
    decode_itype(ir, insn);

    ir->opcode = rv_insn_jalr;
    return true;
}

/* JAL: J-type
 *     31     30       21     20    19        12 11   7 6      0
 * | imm[20] | imm[10:1] | imm[11] | imm[19:12] |  rd  | opcode |
 */
static inline bool op_jal(rv_insn_t *ir, const uint32_t insn)
{
    /* inst imm[20] imm[10:1] imm[11] imm[19:12] rd opcode
     * ----+-------+---------+-------+----------+--+-------
     * JALR imm[20] imm[10:1] imm[11] imm[19:12] rd 1101111
     */

    /* decode J-type */
    decode_jtype(ir, insn);

    ir->opcode = rv_insn_jal;
    return true;
}

FORCE_INLINE bool csr_is_writable(const uint32_t csr)
{
    return csr < 0xc00;
}

/* SYSTEM: I-type
 *  31       20 19   15 14    12 11   7 6      0
 * | imm[11:0] |  rs1  | funct3 |  rd  | opcode |
 */
static inline bool op_system(rv_insn_t *ir, const uint32_t insn)
{
    /* inst   imm[11:0]    rs1   funct3 rd    opcode
     * ------+------------+-----+------+-----+-------
     * ECALL  000000000000 00000 000    00000 1110011
     * EBREAK 000000000001 00000 000    00000 1110011
     * WFI    000100000101 00000 000    00000 1110011
     * URET   000000000010 00000 000    00000 1110011
     * SRET   000100000010 00000 000    00000 1110011
     * HRET   001000000010 00000 000    00000 1110011
     * MRET   001100000010 00000 000    00000 1110011
     */

    /* inst        funct7  rs2 rs1 funct3 rd     opcode
     * -----------+-------+---+---+------+------+-------
     * SFENCE.VMA  0001001 rs2 rs1  000   00000  1110011
     */

    /* 解码 I-type。 */
    decode_itype(ir, insn);

    /* 根据 funct3 字段分派。 */
    switch (decode_funct3(insn)) {
    case 0:
        if ((insn >> 25) == 0b0001001) { /* SFENCE.VMA */
            ir->opcode = rv_insn_sfencevma;
            break;
        }

        /* 根据 imm 字段分派。 */
        switch (ir->imm) {
        case 0: /* ECALL：环境调用。 */
            ir->opcode = rv_insn_ecall;
            break;
        case 1: /* EBREAK：环境断点。 */
            ir->opcode = rv_insn_ebreak;
            break;
        case 0x105: /* WFI：等待中断。 */
            ir->opcode = rv_insn_wfi;
            break;
        case 0x002: /* URET：从 U-mode trap 返回。 */
        case 0x202: /* HRET：从 H-mode trap 返回。 */
            /* 非法指令。 */
            return false;
#if RV32_HAS(SYSTEM)
        case 0x102: /* SRET：从 S-mode trap 返回。 */
            ir->opcode = rv_insn_sret;
            break;
#endif
        case 0x302: /* MRET */
            ir->opcode = rv_insn_mret;
            break;
        default: /* 非法指令。 */
            return false;
        }
        break;

#if RV32_HAS(Zicsr)
    /* 所有 CSR 指令都会对单个 CSR 执行原子读-改-写。
     * 寄存器操作数
     * ---------------------------------------------------------
     * Instruction         rd          rs1        Read    Write
     * -------------------+-----------+----------+-------+------
     * CSRRW               x0          -          no      yes
     * CSRRW               !x0         -          yes     yes
     * CSRRS/C             -           x0         yes     no
     * CSRRS/C             -           !x0        yes     yes
     *
     * 立即数操作数
     * --------------------------------------------------------
     * Instruction         rd          uimm       Read    Write
     * -------------------+-----------+----------+-------+------
     * CSRRWI              x0          -          no      yes
     * CSRRWI              !x0         -          yes     yes
     * CSRRS/CI            -           0          yes     no
     * CSRRS/CI            -           !0         yes     yes
     */

    /* inst   imm[11:0] rs1  funct3 rd opcode
     * ------+---------+----+------+--+--------
     * CSRRW  csr       rs1  001    rd 1110011
     * CSRRS  csr       rs1  010    rd 1110011
     * CSRRC  csr       rs1  011    rd 1110011
     * CSRRWI csr       uimm 101    rd 1110011
     * CSRRSI csr       uimm 110    rd 1110011
     * CSRRCI csr       uimm 111    rd 1110011
     */
    case 1: /* CSRRW：原子读写 CSR。 */
        ir->opcode = rv_insn_csrrw;
        break;
    case 2: /* CSRRS：原子读取 CSR 并置位。 */
        ir->opcode = rv_insn_csrrs;
        break;
    case 3: /* CSRRC：原子读取 CSR 并清位。 */
        ir->opcode = rv_insn_csrrc;
        break;
    case 5: /* CSRRWI */
        ir->opcode = rv_insn_csrrwi;
        break;
    case 6: /* CSRRSI */
        ir->opcode = rv_insn_csrrsi;
        break;
    case 7: /* CSRRCI */
        ir->opcode = rv_insn_csrrci;
        break;
#endif /* RV32_HAS(Zicsr) */

    default: /* 非法指令。 */
        return false;
    }

    return csr_is_writable(ir->imm) || (ir->rs1 == rv_reg_zero);
}

/* MISC-MEM: I-type
 *  31       20 19   15 14    12 11   7 6      0
 * | imm[11:0] |  rs1  | funct3 |  rd  | opcode |
 */
static inline bool op_misc_mem(rv_insn_t *ir, const uint32_t insn)
{
    /* inst      fm       pred      succ       rs1   funct3  rd   opcode
     * ------+---------+----------+-----------+-----+-------+----+-------
     * FENCE   FM[3:0]   pred[3:0]  succ[3:0]  rs1   000     rd   0001111
     * FENCEI            imm[11:0]             rs1   001     rd   0001111
     */

    const uint32_t funct3 = decode_funct3(insn);

    switch (funct3) {
    case 0b000:
        ir->opcode = rv_insn_fence;
        return true;
#if RV32_HAS(Zifencei)
    case 0b001:
        ir->opcode = rv_insn_fencei;
        return true;
#endif /* RV32_HAS(Zifencei) */
    default:
        return false;
    }
}

#if RV32_HAS(EXT_A)
/* AMO: R-type
 *  31    27  26   25  24   20 19   15 14    12 11   7 6      0
 * | funct5 | aq | rl |  rs2  |  rs1  | funct3 |  rd  | opcode |
 */
static inline bool op_amo(rv_insn_t *ir, const uint32_t insn)
{
    /* inst      funct5 aq rl rs2   rs1 funct3 rd  opcode
     * ---------+------+--+--+-----+---+------+---+-------
     * LR.W      00010  aq rl 00000 rs1 010    rd  0101111
     * SC.W      00011  aq rl rs2   rs1 010    rd  0101111
     * AMOSWAP.W 00001  aq rl rs2   rs1 010    rd  0101111
     * AMOADD.W  00000  aq rl rs2   rs1 010    rd  0101111
     * AMOXOR.W  00100  aq rl rs2   rs1 010    rd  0101111
     * AMOAND.W  01100  aq rl rs2   rs1 010    rd  0101111
     * AMOOR.W   01000  aq rl rs2   rs1 010    rd  0101111
     * AMOMIN.W  10000  aq rl rs2   rs1 010    rd  0101111
     * AMOMAX.W  10100  aq rl rs2   rs1 010    rd  0101111
     * AMOMINU.W 11000  aq rl rs2   rs1 010    rd  0101111
     * AMOMAXU.W 11100  aq rl rs2   rs1 010    rd  0101111
     */

    /* 解码 R-type。 */
    decode_rtype(ir, insn);

    /* 计算 funct5 字段。 */
    const uint32_t funct5 = (decode_funct7(insn) >> 2) & 0x1f;

    /* 根据 funct5 字段分派。 */
    switch (funct5) {
    case 0b00010: /* LR.W：保留加载。 */
        ir->opcode = rv_insn_lrw;
        break;
    case 0b00011: /* SC.W：条件存储。 */
        ir->opcode = rv_insn_scw;
        break;
    case 0b00001: /* AMOSWAP.W：原子交换。 */
        ir->opcode = rv_insn_amoswapw;
        break;
    case 0b00000: /* AMOADD.W：原子加。 */
        ir->opcode = rv_insn_amoaddw;
        break;
    case 0b00100: /* AMOXOR.W：原子异或。 */
        ir->opcode = rv_insn_amoxorw;
        break;
    case 0b01100: /* AMOAND.W：原子与。 */
        ir->opcode = rv_insn_amoandw;
        break;
    case 0b01000: /* AMOOR.W：原子或。 */
        ir->opcode = rv_insn_amoorw;
        break;
    case 0b10000: /* AMOMIN.W：原子有符号最小值。 */
        ir->opcode = rv_insn_amominw;
        break;
    case 0b10100: /* AMOMAX.W：原子有符号最大值。 */
        ir->opcode = rv_insn_amomaxw;
        break;
    case 0b11000: /* AMOMINU.W */
        ir->opcode = rv_insn_amominuw;
        break;
    case 0b11100: /* AMOMAXU.W */
        ir->opcode = rv_insn_amomaxuw;
        break;
    default: /* 非法指令。 */
        return false;
    }
    return true;
}
#else
#define op_amo OP_UNIMP
#endif /* RV32_HAS(EXT_A) */

#if RV32_HAS(EXT_F)
/* LOAD-FP: I-type
 *  31       20 19   15 14   12 11   7 6      0
 * | imm[11:0] |  rs1  | width |  rd  | opcode |
 */
static inline bool op_load_fp(rv_insn_t *ir, const uint32_t insn)
{
    /* inst imm[11:0] rs1 width rd opcode
     * ----+---------+---+-----+--+-------
     * FLW  imm[11:0] rs1 010   rd 0000111
     */

    /* 解码 I-type。 */
    decode_itype(ir, insn);

    ir->opcode = rv_insn_flw;
    return true;
}

/* STORE-FP: S-type
 *  31       25 24   20 19   15 14   12 11       7 6      0
 * | imm[11:5] |  rs2  |  rs1  | width | imm[4:0] | opcode |
 */
static inline bool op_store_fp(rv_insn_t *ir, const uint32_t insn)
{
    /* inst imm[11:5] rs2 rs1 width imm[4:0] opcode
     * ----+---------+---+---+-----+--------+-------
     * FSW  imm[11:5] rs2 rs1 010   imm[4:0] 0100111
     */

    /* 解码 S-type。 */
    decode_stype(ir, insn);

    ir->opcode = rv_insn_fsw;
    return true;
}

/* OP-FP: R-type
 *  31    27 26   25 24   20 19   15 14    12 11   7 6      0
 * | funct5 |  fmt  |  rs2  |  rs1  |   rm   |  rd  | opcode |
 */
static inline bool op_op_fp(rv_insn_t *ir, const uint32_t insn)
{
    /* inst      funct7  rs2   rs1 rm  rd opcode
     * ---------+-------+-----+---+---+--+-------
     * FADD.S    0000000 rs2   rs1 rm  rd 1010011
     * FSUB.S    0000100 rs2   rs1 rm  rd 1010011
     * FMUL.S    0001000 rs2   rs1 rm  rd 1010011
     * FDIV.S    0001100 rs2   rs1 rm  rd 1010011
     * FSQRT.S   0101100 00000 rs1 rm  rd 1010011
     * FMV.W.X   1111000 00000 rs1 000 rd 1010011
     * FSGNJ.S   0010000 rs2   rs1 000 rd 1010011
     * FSGNJN.S  0010000 rs2   rs1 001 rd 1010011
     * FSGNJX.S  0010000 rs2   rs1 010 rd 1010011
     * FCVT.W.S  1100000 00000 rs1 rm  rd 1010011
     * FCVT.WU.S 1100000 00001 rs1 rm  rd 1010011
     * FMIN.S    0010100 rs2   rs1 000 rd 1010011
     * FMAX.S    0010100 rs2   rs1 001 rd 1010011
     * FMV.X.W   1110000 00000 rs1 000 rd 1010011
     * FCLASS.S  1110000 00000 rs1 001 rd 1010011
     * FEQ.S     1010000 rs2   rs1 010 rd 1010011
     * FLT.S     1010000 rs2   rs1 001 rd 1010011
     * FLE.S     1010000 rs2   rs1 000 rd 1010011
     * FCVT.S.W  1101000 00000 rs1 rm  rd 1010011
     * FCVT.S.WU 1101000 00001 rs1 rm  rd 1010011
     */

    /* 解码 R-type。 */
    ir->rm = decode_funct3(insn);
    decode_rtype(ir, insn);

    /* 根据 funct7 字段分派。 */
    switch (decode_funct7(insn)) {
    case 0b0000000: /* FADD.S */
        ir->opcode = rv_insn_fadds;
        break;
    case 0b0000100: /* FSUB.S */
        ir->opcode = rv_insn_fsubs;
        break;
    case 0b0001000: /* FMUL.S */
        ir->opcode = rv_insn_fmuls;
        break;
    case 0b0001100: /* FDIV.S */
        ir->opcode = rv_insn_fdivs;
        break;
    case 0b0101100: /* FSQRT.S */
        ir->opcode = rv_insn_fsqrts;
        break;
    case 0b0010000:
        /* 根据 rm 区域分派。 */
        switch (ir->rm) {
        case 0b000: /* FSGNJ.S */
            ir->opcode = rv_insn_fsgnjs;
            break;
        case 0b001: /* FSGNJN.S */
            ir->opcode = rv_insn_fsgnjns;
            break;
        case 0b010: /* FSGNJX.S */
            ir->opcode = rv_insn_fsgnjxs;
            break;
        default: /* 非法指令。 */
            return false;
        }
        break;
    case 0b1100000:
        /* 根据 rs2 区域分派。 */
        switch (ir->rs2) {
        case 0b00000: /* FCVT.W.S */
            ir->opcode = rv_insn_fcvtws;
            break;
        case 0b00001: /* FCVT.WU.S */
            ir->opcode = rv_insn_fcvtwus;
            break;
        default: /* 非法指令。 */
            return false;
        }
        break;
    case 0b0010100:
        /* 根据 rm 区域分派。 */
        switch (ir->rm) {
        case 0b000: /* FMIN.S */
            ir->opcode = rv_insn_fmins;
            break;
        case 0b001: /* FMAX.S */
            ir->opcode = rv_insn_fmaxs;
            break;
        default: /* 非法指令。 */
            return false;
        }
        break;
    case 0b1110000:
        /* 根据 rm 区域分派。 */
        switch (ir->rm) {
        case 0b000: /* FMV.X.W */
            ir->opcode = rv_insn_fmvxw;
            break;
        case 0b001: /* FCLASS.S */
            ir->opcode = rv_insn_fclasss;
            break;
        default: /* 非法指令。 */
            return false;
        }
        break;
    case 0b1010000:
        /* 根据 rm 区域分派。 */
        switch (ir->rm) {
        case 0b010: /* FEQ.S */
            ir->opcode = rv_insn_feqs;
            break;
        case 0b001: /* FLT.S */
            ir->opcode = rv_insn_flts;
            break;
        case 0b000: /* FLE.S */
            ir->opcode = rv_insn_fles;
            break;
        default: /* 非法指令。 */
            return false;
        }
        break;
    case 0b1101000:
        /* 根据 rs2 区域分派。 */
        switch (ir->rs2) {
        case 0b00000: /* FCVT.S.W */
            ir->opcode = rv_insn_fcvtsw;
            break;
        case 0b00001: /* FCVT.S.WU */
            ir->opcode = rv_insn_fcvtswu;
            break;
        default: /* 非法指令。 */
            return false;
        }
        break;
    case 0b1111000: /* FMV.W.X */
        ir->opcode = rv_insn_fmvwx;
        break;
    default: /* 非法指令。 */
        return false;
    }
    return true;
}

/* F-MADD: R4-type
 *  31   27 26   25 24   20 19   15 14    12 11   7 6      0
 * |  rs3  |  fmt  |  rs2  |  rs1  |   rm   |  rd  | opcode |
 */
static inline bool op_madd(rv_insn_t *ir, const uint32_t insn)
{
    /* inst    rs3 fmt rs2 rs1 rm rd opcode
     * -------+---+---+---+---+--+--+-------
     * FMADD.S rs3 00  rs2 rs1 rm rd 1000011
     */

    /* 解码 R4-type。 */
    decode_r4type(ir, insn);

    ir->opcode = rv_insn_fmadds;
    return true;
}

/* F-MSUB: R4-type
 *  31   27 26   25 24   20 19   15 14    12 11   7 6      0
 * |  rs3  |  fmt  |  rs2  |  rs1  |   rm   |  rd  | opcode |
 */
static inline bool op_msub(rv_insn_t *ir, const uint32_t insn)
{
    /* inst    rs3 fmt rs2 rs1 rm rd opcode
     * -------+---+---+---+---+--+--+-------
     * FMSUB.S rs3 00  rs2 rs1 rm rd 1000111
     */

    /* 解码 R4-type。 */
    decode_r4type(ir, insn);

    ir->opcode = rv_insn_fmsubs;
    return true;
}

/* F-NMADD: R4-type
 *  31   27 26   25 24   20 19   15 14    12 11   7 6      0
 * |  rs3  |  fmt  |  rs2  |  rs1  |   rm   |  rd  | opcode |
 */
static inline bool op_nmadd(rv_insn_t *ir, const uint32_t insn)
{
    /* inst    rs3 fmt rs2 rs1 rm rd opcode
     * -------+---+---+---+---+--+--+-------
     * FMSUB.S rs3 00  rs2 rs1 rm rd 1001111
     */

    /* 解码 R4-type。 */
    decode_r4type(ir, insn);

    ir->opcode = rv_insn_fnmadds;
    return true;
}

/* F-NMSUB: R4-type
 *  31   27 26   25 24   20 19   15 14    12 11   7 6      0
 * |  rs3  |  fmt  |  rs2  |  rs1  |   rm   |  rd  | opcode |
 */
static inline bool op_nmsub(rv_insn_t *ir, const uint32_t insn)
{
    /* inst    rs3 fmt rs2 rs1 rm rd opcode
     * -------+---+---+---+---+--+--+-------
     * FMSUB.S rs3 00  rs2 rs1 rm rd 1001011
     */

    /* 解码 R4-type。 */
    decode_r4type(ir, insn);

    ir->opcode = rv_insn_fnmsubs;
    return true;
}

#else /* !RV32_HAS(EXT_F) */
#define op_load_fp OP_UNIMP
#define op_store_fp OP_UNIMP
#define op_op_fp OP_UNIMP
#define op_madd OP_UNIMP
#define op_msub OP_UNIMP
#define op_nmadd OP_UNIMP
#define op_nmsub OP_UNIMP
#endif /* RV32_HAS(EXT_F) */

#if RV32_HAS(EXT_C)
/* C.ADDI: CI-format
 *  15    13    12    11       7 6        2 1  0
 * | funct3 | imm[5] |  rd/rs1  | imm[4:0] | op |
 */
static inline bool op_caddi(rv_insn_t *ir, const uint32_t insn)
{
    /* inst   funct3 imm[5]   rd/rs1    imm[4:0]   op
     * ------+------+--------+---------+----------+--
     * C.NOP  000    nzimm[5] 00000     nzimm[4:0] 01
     * C.ADDI 000    nzimm[5] rs1/rd!=0 nzimm[4:0] 01
     */

    ir->rd = c_decode_rd(insn);

    /* 根据 rd/rs1 字段分派。 */
    switch (ir->rd) {
    case 0: /* C.NOP */
        ir->opcode = rv_insn_cnop;
        break;
    default: /* C.ADDI */
        /* 将 6 位有符号立即数加到 rds；对 X0 寄存器则作为 NOP。 */
        ir->imm = c_decode_citype_imm(insn);
        ir->opcode = rv_insn_caddi;
        break;
    }
    return true;
}

/* C.ADDI4SPN: CIW-format
 *  15    13 12    5 4   2 1  0
 * | funct3 |  imm  | rd' | op |
 */
static inline bool op_caddi4spn(rv_insn_t *ir, const uint32_t insn)
{
    /* inst       funct3 imm                 rd' op
     * ----------+------+-------------------+---+--
     * C.ADDI4SPN 000    nzuimm[5:4|9:6|2|3] rd' 00
     */

    ir->imm = c_decode_caddi4spn_nzuimm(insn);
    ir->rd = c_decode_rdc(insn) | 0x08;

    /* 编码点：nzuimm = 0 为保留编码。 */
    if (!ir->imm)
        return false;
    ir->opcode = rv_insn_caddi4spn;
    return true;
}

/* C.LI: CI-format
 *  15    13    12    11       7 6        2 1  0
 * | funct3 | imm[5] |  rd/rs1  | imm[4:0] | op |
 */
static inline bool op_cli(rv_insn_t *ir, const uint32_t insn)
{
    /* inst funct3 imm[5] rd/rs1    imm[4:0] op
     * ----+------+------+---------+--------+--
     * C.LI 010    imm[5] rs1/rd!=0 imm[4:0] 01
     */

    ir->imm = c_decode_citype_imm(insn);
    ir->rd = c_decode_rd(insn);
    ir->opcode = rv_insn_cli;
    return true;
}

/* C.LUI: CI-format
 *  15    13    12    11       7 6        2 1  0
 * | funct3 | imm[5] |  rd/rs1  | imm[4:0] | op |
 */
static inline bool op_clui(rv_insn_t *ir, const uint32_t insn)
{
    /* inst       funct3 imm[5]    rd/rs1    imm[4:0]         op
     * ----------+------+---------+---------+----------------+--
     * C.ADDI16SP 011    nzimm[9]  2         nzimm[4|6|8:7|5] 01
     * C.LUI      011    nzimm[17] rd!={0,2} nzimm[16:12]     01
     */

    ir->rd = c_decode_rd(insn);
    /* 根据 rd/rs1 区域分派。 */
    switch (ir->rd) {
    case 0: /* 编码点：rd = x0 表示 HINTS。 */
        ir->opcode = rv_insn_cnop;
        break;
    case 2: { /* C.ADDI16SP */
        ir->imm = c_decode_caddi16sp_nzimm(insn);
        /* 编码点：nzimm = 0 为保留编码。 */
        if (!(uint32_t) ir->imm)
            return false;
        ir->opcode = rv_insn_caddi16sp;
        break;
    }
    default: { /* C.LUI */
        ir->imm = c_decode_clui_nzimm(insn);
        /* 编码点：nzimm = 0 为保留编码。 */
        if (!ir->imm)
            return false;
        ir->opcode = rv_insn_clui;
        break;
    }
    }
    return true;
}

/* MISC-ALU：CB 格式和 CA 格式。
 *
 * C.SRLI C.SRAI C.ANDI: CB-format
 *  15    13     12     11    10 9        7 6            2 1  0
 * | funct3 | shamt[5] | funct2 | rd'/rs1' |  shamt[4:0]  | op |
 *
 * CA-format
 *  15                        10 9        7 6      5 4    2 1  0
 * |           funct6           | rd'/rs1' | funct2 | rs2' | op |
 */
static inline bool op_cmisc_alu(rv_insn_t *ir, const uint32_t insn)
{
    /* inst   funct3 shamt[5]  funct2 rd'/rs1' shamt[4:0]  op
     * ------+------+---------+------+--------+-----------+--
     * C.SRLI 100    nzuimm[5] 00     rd'/rs1' nzuimm[4:0] 01
     * C.SRAI 100    nzuimm[5] 01     rd'/rs1' nzuimm[4:0] 01
     * C.ANDI 100    imm[5]    10     rd'/rs1' imm[4:0]    01
     * C.SUB  100    0         11     rd'/rs1' 00 rs2'     01
     * C.XOR  100    0         11     rd'/rs1' 01 rs2'     01
     * C.OR   100    0         11     rd'/rs1' 10 rs2'     01
     * C.AND  100    0         11     rd'/rs1' 11 rs2'     01
     * C.SUBW 100    1         11     rd'/rs1' 00 rs2'     01
     * C.ADDW 100    1         11     rd'/rs1' 01 rs2'     01
     */

    /* 根据 funct2 字段分派。 */
    uint8_t funct2 = (insn & 0x0C00) >> 10;
    switch (funct2) {
    case 0: /* C.SRLI */
        ir->shamt = c_decode_cbtype_shamt(insn);
        ir->rs1 = c_decode_rs1c(insn) | 0x08;

        /* 编码点：shamt[5] = 1 为保留编码。 */
        if (ir->shamt & 0x20)
            return false;

        /* 编码点：rd = x0 表示 HINTS。
         * 编码点：shamt = 0 表示 HINTS。
         */
        ir->opcode = (!ir->rs1 || !ir->shamt) ? rv_insn_cnop : rv_insn_csrli;
        break;
    case 1: /* C.SRAI */
        ir->shamt = c_decode_cbtype_shamt(insn);
        ir->rs1 = c_decode_rs1(insn);

        /* 编码点：shamt[5] = 1 为保留编码。 */
        if (ir->shamt & 0x20)
            return false;
        ir->opcode = rv_insn_csrai;
        break;
    case 2: /* C.ANDI */
        ir->rs1 = c_decode_rs1c(insn) | 0x08;
        ir->imm = c_decode_caddi_imm(insn);
        ir->opcode = rv_insn_candi;
        break;
    case 3: /* 算术类。 */
        ir->rs1 = c_decode_rs1c(insn) | 0x08;
        ir->rs2 = c_decode_rs2c(insn) | 0x08;
        ir->rd = ir->rs1;

        /* 根据 funct6[2] | funct2[1:0] 分派。 */
        switch (((insn & 0x1000) >> 10) | ((insn & 0x0060) >> 5)) {
        case 0: /* SUB */
            ir->opcode = rv_insn_csub;
            break;
        case 1: /* XOR */
            ir->opcode = rv_insn_cxor;
            break;
        case 2: /* OR */
            ir->opcode = rv_insn_cor;
            break;
        case 3: /* AND */
            ir->opcode = rv_insn_cand;
            break;
        case 4: /* SUBW */
        case 5: /* ADDW */
            assert(!"RV64/128C instructions");
            break;
        default: /* 保留编码（case 6、7）。 */
            assert(!"Instruction reserved");
            break;
        }
        break;
    }
    return true;
}

/* C.SLLI: CI-format
 *  15    13     12     11       7 6          2 1  0
 * | funct3 | shamt[5] |  rd/rs1  | shamt[4:0] | op |
 */
static inline bool op_cslli(rv_insn_t *ir, const uint32_t insn)
{
    /* inst   funct3 shamt[5]  rd/rs1    shamt[4:0]  op
     * ------+------+---------+---------+-----------+--
     * C.SLLI 000    nzuimm[5] rs1/rd!=0 nzuimm[4:0] 01
     */
    uint32_t tmp = 0;
    tmp |= (insn & FCI_IMM_12) >> 7;
    tmp |= (insn & FCI_IMM_6_2) >> 2;
    ir->imm = tmp;
    ir->rd = c_decode_rd(insn);
    ir->opcode = ir->rd ? rv_insn_cslli : rv_insn_cnop;
    return true;
}

/* C.LWSP: CI-format
 *  15    13  12   11   7 6   2 1  0
 * | funct3 | imm |  rd  | imm | op |
 */
static inline bool op_clwsp(rv_insn_t *ir, const uint32_t insn)
{
    /* inst   funct3 imm     rd    imm           op
     * ------+------+-------+-----+-------------+--
     * C.LWSP 000    uimm[5] rd!=0 uimm[4:2|7:6] 01
     */
    uint16_t tmp = 0;
    tmp |= (insn & 0x70) >> 2;
    tmp |= (insn & 0x0c) << 4;
    tmp |= (insn & 0x1000) >> 7;
    ir->imm = tmp;
    ir->rd = c_decode_rd(insn);

    /* rd = x0 为保留编码。 */
    ir->opcode = ir->rd ? rv_insn_clwsp : rv_insn_cnop;
    return true;
}

/* C.SWSP: CSS-Format
 *  15    13 12    7 6   2 1  0
 * | funct3 |  imm  | rs2 | op |
 */
static inline bool op_cswsp(rv_insn_t *ir, const uint32_t insn)
{
    /* inst   funct3 imm           rs2 op
     * ------+------+-------------+---+--
     * C.LWSP 110    uimm[5:2|7:6] rs2 10
     */
    ir->imm = (insn & 0x1e00) >> 7 | (insn & 0x180) >> 1;
    ir->rs2 = c_decode_rs2(insn);
    ir->opcode = rv_insn_cswsp;
    return true;
}

/* C.LW: CL-format
 *  15    13 12   10 9    7 6   5 4   2 1  0
 * | funct3 |  imm  | rs1' | imm | rd' | op |
 */
static inline bool op_clw(rv_insn_t *ir, const uint32_t insn)
{
    /* inst funct3 imm       rs1' imm       rd' op
     * ----+------+---------+----+---------+---+--
     * C.LW 010    uimm[5:3] rs1' uimm[7:6] rd' 00
     */
    uint16_t tmp = 0;
    tmp |= (insn & 0b0000000001000000) >> 4;
    tmp |= (insn & FC_IMM_12_10) >> 7;
    tmp |= (insn & 0b0000000000100000) << 1;
    ir->imm = tmp;
    ir->rd = c_decode_rdc(insn) | 0x08;
    ir->rs1 = c_decode_rs1c(insn) | 0x08;
    ir->opcode = rv_insn_clw;
    return true;
}

/* C.SW: CS-format
 *  15    13 12   10 9    7 6   5 4    2 1  0
 * | funct3 |  imm  | rs1' | imm | rs2' | op |
 */
static inline bool op_csw(rv_insn_t *ir, const uint32_t insn)
{
    /* inst funct3 imm       rs1' imm       rs2' op
     * ----+------+---------+----+---------+----+--
     * C.SW 110    uimm[5:3] rs1' uimm[2|6] rs2' 00
     */
    uint32_t tmp = 0;
    /*               ....xxxx....xxxx     */
    tmp |= (insn & 0b0000000001000000) >> 4;
    tmp |= (insn & FC_IMM_12_10) >> 7;
    tmp |= (insn & 0b0000000000100000) << 1;
    ir->imm = tmp;
    ir->rs1 = c_decode_rs1c(insn) | 0x08;
    ir->rs2 = c_decode_rs2c(insn) | 0x08;
    ir->opcode = rv_insn_csw;
    return true;
}

/* C.J: CR-format
 *  15    13 12    2 1  0
 * | funct3 |  imm  | op |
 */
static inline bool op_cj(rv_insn_t *ir, const uint32_t insn)
{
    /* inst funct3 imm                        op
     * ----+------+--------------------------+--
     * C.J  101    imm[11|4|9:8|10|6|7|3:1|5] 01
     */
    ir->imm = c_decode_cjtype_imm(insn);
    ir->opcode = rv_insn_cj;
    return true;
}

/* C.JAL: CR-format
 *  15    13 12    2 1  0
 * | funct3 |  imm  | op |
 */
static inline bool op_cjal(rv_insn_t *ir, const uint32_t insn)
{
    /* inst  funct3 imm                        op
     * -----+------+--------------------------+--
     * C.JAL 001    imm[11|4|9:8|10|6|7|3:1|5] 01
     */
    ir->imm = sign_extend_h(c_decode_cjtype_imm(insn));
    ir->opcode = rv_insn_cjal;
    return true;
}

/* C.CR: CR-format
 *  15    12 11    7 6    2 1  0
 * | funct4 |  rs1  |  rs2 | op |
 */
static inline bool op_ccr(rv_insn_t *ir, const uint32_t insn)
{
    /* inst     funct4 rs1       rs2    op
     * --------+------+---------+------+--
     * C.JR     100    rs1!=0    0      10
     * C.MV     100    rd!=0     rs2!=0 10
     * C.EBREAK 100    0         0      10
     * C.JALR   100    rs1!=0    0      10
     * C.ADD    100    rs1/rd!=0 rs2!=0 10
     */
    ir->rs1 = c_decode_rs1(insn);
    ir->rs2 = c_decode_rs2(insn);
    ir->rd = ir->rs1;

    /* 根据 funct4[0] 字段分派。 */
    switch ((insn & 0x1000) >> 12) {
    case 0:
        /* 根据 rs2 字段分派。 */
        switch (ir->rs2) {
        case 0: /* C.JR */
            /* 编码点：rd = x0 为保留编码。 */
            if (!ir->rs1)
                return false;
            ir->opcode = rv_insn_cjr;
            break;
        default: /* C.MV */
            /* 编码点：rd = x0 表示 HINTS。 */
            ir->opcode = ir->rd ? rv_insn_cmv : rv_insn_cnop;
            break;
        }
        break;
    case 1:
        if (!ir->rs1 && !ir->rs2) /* C.EBREAK */
            ir->opcode = rv_insn_ebreak;
        else if (ir->rs1 && ir->rs2) { /* C.ADD */
            /* 编码点：rd = x0 表示 HINTS。 */
            ir->opcode = ir->rd ? rv_insn_cadd : rv_insn_cnop;
        } else if (ir->rs1 && !ir->rs2) /* C.JALR */
            ir->opcode = rv_insn_cjalr;
        else { /* rs2 != x0 且 rs1 = x0。 */
            /* HINT。 */
            ir->opcode = rv_insn_cnop;
        }
        break;
    }
    return true;
}

/* C.BEQZ: CB-format
 *  15    13 12     10 9    7 6       2 1  0
 * | funct3 |   imm   | rs1' |   imm   | op |
 */
static inline bool op_cbeqz(rv_insn_t *ir, const uint32_t insn)
{
    /* inst   funct3 imm        rs1' imm            op
     * ------+------+----------+----+--------------+--
     * C.BEQZ 110    imm[8|4:3] rs1' imm[7:6|2:1|5] 01
     */
    ir->imm = sign_extend_h(c_decode_cbtype_imm(insn));
    ir->rs1 = c_decode_rs1c(insn) | 0x08;
    ir->opcode = rv_insn_cbeqz;
    return true;
}

/* C.BNEZ: CB-format
 *  15    13 12     10 9    7 6       2 1  0
 * | funct3 |   imm   | rs1' |   imm   | op |
 */
static inline bool op_cbnez(rv_insn_t *ir, const uint32_t insn)
{
    /* inst   funct3 imm        rs1' imm            op
     * ------+------+----------+----+--------------+--
     * C.BNEZ 111    imm[8|4:3] rs1' imm[7:6|2:1|5] 01
     */
    ir->imm = sign_extend_h(c_decode_cbtype_imm(insn));
    ir->rs1 = c_decode_rs1c(insn) | 0x08;
    ir->opcode = rv_insn_cbnez;
    return true;
}

#else /* !RV32_HAS(EXT_C) */
#define op_caddi4spn OP_UNIMP
#define op_caddi OP_UNIMP
#define op_cslli OP_UNIMP
#define op_cjal OP_UNIMP
#define op_clw OP_UNIMP
#define op_cli OP_UNIMP
#define op_clwsp OP_UNIMP
#define op_clui OP_UNIMP
#define op_cmisc_alu OP_UNIMP
#define op_ccr OP_UNIMP
#define op_cj OP_UNIMP
#define op_csw OP_UNIMP
#define op_cbeqz OP_UNIMP
#define op_cswsp OP_UNIMP
#define op_cbnez OP_UNIMP
#endif /* RV32_HAS(EXT_C) */

#if RV32_HAS(EXT_C) && RV32_HAS(EXT_F)
/* C.FLWSP: CI-format
 *  15    13  12   11   7 6   2 1  0
 * | funct3 | imm |  rd  | imm | op |
 */
static inline bool op_cflwsp(rv_insn_t *ir, const uint32_t insn)
{
    /* inst    funct3 imm     rd    imm           op
     * -------+------+-------+-----+-------------+--
     * C.FLWSP 001    uimm[5] rd    uimm[4:2|7:6] 10
     */
    uint16_t tmp = 0;
    tmp |= (insn & 0x70) >> 2;
    tmp |= (insn & 0x0c) << 4;
    tmp |= (insn & 0x1000) >> 7;
    ir->imm = tmp;
    ir->rd = c_decode_rd(insn);
    ir->opcode = rv_insn_cflwsp;
    return true;
}

/* C.FSWSP: CSS-Format
 *  15    13 12    7 6   2 1  0
 * | funct3 |  imm  | rs2 | op |
 */
static inline bool op_cfswsp(rv_insn_t *ir, const uint32_t insn)
{
    /* inst    funct3 imm           rs2 op
     * -------+------+-------------+---+--
     * C.FSWSP 111    uimm[5:2|7:6] rs2 10
     */
    ir->imm = (insn & 0x1e00) >> 7 | (insn & 0x180) >> 1;
    ir->rs2 = c_decode_rs2(insn);
    ir->opcode = rv_insn_cfswsp;
    return true;
}

/* C.FLW: CL-format
 *  15    13 12   10 9    7 6   5 4   2 1  0
 * | funct3 |  imm  | rs1' | imm | rd' | op |
 */
static inline bool op_cflw(rv_insn_t *ir, const uint32_t insn)
{
    /* inst  funct3 imm       rs1' imm       rd' op
     * -----+------+---------+----+---------+---+--
     * C.FLW 010    uimm[5:3] rs1' uimm[7:6] rd' 00
     */
    uint16_t tmp = 0;
    tmp |= (insn & 0b0000000001000000) >> 4;
    tmp |= (insn & FC_IMM_12_10) >> 7;
    tmp |= (insn & 0b0000000000100000) << 1;
    ir->imm = tmp;
    ir->rd = c_decode_rdc(insn) | 0x08;
    ir->rs1 = c_decode_rs1c(insn) | 0x08;
    ir->opcode = rv_insn_cflw;
    return true;
}

/* C.FSW: CS-format
 *  15    13 12   10 9    7 6   5 4    2 1  0
 * | funct3 |  imm  | rs1' | imm | rs2' | op |
 */
static inline bool op_cfsw(rv_insn_t *ir, const uint32_t insn)
{
    /* inst  funct3 imm       rs1' imm       rs2' op
     * -----+------+---------+----+---------+----+--
     * C.FSW 110    uimm[5:3] rs1' uimm[2|6] rs2' 00
     */
    uint32_t tmp = 0;
    /*               ....xxxx....xxxx     */
    tmp |= (insn & 0b0000000001000000) >> 4;
    tmp |= (insn & FC_IMM_12_10) >> 7;
    tmp |= (insn & 0b0000000000100000) << 1;
    ir->imm = tmp;
    ir->rs1 = c_decode_rs1c(insn) | 0x08;
    ir->rs2 = c_decode_rs2c(insn) | 0x08;
    ir->opcode = rv_insn_cfsw;
    return true;
}

#else /* !(RV32_HAS(EXT_C) && RV32_HAS(EXT_F)) */
#define op_cfsw OP_UNIMP
#define op_cflw OP_UNIMP
#define op_cfswsp OP_UNIMP
#define op_cflwsp OP_UNIMP
#endif /* RV32_HAS(EXT_C) && RV32_HAS(EXT_F) */

/* 所有未实现 opcode 的处理器。 */
static inline bool op_unimp(rv_insn_t *ir UNUSED, uint32_t insn UNUSED)
{
    return false;
}

/* RV32 解码处理器类型。 */
typedef bool (*decode_t)(rv_insn_t *ir, uint32_t insn);

/* 解码 RISC-V 指令。 */
bool rv_decode(rv_insn_t *ir, uint32_t insn)
{
    bool ret;
    assert(ir);
    decode_t op;

#define OP_UNIMP op_unimp
#define OP(insn) op_##insn

    /* RV32 基础 opcode 映射表。 */
    /* clang-format off */
    static const decode_t rv_jump_table[] = {
    //  000         001           010        011           100         101        110        111
        OP(load),   OP(load_fp),  OP(unimp), OP(misc_mem), OP(op_imm), OP(auipc), OP(unimp), OP(unimp), // 00
        OP(store),  OP(store_fp), OP(unimp), OP(amo),      OP(op),     OP(lui),   OP(unimp), OP(unimp), // 01
        OP(madd),   OP(msub),     OP(nmsub), OP(nmadd),    OP(op_fp),  OP(unimp), OP(unimp), OP(unimp), // 10
        OP(branch), OP(jalr),     OP(unimp), OP(jal),      OP(system), OP(unimp), OP(unimp), OP(unimp), // 11
    };

#if RV32_HAS(EXT_C)
    /* RV32C opcode 映射表。 */
    static const decode_t rvc_jump_table[] = {
    //  00             01             10          11
        OP(caddi4spn), OP(caddi),     OP(cslli),  OP(unimp),  // 000
        OP(unimp),      OP(cjal),      OP(unimp), OP(unimp),  // 001
        OP(clw),       OP(cli),       OP(clwsp),  OP(unimp),  // 010
        OP(cflw),      OP(clui),      OP(cflwsp), OP(unimp),  // 011
        OP(unimp),     OP(cmisc_alu), OP(ccr),    OP(unimp),  // 100
        OP(unimp),      OP(cj),        OP(unimp), OP(unimp),  // 101
        OP(csw),       OP(cbeqz),     OP(cswsp),  OP(unimp),  // 110
        OP(cfsw),      OP(cbnez),     OP(cfswsp), OP(unimp),  // 111
    };
#endif
    /* clang-format on */

    /* 压缩扩展指令。 */
#if RV32_HAS(EXT_C)
    /* 若最后 2 位为 0b00、0b01 或 0b10，则该指令是 16 位指令。
     */
    if (is_compressed(insn)) {
        insn &= 0x0000FFFF;
        const uint16_t c_index = (insn & FC_FUNC3) >> 11 | (insn & FC_OPCODE);

        /* 解码压缩指令。 */
        op = rvc_jump_table[c_index];
        assert(op);
        ret = op(ir, insn);

        goto end;
    }
#endif

    /* 标准非压缩指令。 */
    const uint32_t index = (insn & INSN_6_2) >> 2;

    /* 解码指令。 */
    op = rv_jump_table[index];
    assert(op);
    ret = op(ir, insn);

end:

#if RV32_HAS(RV32E)
    /* RV32E 禁止整数寄存器使用 x16-x31；但启用 F 扩展时，浮点寄存器不受 16 个
     * 寄存器限制。
     */
    if ((op != op_store_fp && op != op_load_fp && op != op_op_fp) &&
        unlikely(ir->rd > 15 || ir->rs1 > 15 || ir->rs2 > 15))
        ret = false;
#endif

    return ret;

#undef OP_UNIMP
#undef OP
}
