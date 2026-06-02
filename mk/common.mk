# 通用构建规则和工具宏
#
# 提供：
# - 构建输出详细程度控制
# - 功能开关宏，并兼容旧式 ENABLE_* 参数
# - 彩色输出辅助函数
# - 配置文件存在性校验

ifndef _MK_COMMON_INCLUDED
_MK_COMMON_INCLUDED := 1

# 输出详细程度控制。
# `make V=1` 等价于 `make VERBOSE=1`。
ifeq ("$(origin V)", "command line")
    VERBOSE = $(V)
endif
ifeq ("$(VERBOSE)","1")
    Q :=
    VECHO = @true
    REDIR =
else
    Q := @
    VECHO = @$(PRINTF)
    REDIR = >/dev/null
endif

# 功能开关宏（兼容旧构建参数）。

# 这些宏让旧式 ENABLE_* 参数可以和 Kconfig 生成的 CONFIG_* 一起工作。

# 将 CONFIG_* 转换成内部使用的 0/1 功能值。
define config-to-feature
$(if $(filter y,$(CONFIG_$(strip $1))),1,0)
endef

# 获取指定功能是否启用，同时支持 ENABLE_* 和 CONFIG_*。
POSITIVE_WORDS = 1 true yes y
NEGATIVE_WORDS = 0 false no n
define has
$(if $(filter $(firstword $(ENABLE_$(strip $1))),$(NEGATIVE_WORDS)),0,$(if $(filter $(firstword $(ENABLE_$(strip $1))),$(POSITIVE_WORDS)),1,$(call config-to-feature,$1)))
endef

# 根据配置写入编译器宏 RV32_FEATURE_*。
define set-feature
$(eval CFLAGS += -DRV32_FEATURE_$(strip $1)=$(call has,$1))
endef

# 彩色输出。

GREEN = \033[32m
YELLOW = \033[33m
RED = \033[31m
BLUE = \033[34m
NC = \033[0m

notice = $(PRINTF) "$(GREEN)$(strip $1)$(NC)\n"
noticex = $(shell echo "$(GREEN)$(strip $1)$(NC)\n")
warn = $(PRINTF) "$(YELLOW)$(strip $1)$(NC)\n"
warnx = $(shell echo "$(YELLOW)$(strip $1)$(NC)\n")
error_msg = $(PRINTF) "$(RED)$(strip $1)$(NC)\n"

# 配置校验。

# 不需要 .config 的目标。
# artifact 只下载预构建二进制，并通过 compat.mk 读取 ENABLE_* 来决定内容。
CONFIG_TARGETS := config menuconfig defconfig oldconfig savedefconfig \
                  clean distclean env-check artifact fetch-checksum build-linux-image

# 这些目标可以跳过耗时依赖探测（pkg-config、llvm-config 等）。
# 这样能显著加快 `make clean` 等非构建命令。
SKIP_DEPS_TARGETS := clean distclean
SKIP_DEPS_CHECK := $(filter $(SKIP_DEPS_TARGETS),$(MAKECMDGOALS))

# 会生成 .config 的目标。
CONFIG_GENERATORS := config menuconfig defconfig oldconfig

# 识别 *_defconfig 形式的预设配置目标，例如 jit_defconfig。
DEFCONFIG_GOALS := $(filter %_defconfig,$(MAKECMDGOALS))

# 判断当前目标是否需要配置。
# MAKECMDGOALS 为空表示执行默认目标 all，也就是构建目标。
BUILD_GOALS := $(filter-out $(CONFIG_TARGETS) $(DEFCONFIG_GOALS),$(MAKECMDGOALS))
IS_DEFAULT_BUILD := $(if $(MAKECMDGOALS),,yes)
NEEDS_CONFIG := $(if $(or $(BUILD_GOALS),$(IS_DEFAULT_BUILD)),yes,)
HAS_CONFIG_GEN := $(filter $(CONFIG_GENERATORS),$(MAKECMDGOALS))$(DEFCONFIG_GOALS)

# require-config 占位宏。
# 实际依赖在顶层 Makefile 中通过 $(BIN) 对 .config 的依赖完成。
# 保留该宏是为了兼容已有 include 顺序和调用点。
define require-config
endef

# 构建目录。
OUT ?= build
# $(OUT) 目标在顶层 Makefile 中定义，避免重复规则。

# 标准伪目标。

.PHONY: config menuconfig defconfig oldconfig savedefconfig
.PHONY: clean distclean env-check

# 可复用模板。

# 目录创建模板。
# 用法：$(eval $(call make-dir,$(OUT)/subdir))
define make-dir
$(1):
	$$(Q)mkdir -p $$@
endef

# 兼容源码包的条件式子模块/克隆辅助模板。
# 只有路径在 .gitmodules 中登记为子模块时才使用 git submodule；否则即使当前目录是
# git 仓库，也按普通依赖目录处理并在缺失时 clone。这样可兼容去掉 .gitmodules 的
# 源码包、教学仓库快照或手动解压的依赖目录。
# $(1)：目标目录
# $(2)：仓库 URL
# $(3)：分支或标签，可选，默认使用仓库默认分支
define ensure-submodule
	@if [ -d .git ] && [ -f .gitmodules ] && \
	    git config --file .gitmodules --get-regexp '^submodule\..*\.path$$' 2>/dev/null | \
	        awk '{ print $$2 }' | grep -qx "$(1)"; then \
	    git submodule update --init --depth=1 "$(1)" || { \
	        echo "错误：更新子模块 $(1) 失败" >&2; \
	        exit 1; \
	    }; \
	else \
	    if [ -d "$(1)" ] && [ ! -d "$(1)/.git" ]; then \
	        echo "警告：目录 $(1) 已存在但不是 git 仓库，正在删除..." >&2; \
	        rm -rf "$(1)"; \
	    fi; \
	    if [ ! -d "$(1)/.git" ]; then \
	        CLONE_OPTS="--depth=1"; \
	        if [ -n "$(3)" ]; then \
	            CLONE_OPTS="$$CLONE_OPTS --branch=$(3)"; \
	        fi; \
	        git clone $$CLONE_OPTS "$(2)" "$(1)" || { \
	            echo "错误：克隆 $(2) 到 $(1) 失败" >&2; \
	            exit 1; \
	        }; \
	    fi; \
	fi
endef

# 通用对象文件编译规则。
# $(1)：输出目录变量名，例如 OUT
# $(2)：源码目录，例如 src
# $(3)：额外 CFLAGS，可选
# $(4)：额外前置依赖，可选
# 用法：$(eval $(call compile-rule,OUT,src,$(EXTRA_CFLAGS)))
define compile-rule
$$($(1))/%.o: $(2)/%.c $(4) $$(CONFIG_HEADER) | $$($(1))
	$$(Q)mkdir -p $$(dir $$@)
	$$(VECHO) "  CC\t$$@\n"
	$$(Q)$$(CC) -o $$@ $$(CFLAGS) $(3) -c -MMD -MF $$@.d $$<
endef

# 测试框架模板。

# 通用测试框架。
# $(1)：测试名称，例如 cache、map、path
# $(2)：测试对象文件 basename 列表，例如 test-cache.o
# $(3)：来自 src/ 的额外对象依赖，使用完整路径
# $(4)：需要额外创建的子目录，可选
#
# 生成内容：
#   - $(1)_TEST_SRCDIR、$(1)_TEST_OUTDIR、$(1)_TEST_TARGET、$(1)_TEST_OBJS
#   - 测试对象文件编译规则
#   - 测试二进制链接规则
#
# 用法：$(eval $(call test-framework,cache,test-cache.o,$(OUT)/cache.o $(OUT)/mpool.o))
define test-framework
$(1)_TEST_SRCDIR := tests/$(1)
$(1)_TEST_OUTDIR := $$(OUT)/$(1)
$(1)_TEST_TARGET := $$($(1)_TEST_OUTDIR)/test-$(1)
$(1)_TEST_OBJS := $$(addprefix $$($(1)_TEST_OUTDIR)/, $(2)) $(3)

# 创建测试输出目录。
$$($(1)_TEST_OUTDIR) $(4):
	$$(Q)mkdir -p $$@

# 编译测试对象文件。
$$($(1)_TEST_OUTDIR)/%.o: $$($(1)_TEST_SRCDIR)/%.c $$(CONFIG_HEADER) | $$($(1)_TEST_OUTDIR) $(4)
	$$(VECHO) "  CC\t$$@\n"
	$$(Q)$$(CC) -o $$@ $$(CFLAGS) -I./src -c -MMD -MF $$@.d $$<

# 链接测试二进制。
$$($(1)_TEST_TARGET): $$($(1)_TEST_OBJS)
	$$(VECHO) "  LD\t$$@\n"
	$$(Q)$$(CC) $$^ -o $$@ $$(LDFLAGS)

# 记录测试对象文件的自动依赖。
deps += $$($(1)_TEST_OBJS:%.o=%.o.d)
endef

# 简单测试运行器：执行二进制并检查退出码。
# $(1)：测试名称
# 用法：$(eval $(call run-test-simple,map))
define run-test-simple
run-test-$(1): $$($(1)_TEST_TARGET)
	$$(VECHO) "正在运行 test-$(1) ... "
	$$(Q)$$< && $$(call notice, [OK]) || { $$(PRINTF) "失败。\n"; exit 1; }
endef

# 单个测试动作规则：生成一个 .out 文件。
# $(1)：测试名称
# $(2)：动作名称
define run-test-action
$$($(1)_TEST_OUTDIR)/$(2).out: $$($(1)_TEST_TARGET) $$($(1)_TEST_SRCDIR)/$(2).in $$($(1)_TEST_SRCDIR)/$(2).expect
	$$(Q)$$($(1)_TEST_TARGET) $$($(1)_TEST_SRCDIR)/$(2).in > $$@
endef

# 输出比较测试运行器：把运行结果与 expect 文件比较。
# $(1)：测试名称
# $(2)：测试动作名称列表
# 用法：$(eval $(call run-test-compare,cache,cache-new cache-put cache-get))
define run-test-compare
$(1)_TEST_ACTIONS := $(2)
$(1)_TEST_OUT := $$(addprefix $$($(1)_TEST_OUTDIR)/, $$($(1)_TEST_ACTIONS:%=%.out))

# 为每个动作生成独立规则，便于并行构建。
$$(foreach e,$(2),$$(eval $$(call run-test-action,$(1),$$(e))))

run-test-$(1): $$($(1)_TEST_OUT)
	$$(Q)$$(foreach e,$$($(1)_TEST_ACTIONS),\
	    $$(PRINTF) "正在运行 $$(e) ... "; \
	    if cmp $$($(1)_TEST_SRCDIR)/$$(e).expect $$($(1)_TEST_OUTDIR)/$$(e).out; then \
	        $$(call notice, [OK]); \
	    else \
	        $$(PRINTF) "失败。\n"; \
	        exit 1; \
	    fi; \
	)
endef

# 功能扩展模板。

# 为一组扩展设置编译器功能宏。
# $(1)：扩展名列表
# 用法：$(call set-features,EXT_M EXT_A EXT_F EXT_C)
define set-features
$(foreach ext,$(1),$(call set-feature,$(ext)))
endef

endif # _MK_COMMON_INCLUDED
