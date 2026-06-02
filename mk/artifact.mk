# 预构建产物和基准程序构建
#
# 负责下载预构建二进制，或在关闭预构建时从源码构建测试程序。

ifndef _MK_ARTIFACT_INCLUDED
_MK_ARTIFACT_INCLUDED := 1

ENABLE_PREBUILT ?= 1

# 说明：CC 和 CROSS_COMPILE 已在 mk/toolchain.mk 中设置。

BIN_DIR := $(abspath $(OUT))

TEST_SUITES += \
	ansibench \
	rv8-bench

# ieee754 需要 F 扩展。
# smolnes、ticks 包含内联汇编，只能在 RISC-V 目标上工作。
TEST_BENCHES += \
	captcha \
	donut \
	fcalc \
	hamilton \
	jit \
	lena \
	line \
	maj2random \
	mandelbrot \
	nqueens \
	nyancat \
	pi \
	puzzle \
	qrcode \
	richards \
	rvsim \
	spirograph \
	uaes

SCIMARK2_URL := https://math.nist.gov/scimark2/scimark2_1c.zip
SCIMARK2_SHA1 := de278c5b8cef84ab6dda41855052c7bfef919e36

# 创建输出目录；使用 order-only prerequisite 模式。
$(BIN_DIR)/linux-x86-softfp $(BIN_DIR)/riscv32 $(BIN_DIR)/linux-image:
	$(Q)mkdir -p $@

# URL 和 API 配置，作为唯一数据来源。
PREBUILT_REPO := sysprog21/rv32emu-prebuilt
GITHUB_API_URL := https://api.github.com/repos/$(PREBUILT_REPO)/releases
GITHUB_BLOB_URL := https://github.com/$(PREBUILT_REPO)/releases/download

# 根据 release tag 生成二进制下载 URL。
# $(1)：release tag
prebuilt-url = $(GITHUB_BLOB_URL)/$(1)

# HTTP 工具由 mk/http.mk 提供；该文件应先于本文件 include。
# 提供：HTTP_TOOL、HTTP_GET、HTTP_DOWNLOAD、HTTP_DOWNLOAD_QUIET。

# 模式配置：集中管理不同模式所需的 release 信息。
# 每种模式定义：TAG、STAMP、CHECKSUMS、TARBALL、SENTINEL、EXTRACT、VERIFY。

# SYSTEM 模式：Linux 内核引导。
MODE_SYSTEM_TAG        := Linux-Image
MODE_SYSTEM_STAMP      := $(BIN_DIR)/.stamp-linux-image
MODE_SYSTEM_CHECKSUMS  := sha1sum-linux-image
MODE_SYSTEM_TARBALL    := rv32emu-linux-image-prebuilt.tar.gz
MODE_SYSTEM_SENTINEL   := $(BIN_DIR)/linux-image/Image
MODE_SYSTEM_EXTRACT    := yes
MODE_SYSTEM_VERIFY     := $(BIN_DIR)/sha1sum-linux-image:$(BIN_DIR)/

# ARCH_TEST 模式：RISC-V 合规测试。
MODE_ARCH_TAG          := sail
MODE_ARCH_STAMP        := $(BIN_DIR)/.stamp-sail
MODE_ARCH_CHECKSUMS    := rv32emu-prebuilt-sail-$(HOST_PLATFORM).sha
MODE_ARCH_TARBALL      := rv32emu-prebuilt-sail-$(HOST_PLATFORM)
MODE_ARCH_SENTINEL     := $(BIN_DIR)/riscv_sim_RV32
MODE_ARCH_EXTRACT      := no
MODE_ARCH_VERIFY       := $(BIN_DIR)/rv32emu-prebuilt-sail-$(HOST_PLATFORM).sha:$(BIN_DIR)/

# ELF 模式：默认预构建二进制。
MODE_ELF_TAG           := ELF
MODE_ELF_STAMP         := $(BIN_DIR)/.stamp-prebuilt
MODE_ELF_CHECKSUMS     := sha1sum-linux-x86-softfp sha1sum-riscv32
MODE_ELF_TARBALL       := rv32emu-prebuilt.tar.gz
MODE_ELF_SENTINEL      := $(BIN_DIR)/riscv32/coremark
MODE_ELF_EXTRACT       := yes
MODE_ELF_VERIFY        := $(BIN_DIR)/sha1sum-linux-x86-softfp:$(BIN_DIR)/linux-x86-softfp/ $(BIN_DIR)/sha1sum-riscv32:$(BIN_DIR)/riscv32/

# 根据当前配置选择生效的模式。
ifeq ($(call has, SYSTEM), 1)
    ACTIVE_TAG       := $(MODE_SYSTEM_TAG)
    ACTIVE_STAMP     := $(MODE_SYSTEM_STAMP)
    ACTIVE_CHECKSUMS := $(MODE_SYSTEM_CHECKSUMS)
    ACTIVE_TARBALL   := $(MODE_SYSTEM_TARBALL)
    ACTIVE_SENTINEL  := $(MODE_SYSTEM_SENTINEL)
    ACTIVE_EXTRACT   := $(MODE_SYSTEM_EXTRACT)
    ACTIVE_VERIFY    := $(MODE_SYSTEM_VERIFY)
else ifeq ($(call has, ARCH_TEST), 1)
    ACTIVE_TAG       := $(MODE_ARCH_TAG)
    ACTIVE_STAMP     := $(MODE_ARCH_STAMP)
    ACTIVE_CHECKSUMS := $(MODE_ARCH_CHECKSUMS)
    ACTIVE_TARBALL   := $(MODE_ARCH_TARBALL)
    ACTIVE_SENTINEL  := $(MODE_ARCH_SENTINEL)
    ACTIVE_EXTRACT   := $(MODE_ARCH_EXTRACT)
    ACTIVE_VERIFY    := $(MODE_ARCH_VERIFY)
else
    ACTIVE_TAG       := $(MODE_ELF_TAG)
    ACTIVE_STAMP     := $(MODE_ELF_STAMP)
    ACTIVE_CHECKSUMS := $(MODE_ELF_CHECKSUMS)
    ACTIVE_TARBALL   := $(MODE_ELF_TARBALL)
    ACTIVE_SENTINEL  := $(MODE_ELF_SENTINEL)
    ACTIVE_EXTRACT   := $(MODE_ELF_EXTRACT)
    ACTIVE_VERIFY    := $(MODE_ELF_VERIFY)
endif

# 核心宏。

# 从 GitHub API 获取 tag 的 shell 命令。
# $(1)：tag 匹配模式
FETCH_TAG_CMD = $(call HTTP_GET,$(GITHUB_API_URL)) | grep '"tag_name"' | grep "$(1)" | head -n 1 | sed -E 's/.*"tag_name": "([^"]+)".*/\1/'

# 在解析阶段从 GitHub API 获取最新 release tag。
# $(1)：tag 匹配模式，例如 ELF、Linux-Image、sail。
define fetch-releases-tag
    $(eval LATEST_RELEASE := $(shell $(call FETCH_TAG_CMD,$(1)))) \
    $(if $(LATEST_RELEASE),, \
        $(error 获取最新 release tag 失败) \
    )
endef

# 检查 artifact 是否完整存在：stamp、checksum 和代表性二进制。
# 说明：wildcard 只检查存在性，不检查内容。
# 空 checksum 文件会在 recipe 阶段由 fetch-checksum-files 处理。
# $(1)：stamp 文件
# $(2)：checksum 文件 basename 列表，用空格分隔
# $(3)：代表性二进制
# foreach/if 组合会对每个缺失文件输出 "x"；只要有 "x"，
# 外层 $(if ...) 返回空，否则返回 "yes" 表示所有文件存在。
check-sentinels = $(and $(wildcard $(1)),$(if $(foreach f,$(2),$(if $(wildcard $(BIN_DIR)/$(f)),,x)),,yes),$(wildcard $(3)))

# 处理 SHA-1 校验结果：失败时重新拉取，拉取后再次校验。
# $(1)：是否解压 tarball（yes/no）
# $(2)：保存校验结果的临时文件
# $(3)：成功时创建的 stamp 文件
# $(4)：恢复拉取使用的 tag 匹配模式
# $(5)：tarball 文件名
# $(6)：校验规格，格式为 "checksum_file:verify_dir" 对列表
define handle-sha1-result
	$(Q)if [ "$$(cat "$(2)" 2>/dev/null || echo 0)" = "1" ]; then \
	    $(call warn, SHA-1 校验失败！); \
	    blob_url="$(PREBUILT_BLOB_URL)"; \
	    if [ -z "$(LATEST_RELEASE)" ]; then \
	        echo "正在尝试获取 $(4) 的最新 tag 以恢复..."; \
	        tag=$$($(call FETCH_TAG_CMD,$(4))); \
	        if [ -z "$$tag" ]; then \
	             echo "错误：恢复失败，无法获取 tag。" >&2; \
	             rm -f "$(2)" "$(3)"; exit 1; \
	        fi; \
	        blob_url="$(call prebuilt-url,$$tag)"; \
	    fi; \
	    $(PRINTF) "正在从 $$blob_url 重新获取预构建二进制...\n"; \
	    rm -f "$(3)"; \
	    $(call HTTP_DOWNLOAD,"$$blob_url/$(5)","$(BIN_DIR)/$(5)") || exit 1; \
	    $(if $(filter yes,$(1)),tar --strip-components=1 -zxf "$(BIN_DIR)/$(5)" -C "$(BIN_DIR)" || exit 1;) \
	    $(PRINTF) "重新获取后再次校验... "; \
	    reverify_ok=1; \
	    for spec in $(6); do \
	        checksum=$$(echo "$$spec" | cut -d: -f1); \
	        dir=$$(echo "$$spec" | cut -d: -f2); \
	        if ! (cd "$$dir" && $(SHA1SUM) -c "$$checksum" >/dev/null 2>&1); then \
	            reverify_ok=0; break; \
	        fi; \
	    done; \
	    if [ "$$reverify_ok" = "1" ]; then \
	        $(call notice, [OK]); \
	        touch "$(3)"; \
	    else \
	        echo "失败" >&2; \
	        echo "错误：重新获取成功，但校验仍失败。" >&2; \
	        echo "下载的归档文件可能已损坏。" >&2; \
	        rm -f "$(2)"; exit 1; \
	    fi; \
	else \
	    $(call notice, [OK]); \
	    touch "$(3)"; \
	fi; \
	rm -f "$(2)"
endef

# 当 checksum 文件缺失或为空时下载它们。
# 处理 checksum 文件存在但为空的边界情况；必要时重新获取 tag。
# $(1)：需要下载的 checksum 文件 basename 列表，用空格分隔
# $(2)：恢复拉取使用的 tag 匹配模式
define fetch-checksum-files
	$(Q)missing=0; \
	for f in $(1); do \
	    if [ ! -s "$(BIN_DIR)/$$f" ]; then missing=1; break; fi; \
	done; \
	if [ "$$missing" = "1" ]; then \
	    blob_url="$(PREBUILT_BLOB_URL)"; \
	    if [ -z "$(LATEST_RELEASE)" ]; then \
	        tag=$$($(call FETCH_TAG_CMD,$(2))); \
	        if [ -z "$$tag" ]; then \
	            echo "错误：无法获取 release tag。" >&2; exit 1; \
	        fi; \
	        blob_url="$(call prebuilt-url,$$tag)"; \
	    fi; \
	    $(foreach f,$(1),$(call HTTP_DOWNLOAD_QUIET,"$$blob_url/$(f)","$(BIN_DIR)/$(f)") || exit 1;) \
	    $(call notice, [OK]); \
	else \
	    $(call notice, [cached]); \
	fi
endef

# release tag 获取；仅在 artifact 相关目标需要时执行。
LATEST_RELEASE ?=

# 只有请求 artifact 相关目标时才获取 release，避免 make defconfig 等无关目标访问网络。
ARTIFACT_TARGETS := artifact fetch-checksum scimark2 ieeelib \
                    check misalign doom quake arch-test system gdbstub-test

ifneq ($(filter $(ARTIFACT_TARGETS),$(MAKECMDGOALS)),)
ifeq ($(call has, PREBUILT), 1)
    # 校验 HTTP 下载工具是否可用。
    ifeq ($(HTTP_TOOL),)
        $(error 未找到 HTTP 下载工具。请安装 curl 或 wget。)
    endif
    # 只有 artifact 已完整存在时才跳过 LATEST_RELEASE 获取。
    ifeq ($(LATEST_RELEASE),)
        ifeq ($(call check-sentinels,$(ACTIVE_STAMP),$(ACTIVE_CHECKSUMS),$(ACTIVE_SENTINEL)),)
            $(call fetch-releases-tag,$(ACTIVE_TAG))
        endif
    endif
endif
endif

ifeq ($(call has, PREBUILT), 1)
    PREBUILT_BLOB_URL = $(call prebuilt-url,$(LATEST_RELEASE))
else
    # 源码构建：为了 x86 兼容性，关闭硬件浮点。
    CFLAGS := -m32 -mno-sse -mno-sse2 -msoft-float -O2 -Wno-unused-result -L$(BIN_DIR)
    LDFLAGS := -lsoft-fp -lm

    CFLAGS_CROSS := -march=rv32im -mabi=ilp32 -O2 -Wno-implicit-function-declaration
    LDFLAGS_CROSS := -lm -lsemihost
endif

# 构建目标。
.PHONY: artifact fetch-checksum scimark2 ieeelib

# 保存校验结果的临时文件。
VERIFY_RESULT_FILE := $(BIN_DIR)/.verify_result

artifact: fetch-checksum ieeelib scimark2
ifeq ($(call has, PREBUILT), 1)
	$(Q)$(PRINTF) "正在校验预构建二进制... "
	$(Q)rm -f "$(VERIFY_RESULT_FILE)" && echo 0 > "$(VERIFY_RESULT_FILE)"
	$(Q)for spec in $(ACTIVE_VERIFY); do \
	    checksum=$$(echo "$$spec" | cut -d: -f1); \
	    dir=$$(echo "$$spec" | cut -d: -f2); \
	    if [ ! -s "$$checksum" ]; then \
	        echo 1 > "$(VERIFY_RESULT_FILE)"; break; \
	    elif ! (cd "$$dir" && $(SHA1SUM) -c "$$checksum" >/dev/null 2>&1); then \
	        echo 1 > "$(VERIFY_RESULT_FILE)"; break; \
	    fi; \
	done
	$(call handle-sha1-result,$(ACTIVE_EXTRACT),$(VERIFY_RESULT_FILE),$(ACTIVE_STAMP),$(ACTIVE_TAG),$(ACTIVE_TARBALL),$(ACTIVE_VERIFY))
else
ifeq ($(call has, SYSTEM), 1)
	$(Q)(mkdir -p /tmp/rv32emu-linux-image-prebuilt/linux-image)
	$(Q)(cd $(BIN_DIR) && $(SHA1SUM) linux-image/Image >> sha1sum-linux-image)
	$(Q)(cd $(BIN_DIR) && $(SHA1SUM) linux-image/rootfs.cpio >> sha1sum-linux-image)
	$(Q)(cd $(BIN_DIR) && $(SHA1SUM) linux-image/simplefs.ko >> sha1sum-linux-image)
	$(Q)(mv $(BIN_DIR)/sha1sum-linux-image /tmp)
	$(Q)(mv $(BIN_DIR)/linux-image/Image /tmp/rv32emu-linux-image-prebuilt/linux-image)
	$(Q)(mv $(BIN_DIR)/linux-image/rootfs.cpio /tmp/rv32emu-linux-image-prebuilt/linux-image)
	$(Q)(mv $(BIN_DIR)/linux-image/simplefs.ko /tmp/rv32emu-linux-image-prebuilt/linux-image)
else
	$(Q)if [ -d .git ]; then \
	    git submodule update --init --depth=1 $(addprefix ./tests/,$(foreach tb,$(TEST_SUITES),$(tb))) || { \
	        echo "错误：更新测试套件子模块失败" >&2; \
	        exit 1; \
	    }; \
	else \
	    for tb in $(TEST_SUITES); do \
	        case "$$tb" in \
	            ansibench|rv8-bench) \
	                if [ -d "./tests/$$tb" ] && [ ! -d "./tests/$$tb/.git" ]; then \
	                    echo "警告：正在删除没有 .git 的既有目录 ./tests/$$tb" >&2; \
	                    rm -rf "./tests/$$tb"; \
	                fi; \
	                if [ ! -d "./tests/$$tb/.git" ]; then \
	                    case "$$tb" in \
	                        ansibench) git clone --depth=1 https://github.com/sysprog21/ansibench "./tests/$$tb" ;; \
	                        rv8-bench) git clone --depth=1 https://github.com/sysprog21/rv8-bench "./tests/$$tb" ;; \
	                    esac || { \
	                        echo "错误：克隆测试套件 $$tb 失败" >&2; \
	                        exit 1; \
	                    }; \
	                fi; \
	                ;; \
	            *) \
	                echo "警告：未知测试套件 '$$tb'，跳过" >&2; \
	                ;; \
	        esac; \
	    done; \
	fi
	$(Q)for tb in $(TEST_SUITES); do \
	    CC=$(CC) CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)" BINDIR=$(BIN_DIR)/linux-x86-softfp $(MAKE) -C ./tests/$$tb; \
	done
	$(Q)for tb in $(TEST_SUITES); do \
	    CC=$(CROSS_COMPILE)gcc CFLAGS="$(CFLAGS_CROSS)" LDFLAGS="$(LDFLAGS_CROSS)" BINDIR=$(BIN_DIR)/riscv32 $(MAKE) -C ./tests/$$tb; \
	done

	$(Q)$(PRINTF) "正在构建独立测试程序...\n"
	$(Q)for tb in $(TEST_BENCHES); do \
	    $(CC) $(CFLAGS) -o $(BIN_DIR)/linux-x86-softfp/$$tb ./tests/$$tb.c $(LDFLAGS); \
	done
	$(Q)for tb in $(TEST_BENCHES); do \
	    $(CROSS_COMPILE)gcc $(CFLAGS_CROSS) -o $(BIN_DIR)/riscv32/$$tb ./tests/$$tb.c $(LDFLAGS_CROSS); \
	done

	$(call ensure-submodule,tests/doom,https://github.com/sysprog21/doom_riscv)
	$(call ensure-submodule,tests/quake,https://github.com/sysprog21/quake-embedded)
	$(Q)$(PRINTF) "正在构建 doom...\n"
	$(Q)$(MAKE) -C ./tests/doom/src/riscv CROSS=$(CROSS_COMPILE)
	$(Q)cp ./tests/doom/src/riscv/doom-riscv.elf $(BIN_DIR)/riscv32/doom
	$(Q)$(PRINTF) "正在构建 quake...\n"
	$(Q)cd ./tests/quake && mkdir -p build && cd build && \
	    cmake -DCMAKE_TOOLCHAIN_FILE=../port/boards/rv32emu/toolchain.cmake \
	          -DCROSS_COMPILE=$(CROSS_COMPILE) \
	          -DCMAKE_BUILD_TYPE=RELEASE -DBOARD_NAME=rv32emu .. && \
	    make
	$(Q)cp ./tests/quake/build/port/boards/rv32emu/quake $(BIN_DIR)/riscv32/quake

	$(Q)(cd $(BIN_DIR)/linux-x86-softfp; for fd in *; do $(SHA1SUM) "$$fd"; done) >> $(BIN_DIR)/sha1sum-linux-x86-softfp
	$(Q)(cd $(BIN_DIR)/riscv32; for fd in *; do $(SHA1SUM) "$$fd"; done) >> $(BIN_DIR)/sha1sum-riscv32
endif
endif

fetch-checksum:
ifeq ($(call has, PREBUILT), 1)
	$(Q)$(PRINTF) "正在获取 checksum 文件... "
	$(call fetch-checksum-files,$(ACTIVE_CHECKSUMS),$(ACTIVE_TAG))
endif

scimark2: | $(BIN_DIR)/linux-x86-softfp $(BIN_DIR)/riscv32
ifeq ($(call has, PREBUILT), 0)
ifeq ($(call has, SYSTEM), 0)
	$(call prologue,scimark2)
	$(Q)$(call download,$(SCIMARK2_URL))
	$(call verify-sha,$(SHA1SUM),$(SCIMARK2_SHA1),$(notdir $(SCIMARK2_URL)))
	$(Q)$(call extract,./tests/scimark2,$(notdir $(SCIMARK2_URL)),0)
	$(call epilogue,$(notdir $(SCIMARK2_URL)))
	$(Q)$(PRINTF) "正在构建 scimark2...\n"
	$(Q)$(MAKE) -C ./tests/scimark2 CC=$(CC) CFLAGS="-m32 -O2"
	$(Q)cp ./tests/scimark2/scimark2 $(BIN_DIR)/linux-x86-softfp/scimark2
	$(Q)$(MAKE) -C ./tests/scimark2 clean && $(RM) ./tests/scimark2/scimark2.o
	$(Q)$(MAKE) -C ./tests/scimark2 CC=$(CROSS_COMPILE)gcc CFLAGS="-march=rv32imf -mabi=ilp32 -O2"
	$(Q)cp ./tests/scimark2/scimark2 $(BIN_DIR)/riscv32/scimark2
endif
endif

ieeelib:
ifeq ($(call has, PREBUILT), 0)
	$(call ensure-submodule,src/ieeelib,https://github.com/sysprog21/ieeelib)
	$(Q)$(MAKE) -C ./src/ieeelib CC=$(CC) CFLAGS="$(CFLAGS)" BINDIR=$(BIN_DIR)
endif

endif # _MK_ARTIFACT_INCLUDED
