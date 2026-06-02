# 外部数据依赖：Doom、Quake、Linux kernel 等
#
# 提供下载、解压和校验外部资源的模板。

ifndef _MK_EXTERNAL_INCLUDED
_MK_EXTERNAL_INCLUDED := 1

# SHA 工具，用于资源校验。
# 使用 command -v 以保持 POSIX 兼容，避免依赖 which。

SHA1SUM := $(shell command -v sha1sum 2>/dev/null)
ifndef SHA1SUM
    SHA1SUM := $(shell command -v shasum 2>/dev/null)
endif
ifndef SHA1SUM
    $(warning 未找到 SHA1SUM 工具，将跳过校验。)
endif

SHA256SUM := $(shell command -v sha256sum 2>/dev/null)
ifeq ($(SHA256SUM),)
    SHA256SUM_SHASUM := $(shell command -v shasum 2>/dev/null)
    ifneq ($(SHA256SUM_SHASUM),)
        # 使用前确认 shasum 支持 -a 256。
        SHA256SUM_TEST := $(shell echo test | $(SHA256SUM_SHASUM) -a 256 >/dev/null 2>&1 && echo ok)
        ifeq ($(SHA256SUM_TEST),ok)
            SHA256SUM := $(SHA256SUM_SHASUM) -a 256
        endif
    endif
endif
ifeq ($(SHA256SUM),)
    $(warning 未找到 SHA256SUM 工具，将跳过校验。)
endif

# 下载、解压和校验模板。

# 打印下载提示。
# $(1)：目标文件
define prologue
	$(VECHO) "  GET\t$(1)\n"
endef

# 判断输入是否是 git clone 命令（第一个单词为 git）。
# $(1)：URL 或 git 命令
define is-git-clone
$(filter git,$(firstword $(1)))
endef

# HTTP 工具由 mk/http.mk 提供；该文件应先于本文件 include。
# 提供：HTTP_TOOL、HTTP_GET、HTTP_DOWNLOAD、HTTP_DOWNLOAD_QUIET。

# 从 URL 下载，支持 git clone 或带重试的 HTTP 下载。
# $(1)：URL 或 git 命令
define download
$(if $(call is-git-clone,$(1)),\
	$(1),\
	$(call HTTP_DOWNLOAD,"$(strip $(1))","$(notdir $(1))"))
endef

# 解压归档到目标目录。
# $(1)：目标目录
# $(2)：归档文件（.zip 或 .tar.gz）
# $(3)：strip-components 层级，仅用于 tar
define extract
$(if $(filter .zip,$(suffix $(2))),\
	unzip -q -d $(1) $(2),\
	tar -xf $(2) --strip-components=$(3) -C $(1))
endef

# 校验 SHA；运行时自动判断目标是文件还是目录。
# $(1)：SHA 命令（sha1sum 或 sha256sum；工具不可用时可能为空）
# $(2)：期望 SHA 值
# $(3)：待校验路径（文件或目录）
# 行为：校验失败时退出；没有 SHA 工具时跳过。
define verify-sha
	@if [ -z "$(1)" ]; then \
	    echo "跳过 $(3) 的 SHA 校验（没有可用的 SHA 工具）"; \
	elif [ -d "$(3)" ]; then \
	    FILE_HASHES=$$(find "$(3)" -type f -not -path '*/.git/*' -print0 | LC_ALL=C sort -z | xargs -0 $(1) 2>/dev/null | LC_ALL=C sort); \
	    if [ -z "$$FILE_HASHES" ]; then \
	        echo "目录 $(3) 的 SHA 校验失败：未找到文件"; \
	        exit 1; \
	    fi; \
	    COMPUTED=$$(echo "$$FILE_HASHES" | $(1) | cut -f1 -d' '); \
	    if [ "$$COMPUTED" != "$(2)" ]; then \
	        echo "目录 $(3) 的 SHA 校验失败"; \
	        exit 1; \
	    fi; \
	else \
	    if ! echo "$(2)  $(3)" | $(1) -c - >/dev/null 2>&1; then \
	        echo "$(3) 的 SHA 校验失败"; \
	        exit 1; \
	    fi; \
	fi
endef

# 清理已下载归档。
# $(1)：归档文件名
define epilogue
	$(Q)$(RM) $(1)
endef

# 外部数据定义。

# Doom WAD 文件。
DOOM_DATA_URL := https://www.doomworld.com/3ddownloads/ports/shareware_doom_iwad.zip
DOOM_DATA_DEST := $(OUT)
DOOM_DATA := $(DOOM_DATA_DEST)/DOOM1.WAD
DOOM_DATA_SHA := 5b2e249b9c5133ec987b3ea77596381dc0d6bc1d
DOOM_DATA_SHA_CMD := $(SHA1SUM)

# Quake PAK 文件。
QUAKE_DATA_URL := https://www.libsdl.org/projects/quake/data/quakesw-1.0.6.zip
QUAKE_DATA_DEST := $(OUT)
QUAKE_DATA := $(QUAKE_DATA_DEST)/id1/pak0.pak
QUAKE_DATA_SHA := 36b42dc7b6313fd9cabc0be8b9e9864840929735
QUAKE_DATA_SHA_CMD := $(SHA1SUM)

# Timidity 软件合成器，用于 SDL2_mixer 播放 MIDI。
TIMIDITY_DATA_URL := https://www.libsdl.org/projects/old/SDL_mixer/timidity/timidity.tar.gz
TIMIDITY_DATA_DEST := $(OUT)
TIMIDITY_DATA := $(TIMIDITY_DATA_DEST)/timidity
TIMIDITY_DATA_SKIP_DIR_LEVEL := 0
# find $(TIMIDITY_DATA_DEST)/timidity -type f -not -path '*/.git/*' -print0 | \
#	LC_ALL=C sort -z | \
#	xargs -0 sha1sum | \
#	LC_ALL=C sort | \
#	sha1sum
TIMIDITY_DATA_SHA := cf6217a5d824b717ec4a07e15e6c129a4657ca25
TIMIDITY_DATA_SHA_CMD := $(SHA1SUM)

# Buildroot，用于构建 Linux 镜像。
BUILDROOT_VERSION := 2025.11
BUILDROOT_DATA_DEST := /tmp
BUILDROOT_DATA := $(BUILDROOT_DATA_DEST)/buildroot
BUILDROOT_DATA_URL := git clone https://github.com/buildroot/buildroot "$(BUILDROOT_DATA)" -b $(BUILDROOT_VERSION) --depth=1
# find /tmp/buildroot -type f -not -path '*/.git/*' -print0 | \
#	LC_ALL=C sort -z | \
#	xargs -0 sha1sum | \
#	LC_ALL=C sort | \
#	sha1sum
BUILDROOT_DATA_SHA := 70999b51eb4034eb96457a0ac210365c9cc7c2bb
BUILDROOT_DATA_SHA_CMD := $(SHA1SUM)

# Linux 内核；使用惰性求值，避免解析 Makefile 时访问网络。
LINUX_VERSION := 6
LINUX_PATCHLEVEL := 1
LINUX_CDN_BASE_URL := https://cdn.kernel.org/pub/linux/kernel
LINUX_CDN_VERSION_URL := $(LINUX_CDN_BASE_URL)/v$(LINUX_VERSION).x
LINUX_DATA_DEST := /tmp/linux
LINUX_DATA := $(LINUX_DATA_DEST)/linux-$(LINUX_VERSION).$(LINUX_PATCHLEVEL).%.tar.gz
LINUX_DATA_SKIP_DIR_LEVEL := 1
LINUX_DATA_SHA_CMD := $(SHA256SUM)

# simplefs 内核模块。
SIMPLEFS_VERSION := rel2025.0
SIMPLEFS_DATA_DEST := /tmp
SIMPLEFS_DATA := $(SIMPLEFS_DATA_DEST)/simplefs
SIMPLEFS_DATA_URL := git clone https://github.com/sysprog21/simplefs "$(SIMPLEFS_DATA)" -b $(SIMPLEFS_VERSION) --depth=1
# find /tmp/simplefs -type f -not -path '*/.git/*' -print0 | \
#	LC_ALL=C sort -z | \
#	xargs -0 sha1sum | \
#	LC_ALL=C sort | \
#	sha1sum
SIMPLEFS_DATA_SHA := 863936f72e0781b240c5ec4574510c57f0394b99
SIMPLEFS_DATA_SHA_CMD := $(SHA1SUM)

# 下载规则模板。

# 为每个外部目标生成下载、解压和校验规则。
# $(1)：目标名，例如 DOOM、QUAKE。
define download-extract-verify
$($(1)_DATA):
	$$(call prologue,$$@)
	$(Q)mkdir -p $($(1)_DATA_DEST)
	$(Q)$$(call download,$($(1)_DATA_URL))
	$(Q)$(if $(call is-git-clone,$($(1)_DATA_URL)),,\
	    $$(call extract,$($(1)_DATA_DEST),$(notdir $($(1)_DATA_URL)),$(or $($(1)_DATA_SKIP_DIR_LEVEL),0)))
	$$(call verify-sha,$($(1)_DATA_SHA_CMD),$($(1)_DATA_SHA),$($(1)_DATA))
	$(if $(call is-git-clone,$($(1)_DATA_URL)),,\
	    $$(call epilogue,$(notdir $($(1)_DATA_URL))))
endef

# 为静态外部数据生成规则；这些 URL 在解析阶段已知。
EXTERNAL_DATA_STATIC := DOOM QUAKE TIMIDITY BUILDROOT SIMPLEFS
$(foreach T,$(EXTERNAL_DATA_STATIC),$(eval $(call download-extract-verify,$(T))))

# Linux 内核使用特殊规则并延迟网络探测，避免解析阶段执行 wget。
# 只有实际构建该目标时才探测最新 patch 版本和 SHA。
# 说明：使用 awk 做可移植版本排序；sort -V 是 GNU 特性。
$(LINUX_DATA_DEST)/linux-$(LINUX_VERSION).$(LINUX_PATCHLEVEL).%.tar.gz:
	$(Q)mkdir -p $(LINUX_DATA_DEST)
	$(VECHO) "  GET\t$@\n"
	$(Q)LINUX_TARBALL=$$(wget -q -O- $(LINUX_CDN_VERSION_URL) 2>/dev/null | \
	    grep -oE 'linux-$(LINUX_VERSION)\.$(LINUX_PATCHLEVEL)\.[0-9]+\.tar\.gz' | \
	    awk -F'[.-]' '{print $$4, $$0}' | sort -rn | head -1 | awk '{print $$2}'); \
	if [ -z "$$LINUX_TARBALL" ]; then \
	    echo "错误：无法从 $(LINUX_CDN_VERSION_URL) 探测 Linux 内核 tarball"; \
	    exit 1; \
	fi; \
	LINUX_SHA=$$(wget -q -O- $(LINUX_CDN_VERSION_URL)/sha256sums.asc 2>/dev/null | \
	    grep "$$LINUX_TARBALL" | awk '{print $$1}'); \
	if [ -z "$$LINUX_SHA" ]; then \
	    echo "错误：无法获取 $$LINUX_TARBALL 的 SHA256"; \
	    exit 1; \
	fi; \
	wget -q --show-progress --continue "$(LINUX_CDN_VERSION_URL)/$$LINUX_TARBALL" && \
	tar -xf "$$LINUX_TARBALL" --strip-components=$(LINUX_DATA_SKIP_DIR_LEVEL) -C $(LINUX_DATA_DEST) && \
	if [ -z "$(LINUX_DATA_SHA_CMD)" ]; then \
	    echo "跳过 $$LINUX_TARBALL 的 SHA 校验（没有可用的 SHA 工具）"; \
	elif ! echo "$$LINUX_SHA  $$LINUX_TARBALL" | $(LINUX_DATA_SHA_CMD) -c - >/dev/null 2>&1; then \
	    echo "$$LINUX_TARBALL 的 SHA 校验失败"; exit 1; \
	fi && \
	$(RM) "$$LINUX_TARBALL"

# Demo 应用：Doom、Quake。

ifeq ($(CONFIG_SDL),y)
doom: artifact $(DOOM_DATA) $(BIN)
	(cd $(OUT); LC_ALL=C ../$(BIN) riscv32/doom)

ifeq ($(CONFIG_EXT_F),y)
quake: artifact $(QUAKE_DATA) $(BIN)
	(cd $(OUT); LC_ALL=C ../$(BIN) riscv32/quake)
endif
endif

.PHONY: doom quake

endif # _MK_EXTERNAL_INCLUDED
