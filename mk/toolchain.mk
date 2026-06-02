# 编译器和工具链探测，并与 Kconfig 集成
#
# 本文件探测编译器类型，并设置相应编译/链接参数。
# 通过 CONFIG_* 变量与 Kconfig 配置协同工作。

ifndef _MK_TOOLCHAIN_INCLUDED
_MK_TOOLCHAIN_INCLUDED := 1

# 交叉编译支持。
# 如果用户未提供 CROSS_COMPILE，下方会自动探测。
SYSROOT ?=

# 平台探测。

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

ifeq ($(UNAME_S),Darwin)
    PRINTF = printf
else
    PRINTF = env printf
endif

ifeq ($(UNAME_M),x86_64)
    HOST_PLATFORM := x86
else ifeq ($(UNAME_M),aarch64)
    HOST_PLATFORM := aarch64
else ifeq ($(UNAME_M),arm64)
    HOST_PLATFORM := arm64
else
    HOST_PLATFORM := unknown
endif

# 编译器探测。

# WebAssembly 构建：当 Kconfig 启用 BUILD_WASM 时强制使用 emcc。
ifeq ($(CONFIG_BUILD_WASM),y)
    CC     := emcc
endif

# 默认工具。
CC      ?= cc
AR      ?= ar
RANLIB  ?= ranlib
STRIP   ?= strip

# 应用交叉编译前缀。
ifneq ($(CROSS_COMPILE),)
    ifeq ($(origin CC),default)
        CC      := $(CROSS_COMPILE)$(CC)
        AR      := $(CROSS_COMPILE)$(AR)
        RANLIB  := $(CROSS_COMPILE)$(RANLIB)
        STRIP   := $(CROSS_COMPILE)$(STRIP)
    endif
endif

# 构建期工具使用的宿主工具链。
HOSTCC  ?= cc
HOSTAR  ?= ar

# 从版本字符串探测编译器类型，并缓存输出以避免重复 shell 调用。
CC_VERSION_OUTPUT := $(shell $(CC) --version 2>&1)

CC_IS_EMCC  := $(if $(findstring emcc,$(CC_VERSION_OUTPUT)),1,)
CC_IS_CLANG := $(if $(and $(findstring clang,$(CC_VERSION_OUTPUT)),$(if $(CC_IS_EMCC),,1)),1,)
CC_IS_GCC   := $(if $(and $(findstring Free Software Foundation,$(CC_VERSION_OUTPUT)),$(if $(CC_IS_EMCC)$(CC_IS_CLANG),,1)),1,)

# 校验是否为受支持编译器。
ifeq ("$(CC_IS_CLANG)$(CC_IS_GCC)$(CC_IS_EMCC)", "")
$(error 不支持当前编译器。仅支持 GCC、Clang 和 Emscripten。)
endif

# 一致性检查：CONFIG_BUILD_WASM 已启用但 CC 不是 emcc 时给出警告。
ifeq ($(CONFIG_BUILD_WASM),y)
    ifneq ($(CC_IS_EMCC),1)
        $(warning [配置不一致] CONFIG_BUILD_WASM 已启用，但 CC 不是 Emscripten。)
        $(warning 当前编译器：$(CC)。请运行 'make defconfig' 重置，或检查 CC 覆盖设置。)
    endif
endif

# Emscripten 版本探测，复用已缓存的 CC_VERSION_OUTPUT。
ifeq ("$(CC_IS_EMCC)", "1")
    EMCC_VERSION := $(word 10,$(CC_VERSION_OUTPUT))
    EMCC_MAJOR := $(word 1,$(subst ., ,$(EMCC_VERSION)))
    EMCC_MINOR := $(word 2,$(subst ., ,$(EMCC_VERSION)))
    EMCC_PATCH := $(word 3,$(subst ., ,$(EMCC_VERSION)))

    # Emscripten 构建使用配套工具。
    AR     := emar
    RANLIB := emranlib
    STRIP  := emstrip
endif

# RISC-V 交叉编译器探测。

TOOLCHAIN_LIST := riscv-none-elf- \
                  riscv32-unknown-elf- \
                  riscv64-unknown-elf- \
                  riscv-none-embed-

define check-cross-tools
$(shell which $(1)gcc >/dev/null 2>&1 && \
        which $(1)cpp >/dev/null 2>&1 && \
        echo | $(1)cpp -dM - 2>/dev/null | grep __riscv >/dev/null && \
        echo "$(1) ")
endef

# 用户未提供 CROSS_COMPILE 时自动探测 RISC-V 工具链。
ifeq ($(CROSS_COMPILE),)
    CROSS_COMPILE := $(word 1,$(foreach prefix,$(TOOLCHAIN_LIST),$(call check-cross-tools,$(prefix))))
endif
export CROSS_COMPILE

# CET 保护相关编译参数，复用平台探测得到的 UNAME_M。

CFLAGS_NO_CET :=
ifeq ($(UNAME_M),$(filter $(UNAME_M),i386 x86_64))
    # JIT 需要关闭 Intel Control-flow Enforcement Technology。
    CFLAGS_NO_CET := -fcf-protection=none
endif

# macOS 链接器兼容处理。

ifeq ($(UNAME_S),Darwin)
    ifneq ("$(CC_IS_CLANG)$(CC_IS_GCC)", "")
        # Xcode 15+ 会对重复 -l 选项给出警告。
        LD_VERSION := $(shell ld -version_details 2>/dev/null | head -n 1)
        ifneq ($(shell echo "$(LD_VERSION)" | grep -E "(15|16|17|18|19|[2-9][0-9])\.[0-9]"),)
            LDFLAGS += -Wl,-no_warn_duplicate_libraries
        endif
    endif
endif

# 从 Kconfig 推导编译/链接参数。

KCONFIG_CFLAGS :=
KCONFIG_LDFLAGS :=

# 优化级别。
ifeq ($(CONFIG_OPTIMIZE_SIZE),y)
    KCONFIG_CFLAGS += -Os
else
    OPT_LEVEL := $(if $(CONFIG_OPTIMIZE_LEVEL),-O$(CONFIG_OPTIMIZE_LEVEL),-O2)
    KCONFIG_CFLAGS += $(OPT_LEVEL)
endif

# 调试符号。
ifeq ($(CONFIG_DEBUG_SYMBOLS),y)
    KCONFIG_CFLAGS += -g
endif

# 链接时优化。
ifeq ($(CONFIG_LTO),y)
    ifeq ("$(CC_IS_EMCC)", "1")
        ifeq ($(CONFIG_SDL),y)
            $(warning Emscripten 的 SDL 构建不支持 LTO。)
        else
            KCONFIG_CFLAGS += -flto
            KCONFIG_LDFLAGS += -flto
        endif
    else ifeq ("$(CC_IS_GCC)", "1")
        KCONFIG_CFLAGS += -flto=auto
        KCONFIG_LDFLAGS += -flto=auto
    else ifeq ("$(CC_IS_CLANG)", "1")
        KCONFIG_CFLAGS += -flto=thin -fsplit-lto-unit
        KCONFIG_LDFLAGS += -flto=thin
    endif
endif

# 未定义行为检测器。
ifeq ($(CONFIG_UBSAN),y)
    KCONFIG_CFLAGS += -fsanitize=undefined -fno-sanitize=alignment -fno-sanitize-recover=all
    KCONFIG_LDFLAGS += -fsanitize=undefined -fno-sanitize=alignment -fno-sanitize-recover=all
endif

# sysroot 支持。
ifneq ($(SYSROOT),)
    KCONFIG_CFLAGS += --sysroot=$(SYSROOT)
    KCONFIG_LDFLAGS += --sysroot=$(SYSROOT)
endif

# T2C（二级编译器）的 LLVM 探测。
#
# 支持 LLVM 18 到 21。探测会延后到 CONFIG_T2C 启用后，
# 避免普通构建中执行昂贵的 shell 调用。
#
# 用户可通过 `make LLVM_CONFIG=/path/to/llvm-config` 覆盖。

# 支持的 LLVM 版本范围。
LLVM_MIN_VERSION := 18
LLVM_MAX_VERSION := 21

# 检查 PATH 中是否存在带版本号的 llvm-config。
# 用法：$(call llvm-config-path,VERSION)
define llvm-config-path
$(shell which llvm-config-$(1) 2>/dev/null)
endef

# 检查 Homebrew 是否安装了指定版本 LLVM。
# 用法：$(call llvm-homebrew-config,VERSION)
define llvm-homebrew-config
$(shell brew --prefix llvm@$(1) 2>/dev/null | xargs -I{} sh -c 'test -x {}/bin/llvm-config && echo {}/bin/llvm-config' 2>/dev/null)
endef

# 检查通用 llvm-config，并校验版本范围。
# 用法：$(call llvm-config-generic,MIN,MAX)
define llvm-config-generic
$(shell which llvm-config 2>/dev/null | xargs -I{} sh -c 'ver=$$({} --version 2>/dev/null | cut -d. -f1); [ "$$ver" -ge $(1) ] && [ "$$ver" -le $(2) ] && echo {}' 2>/dev/null)
endef

# 自动探测 LLVM 配置。
# 优先级：带版本号二进制（18-21）、Homebrew 版本化安装、
# Homebrew 通用安装、最后是带版本校验的通用 llvm-config。
# 为稳定性优先选择最老的受支持版本 18；需要更新版本时可设置 LLVM_CONFIG。
define detect-llvm-config
$(strip $(or \
    $(call llvm-config-path,18),\
    $(call llvm-config-path,19),\
    $(call llvm-config-path,20),\
    $(call llvm-config-path,21),\
    $(call llvm-homebrew-config,18),\
    $(call llvm-homebrew-config,19),\
    $(call llvm-homebrew-config,20),\
    $(call llvm-homebrew-config,21),\
    $(shell brew --prefix llvm 2>/dev/null | xargs -I{} sh -c 'test -x {}/bin/llvm-config && echo {}/bin/llvm-config' 2>/dev/null),\
    $(call llvm-config-generic,$(LLVM_MIN_VERSION),$(LLVM_MAX_VERSION))))
endef

# 探测 Homebrew LLVM 前缀，用于补充库搜索路径。
define detect-homebrew-llvm-prefix
$(strip $(or \
    $(shell which brew >/dev/null 2>&1 && brew --prefix llvm@18 2>/dev/null),\
    $(shell which brew >/dev/null 2>&1 && brew --prefix llvm@19 2>/dev/null),\
    $(shell which brew >/dev/null 2>&1 && brew --prefix llvm@20 2>/dev/null),\
    $(shell which brew >/dev/null 2>&1 && brew --prefix llvm@21 2>/dev/null),\
    $(shell which brew >/dev/null 2>&1 && brew --prefix llvm 2>/dev/null)))
endef

# 获取 LLVM 主版本号。
# 用法：$(call llvm-version,LLVM_CONFIG_PATH)
define llvm-version
$(shell $(1) --version 2>/dev/null | cut -d. -f1)
endef

# 检查 LLVM 库是否可用。
# 用法：$(call llvm-check-libs,LLVM_CONFIG_PATH)
# 成功返回字符串 "0"，失败返回非零字符串。
# 示例：ifeq ($(call llvm-check-libs,$(LLVM_CONFIG)),0)
define llvm-check-libs
$(shell $(1) --libs 2>/dev/null 1>&2; echo $$?)
endef

# 获取 LLVM 编译参数。
# 用法：$(call llvm-cflags,LLVM_CONFIG_PATH)
define llvm-cflags
$(shell $(1) --cflags 2>/dev/null)
endef

# 获取 LLVM 链接库文件列表。
# 用法：$(call llvm-libfiles,LLVM_CONFIG_PATH)
define llvm-libfiles
$(shell $(1) --libfiles 2>/dev/null)
endef

# 版本比较工具。

version_num = $(shell printf "%d%03d%03d" $(1) $(2) $(3) 2>/dev/null || echo 0)
version_eq = $(shell echo "$$(($(call version_num,$(1),$(2),$(3)) == $(call version_num,$(4),$(5),$(6))))")
version_lt = $(shell echo "$$(($(call version_num,$(1),$(2),$(3)) < $(call version_num,$(4),$(5),$(6))))")
version_lte = $(shell echo "$$(($(call version_num,$(1),$(2),$(3)) <= $(call version_num,$(4),$(5),$(6))))")
version_gt = $(shell echo "$$(($(call version_num,$(1),$(2),$(3)) > $(call version_num,$(4),$(5),$(6))))")
version_gte = $(shell echo "$$(($(call version_num,$(1),$(2),$(3)) >= $(call version_num,$(4),$(5),$(6))))")

endif # _MK_TOOLCHAIN_INCLUDED
