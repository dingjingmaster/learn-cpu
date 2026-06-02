/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/*
 * PLIC 设备接口和寄存器偏移。
 *
 * 系统模式通过这些定义模拟平台级外部中断控制器，把 UART/RTC/virtio 等设备的
 * 中断汇总给 RISC-V 中断处理路径。
 */

#pragma once

#include <stdint.h>

enum PLIC_REG {
    PLIC_INTR_PENDING = 0x1000,
    PLIC_INTR_ENABLE = 0x2000,
    PLIC_INTR_PRIORITY_THRESHOLD = 0x200000,
    PLIC_INTR_CLAIM_OR_COMPLETE = 0x200004,
};

/* PLIC 运行时状态。 */
typedef struct {
    uint32_t masked;
    uint32_t ip;
    uint32_t ie;
    /* 输入中断线状态（电平触发），由外部设备模型设置。 */
    uint32_t active;
    /* 接收 PLIC 中断的 RISC-V 实例。 */
    void *rv;
} plic_t;

/* 刷新 PLIC 状态，并同步外部中断到目标 hart。 */
void plic_update_interrupts(plic_t *plic);

/* 从 PLIC MMIO 空间读取 32 位数据。 */
uint32_t plic_read(plic_t *plic, const uint32_t addr);

/* 向 PLIC MMIO 空间写入 32 位数据。 */
void plic_write(plic_t *plic, const uint32_t addr, uint32_t value);

/* 创建 PLIC 实例。 */
plic_t *plic_new();

/* 销毁 PLIC 实例。 */
void plic_delete(plic_t *plic);
