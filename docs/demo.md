# 演示程序

## Doom

**来源**：[doom_riscv](https://github.com/sysprog21/doom_riscv)

**命令**：`make doom`

[Doom](https://en.wikipedia.org/wiki/Doom_(franchise)) 是 id Software 在 1993
年推出的第一人称射击游戏，也是开源游戏移植和复古平台移植中常见的测试目标。
它适合验证 rv32emu 的整数执行、SDL 图形、输入事件和音频系统调用。

![Doom 游戏画面](https://imgur.com/bLc5LG8.gif)

### 主要按键

完整按键列表见游戏内 `"READ THIS!"` 菜单。

* 前进/后退：上方向键/下方向键
* 左移/右移：逗号键（`,`）/句号键（`.`）
* 左转/右转：左方向键/右方向键
* 射击：鼠标左键或 CTRL 键
* 奔跑：Shift 键
* `1`：拳头
* `3`：霰弹枪

### 音乐和音效

Doom 的音乐和音效均已支持。

## Quake

**来源**：[quake-embedded](https://github.com/sysprog21/quake-embedded/)

**命令**：`make quake`

[Quake](https://en.wikipedia.org/wiki/Quake_(series)) 是 id Software 在 1996 年
推出的第一人称射击游戏，使用完整 3D 引擎。该 demo 适合验证 RV32F 浮点、
图形帧刷新、输入事件和较重的解释/JIT 执行路径。

![Quake 游戏画面](https://imgur.com/gXKb7D0.gif)

### 默认按键

* 前进/后退：上方向键/下方向键
* 左移/右移：逗号键（`,`）/句号键（`.`）
* 左转/右转：左方向键/右方向键
* 上浮/下潜：D 键/C 键
* 射击：鼠标左键或 CTRL 键
* 切换武器：斜杠键（`/`）
* 奔跑：Shift 键

可以使用鼠标调整俯仰角和偏航角。

### 音乐和音效

Quake 当前支持音效；音乐暂不可用，因为原游戏音乐依赖 CD-ROM，提取出的 pak
文件不包含音乐或背景音乐资源。

### 限制

* 暂不支持鼠标滚轮输入
* 暂未实现 Quake 中的音乐相关函数
