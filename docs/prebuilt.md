# 预构建二进制

项目提供预构建二进制，主要用于两类场景：

* RISC-V 架构测试需要 [RISC-V Sail Model](https://github.com/riscv/sail-riscv)
  参考模拟器。
* 一组已编译好的 RISC-V ELF 程序可用于 ISA 模拟验证、基准测试和 demo 运行。

部分预构建产物存放在
[rv32emu-prebuilt](https://github.com/sysprog21/rv32emu-prebuilt)。测试或基准测试
运行时，构建系统默认会把它们下载到 `build/linux-x86-softfp/` 和
`build/riscv32/`。

RISC-V 二进制使用
[xPack RISC-V GCC toolchain](https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack)
编译，常用参数为 `-march=rv32im -mabi=ilp32`。x86 参考二进制使用 GCC 的
`-m32 -mno-sse -mno-sse2 -msoft-float` 编译，并使用
[ieeelib](https://github.com/sysprog21/ieeelib) 作为 soft-fp 库。

## 获取可执行文件

手动获取预构建二进制：

```sh
make artifact
```

从源码重新构建二进制（需要 RISC-V 交叉编译器）：

```sh
make artifact ENABLE_PREBUILT=0 [CROSS_COMPILE=<COMPILER_PREFIX>]
```

编译器前缀取决于工具链，例如 `riscv-none-elf-`、`riscv32-unknown-elf-`。

`rv32emu-prebuilt` 中的预构建程序来自以下仓库或测试源：

| 名称 | 来源 |
| --- | --- |
| `coremark`、`stream`、`nbench` | [ansibench](https://github.com/sysprog21/ansibench) |
| `aes`、`dhrystone`、`miniz`、`norx`、`primes`、`qsort`、`sha512` | [rv8-bench](https://github.com/sysprog21/rv8-bench) |
| `captcha` | [tests/captcha.c](/tests/captcha.c) |
| `donut` | [tests/donut.c](/tests/donut.c) |
| `doom` | [sysprog21/doom_riscv](https://github.com/sysprog21/doom_riscv) |
| `fcalc` | [tests/fcalc.c](/tests/fcalc.c) |
| `hamilton` | [tests/hamilton.c](/tests/hamilton.c) |
| `jit` | [tests/jit.c](/tests/jit.c) |
| `lena` | [tests/lena.c](/tests/lena.c) |
| `line` | [tests/line.c](/tests/line.c) |
| `maj2random` | [tests/maj2random.c](/tests/maj2random.c) |
| `mandelbrot` | [tests/mandelbrot.c](/tests/mandelbrot.c) |
| `nqueens` | [tests/nqueens.c](/tests/nqueens.c) |
| `nyancat` | [tests/nyancat.c](/tests/nyancat.c) |
| `pi` | [tests/pi.c](/tests/pi.c) |
| `puzzle` | [tests/puzzle.c](/tests/puzzle.c) |
| `qrcode` | [tests/qrcode.c](/tests/qrcode.c) |
| `richards` | [tests/richards.c](/tests/richards.c) |
| `rvsim` | [tests/rvsim.c](/tests/rvsim.c) |
| `spirograph` | [tests/spirograph.c](/tests/spirograph.c) |
| `uaes` | [tests/uaes.c](/tests/uaes.c) |

用于浮点性能测试的 RISC-V 程序会使用 `-march=rv32imf`：

| 名称 | 来源 |
| --- | --- |
| `quake` | [sysprog21/quake-embedded](https://github.com/sysprog21/quake-embedded) |
| `scimark2` | [SciMark 2.0](https://math.nist.gov/scimark2) |

`build/` 目录下还保留了一些独立 RISC-V ELF，仅用于测试：

| 文件 | 来源 |
| --- | --- |
| `hello.elf` | [tests/asm-hello](/tests/asm-hello) |
| `cc.elf` | [tests/cc](/tests/cc) |
| `chacha20.elf` | [tests/chacha20](/tests/chacha20) |
| `ieee754.elf` | [tests/ieee754.c](/tests/ieee754.c)，需要 RV32F |
| `jit-bf.elf` | [ezaki-k/xkon_beta](https://github.com/ezaki-k/xkon_beta) |
| `readelf.elf` | [tests/readelf](/tests/readelf) |
| `smolnes.elf` | [tests/smolnes](/tests/smolnes.c)，需要 RV32M |

## 运行基准程序

```sh
make defconfig          # 或 make jit_defconfig 启用 JIT
make
build/rv32emu <benchmark>
```
