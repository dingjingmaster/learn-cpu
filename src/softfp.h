/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/*
 * RV32F 软件浮点辅助接口。
 *
 * 当启用 F 扩展时，解释器通过 SoftFloat 完成 IEEE 754 单精度运算。本头文件
 * 封装浮点寄存器访问、舍入模式、异常标志和常用转换/分类辅助。
 */

#pragma once

#include <stdint.h>

#include "riscv.h"
#include "riscv_private.h"

/* clang-format off */
enum {
    //                    ....xxxx....xxxx....xxxx....xxxx
    FMASK_SIGN        = 0b10000000000000000000000000000000,
    FMASK_EXPN        = 0b01111111100000000000000000000000,
    FMASK_FRAC        = 0b00000000011111111111111111111111,
    FMASK_QNAN        = 0b00000000010000000000000000000000,
    //                    ....xxxx....xxxx....xxxx....xxxx
    FFLAG_MASK        = 0b00000000000000000000000000011111,
    FFLAG_INVALID_OP  = 0b00000000000000000000000000010000,
    FFLAG_DIV_BY_ZERO = 0b00000000000000000000000000001000,
    FFLAG_OVERFLOW    = 0b00000000000000000000000000000100,
    FFLAG_UNDERFLOW   = 0b00000000000000000000000000000010,
    FFLAG_INEXACT     = 0b00000000000000000000000000000001,
    //                    ....xxxx....xxxx....xxxx....xxxx
    RV_NAN            = 0b01111111110000000000000000000000
};
/* clang-format on */

/* 计算 fclass 结果。 */
static inline uint32_t calc_fclass(uint32_t f)
{
    const uint32_t sign = f & FMASK_SIGN;
    const uint32_t expn = f & FMASK_EXPN;
    const uint32_t frac = f & FMASK_FRAC;

    uint32_t out = 0;

    /*
     * 0x001    rs1 为 -INF
     * 0x002    rs1 为负正规数
     * 0x004    rs1 为负非正规数
     * 0x008    rs1 为 -0
     * 0x010    rs1 为 +0
     * 0x020    rs1 为正非正规数
     * 0x040    rs1 为正正规数
     * 0x080    rs1 为 +INF
     * 0x100    rs1 为 signaling NaN
     * 0x200    rs1 为 quiet NaN
     */

    /* 检查指数位。 */
    if (expn) {
        if (expn != FMASK_EXPN) {
            /* 检查是否为负正规数或正正规数。 */
            out = sign ? 0x002 : 0x040;
        } else {
            /* 检查是否为 NaN。 */
            if (frac) {
                out = frac & FMASK_QNAN ? 0x200 : 0x100;
            } else if (!sign) {
                /* 检查是否为 +INF。 */
                out = 0x080;
            } else {
                /* 检查是否为 -INF。 */
                out = 0x001;
            }
        }
    } else if (frac) {
        /* 检查是否为负非正规数或正非正规数。 */
        out = sign ? 0x004 : 0x020;
    } else {
        /* 检查是否为 +0 或 -0。 */
        out = sign ? 0x008 : 0x010;
    }

    return out;
}

static inline bool is_nan(uint32_t f)
{
    const uint32_t expn = f & FMASK_EXPN;
    const uint32_t frac = f & FMASK_FRAC;
    return (expn == FMASK_EXPN && frac);
}

static inline void set_fflag(riscv_t *rv)
{
    if (softfloat_exceptionFlags & softfloat_flag_invalid)
        rv->csr_fcsr |= FFLAG_INVALID_OP;
    if (softfloat_exceptionFlags & softfloat_flag_infinite)
        rv->csr_fcsr |= FFLAG_DIV_BY_ZERO;
    if (softfloat_exceptionFlags & softfloat_flag_overflow)
        rv->csr_fcsr |= FFLAG_OVERFLOW;
    if (softfloat_exceptionFlags & softfloat_flag_underflow)
        rv->csr_fcsr |= FFLAG_UNDERFLOW;
    if (softfloat_exceptionFlags & softfloat_flag_inexact)
        rv->csr_fcsr |= FFLAG_INEXACT;
    softfloat_exceptionFlags = 0;
}

static inline void set_rounding_mode(riscv_t *rv, uint8_t rm)
{
    const uint32_t frm = (rv->csr_fcsr >> 5) & (~(1 << 3));

    if (likely(rm == 0b111))
        rm = frm;

    switch (rm) {
    case 0b000:
        softfloat_roundingMode = softfloat_round_near_even;
        break;
    case 0b001:
        softfloat_roundingMode = softfloat_round_minMag;
        break;
    case 0b010:
        softfloat_roundingMode = softfloat_round_min;
        break;
    case 0b011:
        softfloat_roundingMode = softfloat_round_max;
        break;
    case 0b100:
        softfloat_roundingMode = softfloat_round_near_maxMag;
        break;
    default:
        __UNREACHABLE;
        break;
    }
}
