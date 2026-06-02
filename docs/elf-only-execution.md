# 只运行 ELF 文件分支分析

本文分析项目中“只运行 ELF 文件”相关分支具体做了哪些工作，并按源码执行顺序说明从命令行参数、虚拟机创建、ELF 装载、系统调用到退出清理的完整过程。

## 1. 分支定位

项目里需要区分两个概念：

- `CONFIG_ELF_LOADER=y`：Kconfig 中的“系统模式 ELF 装载器”。它依赖 `CONFIG_SYSTEM=y`，含义是在系统模式下不走 Linux 内核镜像启动路径，而是直接装载 ELF 文件。
- `!RV32_HAS(SYSTEM_MMIO)`：源码中大量使用的实际执行分支。它表示“不是带 MMIO 设备的 Linux 内核启动路径”，因此既包含普通非系统 ELF 运行，也包含 `SYSTEM + ELF_LOADER` 的系统模式 ELF 运行。

对应关系如下：

| 配置组合 | `SYSTEM_MMIO` | 运行对象 | 主要特征 |
| --- | --- | --- | --- |
| `CONFIG_SYSTEM=n` | 0 | 普通 ELF 程序 | 默认用户态模拟路径，直接内存访问，不初始化 Linux 设备 |
| `CONFIG_SYSTEM=y` 且 `CONFIG_ELF_LOADER=y` | 0 | ELF 程序 | 系统模式 ELF 装载，使用系统/MMU 相关 I/O 回调，但不装载 kernel/initrd |
| `CONFIG_SYSTEM=y` 且 `CONFIG_ELF_LOADER=n` | 1 | Linux kernel/initrd | 内核引导路径，装载 DTB、initrd、UART、PLIC、virtio-blk 等设备 |

`src/feature.h` 中把 `SYSTEM_MMIO` 定义为 `SYSTEM && !ELF_LOADER`。因此只要启用 `ELF_LOADER`，系统模式也会从内核/MMIO 启动分支切到 ELF 装载分支。

当前仓库的 `.config` 中 `CONFIG_SYSTEM` 未启用，因此普通 `make` 构建出的运行路径属于第一类：直接运行一个 ELF 程序。

## 2. 构建阶段做的工作

构建系统先把 Kconfig 选项转换成源码使用的 `RV32_FEATURE_*` 宏：

- `Makefile` 调用 `$(call set-features, ELF_LOADER ...)`、`$(call set-features, SYSTEM ...)`。
- `mk/common.mk` 中的 `set-feature` 会把配置写入 `CFLAGS`，例如 `-DRV32_FEATURE_ELF_LOADER=1`。
- `src/feature.h` 再通过 `RV32_HAS(x)` 宏统一给源码判断使用。

内存布局由 `mk/system.mk` 选择：

- 内核引导模式，也就是 `CONFIG_SYSTEM=y && CONFIG_ELF_LOADER!=y`，会设置 `MEM_SIZE`、`DTB_SIZE`、`INITRD_SIZE`，并为 DTB、initrd、设备模型准备地址空间。
- ELF 装载模式，也就是 `CONFIG_ELF_LOADER=y`，会设置 `USER_MEM_SIZE ?= 4096`，最终传入 `-DMEM_SIZE=...`，为 ELF 程序提供 4GB 的 32 位地址空间。
- 非系统模式同样使用 `USER_MEM_SIZE ?= 4096`，也就是默认 4GB 虚拟地址空间。

这一步的核心效果是：只运行 ELF 的分支不会准备 kernel/initrd/DTB 的内存布局，也不会把系统运行目标绑定到 `-k`、`-i` 参数。

## 3. 总体执行流程

只运行 ELF 的主流程可以概括为：

```text
main()
  parse_args()
  组装 vm_attr_t
  attr.data.user.elf_program = opt_prog_name
  rv_create(&attr)
    memory_new()
    rv_reset()
    初始化 fd_map 和标准输入输出
    elf_open()
    查找 _end、exit/_exit 等符号
    elf_load()
    rv_set_pc(e_entry)
    安装内存访问和 ecall/ebreak/trap 回调
    初始化基本块、IR、JIT/解释器缓存
  rv_run()
    rv_step() 循环执行基本块
    ecall -> syscall_handler()
    exit syscall -> rv_halt()
  可选导出寄存器或架构测试 signature
  rv_delete()
  返回 attr.exit_code
```

下面按代码路径展开。

## 4. 命令行参数与 `vm_attr_t`

入口在 `src/main.c`。

`parse_args()` 会按编译分支开放不同参数：

- `!SYSTEM_MMIO` 时支持 `-t` 跟踪执行位置。
- `SYSTEM_MMIO` 时才支持 `-k`、`-i`、`-b`、`-x`，分别用于 kernel、initrd、bootargs 和 virtio-blk。只运行 ELF 时这些参数不会进入当前编译分支。
- `-q`、`-m`、`-p`、`-d`、`-a` 属于通用选项。

参数解析结束后：

- `optind` 指向第一个非选项参数，也就是要运行的 ELF 文件。
- `prog_args = &args[optind]` 保存传给客体程序的参数数组。
- `opt_prog_name = prog_args[0]` 保存 ELF 路径。
- `prog_argc = argc - emu_argc - 1` 计算客体程序可见的 `argc`。

随后 `main()` 构造 `vm_attr_t attr`。只运行 ELF 时关键字段是：

- `mem_size = MEM_SIZE`，来自构建系统，默认 4GB。
- `stack_size = 0x1000`，保留 4KB 主栈。
- `args_offset_size = 0x1000`，保留 4KB 参数偏移区。
- `argc/argv`，传给客体 ELF 的参数。
- `run_flag`，由 trace、gdbstub、profile 等选项组合。
- `fd_stdin/fd_stdout/fd_stderr`，初始映射到宿主标准输入输出。
- `attr.data.user.elf_program = opt_prog_name`，这是进入 ELF 装载分支的关键字段。

如果是 `SYSTEM_MMIO` 分支，`attr.data.system` 会填入 kernel、initrd、bootargs、vblk；只运行 ELF 时不会填这些字段。

## 5. 创建虚拟机与初始化内存

`rv_create()` 位于 `src/riscv.c`。

第一步是分配并保存运行时对象：

- `calloc(1, sizeof(riscv_t))` 创建 CPU/运行时状态。
- `rv->data = rv_attr` 保存 `vm_attr_t` 指针。
- `attr->mem = memory_new(attr->mem_size)` 创建客体内存。

`memory_new()` 位于 `src/io.c`。在支持 `mmap` 的平台上，它不是一次性提交 4GB 物理内存，而是：

- 用 `mmap(PROT_NONE)` 预留一段客体地址空间。
- 注册 `SIGSEGV/SIGBUS` 处理器。
- 首次访问某个 64KB chunk 时，通过 `mprotect(PROT_READ | PROT_WRITE)` 激活该 chunk。
- `memory_gc()` 可以回收全零 chunk，降低宿主实际内存占用。

因此 ELF 分支虽然默认给客体 4GB 地址空间，但实际占用取决于 ELF 段、栈、堆和运行时访问过的页面。

## 6. `rv_reset()` 设置寄存器、栈和参数

`rv_create()` 调用 `rv_reset(rv, 0U)` 初始化 CPU 状态。

`rv_reset()` 的主要工作：

- 清零 32 个整数寄存器。
- 设置初始 `PC = 0`。真正入口稍后由 ELF header 的 `e_entry` 覆盖。
- 设置默认栈指针：

```text
sp = mem_size - stack_size - args_offset_size
```

在 `!SYSTEM_MMIO` 分支中，它还会把客体程序的 `argc/argv` 写入客体内存：

- 参数字符串写到靠近内存顶部的参数区。
- 再构造栈帧，布局为 `argc`、`argv[0..n-1]`、`NULL`。
- 最后把 `sp` 重置为这个栈帧的起始地址。

特权级初始化也在这里完成：

- `CONFIG_SYSTEM=n` 时，模拟器默认进入 M 模式。
- `CONFIG_SYSTEM=y` 时，包括 `SYSTEM + ELF_LOADER`，默认进入 S 模式，并清空 `satp`、iTLB、dTLB 等地址转换状态。

这说明普通 ELF 模式和系统 ELF_LOADER 模式虽然都不走 `SYSTEM_MMIO`，但初始特权级和内存访问回调不同。

## 7. 打开、校验和装载 ELF

ELF 处理代码在 `src/elf.c`。

`rv_create()` 进入 `#if !RV32_HAS(SYSTEM_MMIO)` 后会：

1. `elf_new()` 创建 ELF 读取对象。
2. `elf_open(elf, attr->data.user.elf_program)` 打开 ELF 文件。
3. 查询符号并设置运行时辅助地址。
4. `elf_load(elf, attr->mem)` 把 ELF 段写入客体内存。
5. `get_elf_header()` 读取 ELF header。
6. `rv_set_pc(rv, hdr->e_entry)` 把 PC 设置到 ELF 入口地址。

`elf_open()` 做的事情：

- 通过 `sanitize_path()` 处理输入路径。
- 支持 `mmap` 时直接把 ELF 文件映射到宿主内存；否则用 `fread()` 读入缓冲区。
- 把 `e->hdr` 指向 ELF header。
- 校验 magic 是否为 `\177ELF`。
- 校验 `EI_CLASS == ELFCLASS32`，只接受 32 位 ELF。
- 校验 `e_machine == EM_RISCV`，只接受 RISC-V ELF。

`elf_load()` 只处理 program header 中的 `PT_LOAD` 段：

- 用 `p_vaddr` 作为客体虚拟地址。
- 把文件里实际存在的数据从 `p_offset` 复制到客体内存。
- 对 `memsz` 大于文件数据的区域补零，这对应 `.bss` 等运行时需要清零的段。

注意这里是最小 ELF 装载器，不做动态链接器工作，也不解析复杂 loader 语义。它适合装载已经面向该模拟器/裸机环境构建好的 RV32 ELF。

## 8. 符号辅助：`_end`、`exit`、架构测试符号

ELF 装载前后会查询几个符号：

- `_end`：如果 ELF 符号表里存在 `_end`，`rv_create()` 把 `attr->break_addr` 设置为 `_end` 的地址。`syscall_brk()` 会基于这个地址维护客体堆顶。
- `exit` 或 `_exit`：仅在非系统构建中查找，保存到 `attr->exit_addr`。执行循环看到基本块入口等于该地址时，会把 `attr->on_exit` 置为 true，用于区分正常退出阶段和异常关闭标准 fd。
- `tohost/fromhost`：架构测试模式下，`main()` 会重新打开 ELF，读取这两个符号地址，并写入 `rv` 状态。测试程序写 `tohost` 后，模拟器可以识别测试完成并停止。
- `begin_signature/end_signature`：架构测试结束后，`dump_test_signature()` 会根据这两个符号导出测试 signature；如果没有这些符号，则使用 `.data` section 范围。

这些符号不是 ELF 运行的必要条件，但会影响堆管理、退出识别和测试输出。

## 9. 安装内存 I/O 和异常回调

ELF 装载完成后，`rv_create()` 根据配置安装不同的 `riscv_io_t`。

普通非系统 ELF 路径：

- `mem_ifetch = memory_ifetch`
- `mem_read_w/s/b = memory_read_w/s/b`
- `mem_write_w/s/b = memory_write_w/s/b`
- `on_ecall = ecall_handler`
- `on_ebreak = ebreak_handler`
- `on_memcpy = memcpy_handler`
- `on_memset = memset_handler`
- `on_trap = trap_handler`

这是一套直接内存访问路径，没有 MMIO 设备，也没有 Linux 内核的页表和设备中断。

`SYSTEM + ELF_LOADER` 路径：

- 仍然走 `!SYSTEM_MMIO` 的 ELF 装载逻辑。
- 但因为 `RV32_HAS(SYSTEM)` 为真，会安装 `system.c` 中的 `mmu_io`。
- `mmu_io` 会通过 `mmu_translate()` 做 Sv32 地址转换，并在页故障或权限问题时设置重新翻译标记。

也就是说 `ELF_LOADER` 不是简单地等于普通用户态 ELF。它共享 ELF 装载入口，但保留系统模式的 MMU/trap 处理能力。

## 10. 系统调用和文件描述符映射

系统调用处理在 `src/syscall.c`，触发入口在 `src/emulate.c` 的 `ecall_handler()`。

文件描述符映射由 `rv_create()` 初始化：

- `attr->fd_map = map_init(int, FILE *, map_cmp_int)`。
- `rv_remap_stdstream()` 把客体 fd 0、1、2 映射到宿主 `stdin`、`stdout`、`stderr`。
- `open` 会在宿主打开文件，并给客体分配从 3 开始的新 fd。
- `read/write` 会在客体内存和宿主 `FILE *` 之间搬运数据。

当前支持的常用 syscall 包括：

- `read = 63`
- `write = 64`
- `close = 57`
- `lseek = 62`
- `fstat = 80`
- `exit = 93`
- `gettimeofday = 169`
- `brk = 214`
- `clock_gettime = 403`
- `open = 1024`

`ecall_handler()` 的分支差异：

- 启用 `ELF_LOADER` 时，处理器先 `rv->PC += 4`，然后直接调用 `syscall_handler(rv)`。
- 普通非系统构建中，处理器先记录 `ECALL_M` trap，再调用 `syscall_handler(rv)`；没有 trap vector 时，默认 trap handler 会跳过当前指令。
- 系统但非 ELF_LOADER 的内核/MMIO 模式中，U 模式 ecall 通常交给 guest OS trap 处理，S 模式 ecall 则作为 SBI 调用进入 `syscall_handler()`。

`syscall_handler()` 从寄存器取系统调用号：

- 普通 RV32 使用 `a7`。
- RV32E 使用 `t0`。

然后按 syscall 编号分派到具体实现，并把返回值写回 `a0`。例如 `syscall_write()` 从客体内存读取 buffer，再写到宿主文件；`syscall_brk()` 更新 `attr->break_addr`；`syscall_exit()` 调用 `rv_halt(rv)` 并保存 `attr->exit_code`。

## 11. 执行循环：从 PC 到基本块

`rv_run()` 位于 `src/riscv.c`。

默认模式下，它循环调用 `rv_step(rv)`，直到 `rv_has_halted(rv)` 为真：

```text
for (; !rv_has_halted(rv);)
    rv_step(rv);
```

如果启用了 trace，并且处于 `!SYSTEM_MMIO` 分支，则进入 `rv_run_and_trace()`：

- 每次执行前读取当前 PC。
- 通过 `elf_find_symbol()` 尝试把 PC 映射到符号名。
- 打印地址和符号。
- 再执行一次 `rv_step()`。

`rv_step()` 位于 `src/emulate.c`，它不是每次只执行一条指令，而是按 `cycle_per_step` 执行一批 cycle：

- 根据当前 `PC` 查找已有基本块。
- 找不到就调用 `block_find_or_translate()` 翻译新基本块。
- 如果启用基本块链接，会把上一个基本块的 taken/untaken 目标连到当前基本块，减少后续分派开销。
- 如果启用 JIT，热点基本块会进入一级 JIT；启用 T2C 时，热点还可能排队进入二级 LLVM JIT。
- 否则使用解释器执行 `block->ir_head` 开始的 IR 链。

只运行 ELF 的普通路径不会检查 UART、PLIC、RTC、virtio-blk 等 MMIO 中断；这些只在 `SYSTEM_MMIO` 分支中启用。

## 12. 退出与收尾

目标 ELF 正常退出时通常会执行 `exit` 系统调用：

1. 客体程序执行 `ecall`。
2. `ecall_handler()` 分派到 `syscall_handler()`。
3. `syscall_exit()` 调用 `rv_halt(rv)`。
4. `syscall_exit()` 把客体 `a0` 中的退出码保存到 `attr->exit_code`。
5. `rv_run()` 主循环看到 `rv_has_halted(rv)` 为真后返回。
6. `main()` 最终 `return attr.exit_code`。

运行结束后，`main()` 还会根据选项做后处理：

- `-d`：调用 `dump_registers()` 导出寄存器 JSON。
- `-a`：调用 `dump_test_signature()` 导出架构测试 signature。
- profile：`rv_run()` 结束前调用 `rv_profile()` 输出基本块关系数据。

最后 `rv_delete()` 释放运行时资源：

- 删除文件描述符映射。
- 释放客体内存。
- 释放基本块、IR、融合池、JIT cache 等执行结构。
- 只有 `SYSTEM_MMIO` 分支才需要释放 UART、PLIC、RTC、virtio-blk 等设备。

## 13. 与内核引导分支的关键差异

只运行 ELF 分支没有做这些事情：

- 不要求 `-k` 指定 Linux kernel。
- 不要求 `-i` 指定 initrd/rootfs。
- 不构造 DTB。
- 不初始化 UART、PLIC、Goldfish RTC、virtio-blk 等 MMIO 设备。
- 不设置 Linux hart 启动参数 `a0=hartid`、`a1=dtb_addr`。
- 不使用 kernel/initrd/DTB 的固定内存布局。

它真正做的是：

- 从命令行找到一个 RV32 ELF。
- 建立 4GB 客体地址空间。
- 构造 `argc/argv` 栈。
- 校验并装载 ELF 的 `PT_LOAD` 段。
- 设置 PC 到 ELF 入口。
- 通过 `ecall` 模拟一小组 newlib/POSIX 风格系统调用。
- 用解释器或 JIT 执行基本块，直到目标程序调用 `exit` 或测试写 `tohost`。

## 14. 学习代码时建议关注的文件顺序

建议按下面顺序阅读：

1. `configs/Kconfig`：理解 `SYSTEM`、`ELF_LOADER`、`SYSTEM_MMIO` 的配置关系。
2. `src/feature.h`：确认编译期宏如何变成 `RV32_HAS(...)`。
3. `mk/system.mk`：理解 ELF 分支和 kernel/initrd 分支的内存布局差异。
4. `src/main.c`：看命令行参数如何进入 `vm_attr_t`。
5. `src/riscv.c`：重点看 `rv_create()`、`rv_run()`、`rv_reset()`、`rv_delete()`。
6. `src/elf.c`：看 ELF 文件如何校验、查符号、装载 `PT_LOAD` 段。
7. `src/io.c`：看 4GB 客体内存如何用按需分页降低实际占用。
8. `src/emulate.c`：看 `rv_step()`、`ecall_handler()`、trap handler 和基本块执行。
9. `src/syscall.c`：看 `read/write/open/brk/exit` 如何桥接客体程序和宿主环境。

