/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/*
 * Emscripten/浏览器运行时接口。
 *
 * 本头文件声明 C 与 JavaScript 之间的桥接函数。系统模式用于连接终端和运行按钮，
 * 用户模式用于把浏览器输入缓冲区交给 UART/标准输入路径。
 */

#pragma once

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>

/* 终止 CPU 主循环。 */
void indirect_rv_halt();

#if RV32_HAS(SYSTEM_MMIO)
/* 桥接 xterm.js 终端与 UART。 */
extern uint8_t input_buf_size;

char *get_input_buf();
uint8_t get_input_buf_cap();
void set_input_buf_size(uint8_t size);
#endif

/* 启用或禁用 index.html 中的运行按钮，避免进程运行期间重复执行。
 */
void enable_run_button();
void disable_run_button();
#endif
