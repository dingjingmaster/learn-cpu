/*
 * rv32emu 可依据 MIT 许可证自由再分发。使用和再分发规则见 LICENSE 文件。
 */

/*
 * 功能开关默认值。
 *
 * 构建系统会通过 -DRV32_FEATURE_* 注入真实配置；当某个宏未由构建系统设置时，
 * 这里提供保守默认值，保证源码仍可被静态分析或独立编译器前端解析。
 */

#pragma once

/* 功能配置说明
 *
 * 功能由 Kconfig（configs/Kconfig）控制，并通过 -DRV32_FEATURE_* 编译参数传入。
 * 只有构建系统没有设置对应特性宏时，才会使用下面的默认值。
 *
 * Kconfig 约束（非法组合在配置阶段阻止）：
 *   - T2C 需要 JIT 和 LLVM18（下方也会二次保护）。
 *   - JIT 与 Emscripten 不兼容（WASM 只使用解释器）。
 *   - GDBSTUB 与 Emscripten 不兼容。
 *   - SDL 需要 SDL2 库或 Emscripten。
 *   - SDL_MIXER 需要 SDL。
 *   - ELF_LOADER 需要 SYSTEM 模式。
 *
 * 派生特性（由其他特性计算得到）：
 *   - SYSTEM_MMIO = SYSTEM && !ELF_LOADER，用于带 MMIO 设备的 Linux 内核启动。
 *
 * 简化规则（Kconfig 保证成立）：
 *   - RV32_HAS(T2C) 蕴含 RV32_HAS(JIT)，源码无需同时检查二者。
 *   - RV32_HAS(ELF_LOADER) 蕴含 RV32_HAS(SYSTEM)。
 *   - RV32_HAS(SDL_MIXER) 蕴含 RV32_HAS(SDL)。
 *   - 使用 RV32_HAS(SYSTEM_MMIO) 表示 SYSTEM && !ELF_LOADER。
 */

/* 标准整数乘除扩展 M。 */
#ifndef RV32_FEATURE_EXT_M
#define RV32_FEATURE_EXT_M 1
#endif

/* 标准原子指令扩展 A。 */
#ifndef RV32_FEATURE_EXT_A
#define RV32_FEATURE_EXT_A 1
#endif

/* 标准单精度浮点扩展 F。 */
#ifndef RV32_FEATURE_EXT_F
#define RV32_FEATURE_EXT_F 1
#endif

/* 标准压缩指令扩展 C。 */
#ifndef RV32_FEATURE_EXT_C
#define RV32_FEATURE_EXT_C 1
#endif

/* RV32E 基础整数指令集。 */
#ifndef RV32_FEATURE_RV32E
#define RV32_FEATURE_RV32E 0
#endif

/* 控制与状态寄存器 CSR 指令扩展。 */
#ifndef RV32_FEATURE_Zicsr
#define RV32_FEATURE_Zicsr 1
#endif

/* 指令取指栅栏扩展。 */
#ifndef RV32_FEATURE_Zifencei
#define RV32_FEATURE_Zifencei 1
#endif

/* Zba 地址生成指令。 */
#ifndef RV32_FEATURE_Zba
#define RV32_FEATURE_Zba 1
#endif

/* Zbb 基础位操作指令。 */
#ifndef RV32_FEATURE_Zbb
#define RV32_FEATURE_Zbb 1
#endif

/* Zbc 无进位乘法指令。 */
#ifndef RV32_FEATURE_Zbc
#define RV32_FEATURE_Zbc 1
#endif

/* Zbs 单比特操作指令。 */
#ifndef RV32_FEATURE_Zbs
#define RV32_FEATURE_Zbs 1
#endif

/* 面向 SDL 的实验性系统调用。 */
#ifndef RV32_FEATURE_SDL
#define RV32_FEATURE_SDL 1
#endif

/* GDB 远程调试。 */
#ifndef RV32_FEATURE_GDBSTUB
#define RV32_FEATURE_GDBSTUB 0
#endif

/* 实验性一级 JIT 编译器。 */
#ifndef RV32_FEATURE_JIT
#define RV32_FEATURE_JIT 0
#endif

/* 实验性二级 JIT 编译器。 */
#ifndef RV32_FEATURE_T2C
#define RV32_FEATURE_T2C 0
#endif

/* T2C（二级编译器）依赖 JIT（一级编译器）。Kconfig 已约束此关系，这里再做
 * 一层兜底，源码就可以只判断 RV32_HAS(T2C)，无需额外判断 JIT。
 */
#if !RV32_FEATURE_JIT
#undef RV32_FEATURE_T2C
#define RV32_FEATURE_T2C 0
#endif

/* 系统模拟模式。 */
#ifndef RV32_FEATURE_SYSTEM
#define RV32_FEATURE_SYSTEM 0
#endif

/* 使用 ELF 加载器而不是 Linux 内核启动路径。 */
#ifndef RV32_FEATURE_ELF_LOADER
#define RV32_FEATURE_ELF_LOADER 0
#endif

/* 微操作融合。 */
#ifndef RV32_FEATURE_MOP_FUSION
#define RV32_FEATURE_MOP_FUSION 1
#endif

/* 基本块链接。 */
#ifndef RV32_FEATURE_BLOCK_CHAINING
#define RV32_FEATURE_BLOCK_CHAINING 1
#endif

/* 彩色日志。 */
#ifndef RV32_FEATURE_LOG_COLOR
#define RV32_FEATURE_LOG_COLOR 1
#endif

/* RISC-V 架构测试支持。 */
#ifndef RV32_FEATURE_ARCH_TEST
#define RV32_FEATURE_ARCH_TEST 0
#endif

/* 系统模拟的 MMIO 支持。
 * 当 SYSTEM 模式启用且未使用 ELF_LOADER 时开启，对应启动完整 Linux 内核并通过
 * 内存映射 I/O 访问虚拟设备（UART、PLIC、virtio-blk、Goldfish RTC）。
 */
#if RV32_FEATURE_SYSTEM && !RV32_FEATURE_ELF_LOADER
/* Goldfish RTC 虚拟时钟。 */
#ifndef RV32_FEATURE_GOLDFISH_RTC
#define RV32_FEATURE_GOLDFISH_RTC 1
#endif
#define RV32_FEATURE_SYSTEM_MMIO 1
#else
#define RV32_FEATURE_SYSTEM_MMIO 0
#endif

/* 功能测试宏。 */
#define RV32_HAS(x) RV32_FEATURE_##x
