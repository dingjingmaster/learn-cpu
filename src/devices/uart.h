/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/*
 * 8250/16550 UART 设备接口。
 *
 * 定义 UART 中断位、状态结构和 MMIO 读写函数。系统模式使用它提供 Linux 控制台，
 * WASM 模式还会把浏览器输入缓冲区接入该设备。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define IRQ_UART_SHIFT 1
#define IRQ_UART_BIT (1 << IRQ_UART_SHIFT)

enum UART_REG {
    U8250_THR_RBR_DLL = 0,
    U8250_IER_DLH,
    U8250_IIR_FCR,
    U8250_LCR,
    U8250_MCR,
    U8250_LSR,
    U8250_MSR,
    U8250_SR,
};

typedef struct {
    uint8_t dll, dlh;                    /* 分频器寄存器，当前忽略其值。 */
    uint8_t lcr;                         /* UART 线路配置。 */
    uint8_t ier;                         /* 中断使能配置。 */
    uint8_t current_intr, pending_intrs; /* 当前中断和待处理中断状态。 */
    uint8_t mcr;       /* 其他输出信号；loopback 模式当前忽略。 */
    int in_fd, out_fd; /* 宿主输入/输出文件描述符。 */
    bool in_ready;
} u8250_state_t;

/* 刷新 UART 中断状态。 */
void u8250_update_interrupts(u8250_state_t *uart);

/* 轮询宿主输入，更新 UART 输入就绪状态。 */
void u8250_check_ready(u8250_state_t *uart);

/* 从 UART MMIO 寄存器读取 32 位数据。 */
uint32_t u8250_read(u8250_state_t *uart, uint32_t addr);

/* 向 UART MMIO 寄存器写入 32 位数据。 */
void u8250_write(u8250_state_t *uart, uint32_t addr, uint32_t value);

/* 创建 UART 实例。 */
u8250_state_t *u8250_new();

/* 销毁 UART 实例。 */
void u8250_delete(u8250_state_t *uart);
