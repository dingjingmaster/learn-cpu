# 基础镜像

本目录相关 Docker 镜像用于开发和测试，目标是同时支持 `x86-64` 和 `aarch64`
平台。

基础镜像包含 `gcc` 和 `sail` 工具链。项目选择从源码构建这些工具链，并固定到
已验证版本，避免发行版包版本差异影响 RISC-V 架构测试和交叉编译。

当工具链需要升级时，运行 `build.sh` 重新构建镜像；脚本会自动完成构建并推送到
Docker Hub。

## 版本细节

`gcc` 使用 `tags/2023.10.06`。在 M1 等环境中，如果直接使用
`apt install gcc-riscv64-unknown-elf` 提供的二进制，编译模拟器时可能出现
`"unsupported ISA subset 'z'"` 错误。

`sail` 使用 `sail-0.16`。较新的 `0.17.x` 系列在当前构建链路中会触发编译错误。

参考模拟器使用 `sail-riscv` 的提交
`9547a30bf84572c458476591b569a95f5232c1c7`。该提交与
[x86-64 reference sail emulator](https://github.com/sysprog21/rv32emu/commit/01b00b6f175f57ef39ffd1f4fa6a611891e36df3#diff-3b436c5e32c40ecca4095bdacc1fb69c0759096f86e029238ce34bbe73c6e68f)
加入 `rv32emu` 仓库的时间最接近。
