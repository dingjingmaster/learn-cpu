# learn-cpu

`learn-cpu` 是一个面向学习和实验的 32 位 RISC-V 模拟器项目，代码基础来自
`rv32emu`。它可以加载并执行 RV32 ELF 程序，也可以在系统模拟模式下引导
Linux 内核；在性能路径上提供解释器、一级模板 JIT 和二级 LLVM JIT。

项目适合用来学习 CPU 指令解码、解释执行、内存模型、系统调用、MMIO 设备、
JIT 代码生成，以及一个中等规模 C 项目的构建组织方式。

## 快速开始

```sh
make defconfig
make
build/rv32emu build/hello.elf
```

常用目标：

| 命令 | 作用 |
| --- | --- |
| `make defconfig` | 生成默认配置 `.config` |
| `make config` | 打开 Kconfig 交互式配置菜单 |
| `make` | 构建模拟器，默认输出为 `build/rv32emu` |
| `make check` | 构建并运行项目测试 |
| `make jit_defconfig` | 生成启用 JIT 的配置 |
| `make system_defconfig` | 生成系统模拟模式配置 |
| `make wasm_defconfig` | 生成 WebAssembly 构建配置 |
| `make clean` | 清理构建产物 |
| `make cleanconfig` | 清理构建产物和配置文件 |

## 运行方式

用户态 ELF 执行：

```sh
make defconfig
make
build/rv32emu build/hello.elf
```

启用 JIT 后执行：

```sh
make jit_defconfig
make
build/rv32emu build/dhrystone.elf
```

系统模拟模式用于运行内核镜像、initrd/rootfs 和 virtio 块设备：

```sh
make system_defconfig
make
build/rv32emu -k <kernel-image> -i <rootfs-image> -b "<bootargs>"
```

程序参数和常用选项：

| 选项 | 说明 |
| --- | --- |
| `-t` | 在非系统模式下打印执行 trace |
| `-g` | 启用 GDB 远程调试 stub |
| `-d <file>` | 将寄存器状态以 JSON 写入文件，`-` 表示标准输出 |
| `-a <file>` | 输出 RISC-V 架构测试 signature |
| `-m` | 允许非对齐内存访问 |
| `-p` | 生成 profiling 数据 |
| `-q` | 静默普通输出，只保留显式导出内容 |
| `-k <image>` | 系统模式下指定内核镜像 |
| `-i <image>` | 系统模式下指定 rootfs/initrd |
| `-x vblk:<image>[,readonly]` | 添加 virtio-blk 磁盘镜像 |
| `-b <bootargs>` | 指定内核启动参数 |

## 项目结构

```text
.
├── Makefile                  # 顶层构建入口，串联配置、工具链、依赖和目标
├── mk/                       # 拆分后的构建模块
│   ├── common.mk             # 通用宏、输出控制、测试模板
│   ├── kconfig.mk            # Kconfig 配置生成与菜单入口
│   ├── toolchain.mk          # 编译器、平台和依赖探测
│   ├── deps.mk               # SDL、LLVM、工具链等依赖检查
│   ├── softfloat.mk          # Berkeley SoftFloat 集成
│   ├── tests.mk              # 单元测试、示例和检查目标
│   ├── system.mk             # Linux 系统模式镜像和 DTB 构建
│   ├── wasm.mk               # WebAssembly 构建和网页资源
│   └── artifact.mk           # 预构建测试程序和基准程序下载
├── configs/                  # Kconfig 菜单和预设配置
├── src/                      # 模拟器核心源码
│   ├── main.c                # 命令行入口，解析参数并创建/运行模拟器
│   ├── riscv.c/.h            # RISC-V 实例生命周期、寄存器、内存和系统模式装载
│   ├── emulate.c             # 主执行循环、解释器调度、trap 和宏操作融合
│   ├── decode.c/.h           # RV32/RVC 指令字段解析和内部 IR 构造
│   ├── rv32_template.c       # 解释器指令语义实现
│   ├── jit.c/.h              # 一级模板 JIT 的代码缓存、发射器和运行时辅助
│   ├── rv32_jit.c            # 一级 JIT 指令模板
│   ├── t2c.c                 # 二级 LLVM JIT 编译驱动
│   ├── t2c_template.c        # 二级 JIT 的 LLVM IR 指令语义
│   ├── io.c/.h               # 客体内存、按需分页和内存访问 API
│   ├── syscall.c             # 用户态系统调用转发
│   ├── syscall_sdl.c         # 图形、输入和音频相关 SDL 系统调用
│   ├── system.c/.h           # 特权态、MMU、异常和中断相关系统支持
│   ├── elf.c/.h              # ELF 读取、段装载和符号查询
│   ├── cache.c/.h            # JIT 基本块缓存
│   ├── map.c/.h              # 通用映射容器
│   ├── mpool.c/.h            # 小对象内存池
│   └── devices/              # UART、PLIC、RTC、virtio-blk 等 MMIO 设备
├── tests/                    # 单元测试、示例程序、基准程序和系统测试
├── docs/                     # 指令、系统调用、代码生成、演示程序等说明
├── assets/                   # WebAssembly 页面资源和系统模式配置
├── tools/                    # 构建辅助工具、镜像生成和分析工具
└── docker/                   # 交叉工具链和参考模型容器构建脚本
```

## 核心原理

### 1. 从命令行到模拟器实例

`main.c` 解析命令行参数，生成 `vm_attr_t` 配置结构，然后调用 `rv_create` 创建
`riscv_t` 实例。`vm_attr_t` 会携带内存大小、栈大小、程序参数、日志级别、
JIT/trace/profile 标志，以及用户态 ELF 或系统模式内核/rootfs 信息。

创建完成后，入口调用 `rv_run` 进入执行循环；执行结束后按需导出寄存器、
架构测试 signature 和 profiling 文件，最后调用 `rv_delete` 释放模拟器资源。

### 2. ELF 装载和内存布局

用户态模式通过 `elf.c` 读取 ELF 文件，把可加载段映射到客体内存中，并准备
栈、参数和初始 PC。系统模式则把内核镜像、initrd/rootfs 和 DTB 放入客体内存，
再由模拟器提供必要的 MMIO 设备，使客体内核能够完成启动。

`io.c` 负责客体内存。支持 `mmap` 的平台会先保留一段虚拟地址空间，并使用
`PROT_NONE` 加信号处理实现按需分页：客体第一次访问某个内存块时触发信号，
处理函数再为对应块打开读写权限，从而降低大内存配置下的初始物理内存占用。

### 3. 指令解码和内部表示

`decode.c` 按 RISC-V 指令格式抽取 `rd`、`rs1`、`rs2`、`funct3`、`funct7` 和
各种立即数字段。普通 32 位指令与 RVC 16 位压缩指令会被统一转换成
`rv_insn_t` 内部表示，后续解释器和 JIT 都围绕这份 IR 工作。

RISC-V 的 B/J/S 等格式会把立即数字段拆散放在指令字的不同位置。项目在解码
阶段完成重排和符号扩展，使执行阶段只需使用已经规整好的立即数。

### 4. 解释器执行

解释器语义主要写在 `rv32_template.c` 中，通过 `RVOP(name, { ... })` 宏描述
每条指令的效果。执行循环在 `emulate.c` 中组织基本块和指令调度。

解释器使用尾调用调度思路，让当前指令处理函数直接跳到下一条指令处理函数，
减少传统 `switch` 分发的开销。对于频繁出现的指令组合，`emulate.c` 还会做
宏操作融合，把多条 IR 合并成一个更粗粒度的执行单元。

### 5. JIT 分层

项目支持三层执行：

| 层级 | 文件 | 说明 |
| --- | --- | --- |
| 解释器 | `rv32_template.c`、`emulate.c` | 最通用，便于验证语义 |
| 一级 JIT | `jit.c`、`rv32_jit.c` | 使用模板直接发射 x86-64/Arm64 机器码 |
| 二级 JIT | `t2c.c`、`t2c_template.c` | 使用 LLVM C API 生成并优化热点基本块 |

一级 JIT 更轻量，适合快速把热点基本块转成宿主机器码；二级 JIT 编译成本更高，
但能利用 LLVM 的优化、寄存器分配和目标后端，在长期运行的热点路径上获得更好
性能。JIT 基本块由 `cache.c` 管理，系统模式下还需要在 `SFENCE.VMA`、
`FENCE.I` 等指令后处理地址空间和指令缓存失效。

### 6. 系统调用和设备模拟

用户态程序通过 `ecall` 进入模拟器的系统调用处理逻辑。`syscall.c` 实现了
newlib 常用的一小组系统调用，例如 `read`、`write`、`open`、`close`、`brk`、
`gettimeofday` 和 `clock_gettime`。图形、输入和音频演示程序使用
`syscall_sdl.c` 提供的非标准 SDL 扩展调用。

系统模式启用后，模拟器会提供 PLIC、UART、RTC 和 virtio-blk 等设备。设备
通过 MMIO 暴露给客体系统，Linux 内核把它们当成真实平台设备访问。

### 7. 特权态、MMU 和 trap

`system.c`、`riscv.h` 和 `riscv_private.h` 保存 CSR、特权级、页表、异常和
中断状态。系统模式下，访存会经过 SV32 地址转换，异常和中断通过 `mcause`、
`scause`、`mepc`、`sepc` 等 CSR 交给客体处理。非系统模式下，部分异常会由
模拟器直接处理，例如可选的非对齐访存模拟。

## 构建系统原理

顶层 `Makefile` 只负责组装构建流程，具体逻辑拆在 `mk/` 目录中：

1. `mk/common.mk` 定义通用宏、输出格式、配置检查和测试模板。
2. `mk/kconfig.mk` 负责生成 `.config` 和 `src/feature.h`。
3. `mk/toolchain.mk`、`mk/deps.mk` 探测编译器、SDL、LLVM、RISC-V 工具链等。
4. 顶层 `Makefile` 根据 Kconfig 结果拼接 `CFLAGS`、`LDFLAGS` 和对象文件列表。
5. `mk/softfloat.mk`、`mk/system.mk`、`mk/wasm.mk` 按需加入额外库和资源。
6. `mk/tests.mk` 和 `mk/riscv-arch-test.mk` 提供测试入口。

Kconfig 选项会被转换成 `RV32_FEATURE_*` 宏，源码再通过 `RV32_HAS(...)` 判断
某个功能是否启用。这样同一套源码可以构建出最小解释器、带 JIT 的本地版本、
系统模拟版本和 WebAssembly 版本。

## 重要配置

| 配置 | 作用 |
| --- | --- |
| `CONFIG_EXT_M/A/F/C` | 启用 RV32M/A/F/C 标准扩展 |
| `CONFIG_Zicsr` | 启用 CSR 指令 |
| `CONFIG_Zifencei` | 启用指令取指栅栏 |
| `CONFIG_JIT` | 启用一级 JIT |
| `CONFIG_T2C` | 启用 LLVM 二级 JIT |
| `CONFIG_SYSTEM` | 启用系统模拟模式 |
| `CONFIG_ELF_LOADER` | 系统模式下使用 ELF 装载器 |
| `CONFIG_SDL` | 启用图形和输入支持 |
| `CONFIG_SDL_MIXER` | 启用音频播放支持 |
| `CONFIG_ARCH_TEST` | 启用 RISC-V 架构合规测试支持 |

## 测试和示例

`tests/` 目录包含多类测试资产：

| 目录或文件 | 说明 |
| --- | --- |
| `tests/*.c` | 单文件示例、图形 demo、算法和基准程序 |
| `tests/cache/` | 缓存容器行为测试 |
| `tests/map/` | 映射容器测试 |
| `tests/asm-hello/` | 最小汇编 ELF 示例 |
| `tests/chacha20/` | C 与汇编混合测试 |
| `tests/readelf/` | 在模拟器中运行的 readelf 示例 |
| `tests/system/` | 系统模式、MMU、对齐异常相关测试 |
| `tests/arch-test-target/` | RISCOF/RISC-V 架构测试目标配置 |

运行测试：

```sh
make defconfig
make check
```

运行架构测试通常需要额外工具链和预构建参考模型：

```sh
make ci_defconfig
make arch-test
```

## 学习建议

更完整的分路线阅读计划见 [docs/learning-roadmap.md](docs/learning-roadmap.md)。

如果目标是理解 CPU 模拟器，可以按下面顺序阅读：

1. `src/main.c`：了解程序入口和 `vm_attr_t` 如何描述一次运行。
2. `src/riscv.h`：了解寄存器、CSR、trap 编码和模拟器状态结构。
3. `src/io.c`：了解客体内存如何分配、访问和按需分页。
4. `src/elf.c`：了解 ELF 如何被装载到客体地址空间。
5. `src/decode.c`：了解 RISC-V 指令字段如何被拆解成 IR。
6. `src/emulate.c` 与 `src/rv32_template.c`：了解解释器执行路径。
7. `src/jit.c` 与 `src/rv32_jit.c`：了解一级 JIT 如何发射宿主机器码。
8. `src/t2c.c` 与 `src/t2c_template.c`：了解 LLVM 二级 JIT。
9. `src/system.c` 与 `src/devices/`：了解 Linux 系统模式和设备模拟。

## 常见问题

### 没有 `.config`

先运行一个 defconfig 目标：

```sh
make defconfig
```

### 缺少 SDL2

如果只想运行命令行程序，可以在 `make config` 中关闭 SDL；如果要运行 Doom、
Quake 或图形测试，需要安装 SDL2 和 SDL2_mixer 开发库。

### JIT 构建失败

一级 JIT 只支持 x86-64 和 Arm64 宿主平台。二级 JIT 还需要 LLVM 18 开发库。
如果只是学习解释器，可以使用默认配置或在 `make config` 中关闭 JIT。

### 系统模式无法启动

系统模式需要内核镜像、rootfs/initrd 和正确的启动参数。确认使用了
`make system_defconfig`，并检查 `-k`、`-i`、`-b` 参数是否和镜像匹配。
