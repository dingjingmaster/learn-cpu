# 系统调用

## 背景

系统调用是应用程序与操作系统内核交互的主要方式。程序通过系统调用请求文件 I/O、
时间、内存管理、进程退出等服务；这些服务通常需要特权级能力，不能由普通用户态
程序直接执行。

在 RISC-V 程序中，系统调用参数按调用约定放入寄存器，随后执行 `ecall`。在
rv32emu 的用户态模拟模式下，`ecall` 会进入模拟器的 `syscall_handler`，再由
模拟器把请求转发到宿主系统或内部运行时。

## C 语言中的系统调用

以 `write` 为例，C 函数原型如下：

```c
ssize_t write(int fd, const void *buf, size_t count);
```

三个参数分别是文件描述符、缓冲区地址和写入字节数。标准输出通常使用文件描述符
`1`。注意这里的缓冲区不是以 `\0` 结尾的字符串，写入长度完全由 `count` 决定。

## RISC-V 调用约定

RISC-V 调用约定规定，普通参数从 `a0` 开始依次放入寄存器，系统调用号放入 `a7`，
然后执行 `ecall`。

以 `write` 为例：

* `a0`：文件描述符
* `a1`：字符串/缓冲区地址
* `a2`：写入字节数
* `a7`：系统调用号，`write` 为 `64`

成功后返回值在 `a0` 中；出错时通常返回 `-1` 或负 errno。

下面的汇编程序打印 `RISC-V` 并退出：

```assembly
    .equ STDOUT, 1
    .equ WRITE, 64
    .equ EXIT, 93

    .section .rodata
    .align 2
msg:
    .ascii "RISC-V\n"

    .section .text
    .align 2

    .globl  _start
_start:
    li a0, STDOUT  # 文件描述符
    la a1, msg     # 字符串地址
    li a2, 7       # 字符串长度
    li a7, WRITE   # write 系统调用号
    ecall

    # TODO：这里应检查 write 是否出错。
    li a0, 0       # 0 表示成功退出
    li a7, EXIT
    ecall
```

构建和运行：

```sh
riscv-none-elf-gcc -march=rv32i -mabi=ilp32 -nostartfiles -nostdlib -o hello hello.S
build/rv32emu hello
```

RISC-V 有 User、Supervisor、Machine 三种常见特权模式。`ecall` 会触发 trap，
随后由对应特权级的 trap handler 处理。返回指令包括 `uret`、`sret`、`mret`；
其中 `mret` 只能从 M-mode 返回。

RV32/RV64 ABI 要求栈指针保持 16 字节对齐。

## newlib 集成

[newlib](https://sourceware.org/newlib/) 提供大部分 C 标准库功能，例如 `malloc`、
`printf`、`memcpy` 等。为了让交叉编译出的程序在模拟器中运行，rv32emu 实现了
newlib 常用的一小组系统调用。

当前用户态 semihosting 支持：

| 编号 | 系统调用 | 当前支持 |
| --- | --- | --- |
| 57 | `close` | 从进程对象引用表中删除描述符 |
| 62 | `lseek` | 按 `whence` 调整文件偏移 |
| 63 | `read` | 从描述符读取数据到客体缓冲区 |
| 64 | `write` | 把缓冲区写到指定文件描述符 |
| 80 | `fstat` | 当前无实际效果 |
| 93 | `exit` | 以状态码终止客体程序 |
| 169 | `gettimeofday` | 获取当前日期和时间，不获取当前时区 |
| 214 | `brk` | 更新或查询 program break |
| 403 | `clock_gettime` | 获取指定 clock-id 的时间 |
| 1024 | `open` | 打开或创建文件 |

其他未知系统调用会以“未知系统调用”错误失败。

## SDL 图形、事件和声音系统调用

下面这些调用不是 POSIX 或 Linux ABI 的一部分，只是 rv32emu 为图形 demo 和游戏
提供的 SDL 扩展接口。相关客体侧辅助头文件见 [fenster.h](../tests/fenster.h)。

### `draw_frame`

**系统调用号**：`0xBEEF`

**原型**：`void draw_frame(void *base, int width, int height)`

如果窗口尚未创建，则按 `width` 和 `height` 创建 SDL 窗口。`base` 指向的缓冲区
会替换帧缓冲内容。该调用还会轮询 SDL 事件，并把事件写入内部输入队列。

### `setup_queue`

**系统调用号**：`0xC0DE`

**原型**：`void setup_queue(void *base, size_t capacity, size_t *event_count)`

客体需要提供一块连续内存，里面紧密放置事件队列和提交队列。提交队列紧跟在事件
队列之后。如果 `capacity` 不是 2 的幂，模拟器会向上取整。`event_count` 用作
事件通知变量，传入前必须初始化。

事件类型：

* `KEY_EVENT`：按键按下或释放。
* `MOUSE_MOTION_EVENT`：鼠标从上一帧到当前帧的移动。
* `MOUSE_BUTTON_EVENT`：鼠标按键状态变化。
* `QUIT_EVENT`：请求客体程序退出。

### `submit_queue`

**系统调用号**：`0xFEED`

**原型**：`void submit_queue(size_t count)`

客体先向提交队列写入一批请求，再通过该调用通知模拟器处理。模拟器会按顺序立即
执行这些提交。

提交类型：

* `RELATIVE_MODE_SUBMISSION`：启用或关闭鼠标相对模式。
* `WINDOW_TITLE_SUBMISSION`：修改 SDL 窗口标题，未指定时默认为 `rv32emu`。

### `control_audio`

**系统调用号**：`0xD00D`

**原型**：`void control_audio(int request)`

用于播放、停止或调节音乐/音效。客体把声音数据地址放入 `a1`，音量放入 `a2`，
循环标志放入 `a3`。

支持请求：

* `PLAY_MUSIC`：播放音乐，已有音乐会被替换。
* `STOP_MUSIC`：停止音乐。
* `SET_MUSIC_VOLUME`：调整音乐音量。
* `PLAY_SFX`：播放音效，不支持循环。

音乐数据使用 `musicinfo_t`，音效数据使用 `sfxinfo_t`。当前支持 Doom WAV 格式和
普通 WAV（含 RIFF header）格式。

### `setup_audio`

**系统调用号**：`0xBABE`

**原型**：`void setup_audio(int request)`

用于初始化或关闭音频系统。

* `INIT_AUDIO`：初始化音频设备和音效缓冲区。
* `SHUTDOWN_AUDIO`：释放由初始化请求创建的资源。
