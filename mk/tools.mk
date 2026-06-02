# 开发工具：直方图生成器、Linux 镜像构建器和格式化入口。

ifndef _MK_TOOLS_INCLUDED
_MK_TOOLS_INCLUDED := 1

HIST_BIN := $(OUT)/rv_histogram

# 在 macOS 上，gcc-14 需要从 emulate.o、syscall.o、syscall_sdl.o、
# io.o 和 log.o 链接符号。
# 这些符号并不会被 rv_histogram 真正使用，只是为了让链接通过。
#
# riscv.o 和 map.o 是 elf.o 的依赖，不是 rv_histogram 的直接依赖；
# 但为了通过链接，这里也需要把它们加入对象列表。
HIST_OBJS := \
	riscv.o \
	utils.o \
	map.o \
	elf.o \
	decode.o \
	mpool.o \
	utils.o \
	emulate.o \
	syscall.o \
	syscall_sdl.o \
	io.o \
	log.o \
	rv_histogram.o

HIST_OBJS := $(addprefix $(OUT)/, $(HIST_OBJS))
deps += $(HIST_OBJS:%.o=%.o.d)

$(OUT)/%.o: tools/%.c | $(OUT)
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) -o $@ $(CFLAGS) -Wno-missing-field-initializers -Isrc -c -MMD -MF $@.d $<

# 编译直方图工具时关闭 GDBSTUB，避免引入 mini-gdb。
$(HIST_BIN): $(HIST_OBJS)
	$(VECHO) "  LD\t$@\n"
	$(Q)$(CC) -o $@ -D RV32_FEATURE_GDBSTUB=0 $^ $(LDFLAGS)

TOOLS_BIN += $(HIST_BIN)

# 构建 Linux 镜像。
LINUX_IMAGE_SRC = $(BUILDROOT_DATA) $(LINUX_DATA) $(SIMPLEFS_DATA)
build-linux-image: $(LINUX_IMAGE_SRC)
	$(Q)./tools/build-linux-image.sh
	$(Q)$(PRINTF) "构建完成。\n"

# 代码格式化；工具探测延后到 recipe 中执行。
# 使用 find -print0 | xargs -0，安全处理包含特殊字符的路径。
format:
	$(Q)CLANG_FORMAT=$$(which clang-format-20 2>/dev/null); \
	SHFMT=$$(which shfmt 2>/dev/null); \
	DTSFMT=$$(which dtsfmt 2>/dev/null); \
	BLACK=$$(which black 2>/dev/null); \
	if [ -z "$$CLANG_FORMAT" ]; then echo "未找到 clang-format-20。"; exit 1; fi && \
	if [ -z "$$SHFMT" ]; then echo "未找到 shfmt。"; exit 1; fi && \
	if [ -z "$$DTSFMT" ]; then echo "未找到 dtsfmt。"; exit 1; fi && \
	if [ -z "$$BLACK" ]; then echo "未找到 black。"; exit 1; fi && \
	SUBMODULES=$$(git config --file .gitmodules --get-regexp path 2>/dev/null | awk '{ print $$2 }') && \
	PRUNE_PATHS="./$(OUT)" && \
	for subm in $$SUBMODULES; do PRUNE_PATHS="$$PRUNE_PATHS ./$$subm"; done && \
	PRUNE_ARGS=$$(echo "$$PRUNE_PATHS" | tr ' ' '\n' | sed 's/^/-path /;s/$$/ -o/' | tr '\n' ' ' | sed 's/ -o $$//') && \
	find . \( $$PRUNE_ARGS \) -prune -o -name '*.[ch]' -print0 | xargs -0 $$CLANG_FORMAT -i && \
	find . \( $$PRUNE_ARGS \) -prune -o -name '*.sh' -print0 | xargs -0 $$SHFMT -w && \
	find . \( $$PRUNE_ARGS \) -prune -o \( -name '*.dts' -o -name '*.dtsi' \) -print0 | xargs -0 -I{} $$DTSFMT {} && \
	find . \( $$PRUNE_ARGS \) -prune -o \( -name '*.py' -o -name '*.pyi' \) -print0 | xargs -0 $$BLACK --quiet
	$(Q)$(call notice, 所有文件已格式化。)

.PHONY: build-linux-image format

endif # _MK_TOOLS_INCLUDED
