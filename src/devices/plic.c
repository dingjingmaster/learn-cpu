/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/*
 * PLIC（Platform-Level Interrupt Controller）设备模型。
 *
 * 系统模式下 UART、virtio-blk、RTC 等外设通过 PLIC 汇总外部中断。该实现提供
 * pending/enable/claim/complete 等 MMIO 寄存器行为，并把中断状态反馈给
 * RISC-V CSR 路径。
 */

#include <assert.h>
#include <stdlib.h>

#include "plic.h"
#include "riscv.h"
#include "riscv_private.h"

void plic_update_interrupts(plic_t *plic)
{
    riscv_t *rv = (riscv_t *) plic->rv;

    /* 根据当前输入线状态刷新 pending 中断。 */
    plic->ip |= plic->active & ~plic->masked;
    plic->masked |= plic->active;
    /* 把外部中断状态送到目标 RISC-V hart。 */
    if (plic->ip & plic->ie)
        rv->csr_sip |= SIP_SEIP;
    else
        rv->csr_sip &= ~SIP_SEIP;
}

uint32_t plic_read(plic_t *plic, const uint32_t addr)
{
    uint32_t plic_read_val = 0;

    switch (addr) {
    case PLIC_INTR_PENDING:
        plic_read_val = plic->ip;
        break;
    case PLIC_INTR_ENABLE:
        plic_read_val = plic->ie;
        break;
    case PLIC_INTR_PRIORITY_THRESHOLD:
        /* 当前实现不支持优先级，目标优先级阈值固定为 0。 */
        plic_read_val = 0;
        break;
    case PLIC_INTR_CLAIM_OR_COMPLETE:
        /* claim：返回第一个可投递的中断号，并清除 pending 位。 */
        {
            uint32_t intr_candidate = plic->ip & plic->ie;
            if (intr_candidate) {
                plic_read_val = rv_ctz(intr_candidate);
                plic->ip &= ~(1U << (plic_read_val));
            }
            break;
        }
    default:
        return 0;
    }

    return plic_read_val;
}

void plic_write(plic_t *plic, const uint32_t addr, uint32_t value)
{
    switch (addr) {
    case PLIC_INTR_ENABLE:
        plic->ie = (value & ~1);
        break;
    case PLIC_INTR_PRIORITY_THRESHOLD:
        /* 当前实现不支持优先级，目标优先级阈值固定为 0。 */
        break;
    case PLIC_INTR_CLAIM_OR_COMPLETE:
        /* completion：允许该中断线再次被置为 pending。 */
        if (plic->ie & (1U << value))
            plic->masked &= ~(1U << value);
        break;
    default:
        break;
    }

    return;
}

plic_t *plic_new()
{
    plic_t *plic = calloc(1, sizeof(plic_t));
    assert(plic);

    return plic;
}

void plic_delete(plic_t *plic)
{
    free(plic);
}
