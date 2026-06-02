/*
 * rv32emu 可依据 MIT 许可证自由再分发。使用和再分发规则见 LICENSE 文件。
 */

/*
 * 通用工具函数。
 *
 * 这里包含跨平台时间获取、路径规整和小型集合操作。时间函数为系统调用模拟提供
 * gettimeofday/clock_gettime 支持；路径规整用于约束客体 open 路径，避免简单的
 * `..` 逃逸；集合工具用于轻量级去重场景。
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "utils.h"

#if defined(__APPLE__)
#define HAVE_MACH_TIMER
#include <mach/mach_time.h>
#elif !defined(_WIN32) && !defined(_WIN64)
#define HAVE_POSIX_TIMER
#ifdef CLOCK_MONOTONIC
#define CLOCKID CLOCK_MONOTONIC
#else
#define CLOCKID CLOCK_REALTIME
#endif
#endif

#define MAX_PATH_LEN 1024

/* 在尽量避免溢出和精度损失的前提下计算 "x * n / d"。
 *
 * 参考：
 * https://elixir.bootlin.com/linux/v6.10.7/source/include/linux/math.h#L121
 */
#if !defined(HAVE_POSIX_TIMER)
static inline uint64_t mult_frac(uint64_t x, uint64_t n, uint64_t d)
{
    const uint64_t q = x / d;
    const uint64_t r = x % d;

    return q * n + r * n / d;
}
#endif

static void get_time_info(int32_t *tv_sec, int32_t *tv_nsec)
{
#if defined(HAVE_POSIX_TIMER)
    struct timespec t;
    clock_gettime(CLOCKID, &t);
    *tv_sec = t.tv_sec;
    *tv_nsec = t.tv_nsec;
#elif defined(HAVE_MACH_TIMER)
    static mach_timebase_info_data_t info;
    /* 首次运行时获取 Mach timebase。denom == 0 表示 info 尚未初始化。
     */
    if (info.denom == 0)
        (void) mach_timebase_info(&info);
    uint64_t nsecs = mult_frac(mach_absolute_time(), info.numer, info.denom);
    *tv_sec = nsecs / 1e9;
    *tv_nsec = nsecs - (*tv_sec * 1e9);
#else /* 低分辨率计时器。 */
    clock_t t = clock();
    *tv_sec = t / CLOCKS_PER_SEC;
    *tv_nsec = mult_frac(t % CLOCKS_PER_SEC, 1e9, CLOCKS_PER_SEC);
#endif
}

void rv_gettimeofday(struct timeval *tv)
{
    int32_t tv_sec, tv_nsec;
    get_time_info(&tv_sec, &tv_nsec);
    tv->tv_sec = tv_sec;
    tv->tv_usec = tv_nsec / 1000;
}

void rv_clock_gettime(struct timespec *tp)
{
    int32_t tv_sec, tv_nsec;
    get_time_info(&tv_sec, &tv_nsec);
    tp->tv_sec = tv_sec;
    tp->tv_nsec = tv_nsec;
}

char *sanitize_path(const char *input)
{
    size_t n = strnlen(input, MAX_PATH_LEN);

    char *ret = calloc(n + 1, sizeof(char));
    if (!ret)
        return NULL;

    /* 规整后的路径只会比原路径短，因此可以复用同一块缓冲区。
     */
    if (n == 0) {
        ret[0] = '.';
        return ret;
    }

    bool is_root = (input[0] == '/');

    /* 循环不变量：
     * - r 是下一个待读取字节的索引，对应 input[r]。
     * - w 是下一个待写入字节的索引，对应 ret[w]。
     * - dotdot 是 ".." 回退必须停止的位置，原因可能是根目录斜杠，或相对路径
     *   开头连续的 ../../.. 前缀。
     */
    size_t w = 0, r = 0;
    size_t dotdot = 0;
    if (is_root) {
        ret[w] = '/';
        w++;
        r = 1;
        dotdot = 1;
    }

    while (r < n) {
        if (input[r] == '/') {
            /* 空路径元素。 */
            r++;
        } else if (input[r] == '.' && (r + 1 == n || input[r + 1] == '/')) {
            /* "." 元素直接跳过。 */
            r++;
        } else if (input[r] == '.' && input[r + 1] == '.' &&
                   (r + 2 == n || input[r + 2] == '/')) {
            /* ".." 元素：回退到上一个斜杠。 */
            r += 2;

            if (w > dotdot) {
                /* 可以回退已有路径元素。 */
                w--;
                while (w > dotdot && ret[w] != '/') {
                    w--;
                }
            } else if (!is_root) {
                /* 相对路径无法继续回退时，保留 ".." 元素。 */
                if (w > 0) {
                    ret[w] = '/';
                    w++;
                }
                ret[w] = '.';
                w++;
                ret[w] = '.';
                w++;
                dotdot = w;
            }
        } else {
            /* 普通路径元素，必要时先补斜杠。 */
            if ((is_root && w != 1) || (!is_root && w != 0)) {
                ret[w] = '/';
                w++;
            }

            /* 复制路径元素。 */
            for (; r < n && input[r] != '/'; r++) {
                ret[w] = input[r];
                w++;
            }
        }
    }

    /* 空字符串规整为 "."。 */
    if (w == 0) {
        ret[w] = '.';
        w++;
    }

    /* w 之后的缓冲区不再使用，统一填 0，避免残留旧内容。
     */
    memset(ret + w, '\0', n + 1 - w);

    return ret;
}

HASH_FUNC_IMPL(set_hash, SET_SIZE_BITS, 1 << SET_SIZE_BITS);

void set_reset(set_t *set)
{
    memset(set, 0, sizeof(set_t));
}

/**
 * set_add - 向集合插入新元素。
 * @set: 目标集合。
 * @key: 待插入键值。
 */
bool set_add(set_t *set, rv_hash_key_t key)
{
    const rv_hash_key_t index = set_hash(key);

    uint8_t count = 0;
    for (; set->table[index][count]; count++) {
        assert(count < SET_SLOTS_SIZE);
        if (set->table[index][count] == key)
            return false;
    }

    assert(count < SET_SLOTS_SIZE);
    set->table[index][count] = key;
    return true;
}

/**
 * set_has - 检查元素是否已经存在。
 * @set: 目标集合。
 * @key: 待查询键值。
 */
bool set_has(set_t *set, rv_hash_key_t key)
{
    const rv_hash_key_t index = set_hash(key);

    for (uint8_t count = 0; set->table[index][count]; count++) {
        assert(count < SET_SLOTS_SIZE);
        if (set->table[index][count] == key)
            return true;
    }
    return false;
}
