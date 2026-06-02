# 代码生成

## 概览

rv32emu 使用分层执行策略：解释器负责基线执行，一级 JIT 为热点基本块生成宿主
机器码，二级 JIT 使用 LLVM 对更热的路径做进一步优化。

代码生成层支持 x86-64 和 Arm64 宿主架构。`jit.c` 提供跨架构发射 API，
`rv32_jit.c` 定义每条 RISC-V 指令如何发射宿主指令，`t2c.c` 与
`t2c_template.c` 则负责 LLVM IR 生成。

## 执行层级

| 层级 | 说明 |
| --- | --- |
| 解释器 | 使用尾调用调度直接执行 RISC-V 指令 |
| 一级 JIT | 使用模板化发射器为基本块生成宿主机器码 |
| 二级 JIT | 使用 LLVM 编译高频热点基本块 |

基本块达到阈值后会从解释器提升到一级 JIT；如果仍持续高频执行，可进一步交给
二级 LLVM JIT 编译。

## 源文件组织

| 文件 | 作用 |
| --- | --- |
| `src/rv32_template.c` | 解释器指令语义，使用 `RVOP` 宏 |
| `src/rv32_jit.c` | 一级 JIT 指令生成器，使用 `GEN` 宏并由 `jit.c` include |
| `src/rv32_constopt.c` | IR 层常量传播与折叠 |
| `src/jit.c` | 一级 JIT 基础设施、`emit_*` API、寄存器分配和融合处理 |
| `src/t2c.c` | 二级 JIT 驱动，管理 LLVM 模块、执行引擎和缓存 |
| `src/t2c_template.c` | 二级 JIT 指令处理器，使用 `T2C_OP` 宏生成 LLVM IR |
| `src/emulate.c` | 执行循环、尾调用调度、基本块翻译和宏操作融合 |

## 解释器实现

解释器用 `RVOP` 宏定义指令处理器：

```c
RVOP(name, { body })
```

每个处理器接收模拟器状态 `rv`、已解码指令 `ir`、周期计数 `cycle` 和当前 `PC`，
并返回是否继续执行。示例：

```c
RVOP(addi, { rv->X[ir->rd] = rv->X[ir->rs1] + ir->imm; })
```

解释器采用 Tail Call Threaded Code 方式调度。当前指令处理器通过尾调用进入下一
条指令处理器，减少传统 `switch` 分发开销。

## 一级 JIT

一级 JIT 使用 `GEN` 宏定义宿主机器码生成器：

```c
GEN(name, { body })
```

生成器通过 `emit_*` 函数向代码缓冲区写入宿主指令。寄存器分配器会把 RISC-V
寄存器映射到少量宿主寄存器，并在需要时从 `riscv_t` 中加载或回写。

常见辅助宏：

| 宏 | 覆盖指令 |
| --- | --- |
| `GEN_BRANCH` | `beq`、`bne`、`blt`、`bge`、`bltu`、`bgeu` |
| `GEN_CBRANCH` | `cbeqz`、`cbnez` |
| `GEN_ALU_IMM` | `addi`、`xori`、`ori`、`andi` |
| `GEN_ALU_REG` | `add`、`sub`、`xor`、`or`、`and` |
| `GEN_SHIFT_IMM` | `slli`、`srli`、`srai` |
| `GEN_SHIFT_REG` | `sll`、`srl`、`sra` |
| `GEN_LOAD` | `lb`、`lh`、`lw`、`lbu`、`lhu` |
| `GEN_STORE` | `sb`、`sh`、`sw` |

## 二级 JIT

二级 JIT 使用 LLVM C API，把 IR 转成 LLVM IR：

```c
T2C_OP(name, { body })
```

LLVM 会执行优化、寄存器分配和目标代码生成。二级 JIT 编译成本更高，适合长期
执行的热点路径。当前构建要求 LLVM 18 到 21。

## IR 优化

执行或编译前，`src/rv32_constopt.c` 会对基本块做常量传播和折叠。例如 `lui`
后接 `addi` 构造 32 位常量时，可以减少运行时重复计算。该优化同时服务解释器、
一级 JIT 和二级 JIT。

## 双架构支持

`jit.c` 用 `emit_*` API 屏蔽 x86-64 和 Arm64 的指令编码差异。部分常量沿用
x86-64 编码值作为符号标识，例如 `JCC_JE`、`ALU_OP_ADD`，Arm64 后端再映射到
对应条件码或指令序列。

## 基本块链接

当一个已翻译基本块以直接分支结束，并且目标基本块也已经翻译时，JIT 可以修补
跳转目标，让两个基本块直接相连，避免回到调度器。该能力由
`CONFIG_BLOCK_CHAINING` 控制。

## 宏操作融合

`emulate.c` 会识别常见指令序列，并把它们融合成一个更粗粒度操作。融合后的操作
在解释器、一级 JIT 和二级 JIT 中分别有专用处理器，以减少分发次数和中间状态
读写。该能力由 `CONFIG_MOP_FUSION` 控制。

## 内存访问和 MMIO

系统模式下，load/store 需要检查 MMIO 和 MMU。一级 JIT 的 `GEN_LOAD`、
`GEN_STORE` 会根据 `RV32_HAS(SYSTEM_MMIO)` 选择直接 RAM 快路径，或进入 MMU/MMIO
handler。非系统模式可以跳过这些检查，直接访问客体内存。
