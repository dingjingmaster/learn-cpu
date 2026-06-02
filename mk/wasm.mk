# WebAssembly 构建配置
#
# 提供 Emscripten 专用编译参数和运行目标。

ifndef _MK_WASM_INCLUDED
_MK_WASM_INCLUDED := 1

CFLAGS_emcc ?=
deps_emcc :=
ASSETS := assets/wasm
WEB_HTML_RESOURCES := $(ASSETS)/html
WEB_JS_RESOURCES := $(ASSETS)/js
EXPORTED_FUNCS := _main,_indirect_rv_halt,_get_input_buf,_get_input_buf_cap,_set_input_buf_size
DEMO_DIR := demo

WEB_FILES := $(BIN).js \
             $(BIN).wasm \
             $(BIN).worker.js \
             $(OUT)/elf_list.js

# 仅在使用 emcc 时配置 Emscripten 相关选项。
ifeq ("$(CC_IS_EMCC)", "1")

BIN := $(BIN).js

# 尾调用优化；解释器调度依赖该能力。
CFLAGS += -mtail-call

# Emscripten 的 SDL 配置。
ifeq ($(CONFIG_SDL),y)
# 关闭 STRICT，避免 SDL2_mixer 端口编译时被 -Werror 中断。
CFLAGS_emcc += -sSTRICT=0 -sUSE_SDL=2 -sSDL2_MIXER_FORMATS=wav,mid -sUSE_SDL_MIXER=2
OBJS_EXT += syscall_sdl.o
LDFLAGS += -pthread
endif

# Emscripten 构建参数。
CFLAGS_emcc += -sINITIAL_MEMORY=2GB \
               -sALLOW_MEMORY_GROWTH \
               -s"EXPORTED_FUNCTIONS=$(EXPORTED_FUNCS)" \
               -sSTACK_SIZE=4MB \
               -sPTHREAD_POOL_SIZE=navigator.hardwareConcurrency \
               --embed-file build/timidity@/etc/timidity \
               -DMEM_SIZE=0x20000000 \
               -DCYCLE_PER_STEP=2000000 \
               -O3 \
               -w

# 系统模式资源。
ifeq ($(CONFIG_SYSTEM),y)
CFLAGS_emcc += --embed-file build/linux-image/Image@Image \
               --embed-file build/linux-image/rootfs.cpio@rootfs.cpio \
               --embed-file build/minimal.dtb@/minimal.dtb \
               --pre-js $(WEB_JS_RESOURCES)/system-pre.js
else
CFLAGS_emcc += --embed-file build/jit-bf.elf@/jit-bf.elf \
               --embed-file build/coro.elf@/coro.elf \
               --embed-file build/fibonacci.elf@/fibonacci.elf \
               --embed-file build/hello.elf@/hello.elf \
               --embed-file build/ieee754.elf@/ieee754.elf \
               --embed-file build/perfcount.elf@/perfcount.elf \
               --embed-file build/readelf.elf@/readelf.elf \
               --embed-file build/smolnes.elf@/smolnes.elf \
               --embed-file build/riscv32@/riscv32 \
               --embed-file build/DOOM1.WAD@/DOOM1.WAD \
               --embed-file build/id1/pak0.pak@/id1/pak0.pak \
               --pre-js $(WEB_JS_RESOURCES)/user-pre.js
endif

# mimalloc 支持探测。
MIMALLOC_SUPPORT_SINCE_MAJOR := 3
MIMALLOC_SUPPORT_SINCE_MINOR := 1
MIMALLOC_SUPPORT_SINCE_PATCH := 50
ifeq ($(call version_gte,$(EMCC_MAJOR),$(EMCC_MINOR),$(EMCC_PATCH),$(MIMALLOC_SUPPORT_SINCE_MAJOR),$(MIMALLOC_SUPPORT_SINCE_MINOR),$(MIMALLOC_SUPPORT_SINCE_PATCH)), 1)
    CFLAGS_emcc += -sMALLOC=mimalloc
else
    $(warning mimalloc 需要 Emscripten $(MIMALLOC_SUPPORT_SINCE_MAJOR).$(MIMALLOC_SUPPORT_SINCE_MINOR).$(MIMALLOC_SUPPORT_SINCE_PATCH)+)
endif

# ELF 列表生成器。
$(OUT)/elf_list.js: artifact tools/gen-elf-list-js.py
	$(Q)tools/gen-elf-list-js.py > $@

# WASM 构建依赖。
# 系统模式只需要 artifact（linux-image）和 timidity（音频）。
# 用户模式还需要 elf_list.js 供 demo 选择，并嵌入游戏数据。
ifeq ($(CONFIG_SYSTEM),y)
deps_emcc += artifact $(TIMIDITY_DATA)
else
deps_emcc += artifact $(OUT)/elf_list.js $(DOOM_DATA) $(QUAKE_DATA) $(TIMIDITY_DATA)
endif

# 浏览器 TCO 支持探测。

CHROME_SUPPORT_TCO_AT_MAJOR := 112
FIREFOX_SUPPORT_TCO_AT_MAJOR := 121
SAFARI_SUPPORT_TCO_AT_MAJOR := 18
SAFARI_SUPPORT_TCO_AT_MINOR := 2

# 浏览器探测（平台相关）。
ifeq ($(UNAME_S),Darwin)
    CHROME_MAJOR := $(shell "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --version 2>/dev/null | awk '{print $$3}' | cut -f1 -d.)
    FIREFOX_MAJOR := $(shell /Applications/Firefox.app/Contents/MacOS/firefox --version 2>/dev/null | awk '{print $$3}' | cut -f1 -d.)
    SAFARI_VERSION := $(shell mdls -name kMDItemVersion /Applications/Safari.app 2>/dev/null | sed 's/"//g' | awk '{print $$3}')
else ifeq ($(UNAME_S),Linux)
    CHROME_MAJOR := $(shell google-chrome --version 2>/dev/null | awk '{print $$3}' | cut -f1 -d.)
    FIREFOX_MAJOR := $(shell firefox -v 2>/dev/null | awk '{print $$3}' | cut -f1 -d.)
endif

# 浏览器支持提示。
ifneq ($(CHROME_MAJOR),)
ifeq ($(call version_gte,$(CHROME_MAJOR),,,$(CHROME_SUPPORT_TCO_AT_MAJOR),,), 1)
    $(info $(call noticex, Chrome $(CHROME_MAJOR) 支持 TCO))
else
    $(warning Chrome $(CHROME_MAJOR) 不支持 TCO（需要 $(CHROME_SUPPORT_TCO_AT_MAJOR)+）)
endif
endif

ifneq ($(FIREFOX_MAJOR),)
ifeq ($(call version_gte,$(FIREFOX_MAJOR),,,$(FIREFOX_SUPPORT_TCO_AT_MAJOR),,), 1)
    $(info $(call noticex, Firefox $(FIREFOX_MAJOR) 支持 TCO))
else
    $(warning Firefox $(FIREFOX_MAJOR) 不支持 TCO（需要 $(FIREFOX_SUPPORT_TCO_AT_MAJOR)+）)
endif
endif

# Web demo 服务器。

DEMO_IP := 127.0.0.1
DEMO_PORT := 8000

check-demo-dir-exist:
	$(Q)if [ ! -d "$(DEMO_DIR)" ]; then mkdir -p "$(DEMO_DIR)"; fi

define cp-web-file
    $(Q)cp $(1) $(DEMO_DIR)
    $(info)
endef

STATIC_WEB_FILES := $(WEB_JS_RESOURCES)/coi-serviceworker.min.js
ifeq ($(CONFIG_SYSTEM),y)
STATIC_WEB_FILES += $(WEB_HTML_RESOURCES)/system.html
else
STATIC_WEB_FILES += $(WEB_HTML_RESOURCES)/user.html
endif

start_web_deps := check-demo-dir-exist $(BIN)
ifeq ($(CONFIG_SYSTEM),y)
start_web_deps += $(BUILD_DTB) $(BUILD_DTB2C)
endif

start-web: $(start_web_deps)
	$(Q)rm -f $(DEMO_DIR)/*.html
	$(foreach T, $(WEB_FILES), $(call cp-web-file, $(T)))
	$(foreach T, $(STATIC_WEB_FILES), $(call cp-web-file, $(T)))
	$(Q)mv $(DEMO_DIR)/*.html $(DEMO_DIR)/index.html
	$(Q)python3 -m http.server --bind $(DEMO_IP) $(DEMO_PORT) --directory $(DEMO_DIR)

.PHONY: check-demo-dir-exist start-web

endif # CC_IS_EMCC

endif # _MK_WASM_INCLUDED
