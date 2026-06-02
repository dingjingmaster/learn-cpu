/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/*
 * WebAssembly 运行时桥接。
 *
 * 该文件只在 Emscripten 构建中生效，用 EM_JS 暴露浏览器侧 DOM 操作，例如
 * 启用/禁用运行按钮、连接 xterm.js 输入缓冲区，以及在浏览器事件中停止 CPU
 * 主循环。普通本地构建不会编译这些函数。
 */

#include "em_runtime.h"

#if defined(__EMSCRIPTEN__)
#if RV32_HAS(SYSTEM)
EM_JS(void, enable_run_button, (), {
    document.getElementById('runSysButton').disabled = false;
});
EM_JS(void, disable_run_button, (), {
    document.getElementById('runSysButton').disabled = true;
});
#else
EM_JS(void, enable_run_button, (), {
    document.getElementById('runButton').disabled = false;
});
EM_JS(void, disable_run_button, (), {
    document.getElementById('runButton').disabled = true;
});
#endif

#if RV32_HAS(SYSTEM_MMIO)
extern uint8_t input_buf_size;

char *get_input_buf();
uint8_t get_input_buf_cap();
void set_input_buf_size(uint8_t size);
#endif
#endif
