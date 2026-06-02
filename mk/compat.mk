# ENABLE_* 旧参数兼容层。
# 将旧式 ENABLE_* 变量转换为 Kconfig 使用的 CONFIG_*。
#
# 这样已有 CI/CD 脚本和构建命令可以继续配合新的 Kconfig 构建系统工作。
#
# 用法：ENABLE_JIT=1 make  ->  自动设置 CONFIG_JIT=y
#
# 冲突处理：
#   同一标志出现多次时，例如 `make ENABLE_JIT=0 ENABLE_JIT=1`，
#   GNU Make 使用最后一个值；本兼容层遵循该行为。
#
# 可接受取值（会自动去除首尾空白）：
#   启用：  1, y, yes, true, Y, YES, TRUE, Yes, True
#   关闭：  0, n, no, false, N, NO, FALSE, No, False
#
# 弃用提示：该兼容层可能在未来版本移除。
# 请逐步迁移到 `make defconfig` 或 `make <name>_defconfig`。

# 将输入归一化为 `y`、`n` 或空值（未知）。
# 用法：$(call normalize-bool,VALUE)
# 返回：y（启用）、n（关闭）或空值（非法/未知）
# 说明：会去除空白，并处理常见大小写变体。
normalize-bool = $(strip \
    $(if $(filter 1 y yes true Y YES TRUE Yes True,$(strip $(1))),y,\
    $(if $(filter 0 n no false N NO FALSE No False,$(strip $(1))),n,)))

# 把 ENABLE_* 转换为 CONFIG_* 的辅助宏，并执行取值归一化。
# 用法：$(call enable-to-config,FEATURE)
# 说明：使用 override 保证命令行 ENABLE_* 优先于 .config。
define enable-to-config
ifdef ENABLE_$(1)
    ENABLE_$(1)_NORMALIZED := $$(call normalize-bool,$$(ENABLE_$(1)))
    ifeq ($$(ENABLE_$(1)_NORMALIZED),y)
        override CONFIG_$(1) := y
    else ifeq ($$(ENABLE_$(1)_NORMALIZED),n)
        override CONFIG_$(1) := n
    else
        $$(warning ENABLE_$(1)='$$(ENABLE_$(1))' 无效。请使用 0/1、y/n、yes/no 或 true/false。)
    endif
endif
endef

# 核心扩展。
$(eval $(call enable-to-config,EXT_M))
$(eval $(call enable-to-config,EXT_A))
$(eval $(call enable-to-config,EXT_F))
$(eval $(call enable-to-config,EXT_C))

# RV32E 模式（嵌入式，16 个寄存器）。
$(eval $(call enable-to-config,RV32E))

# 位操作扩展。
$(eval $(call enable-to-config,Zba))
$(eval $(call enable-to-config,Zbb))
$(eval $(call enable-to-config,Zbc))
$(eval $(call enable-to-config,Zbs))

# CSR 和 fence 扩展。
$(eval $(call enable-to-config,Zicsr))
$(eval $(call enable-to-config,Zifencei))

# 执行模式。
# JIT 需要协调 CONFIG_INTERPRETER_ONLY，因此在下方单独处理。
$(eval $(call enable-to-config,SYSTEM))
$(eval $(call enable-to-config,GOLDFISH_RTC))
$(eval $(call enable-to-config,ELF_LOADER))
$(eval $(call enable-to-config,T2C))

# 性能选项。
$(eval $(call enable-to-config,MOP_FUSION))
$(eval $(call enable-to-config,BLOCK_CHAINING))
$(eval $(call enable-to-config,LTO))

# 调试。
$(eval $(call enable-to-config,GDBSTUB))
$(eval $(call enable-to-config,UBSAN))

# 图形和音频。
$(eval $(call enable-to-config,SDL))
$(eval $(call enable-to-config,SDL_MIXER))

# 测试。
$(eval $(call enable-to-config,ARCH_TEST))

# 构建选项。
$(eval $(call enable-to-config,PREBUILT))

# JIT 模式特殊处理：根据 JIT 状态设置 INTERPRETER_ONLY。
# 由于 JIT 与 CONFIG_INTERPRETER_ONLY 是互斥选择，需要单独协调。
ifdef ENABLE_JIT
    ENABLE_JIT_NORMALIZED := $(call normalize-bool,$(ENABLE_JIT))
    ifeq ($(ENABLE_JIT_NORMALIZED),y)
        override CONFIG_INTERPRETER_ONLY := n
        override CONFIG_JIT := y
    else ifeq ($(ENABLE_JIT_NORMALIZED),n)
        override CONFIG_INTERPRETER_ONLY := y
        override CONFIG_JIT := n
    else
        $(warning ENABLE_JIT='$(ENABLE_JIT)' 无效。请使用 0/1、y/n、yes/no 或 true/false。)
    endif
endif

# 处理优化级别。
ifdef OPT_LEVEL
    # 将 -O0、-O2、-Ofast 转换成数字级别。
    ifeq ($(OPT_LEVEL),-O0)
        override CONFIG_OPTIMIZE_LEVEL := 0
    else ifeq ($(OPT_LEVEL),-O1)
        override CONFIG_OPTIMIZE_LEVEL := 1
    else ifeq ($(OPT_LEVEL),-O2)
        override CONFIG_OPTIMIZE_LEVEL := 2
    else ifeq ($(OPT_LEVEL),-O3)
        override CONFIG_OPTIMIZE_LEVEL := 3
    else ifeq ($(OPT_LEVEL),-Ofast)
        override CONFIG_OPTIMIZE_LEVEL := 3
    else ifeq ($(OPT_LEVEL),-Os)
        override CONFIG_OPTIMIZE_SIZE := y
    else
        $(warning OPT_LEVEL='$(OPT_LEVEL)' 无效。请使用 -O0、-O1、-O2、-O3、-Ofast 或 -Os。)
    endif
endif

# 处理系统模拟使用的 INITRD_SIZE。
ifdef INITRD_SIZE
    override CONFIG_INITRD_SIZE := $(INITRD_SIZE)
endif
