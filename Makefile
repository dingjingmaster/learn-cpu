# rv32emu/learn-cpu 顶层构建系统
#
# 快速开始：
#   make defconfig    # 生成默认配置
#   make              # 构建 rv32emu
#   make check        # 运行测试
#
# 更多配置和运行方式见 README.md。

.DEFAULT_GOAL := all

# 检查 GNU Make 版本；order-only prerequisites 至少需要 3.80。
ifeq ($(filter 3.80 3.81 3.82 3.83 3.84 4.% 5.% 6.% 7.% 8.% 9.%,$(MAKE_VERSION)),)
$(error 需要 GNU Make 3.80 或更高版本，当前版本：$(MAKE_VERSION))
endif

# 通用构建框架：输出控制、目录创建、测试模板和功能宏。
include mk/common.mk

# Kconfig 集成：生成 .config 和 src/feature.h。
include mk/kconfig.mk

# 加载配置。必须早于 toolchain.mk，使 CONFIG_BUILD_WASM 能影响 CC。
# 构建目标需要 .config；第一次构建前请运行 `make defconfig`。
ifeq ($(NEEDS_CONFIG),yes)
ifeq ($(wildcard .config),)
$(info 未找到 .config。请先运行以下命令之一：)
$(info )
$(info   make defconfig        - 应用默认配置)
$(info   make config           - 打开交互式配置菜单)
$(info   make ci_defconfig     - CI/架构测试配置)
$(info   make jit_defconfig    - 启用 JIT 编译)
$(info   make mini_defconfig   - 面向嵌入式场景的最小构建)
$(info   make system_defconfig - 系统模拟模式)
$(info   make wasm_defconfig   - WebAssembly 构建)
$(info )
$(error 缺少 .config。请先运行 `make defconfig`。)
endif
endif
-include .config
include mk/compat.mk

# 工具链探测；放在 .config 之后以支持 BUILD_WASM 切换编译器。
include mk/toolchain.mk
include mk/deps.mk
$(eval $(require-config))

# 构建产物和基础编译选项。
OUT ?= build
BIN := $(OUT)/rv32emu

CFLAGS = -std=gnu11 $(KCONFIG_CFLAGS) -Wall -Wextra -Werror
CFLAGS += -Wno-unused-label -include src/common.h -Isrc/ $(CFLAGS_NO_CET)
LDFLAGS += $(KCONFIG_LDFLAGS)
OBJS_EXT :=
deps :=

# 将 Kconfig 选项转换为源码使用的 RV32_FEATURE_* 功能宏。
$(call set-features, ELF_LOADER MOP_FUSION BLOCK_CHAINING LOG_COLOR)
$(call set-features, SYSTEM GOLDFISH_RTC ARCH_TEST)
$(call set-features, EXT_M EXT_A EXT_F EXT_C RV32E)
$(call set-features, Zicsr Zifencei Zba Zbb Zbc Zbs)
$(call set-features, SDL SDL_MIXER GDBSTUB JIT)

# 浮点扩展：启用 RV32F 时集成 Berkeley SoftFloat。
ifeq ($(CONFIG_EXT_F),y)
AR := ar
ifeq ("$(CC_IS_CLANG)", "1")
    ifeq ($(UNAME_S),Darwin)
        # macOS 系统自带 ar 即可。
    else ifeq ($(CONFIG_LTO),y)
        LLVM_AR := $(shell which llvm-ar 2>/dev/null)
        ifeq ($(LLVM_AR),)
            $(error Clang + LTO 需要 llvm-ar。请安装 LLVM 或关闭 LTO。)
        endif
        AR = llvm-ar
    else
        LLVM_AR := $(shell which llvm-ar 2>/dev/null)
        ifneq ($(LLVM_AR),)
            AR = llvm-ar
        endif
    endif
endif
ifeq ("$(CC_IS_EMCC)", "1")
AR = emar
endif
include mk/softfloat.mk
OBJS_NEED_SOFTFLOAT := $(OUT)/decode.o $(OUT)/riscv.o
ifeq ($(CONFIG_SYSTEM),y)
DEV_OUT := $(OUT)/devices
OBJS_NEED_SOFTFLOAT += $(DEV_OUT)/uart.o $(DEV_OUT)/plic.o
endif
$(OBJS_NEED_SOFTFLOAT): $(SOFTFLOAT_LIB)
LDFLAGS += $(SOFTFLOAT_LIB) -lm
endif

# SDL 图形/音频扩展：本地构建使用系统 SDL2，WASM 使用 emcc 端口。
ifeq ($(CONFIG_SDL),y)
ifneq ("$(CC_IS_EMCC)", "1")
    ifeq ($(SKIP_DEPS_CHECK),)
    ifeq ($(HAVE_SDL2),)
        $(warning 未找到 SDL2。可运行 `make config` 关闭 SDL。)
    endif
    endif
    ifneq ($(HAVE_SDL2),)
        OBJS_EXT += syscall_sdl.o
        $(OUT)/syscall_sdl.o: CFLAGS += $(SDL2_CFLAGS)
        LDFLAGS += $(SDL2_LIBS) -pthread
        ifeq ($(CONFIG_SDL_MIXER),y)
            ifneq ($(HAVE_SDL2_MIXER),)
                LDFLAGS += $(SDL2_MIXER_LIBS)
            else ifeq ($(SKIP_DEPS_CHECK),)
                $(warning 未找到 SDL2_mixer，音频支持将被关闭。)
            endif
        endif
    endif
endif
endif

# GDB Stub：允许远程 GDB 连接到模拟器调试客体程序。
ifeq ($(CONFIG_GDBSTUB),y)
GDBSTUB_OUT = $(abspath $(OUT)/mini-gdbstub)
GDBSTUB_COMM = 127.0.0.1:1234
src/mini-gdbstub/Makefile:
	$(call ensure-submodule,src/mini-gdbstub,https://github.com/RinHizakura/mini-gdbstub)
GDBSTUB_LIB := $(GDBSTUB_OUT)/libgdbstub.a
$(GDBSTUB_LIB): src/mini-gdbstub/Makefile
	$(MAKE) -C $(dir $<) O=$(dir $@)
OBJS_EXT += gdbstub.o breakpoint.o
CFLAGS += -D'GDBSTUB_COMM="$(GDBSTUB_COMM)"'
LDFLAGS += $(GDBSTUB_LIB) -pthread
gdbstub-test: $(BIN) artifact
	$(Q).ci/gdbstub-test.sh && $(call notice, [OK])
endif

# JIT 编译：一级模板 JIT 可选启用，二级 T2C 依赖 LLVM。
ifeq ($(CONFIG_JIT),y)
    OBJS_EXT += jit.o
    T2C_ENABLED := 0
    ifeq ($(CONFIG_T2C),y)
        # 通过 mk/toolchain.mk 的辅助函数探测 LLVM。
        # 用户可以用 `make LLVM_CONFIG=/path/to/llvm-config` 覆盖。
        ifndef LLVM_CONFIG
            LLVM_CONFIG := $(call detect-llvm-config)
        endif
        ifneq ($(LLVM_CONFIG),)
            LLVM_VERSION := $(call llvm-version,$(LLVM_CONFIG))
            ifeq ($(call llvm-check-libs,$(LLVM_CONFIG)),0)
                T2C_ENABLED := 1
                OBJS_EXT += t2c.o
                CFLAGS += -g $(call llvm-cflags,$(LLVM_CONFIG))
                LDFLAGS += $(call llvm-libfiles,$(LLVM_CONFIG))
                # 在 Homebrew LLVM 场景下补充库搜索路径。
                HOMEBREW_LLVM_PREFIX := $(call detect-homebrew-llvm-prefix)
                ifneq ($(HOMEBREW_LLVM_PREFIX),)
                ifneq ($(findstring $(HOMEBREW_LLVM_PREFIX),$(LLVM_CONFIG)),)
                    LDFLAGS += -L$(HOMEBREW_LLVM_PREFIX)/lib
                endif
                endif
            else
                $(warning 未找到 LLVM $(LLVM_VERSION) 库，T2C 将被关闭。)
            endif
        else
            $(warning 未找到 llvm-config ($(LLVM_MIN_VERSION)-$(LLVM_MAX_VERSION))，T2C 将被关闭。)
        endif
    endif
    CFLAGS += -DRV32_FEATURE_T2C=$(T2C_ENABLED)
    # JIT 只支持 x86_64 或 ARM64；WASM 为交叉编译，跳过宿主检查。
    ifneq ($(CC_IS_EMCC),1)
        ifneq ($(UNAME_M),$(filter $(UNAME_M),x86_64 aarch64 arm64))
            $(error JIT 仅支持 x86_64 和 ARM64 平台。)
        endif
    endif
$(OUT)/jit.o: src/jit.c src/rv32_jit.c $(CONFIG_HEADER)
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) -o $@ $(CFLAGS) -c -MMD -MF $@.d $<
# T2C 优化级别来自 Kconfig；未配置时默认使用 3。
T2C_OPT_LEVEL ?= $(or $(CONFIG_T2C_OPT_LEVEL),3)
$(OUT)/t2c.o: src/t2c.c src/t2c_template.c $(CONFIG_HEADER)
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) -o $@ $(CFLAGS) -DCONFIG_T2C_OPT_LEVEL=$(T2C_OPT_LEVEL) -c -MMD -MF $@.d $<
else
    CFLAGS += -DRV32_FEATURE_T2C=0
endif

# 解释器依赖尾调用调度，这里为主执行文件打开相关优化并关闭栈保护干扰。
$(OUT)/emulate.o: CFLAGS += -foptimize-sibling-calls -fomit-frame-pointer -fno-stack-check -fno-stack-protector

# HTTP 下载工具，被 external.mk 和 artifact.mk 共享。
include mk/http.mk

# 外部依赖、预构建资源、系统模拟和 WebAssembly 支持。
include mk/external.mk
include mk/artifact.mk
include mk/system.mk
include mk/wasm.mk

# 构建目标和对象文件列表。
DTB_DEPS :=
ifeq ($(CONFIG_SYSTEM),y)
ifneq ($(CONFIG_ELF_LOADER),y)
DTB_DEPS := $(BUILD_DTB) $(BUILD_DTB2C)
# 构建 DTB 前确保 DTC 子模块存在。
# BUILD_DTB 使用系统 dtc，只有 BUILD_DTB2C 需要子模块头文件。
$(BUILD_DTB2C): $(DTC_SENTINEL)
endif
endif

OBJS := map.o utils.o decode.o io.o syscall.o
ifeq ($(CC_IS_EMCC), 1)
OBJS += em_runtime.o
endif
OBJS += emulate.o riscv.o log.o elf.o cache.o mpool.o $(OBJS_EXT) main.o
OBJS := $(addprefix $(OUT)/, $(OBJS))
deps += $(OBJS:%.o=%.o.d)

ifeq ($(CONFIG_EXT_F),y)
$(OBJS): $(SOFTFLOAT_LIB)
endif
ifeq ($(CONFIG_GDBSTUB),y)
$(OBJS): $(GDBSTUB_LIB)
endif

$(OUT)/%.o: src/%.c $(deps_emcc) $(CONFIG_HEADER) | $(OUT)
	$(Q)mkdir -p $(dir $@)
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) -o $@ $(CFLAGS) $(CFLAGS_emcc) -c -MMD -MF $@.d $<

$(OUT):
	$(Q)mkdir -p $@

# 链接最终模拟器可执行文件。
$(BIN): $(OBJS) $(DEV_OBJS) | $(OUT)
	$(VECHO) "  LD\t$@\n"
	$(Q)$(CC) -o $@ $(CFLAGS_emcc) $^ $(LDFLAGS)

all: $(DTB_DEPS) $(BIN)
	@$(call notice, 构建完成：$(BIN))

# 开发工具和测试目标。
include mk/tools.mk
include mk/riscv-arch-test.mk
include mk/tests.mk

tool: $(TOOLS_BIN)

# 清理构建产物。
clean:
	$(VECHO) "正在清理... "
	$(Q)$(RM) $(BIN) $(OBJS) $(DEV_OBJS) $(BUILD_DTB) $(BUILD_DTB2C) $(HIST_BIN) $(HIST_OBJS) $(deps) $(WEB_FILES) $(CACHE_OUT)
	$(Q)-$(RM) $(SOFTFLOAT_LIB)
	$(Q)$(call notice, [OK])

# 清理对象文件和配置；保留 artifact 以提升 CI 重复运行效率。
cleanconfig: clean
	$(VECHO) "正在删除配置文件... "
	$(Q)-$(RM) .config $(CONFIG_HEADER)
	$(Q)-$(RM) -r $(SOFTFLOAT_DUMMY_PLAT) $(OUT)/softfloat
	$(Q)$(call notice, [OK])

distclean: cleanconfig
	$(VECHO) "正在删除所有生成文件... "
	$(Q)$(RM) -r $(OUT)/id1 $(DEMO_DIR) $(OUT)/mini-gdbstub $(OUT)/devices
	$(Q)$(RM) *.zip
	$(Q)$(RM) -r $(OUT)/linux-x86-softfp $(OUT)/riscv32 $(OUT)/linux-image
	$(Q)$(RM) $(OUT)/sha1sum-* $(OUT)/.stamp-* $(OUT)/.verify_result
	$(Q)$(RM) $(OUT)/rv32emu-prebuilt*.tar.gz $(OUT)/rv32emu-prebuilt-sail-*
	$(Q)$(call notice, [OK])

.PHONY: all tool clean cleanconfig distclean gdbstub-test

-include $(deps)
