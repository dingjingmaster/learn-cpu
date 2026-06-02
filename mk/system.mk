# 系统模拟外设构建
#
# 为运行 Linux 内核提供设备模拟、DTB 生成和系统模式内存布局。

ifndef _MK_SYSTEM_INCLUDED
_MK_SYSTEM_INCLUDED := 1

# 内存大小工具，所有运行模式共用。
MiB = 1024*1024
compute_size = $(shell echo "obase=16; ibase=10; $(1)*$(MiB)" | bc)

# 系统模式配置。

ifeq ($(CONFIG_SYSTEM),y)

CFLAGS += -Isrc/dtc/libfdt

# 把 DTC 子模块作为普通 target 管理，避免解析 Makefile 时执行 shell。
DTC_SENTINEL := src/dtc/.git
$(DTC_SENTINEL):
	$(call ensure-submodule,src/dtc,https://github.com/dgibson/dtc)

DEV_SRC := src/devices
DEV_OUT := $(OUT)/devices

DTC ?= dtc
BUILD_DTB := $(OUT)/minimal.dtb

# 编译设备树。
$(BUILD_DTB): $(DEV_SRC)/minimal.dts | $(OUT)
	$(VECHO) " DTC\t$@\n"
	$(Q)$(CC) -nostdinc -E -P -x assembler-with-cpp -undef $(CFLAGS_dt) $^ | $(DTC) - > $@

# 构建期工具需要本地编译器；emcc 会生成 wasm，不能用于这些工具。
NATIVE_CC := $(shell which gcc 2>/dev/null || which clang 2>/dev/null)

BIN_TO_C := $(OUT)/bin2c
$(BIN_TO_C): tools/bin2c.c | $(OUT)
ifeq ("$(CC_IS_EMCC)", "1")
	$(Q)$(NATIVE_CC) -Wall -o $@ $^
else
	$(Q)$(CC) -Wall -o $@ $^
endif

BUILD_DTB2C := src/minimal_dtb.h
$(BUILD_DTB2C): $(BIN_TO_C) $(BUILD_DTB)
	$(VECHO) "  BIN2C\t$@\n"
	$(Q)$(BIN_TO_C) $(BUILD_DTB) > $@

# 编译设备对象文件。
$(DEV_OUT):
	$(Q)mkdir -p $@

$(DEV_OUT)/%.o: $(DEV_SRC)/%.c | $(DEV_OUT)
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) -o $@ $(CFLAGS) $(CFLAGS_emcc) -c -MMD -MF $@.d $<

DEV_OBJS := $(patsubst $(DEV_SRC)/%.c, $(DEV_OUT)/%.o, $(wildcard $(DEV_SRC)/*.c))
# 根据配置决定是否保留 Goldfish RTC 外设。
ifneq ($(CONFIG_GOLDFISH_RTC),y)
DEV_OBJS := $(filter-out $(DEV_OUT)/rtc.o, $(DEV_OBJS))
endif
deps := $(DEV_OBJS:%.o=%.o.d)

OBJS_EXT += system.o
OBJS_EXT += dtc/libfdt/fdt.o dtc/libfdt/fdt_ro.o dtc/libfdt/fdt_rw.o dtc/libfdt/fdt_wip.o

# 编译 libfdt 对象前确保 DTC 子模块已就绪。
$(addprefix $(OUT)/,$(filter dtc/%,$(OBJS_EXT))): $(DTC_SENTINEL)

# 内存布局配置。

# 内核引导模式（ELF_LOADER=n）的内存配置。
ifneq ($(CONFIG_ELF_LOADER),y)

MEM_START ?= 0
MEM_SIZE ?= 512
DTB_SIZE ?= 1

# 如果 rootfs.cpio 已存在，则根据实际文件大小自动推导 INITRD_SIZE。
INITRD_FILE := $(OUT)/linux-image/rootfs.cpio
ifneq ($(wildcard $(INITRD_FILE)),)
    INITRD_ACTUAL_BYTES := $(shell stat -f%z $(INITRD_FILE) 2>/dev/null || stat -c%s $(INITRD_FILE) 2>/dev/null)
    ifneq ($(INITRD_ACTUAL_BYTES),)
        INITRD_SIZE ?= $(shell echo "$$(( ($(INITRD_ACTUAL_BYTES) / 1048576) + 2 ))")
    else
        INITRD_SIZE ?= 32
    endif
else
    INITRD_SIZE ?= 32
endif

REAL_MEM_SIZE = $(call compute_size, $(MEM_SIZE))
REAL_DTB_SIZE = $(call compute_size, $(DTB_SIZE))
REAL_INITRD_SIZE = $(call compute_size, $(INITRD_SIZE))

CFLAGS_dt += -DMEM_START=0x$(MEM_START) \
             -DMEM_END=0x$(shell echo "obase=16; ibase=16; $(MEM_START)+$(REAL_MEM_SIZE)" | bc) \
             -DINITRD_START=0x$(shell echo "obase=16; ibase=16; \
                              $(REAL_MEM_SIZE) - $(call compute_size, ($(INITRD_SIZE)+$(DTB_SIZE)))" | bc) \
             -DINITRD_END=0x$(shell echo "obase=16; ibase=16; \
                            $(REAL_MEM_SIZE) - $(call compute_size, $(DTB_SIZE)) - 1" | bc)

CFLAGS += -DMEM_SIZE=0x$(REAL_MEM_SIZE) -DDTB_SIZE=0x$(REAL_DTB_SIZE) -DINITRD_SIZE=0x$(REAL_INITRD_SIZE)

else
# ELF 装载模式：为用户程序提供 4GB 虚拟地址空间。
USER_MEM_SIZE ?= 4096
CFLAGS += -DMEM_SIZE=0x$(call compute_size, $(USER_MEM_SIZE))ULL
endif

# 系统模式运行目标。

LINUX_IMAGE_DIR := linux-image
system_action := ($(BIN) -k $(OUT)/$(LINUX_IMAGE_DIR)/Image -i $(OUT)/$(LINUX_IMAGE_DIR)/rootfs.cpio)
system_deps += artifact $(BUILD_DTB) $(BUILD_DTB2C) $(BIN)

system: $(system_deps)
	$(system_action)

.PHONY: system

else
# 非系统模式：为用户程序提供 4GB 虚拟地址空间。
USER_MEM_SIZE ?= 4096
CFLAGS += -DMEM_SIZE=0x$(call compute_size, $(USER_MEM_SIZE))ULL
endif

# Emscripten 的内存上限。
ifeq ("$(CC_IS_EMCC)", "1")
CFLAGS := $(filter-out -DMEM_SIZE=%,$(CFLAGS))
CFLAGS += -DMEM_SIZE=0x20000000ULL
endif

endif # _MK_SYSTEM_INCLUDED
