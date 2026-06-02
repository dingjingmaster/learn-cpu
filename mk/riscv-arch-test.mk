# RISC-V 架构合规测试
#
# 使用 riscof 框架运行官方 RISC-V 架构测试。

ifndef _MK_RISCV_ARCH_TEST_INCLUDED
_MK_RISCV_ARCH_TEST_INCLUDED := 1

riscof-check:
	$(Q)if [ "$(shell pip show riscof 2>&1 | head -n 1 | cut -d' ' -f1)" = "WARNING:" ]; then \
	    $(PRINTF) "请运行 'pip3 install -r requirements.txt' 安装依赖。\n"; \
	    exit 1; \
	fi

ARCH_TEST_DIR ?= tests/riscv-arch-test
ARCH_TEST_SUITE ?= $(ARCH_TEST_DIR)/riscv-test-suite
export RISCV_TARGET := tests/arch-test-target
# 使用 GNU Make 内置 CURDIR，避免额外调用 shell 执行 pwd。
export TARGETDIR := $(CURDIR)
export RISCV_DEVICE ?= IMACFZicsrZifencei
# 使用带设备名的工作目录，便于并行运行不同架构测试。
export WORK := $(TARGETDIR)/build/arch-test-$(RISCV_DEVICE)

# SKIP_PREREQ=1 会跳过 artifact/git/copy 步骤，适合预取完成后的并行执行。
SKIP_PREREQ ?= 0

ifeq ($(RISCV_DEVICE),FCZicsr)
ARCH_TEST_SUITE := tests/rv32fc-test-suite
endif

# arch-test 前置依赖；SKIP_PREREQ=1 时为支持并行执行而跳过。
# SKIP_PREREQ=1 时不依赖 $(BIN)，避免并行 make 的竞争。
# 二进制必须已经构建完成，recipe 中会再次校验。
ifeq ($(SKIP_PREREQ),1)
ARCH_TEST_DEPS := riscof-check
else
ARCH_TEST_DEPS := riscof-check $(BIN) artifact
endif

arch-test: $(ARCH_TEST_DEPS)
ifeq ($(CROSS_COMPILE),)
	$(error 构建架构测试需要 GNU RISC-V 工具链。请检查工具链是否已安装。)
endif
ifeq ($(SKIP_PREREQ),1)
	$(Q)test -x $(BIN) || \
	    { echo "错误：SKIP_PREREQ=1 需要预先构建模拟器。请先运行 'make'。"; exit 1; }
	$(Q)test -x tests/arch-test-target/sail_cSim/riscv_sim_RV32 || \
	    { echo "错误：SKIP_PREREQ=1 需要预先获取 sail 二进制。请先运行 'make artifact'。"; exit 1; }
	$(Q)test -d $(ARCH_TEST_DIR) || \
	    { echo "错误：SKIP_PREREQ=1 需要测试子模块。请先运行 'git submodule update --init tests/riscv-arch-test/'。"; exit 1; }
else
	$(call ensure-submodule,tests/riscv-arch-test,https://github.com/riscv-non-isa/riscv-arch-test)
	$(Q)cp $(OUT)/rv32emu-prebuilt-sail-$(HOST_PLATFORM) tests/arch-test-target/sail_cSim/riscv_sim_RV32
	$(Q)chmod +x tests/arch-test-target/sail_cSim/riscv_sim_RV32
endif
	$(Q)python3 -B $(RISCV_TARGET)/setup.py --riscv_device=$(RISCV_DEVICE) --hw_data_misaligned_support=$(hw_data_misaligned_support) --work_dir=$(WORK)
	$(Q)grep -q '^\[RISCOF\]' $(WORK)/config.ini || \
	    { echo "错误：config.ini 缺少 RISCOF 节，内容如下："; cat $(WORK)/config.ini; exit 1; }
	$(Q)riscof run --no-clean --work-dir=$(WORK) \
	        --config=$(WORK)/config.ini \
	        --suite=$(ARCH_TEST_SUITE) \
	        --env=$(ARCH_TEST_DIR)/riscv-test-suite/env

.PHONY: riscof-check arch-test

endif # _MK_RISCV_ARCH_TEST_INCLUDED
