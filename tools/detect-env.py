#!/usr/bin/env python3
"""
rv32emu/learn-cpu 构建环境探测脚本。

Kconfig 会通过 `$(shell,...)` 调用本脚本，把宿主编译器、Emscripten、SDL2、
LLVM 和 RISC-V 交叉工具链的探测结果写入配置选项。为了便于 Kconfig 解析，布尔
查询仍固定输出 `y` 或 `n`，编译器类型仍固定输出 `GCC`、`Clang`、
`Emscripten` 或 `Unknown`。

用法：
    python3 detect-env.py [选项]

常用选项：
    --compiler              输出检测到的编译器类型。
    --is-emcc               当前编译器是否为 Emscripten。
    --is-clang              当前编译器是否为 Clang。
    --is-gcc                当前编译器是否为 GCC。
    --have-emcc             系统是否可用 emcc。
    --have-sdl2             系统是否可用 SDL2。
    --have-sdl2-mixer       系统是否可用 SDL2_mixer。
    --have-llvm18           系统是否可用 LLVM 18。
    --have-riscv-toolchain  系统是否可用 RISC-V 交叉工具链。
    --summary               输出完整环境摘要，供人工排查使用。
"""

import os
import shlex
import shutil
import subprocess
import sys


def run_cmd(cmd, timeout=5):
    """执行外部命令，并返回 `(退出码, 标准输出, 标准错误)`。"""
    try:
        if isinstance(cmd, str):
            # Kconfig/Make 可能传入带空格的命令字符串；统一拆成参数列表，避免
            # `shell=True` 带来的引用和注入问题。
            cmd = shlex.split(cmd)
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout
        )
        return result.returncode, result.stdout, result.stderr
    except (FileNotFoundError, subprocess.TimeoutExpired, ValueError, OSError):
        return 1, "", ""


def get_compiler_path():
    """根据环境变量推导当前构建使用的 C 编译器。"""
    cross_compile = os.environ.get("CROSS_COMPILE", "")
    cc = os.environ.get("CC", "")

    if cc:
        # 显式 CC 覆盖优先级最高，支持用户指定 clang、gcc 或 emcc。
        return cc
    elif cross_compile:
        # 交叉编译时 CROSS_COMPILE 通常是前缀，例如 riscv32-unknown-elf-。
        return f"{cross_compile}gcc"
    else:
        # 未指定时遵循 Makefile 默认行为，使用系统 cc。
        return "cc"


def get_compiler_version(compiler):
    """获取编译器版本输出，供后续判断编译器类型。"""
    ret, stdout, _ = run_cmd(
        [compiler, "--version"]
        if not " " in compiler
        else shlex.split(compiler) + ["--version"]
    )
    return stdout if ret == 0 else ""


def detect_compiler_type(version_output):
    """从版本字符串中判断编译器类型。"""
    lower = version_output.lower()

    # emcc 的版本输出通常也包含 clang，因此必须先判断 Emscripten。
    if "emcc" in lower:
        return "Emscripten"
    if "clang" in lower:
        return "Clang"
    if "gcc" in lower or "free software foundation" in lower:
        return "GCC"
    return "Unknown"


def check_pkg_config(package):
    """通过 pkg-config 检查系统库是否存在。"""
    ret, _, _ = run_cmd(["pkg-config", "--exists", package])
    return ret == 0


def check_sdl2_config():
    """检查传统的 sdl2-config 工具是否存在。"""
    return shutil.which("sdl2-config") is not None


def have_sdl2():
    """检查 SDL2 开发环境是否可用。"""
    return check_sdl2_config() or check_pkg_config("sdl2")


def have_sdl2_mixer():
    """检查 SDL2_mixer 开发环境是否可用。"""
    return check_pkg_config("SDL2_mixer")


def have_emcc():
    """检查 Emscripten 编译器 `emcc` 是否可用。"""
    emcc = shutil.which("emcc")
    if emcc:
        # 只找到可执行文件还不够，需要确认它能正常运行。
        # 部分 Emscripten 构建会把版本信息输出到 stderr，因此同时检查 stdout/stderr。
        ret, stdout, stderr = run_cmd([emcc, "--version"])
        combined = (stdout + stderr).lower()
        if ret == 0 and "emcc" in combined:
            return True
    return False


def have_llvm18():
    """检查 LLVM 18 开发环境是否可用。"""
    # Linux 发行版常见命名：llvm-config-18。
    if shutil.which("llvm-config-18"):
        return True

    # macOS/Homebrew 常见安装路径：brew --prefix llvm@18。
    if shutil.which("brew"):
        ret, stdout, _ = run_cmd(["brew", "--prefix", "llvm@18"])
        if ret == 0:
            homebrew_path = os.path.join(stdout.strip(), "bin", "llvm-config")
            if os.access(homebrew_path, os.X_OK):
                return True

    # 最后检查通用 llvm-config，并确认主版本号确实为 18。
    llvm_config = shutil.which("llvm-config")
    if llvm_config:
        ret, stdout, _ = run_cmd([llvm_config, "--version"])
        if ret == 0 and stdout.strip().startswith("18."):
            return True

    return False


def have_riscv_toolchain():
    """检查是否存在可用的 RISC-V 交叉编译工具链。"""
    toolchain_prefixes = [
        "riscv-none-elf-",
        "riscv32-unknown-elf-",
        "riscv64-unknown-elf-",
        "riscv-none-embed-",
    ]

    for prefix in toolchain_prefixes:
        gcc = shutil.which(f"{prefix}gcc")
        if gcc:
            # 优先通过预定义宏确认这是 RISC-V 编译器，避免只靠文件名误判。
            try:
                result = subprocess.run(
                    [gcc, "-dM", "-E", "-x", "c", "-"],
                    input="",
                    capture_output=True,
                    text=True,
                    timeout=5,
                )
                if result.returncode == 0 and "__riscv" in result.stdout:
                    return True
            except (subprocess.TimeoutExpired, OSError):
                pass

            # 回退策略：检查 --version 输出中是否出现 RISC-V 字样。
            ret, stdout, _ = run_cmd([gcc, "--version"])
            if ret == 0 and (
                "riscv" in stdout.lower() or "risc-v" in stdout.lower()
            ):
                return True

    return False


def print_summary():
    """输出完整环境摘要，主要用于人工调试构建配置。"""
    compiler = get_compiler_path()
    version = get_compiler_version(compiler)
    comp_type = detect_compiler_type(version)

    print(f"Compiler: {compiler}")
    print(f"Type: {comp_type}")
    print(f"Emscripten: {'yes' if have_emcc() else 'no'}")
    print(f"SDL2: {'yes' if have_sdl2() else 'no'}")
    print(f"SDL2_mixer: {'yes' if have_sdl2_mixer() else 'no'}")
    print(f"LLVM 18: {'yes' if have_llvm18() else 'no'}")
    print(f"RISC-V Toolchain: {'yes' if have_riscv_toolchain() else 'no'}")


def main():
    """解析命令行选项，并按 Kconfig 期望格式输出探测结果。"""
    if len(sys.argv) < 2:
        print_summary()
        return

    compiler = get_compiler_path()
    version = get_compiler_version(compiler)
    comp_type = detect_compiler_type(version)

    arg = sys.argv[1]

    if arg == "--compiler":
        print(comp_type)
    elif arg == "--is-emcc":
        print("y" if comp_type == "Emscripten" else "n")
    elif arg == "--is-clang":
        print("y" if comp_type == "Clang" else "n")
    elif arg == "--is-gcc":
        print("y" if comp_type == "GCC" else "n")
    elif arg == "--have-emcc":
        print("y" if have_emcc() else "n")
    elif arg == "--have-sdl2":
        print("y" if have_sdl2() else "n")
    elif arg == "--have-sdl2-mixer":
        print("y" if have_sdl2_mixer() else "n")
    elif arg == "--have-llvm18":
        print("y" if have_llvm18() else "n")
    elif arg == "--have-riscv-toolchain":
        print("y" if have_riscv_toolchain() else "n")
    elif arg == "--summary":
        print_summary()
    else:
        print(f"未知选项：{arg}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
