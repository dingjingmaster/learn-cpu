#!/usr/bin/env python3
"""
生成 Web 前端使用的 ELF 文件列表。

WASM demo 页面需要知道 `build/` 和 `build/riscv32/` 下有哪些可运行文件。本脚本
扫描这些目录，过滤掉不适合直接出现在选择列表中的测试资产，然后输出一段 JavaScript
常量定义：

    const elfFiles = [...];

该输出会被构建规则重定向到 `build/elf_list.js`。
"""

import os


def list_files(d, ignore_list=None):
    """列出目录中可展示的文件，并按调用方传入的忽略规则过滤。"""
    if ignore_list is None:
        ignore_list = []
    try:
        if d == "build":
            # build/ 根目录主要放置带 .elf 后缀的示例程序，因此只收集 .elf 文件。
            files = [
                f
                for f in os.listdir(d)
                if os.path.isfile(os.path.join(d, f))
                and f.endswith(".elf")
                and not any(
                    f.endswith(ign) or f.startswith(ign) for ign in ignore_list
                )
            ]
        else:
            # 子目录中的文件需要保留相对父目录的路径，例如 riscv32/foo。
            # 这样 Web 前端可以直接用同一个字符串定位嵌入资源。
            parent_dir = os.path.dirname(d)
            files = [
                os.path.relpath(os.path.join(d, f), start=parent_dir)
                for f in os.listdir(d)
                if os.path.isfile(os.path.join(d, f))
                and not any(
                    f.endswith(ign) or os.path.join(d, f).endswith(ign)
                    for ign in ignore_list
                )
            ]
        return files
    except FileNotFoundError:
        print(f"目录 {d} 不存在。")
        return []


# Web 演示默认展示 build/ 根目录和 build/riscv32/ 中的可执行样例。
elf_exec_dirs = ["build", "build/riscv32"]

# 这些文件通常没有适合浏览器列表展示的交互输出，或属于内部测试资产。
msg_less_ignore_files = [
    "cc.elf",
    "chacha20.elf",
    "riscv32/lena",
    "riscv32/puzzle",
    "riscv32/line",
    "riscv32/captcha",
]  # 需要从前端选择列表中过滤掉的文件。
elf_exec_list = []

for d in elf_exec_dirs:
    files = list_files(d, ignore_list=msg_less_ignore_files)
    elf_exec_list.extend(files)


def gen_elf_list_js():
    """把收集到的 ELF 文件列表输出为 JavaScript 常量定义。"""
    js_code = f"const elfFiles = {elf_exec_list};\n"
    print(js_code)


gen_elf_list_js()
