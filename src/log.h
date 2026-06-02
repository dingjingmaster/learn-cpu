/*
 * Copyright (c) 2020 rxi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/*
 * 轻量日志接口。
 *
 * 对外提供日志级别、锁回调、静默模式、stdout 重定向和 log_impl 输出入口。
 * rv_log_* 宏会在调用处记录文件名与行号，便于定位模拟器运行时错误。
 */

#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

typedef struct {
    va_list ap;
    const char *fmt;
    const char *file;
    struct tm *time;
    void *udata;
    int line;
    int level;
} log_event_t;

typedef void (*log_func_t)(log_event_t *ev);
typedef void (*log_lock_func_t)(bool lock, void *udata);

enum LOG_LEVEL {
    LOG_TRACE = 0,
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL,
};

/* 最低级别日志。 */
#define rv_log_trace(...) log_impl(LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define rv_log_debug(...) log_impl(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define rv_log_info(...) log_impl(LOG_INFO, __FILE__, __LINE__, __VA_ARGS__)
#define rv_log_warn(...) log_impl(LOG_WARN, __FILE__, __LINE__, __VA_ARGS__)
#define rv_log_error(...) log_impl(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define rv_log_fatal(...) log_impl(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)
/* 最高级别日志。 */

#define rv_log_level_string(...) log_level_string(__VA_ARGS__)
#define rv_log_set_lock(...) log_set_lock(__VA_ARGS__)
#define rv_log_set_level(...) log_set_level(__VA_ARGS__)
#define rv_log_set_quiet(...) log_set_quiet(__VA_ARGS__)
/*
 * 默认情况下日志写入 stdout。但 rv_remap_stdstream() 可能会把 stdout 重定向到
 * 文件等其他目标，因此需要在 rv_remap_stdstream() 内调用
 * rv_log_set_stdout_stream()，同步日志模块看到的输出流。
 */
#define rv_log_set_stdout_stream(...) log_set_stdout_stream(__VA_ARGS__)

const char *log_level_string(int level);
void log_set_lock(log_lock_func_t fn, void *udata);
void log_set_level(int level);
void log_set_quiet(bool enable);
void log_set_stdout_stream(FILE *stream);

void log_impl(int level, const char *file, int line, const char *fmt, ...);
