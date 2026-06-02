# rv32emu 组件单元测试
#
# 使用 mk/common.mk 中定义的 test-framework 模板。

ifndef _MK_TESTS_INCLUDED
_MK_TESTS_INCLUDED := 1

# 使用模板定义测试。

# cache 测试：验证 LFU 缓存实现。
# LFU 输出需要额外子目录。
$(eval $(call test-framework,cache,test-cache.o,$(OUT)/cache.o $(OUT)/mpool.o,$(OUT)/cache/lfu))

# map 测试：验证红黑树映射容器实现。
$(eval $(call test-framework,map,test-map.o mt19937.o,$(OUT)/map.o,))

# path 测试：验证路径工具函数。
$(eval $(call test-framework,path,test-path.o,$(OUT)/utils.o,))

# 测试运行器。

# cache 测试使用文件比较：输入 -> 输出 -> 与 expect 比较。
$(eval $(call run-test-compare,cache,cache-new cache-put cache-get cache-replace))

# map 和 path 测试只检查退出码。
$(eval $(call run-test-simple,map))
$(eval $(call run-test-simple,path))

# 主测试目标。

tests: run-test-cache run-test-map run-test-path

# 集成测试：用模拟器运行测试程序。

LOG_FILTER := sed -E '/^[0-9]{2}:[0-9]{2}:[0-9]{2} /d'

# check-test(flags, binary, name, filter, expected)
define check-test
$(Q)true; \
$(PRINTF) "正在运行 $(3) ... "; \
OUTPUT_FILE="$$(mktemp)"; \
trap '$(RM) "$$OUTPUT_FILE"' 0; \
if (LC_ALL=C $(BIN) $(1) $(2) > "$$OUTPUT_FILE") && \
   [ "$$(cat "$$OUTPUT_FILE" | $(LOG_FILTER) | $(4))" = "$(5)" ]; then \
    $(call notice, [OK]); \
else \
    $(PRINTF) "失败。\n"; \
    exit 1; \
fi
endef

# check 测试定义。
CHECK_ELF_FILES :=
ifeq ($(CONFIG_EXT_M),y)
CHECK_ELF_FILES += puzzle fcalc pi
endif

EXPECTED_hello = Hello World!
EXPECTED_puzzle = success in 2005 trials
EXPECTED_fcalc = Performed 12 tests, 0 failures, 100% success rate.
EXPECTED_pi = 3.141592653589793238462643383279502884197169399375105820974944592307816406286208998628034825342117067982148086

check-hello: $(BIN)
	$(call check-test, , $(OUT)/hello.elf, hello.elf, uniq,$(EXPECTED_hello))

# 为每个 ELF 生成独立 check 目标，支持 make -j 并行。
define make-check-target
check-$(1): $(BIN) artifact
	$$(call check-test, , $$(OUT)/riscv32/$(1), $(1), uniq,$$(EXPECTED_$(1)))
endef
$(foreach e,$(CHECK_ELF_FILES),$(eval $(call make-check-target,$(e))))

CHECK_TARGETS := check-hello $(addprefix check-,$(CHECK_ELF_FILES))
check: $(CHECK_TARGETS)

# 系统相关测试。
EXPECTED_aes_sha1 = 89169ec034bec1c6bb2c556b26728a736d350ca3  -
misalign: $(BIN) artifact
	$(call check-test, -m, $(OUT)/riscv32/uaes, uaes.elf, $(SHA1SUM),$(EXPECTED_aes_sha1))

EXPECTED_misalign = MISALIGNED INSTRUCTION FETCH TEST PASSED!
misalign-in-blk-emu: $(BIN)
	$(call check-test, , tests/system/alignment/misalign.elf, misalign.elf, tail -n 1,$(EXPECTED_misalign))

EXPECTED_mmu = Store page fault test passed!
mmu-test: $(BIN)
	$(call check-test, , tests/system/mmu/vm.elf, vm.elf, tail -n 1,$(EXPECTED_mmu))

.PHONY: tests run-test-cache run-test-map run-test-path
.PHONY: check $(CHECK_TARGETS) misalign misalign-in-blk-emu mmu-test

endif # _MK_TESTS_INCLUDED
