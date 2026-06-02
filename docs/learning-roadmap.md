# learn-cpu 代码学习路线

本文给出几条结合源码阅读的学习路线。建议先完成“快速上手与整体骨架”，再根据目标选择解释器、指令集、内存系统、JIT 或系统模式路线。

## 路线 1：快速上手与整体骨架

适合刚开始熟悉项目，目标是知道项目如何配置、构建、运行，以及一次模拟执行从哪里进入。

阅读顺序：

1. [`README.md`](../README.md)
2. [`Makefile`](../Makefile)
3. [`configs/Kconfig`](../configs/Kconfig)
4. [`src/main.c`](../src/main.c)

重点理解：

* `make defconfig` 如何生成 `.config`。
* Kconfig 选项如何转换为 `RV32_FEATURE_*` 功能宏。
* `main.c` 如何解析参数、加载程序、创建 `riscv_t`。
* 程序如何进入 `rv_step()` 执行循环。

动手任务：

```sh
make defconfig
make
./build/rv32emu -h
```

可以先在 `main.c` 中加一条启动日志，确认自己能修改、构建并运行项目。

## 路线 2：解释器核心

这是最推荐的主线。学完后可以理解模拟器如何取指、解码、翻译基本块并执行 RISC-V 指令。

阅读顺序：

1. [`src/main.c`](../src/main.c)
2. [`src/riscv.c`](../src/riscv.c)
3. [`src/emulate.c`](../src/emulate.c)
4. [`src/decode.c`](../src/decode.c)
5. [`src/decode.h`](../src/decode.h)
6. [`src/rv32_template.c`](../src/rv32_template.c)

核心链路：

```text
main()
  -> rv_create / rv_load
  -> rv_step()
  -> block_find_or_translate()
  -> block_translate()
  -> rv_decode()
  -> do_xxx 指令处理函数
```

重点理解：

* `rv_step()` 是外层执行循环。
* `block_translate()` 把连续指令翻译成 IR 链表。
* `rv_decode()` 把原始 32 位或 16 位指令解码为 `rv_insn_t`。
* `rv32_template.c` 中的 `RVOP` 展开后是真正的指令语义。

动手任务：

* 给 `rv_step()` 加日志，打印当前 `PC`。
* 给 `rv_decode()` 加日志，打印解码出的 opcode。
* 选择 `addi`、`lw`、`jalr` 中任意一条，从解码一路追到执行。

## 路线 3：RISC-V 指令集

适合希望通过项目学习 RISC-V ISA 的读者。

阅读顺序：

1. [`src/decode.h`](../src/decode.h)
2. [`src/decode.c`](../src/decode.c)
3. [`src/rv32_template.c`](../src/rv32_template.c)
4. [`src/rv32_constopt.c`](../src/rv32_constopt.c)

建议按扩展分块阅读：

| 扩展 | 学习重点 |
| --- | --- |
| RV32I | 基础整数、跳转、访存、分支 |
| M | 乘法、除法、取余边界语义 |
| A | 原子读改写、LR/SC |
| F | 单精度浮点和 SoftFloat 集成 |
| C | 16 位压缩指令如何展开到等价语义 |
| Zicsr/Zifencei | CSR 读写、trap 相关状态、`fence.i` |

推荐方法：

1. 先看 `decode.h` 中的 `RV_INSN_LIST`。
2. 找某条指令在 `decode.c` 中如何识别。
3. 再看 `rv32_template.c` 中如何执行。
4. 最后看 `rv32_constopt.c` 是否会对它做常量优化。

动手任务：

* 追踪 `addi`、`lw`、`jal`、`csrrw` 各一遍。
* 画出一条指令从机器码到执行的路径。
* 尝试新增一个只用于实验的日志点，观察指令字段。

## 路线 4：ELF、内存和 syscall

适合理解客体程序如何被加载到模拟内存，以及如何通过 `ecall` 和宿主交互。

阅读顺序：

1. [`src/elf.c`](../src/elf.c)
2. [`src/elf.h`](../src/elf.h)
3. [`src/io.c`](../src/io.c)
4. [`src/system.c`](../src/system.c)
5. [`src/syscall.c`](../src/syscall.c)
6. [`src/syscall_sdl.c`](../src/syscall_sdl.c)

核心链路：

```text
ELF 文件
  -> elf_load()
  -> 写入 guest memory
  -> guest 执行 ecall
  -> ecall_handler()
  -> syscall_handler()
```

重点理解：

* ELF program header 如何映射到客体地址空间。
* `memory_t` 如何描述 guest memory。
* `mem_read`、`mem_write` 和 `mem_ifetch` 的差异。
* 用户态 `ecall` 如何进入 `syscall_handler()`。
* `write`、`brk`、`gettimeofday` 等 syscall 如何映射到宿主行为。

动手任务：

* 找到 `write` syscall，理解 Hello World 如何输出。
* 给内存读写加 trace，观察 guest 程序访问了哪些地址。
* 新增一个实验 syscall，例如返回固定数字或打印调试信息。

## 路线 5：JIT 和性能优化

适合已经理解解释器后继续深入。建议不要一开始就读完整 `jit.c`，先看热点判定和基本块缓存，再进入机器码发射。

阅读顺序：

1. [`src/cache.c`](../src/cache.c)
2. [`src/cache.h`](../src/cache.h)
3. [`src/jit.c`](../src/jit.c)
4. [`src/rv32_jit.c`](../src/rv32_jit.c)
5. [`src/t2c.c`](../src/t2c.c)
6. [`src/t2c_template.c`](../src/t2c_template.c)

核心链路：

```text
解释器执行基本块
  -> cache 统计执行频率
  -> runtime_profiler 判断热点
  -> jit_translate()
  -> 后续直接执行宿主机器码
```

重点理解：

* 基本块缓存如何记录频率和替换条目。
* `block->hot`、`block->hot2`、`n_invoke` 的作用。
* 一级 JIT 如何为 x86-64 或 AArch64 发射机器码。
* 二级 T2C 如何把热点块交给 LLVM。
* `jit_cache` 和 `inline_cache` 如何加速块间跳转。

动手任务：

* 打印哪些基本块被判定为热点。
* 用默认配置和 `make jit_defconfig` 对比执行路径。
* 找一条简单指令，例如 `addi`，看它在 `rv32_jit.c` 中如何生成宿主指令。

## 路线 6：系统模式、MMU 和设备

适合理解项目如何从用户态 ELF 模拟器扩展为一台小型 RISC-V 机器。

阅读顺序：

1. [`src/system.c`](../src/system.c)
2. [`src/riscv.c`](../src/riscv.c)
3. [`src/devices/minimal.dts`](../src/devices/minimal.dts)
4. [`src/devices/uart.c`](../src/devices/uart.c)
5. [`src/devices/plic.c`](../src/devices/plic.c)
6. [`src/devices/rtc.c`](../src/devices/rtc.c)
7. [`src/devices/virtio-blk.c`](../src/devices/virtio-blk.c)

核心链路：

```text
guest 访问 MMIO 地址
  -> mem_translate / MMIO handler
  -> UART / RTC / virtio-blk
  -> PLIC 更新中断
  -> trap_handler()
```

重点理解：

* Sv32 页表如何把虚拟地址翻译为物理地址。
* trap 和 interrupt 如何进入 S-mode 或 M-mode handler。
* UART 如何提供控制台输入输出。
* PLIC 如何汇总外设中断。
* virtio-blk 如何解析 virtqueue 并访问磁盘镜像。

动手任务：

* 给 UART 读写加日志，跟踪一次 guest 输出字符的完整路径。
* 查看 `minimal.dts` 中设备地址如何对应到 C 代码里的 MMIO handler。
* 给 PLIC 更新中断处加日志，观察外设中断如何被投递。

## 推荐组合

如果目标是系统性学习，推荐顺序：

```text
路线 1 -> 路线 2 -> 路线 3 -> 路线 4 -> 路线 5 或 路线 6
```

如果目标是学习 RISC-V 指令执行，重点走路线 2 和路线 3。

如果目标是学习模拟器工程实现，重点走路线 2、路线 4 和路线 6。

如果目标是学习性能优化和 JIT，先走路线 2，再走路线 5。

