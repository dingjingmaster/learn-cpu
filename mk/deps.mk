# 统一依赖探测
#
# 提供库和包探测的辅助函数。
# 对 clean、help 等非构建目标会跳过耗时检查。

ifndef _MK_DEPS_INCLUDED
_MK_DEPS_INCLUDED := 1

# 输出详细程度控制；与 mk/common.mk 保持一致。
# `make V=1` 等价于 `make VERBOSE=1`。
ifeq ("$(origin V)","command line")
    VERBOSE = $(V)
endif
ifeq ("$(VERBOSE)","1")
    DEVNULL :=
else
    DEVNULL := 2>/dev/null
endif

# 交叉编译时可通过该变量指定 pkg-config。
PKG_CONFIG ?= pkg-config

# 依赖探测函数（始终可用）。

# dep(type, packages)
# type：cflags 或 libs
# packages：以空格分隔的包名
#
# 用法：
#   CFLAGS += $(call dep,cflags,sdl2)
#   LDFLAGS += $(call dep,libs,sdl2)
#
define dep
$(shell \
    for pkg in $(2); do \
        if command -v $${pkg}-config >/dev/null 2>&1; then \
            $${pkg}-config --$(1) $(DEVNULL); \
        elif command -v $(PKG_CONFIG) >/dev/null 2>&1; then \
            $(PKG_CONFIG) --$(1) $$pkg $(DEVNULL); \
        fi; \
    done \
)
endef

# pkg-exists(package)
# 找到包时返回 "y"，否则返回空。
#
define pkg-exists
$(shell \
    if command -v $(1)-config >/dev/null 2>&1; then \
        echo y; \
    elif $(PKG_CONFIG) --exists $(1) $(DEVNULL); then \
        echo y; \
    fi \
)
endef

# clean/help/distclean 等目标跳过耗时依赖探测。
# SKIP_DEPS_CHECK 由 mk/common.mk 设置。
ifeq ($(SKIP_DEPS_CHECK),)

# SDL2 探测。
# HAVE_SDL2 会导出给 Kconfig 环境探测工具 tools/detect-env.py。
# SDL2_CFLAGS/LIBS 只在 CONFIG_SDL=y 且 .config 已加载后计算。
HAVE_SDL2 := $(call pkg-exists,sdl2)

# SDL2_mixer 探测。
HAVE_SDL2_MIXER := $(shell $(PKG_CONFIG) --exists SDL2_mixer $(DEVNULL) && echo y)

# 导出给 Kconfig。
export HAVE_SDL2
export HAVE_SDL2_MIXER

# 仅在 SDL 启用时计算 SDL 编译/链接参数；该计算会延后到 .config 加载后。
# 构建规则使用这些变量，并由 CONFIG_SDL 控制。
ifeq ($(CONFIG_SDL),y)
ifeq ($(HAVE_SDL2),y)
    SDL2_CFLAGS := $(call dep,cflags,sdl2)
    SDL2_LIBS := $(call dep,libs,sdl2)
endif
ifeq ($(HAVE_SDL2_MIXER),y)
ifeq ($(CONFIG_SDL_MIXER),y)
    SDL2_MIXER_LIBS := $(call dep,libs,SDL2_mixer)
endif
endif
endif

endif # SKIP_DEPS_CHECK

endif # _MK_DEPS_INCLUDED
