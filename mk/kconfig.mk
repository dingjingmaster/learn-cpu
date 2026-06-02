# Kconfig 集成
#
# 使用 kconfiglib 提供配置菜单、.config 管理和配置头文件生成。

ifndef _MK_KCONFIG_INCLUDED
_MK_KCONFIG_INCLUDED := 1

KCONFIG_DIR := tools/kconfig
KCONFIG := configs/Kconfig
CONFIG_HEADER := src/rv32emu_config.h
KCONFIGLIB_REPO := https://github.com/sysprog21/Kconfiglib

# 如果缺少 Kconfig 工具，则自动下载。
# 若上次克隆不完整，会先删除残留目录再重新克隆。
# 安全检查：rm -rf 前确认 KCONFIG_DIR 非空且位于项目相对路径内。
$(KCONFIG_DIR)/kconfiglib.py:
	$(VECHO) "正在下载 Kconfig 工具...\n"
	$(Q)if [ -z "$(KCONFIG_DIR)" ]; then \
	    echo "错误：KCONFIG_DIR 为空"; exit 1; \
	fi
	$(Q)case "$(KCONFIG_DIR)" in \
	    /*|..*) echo "错误：KCONFIG_DIR 必须是项目内的相对路径"; exit 1 ;; \
	esac
	$(Q)if [ -d "$(KCONFIG_DIR)" ] && [ ! -f "$(KCONFIG_DIR)/kconfiglib.py" ]; then \
	    echo "正在删除不完整的 Kconfig 目录..."; \
	    rm -rf "$(KCONFIG_DIR)"; \
	fi
	$(Q)git clone --depth=1 -q $(KCONFIGLIB_REPO) "$(KCONFIG_DIR)"
	@echo "Kconfig 工具已安装到 $(KCONFIG_DIR)"

# 确保所有 Kconfig 工具都已存在。
$(KCONFIG_DIR)/menuconfig.py $(KCONFIG_DIR)/defconfig.py $(KCONFIG_DIR)/genconfig.py \
$(KCONFIG_DIR)/oldconfig.py $(KCONFIG_DIR)/savedefconfig.py: $(KCONFIG_DIR)/kconfiglib.py

# 打开菜单前先输出构建环境探测结果。
env-check:
	@echo "正在检查构建环境..."
	@python3 tools/detect-env.py --summary
	@echo ""

# 交互式配置。
config: env-check $(KCONFIG_DIR)/menuconfig.py
	@python3 $(KCONFIG_DIR)/menuconfig.py $(KCONFIG)
	@python3 $(KCONFIG_DIR)/genconfig.py --header-path $(CONFIG_HEADER) $(KCONFIG)
	@echo "配置已保存到 .config 和 $(CONFIG_HEADER)"

# 应用默认配置；也支持 CONFIG=name 选择命名配置。
# tools/kconfig/defconfig.py
# .config 生成流程解析：
# 1. 若用户通过 make CONFIG=xxx 指定自定义配置：
#    - 先检查 configs/xxx_defconfig 文件是否存在
#    - 存在则调用 Kconfiglib 的 defconfig.py，传入 Kconfig 根文件和自定义defconfig，生成.project根目录的.config
#    - 不存在则抛出错误终止
# 2. 若用户未指定CONFIG，默认加载configs/defconfig，通过defconfig.py生成根目录的.config
# 3. 生成.config后，调用genconfig.py将.config转换为C代码可引用的配置头文件$(CONFIG_HEADER)，该头文件的具体路径为src/rv32emu_config.h
defconfig: $(KCONFIG_DIR)/defconfig.py
	@if [ -n "$(CONFIG)" ]; then \
	    if [ -f "configs/$(CONFIG)_defconfig" ]; then \
	        echo "正在应用 configs/$(CONFIG)_defconfig..."; \
	        python3 $(KCONFIG_DIR)/defconfig.py --kconfig $(KCONFIG) configs/$(CONFIG)_defconfig; \
	    else \
	        echo "错误：未找到 configs/$(CONFIG)_defconfig"; exit 1; \
	    fi; \
	else \
	    echo "正在应用默认配置..."; \
	    python3 $(KCONFIG_DIR)/defconfig.py --kconfig $(KCONFIG) configs/defconfig; \
	fi
	@python3 $(KCONFIG_DIR)/genconfig.py --header-path $(CONFIG_HEADER) $(KCONFIG)
	@echo "配置已应用。"

# 命名 defconfig 的模式规则，例如 make jit_defconfig。
%_defconfig: $(KCONFIG_DIR)/defconfig.py
	@if [ -f "configs/$*_defconfig" ]; then \
	    echo "正在应用 configs/$*_defconfig..."; \
	    python3 $(KCONFIG_DIR)/defconfig.py --kconfig $(KCONFIG) configs/$*_defconfig; \
	    python3 $(KCONFIG_DIR)/genconfig.py --header-path $(CONFIG_HEADER) $(KCONFIG); \
	    echo "配置已应用。"; \
	else \
	    echo "错误：未找到 configs/$*_defconfig"; exit 1; \
	fi

# Kconfig 结构变化后更新已有配置。
oldconfig: $(KCONFIG_DIR)/oldconfig.py
	@python3 $(KCONFIG_DIR)/oldconfig.py $(KCONFIG)
	@python3 $(KCONFIG_DIR)/genconfig.py --header-path $(CONFIG_HEADER) $(KCONFIG)

# 把当前配置保存为最小 defconfig。
savedefconfig: $(KCONFIG_DIR)/savedefconfig.py
	@python3 $(KCONFIG_DIR)/savedefconfig.py --kconfig $(KCONFIG) --out defconfig.new
	@echo "最小配置已保存到 defconfig.new"

# 显式下载或更新 Kconfig 工具。
kconfig-tools: $(KCONFIG_DIR)/kconfiglib.py
	@echo "Kconfig 工具已就绪：$(KCONFIG_DIR)"

# 缺少 .config 时生成默认配置；使用 order-only 以保留用户自定义。
.config: | $(KCONFIG_DIR)/defconfig.py
	@python3 $(KCONFIG_DIR)/defconfig.py --kconfig $(KCONFIG) configs/defconfig
	@echo "已根据默认配置生成 .config"

# 根据 .config 自动生成 C 配置头文件。
$(CONFIG_HEADER): .config $(KCONFIG_DIR)/genconfig.py
	@python3 $(KCONFIG_DIR)/genconfig.py --header-path $(CONFIG_HEADER) $(KCONFIG)
	@echo "已生成 $(CONFIG_HEADER)"

.PHONY: config defconfig oldconfig savedefconfig env-check kconfig-tools

endif # _MK_KCONFIG_INCLUDED
