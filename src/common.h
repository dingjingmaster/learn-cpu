/*
 * rv32emu 可依据 MIT 许可证自由再分发。使用和再分发规则见 LICENSE 文件。
 */

/*
 * 全局公共定义。
 *
 * 该头文件会通过 Makefile 的 -include 自动注入多数编译单元，集中放置属性宏、
 * 条件编译辅助、数组工具、断言和跨平台小工具。不要在这里加入会引入重依赖的
 * 业务接口，否则会放大所有源码文件的编译耦合。
 */

#pragma once

#include <assert.h>
#include <stdint.h>

#include "feature.h"
#include "log.h"

#if defined(__GNUC__) || defined(__clang__)
#define UNUSED __attribute__((unused))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define FORCE_INLINE static inline __attribute__((always_inline))
#else
#define UNUSED
#define likely(x) (x)
#define unlikely(x) (x)
#if defined(_MSC_VER)
#define FORCE_INLINE static inline __forceinline
#else
#define FORCE_INLINE static inline
#endif
#endif

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof(arr[0]))

#define MASK(n) ((n) >= 64 ? ~0ULL : ~(~0ULL << (n)))

#if defined(_MSC_VER)
#include <intrin.h>
static inline int rv_clz(uint32_t v)
{
    /* 输入 0 属于未定义行为。 */
    assert(v);

    uint32_t leading_zero = 0;
    _BitScanReverse(&leading_zero, v);
    return 31 - leading_zero;
}
#elif defined(__GNUC__) || defined(__clang__)
static inline int rv_clz(uint32_t v)
{
    /* https://gcc.gnu.org/onlinedocs/gcc/Other-Builtins.html */
    /* 输入 0 属于未定义行为。 */
    assert(v);

    return __builtin_clz(v);
}
#else /* 通用实现。 */
static inline int rv_clz(uint32_t v)
{
    /* 输入 0 属于未定义行为。 */
    assert(v);

    /* http://graphics.stanford.edu/~seander/bithacks.html#IntegerLogDeBruijn */
    static const uint8_t mul_debruijn[] = {
        0, 9,  1,  10, 13, 21, 2,  29, 11, 14, 16, 18, 22, 25, 3, 30,
        8, 12, 20, 28, 15, 17, 24, 7,  19, 27, 23, 6,  26, 5,  4, 31,
    };

    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;

    return mul_debruijn[(uint32_t) (v * 0x07C4ACDDU) >> 27];
}
#endif

#if defined(_MSC_VER)
#include <intrin.h>
static inline int rv_ctz(uint32_t v)
{
    /* 输入 0 属于未定义行为。 */
    assert(v);

    uint32_t trailing_zero = 0;
    _BitScanForward(&trailing_zero, v);
    return trailing_zero;
}
#elif defined(__GNUC__) || defined(__clang__)
static inline int rv_ctz(uint32_t v)
{
    /* https://gcc.gnu.org/onlinedocs/gcc/Other-Builtins.html */
    /* 输入 0 属于未定义行为。 */
    assert(v);

    return __builtin_ctz(v);
}
#else /* 通用实现。 */
static inline int rv_ctz(uint32_t v)
{
    /* 输入 0 属于未定义行为。 */
    assert(v);

    /* https://graphics.stanford.edu/~seander/bithacks.html#ZerosOnRightMultLookup
     */

    static const int mul_debruijn[32] = {
        0,  1,  28, 2,  29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4,  8,
        31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6,  11, 5,  10, 9};

    return mul_debruijn[((uint32_t) ((v & -v) * 0x077CB531U)) >> 27];
}
#endif

#if defined(__GNUC__) || defined(__clang__)
static inline int rv_popcount(uint32_t v)
{
    /* https://gcc.gnu.org/onlinedocs/gcc/Other-Builtins.html */

    return __builtin_popcount(v);
}
#else /* 通用实现。 */
static inline int rv_popcount(uint32_t v)
{
    /* https://graphics.stanford.edu/~seander/bithacks.html#CountBitsSetParallel
     */

    v -= (v >> 1) & 0x55555555;
    v = (v & 0x33333333) + ((v >> 2) & 0x33333333);
    v = (v + (v >> 4)) & 0x0f0f0f0f;
    return (v * 0x01010101) >> 24;
}
#endif

/*
 * 以 2 为底的整数对数。
 *
 * 输入 x 不能为 0，否则部分平台上的结果未定义。
 *
 */
static inline uint8_t ilog2(uint32_t x)
{
    return 31 - rv_clz(x);
}

/* 对齐属性宏。 */
#if defined(__GNUC__) || defined(__clang__)
#define __ALIGNED(x) __attribute__((aligned(x)))
#elif defined(_MSC_VER)
#define __ALIGNED(x) __declspec(align(x))
#else /* 不支持的编译器。 */
#define __ALIGNED(x)
#endif

/* 紧凑布局属性宏。 */
#if defined(__GNUC__) || defined(__clang__)
#define PACKED(name) name __attribute__((packed))
#elif defined(_MSC_VER)
#define PACKED(name) __pragma(pack(push, 1)) name __pragma(pack(pop))
#else /* 不支持的编译器。 */
#define PACKED(name)
#endif

/* 字节序转换。 */
#if defined(__GNUC__) || defined(__clang__)
#define bswap16(x) __builtin_bswap16(x)
#define bswap32(x) __builtin_bswap32(x)
#else
#define bswap16(x) ((x & 0xff) << 8) | ((x >> 8) & 0xff)
#define bswap32(x)                                                    \
    (bswap16(((x & 0xffff) << 16) | ((x >> 16) & 0xffff)) & 0xffff) | \
        (bswap16(((x & 0xffff) << 16) | ((x >> 16) & 0xffff)) & 0xffff) << 16
#endif

/* __builtin_unreachable() 用来辅助编译器：
 * - 消除程序员已知永远不会执行的死代码。
 * - 告诉编译器该路径是 cold 路径，帮助线性化代码；调用 noreturn 函数也能达到
 *   类似效果。
 */
#if defined(__GNUC__) || defined(__clang__)
#define __UNREACHABLE __builtin_unreachable()
#elif defined(_MSC_VER)
#define __UNREACHABLE __assume(false)
#else /* 不支持的编译器。 */
/* clang-format off */
#define __UNREACHABLE do { /* nop */ } while (0)
/* clang-format on */
#endif

/* 非优化构建通常没有尾调用优化（TCO）。使用编译器属性 musttail 可以在未启用
 * 优化时强制进行 TCO。
 */
#if defined(__has_attribute) && __has_attribute(musttail)
#define MUST_TAIL __attribute__((musttail))
#else
#define MUST_TAIL
#endif

/* preserve_none 调用约定可以降低解释器 threaded dispatch 中的寄存器保存开销。
 * 该属性必须用于函数声明，不能用于 return 语句。macOS clang 会误报支持。
 */
#if defined(__has_attribute) && __has_attribute(preserve_none) && \
    !defined(__APPLE__)
#define PRESERVE_NONE __attribute__((preserve_none))
#else
#define PRESERVE_NONE
#endif

/* 关闭 UBSAN 函数指针类型检查。
 * 非 LLVM 代码通过函数指针调用 LLVM 编译出的 T2C 代码时，函数类型元数据可能不
 * 匹配，UBSAN 会产生误报。该属性用于抑制这类检查。
 *
 * 注意：GCC 支持部分 no_sanitize 变体，但不支持 "function"。GCC 上
 * __has_attribute(no_sanitize) 可能返回 true，但应用 no_sanitize("function")
 * 会产生警告/错误。因此这里只显式检查支持该变体的 __clang__。
 */
#if defined(__clang__) && defined(__has_attribute) && \
    __has_attribute(no_sanitize)
#define DISABLE_UBSAN_FUNC __attribute__((no_sanitize("function")))
#else
#define DISABLE_UBSAN_FUNC
#endif

/* 假设所有 POSIX 兼容环境都提供 mmap 系统调用。Emscripten 被排除，因为它缺少
 * 基于 signal 的按需分页支持，且 C11 原子操作需要默认未启用的特殊编译参数。
 */
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
#define HAVE_MMAP 0
#else
/* 假设为 POSIX 兼容运行时。 */
#define HAVE_MMAP 1
#endif

/* 可移植原子操作。
 *
 * 优先使用 GNU __atomic builtins（GCC 4.7+、Clang 3.1+），因为它们可用于
 * 非 _Atomic 类型，适配当前已有结构体定义。C11 stdatomic 要求所有原子变量都带
 * _Atomic 类型限定。
 *
 * 内存序常量：
 *   ATOMIC_RELAXED - 不提供同步，只保证原子性。
 *   ATOMIC_ACQUIRE - 防止后续读操作重排到其前面。
 *   ATOMIC_RELEASE - 防止前序写操作重排到其后面。
 *   ATOMIC_SEQ_CST - 完整顺序一致性，最强内存序。
 */
#if defined(__GNUC__) || defined(__clang__)
/* GNU __atomic builtins（GCC 4.7+、Clang 3.1+）。 */
#define HAVE_C11_ATOMICS 0

#define ATOMIC_RELAXED __ATOMIC_RELAXED
#define ATOMIC_ACQUIRE __ATOMIC_ACQUIRE
#define ATOMIC_RELEASE __ATOMIC_RELEASE
#define ATOMIC_SEQ_CST __ATOMIC_SEQ_CST

#define ATOMIC_LOAD(ptr, order) __atomic_load_n(ptr, order)
#define ATOMIC_STORE(ptr, val, order) __atomic_store_n(ptr, val, order)
#define ATOMIC_FETCH_ADD(ptr, val, order) __atomic_fetch_add(ptr, val, order)
#define ATOMIC_FETCH_SUB(ptr, val, order) __atomic_fetch_sub(ptr, val, order)
#define ATOMIC_EXCHANGE(ptr, val, order) __atomic_exchange_n(ptr, val, order)
#define ATOMIC_COMPARE_EXCHANGE_WEAK(ptr, expected, desired, succ, fail) \
    __atomic_compare_exchange_n(ptr, expected, desired, 1, succ, fail)

#elif !defined(__EMSCRIPTEN__) && defined(__STDC_VERSION__) && \
    (__STDC_VERSION__ >= 201112L) && !defined(__STDC_NO_ATOMICS__)
/* C11 原子操作回退实现，需要 GNU __typeof__ 扩展做类型推导。
 * 注意：按 C11 标准，强转为 (_Atomic T*) 严格说是未定义行为；但这是常见惯用法，
 * 在 GCC/Clang 上能正确工作。
 */
#include <stdatomic.h>
#define HAVE_C11_ATOMICS 1

#define ATOMIC_RELAXED memory_order_relaxed
#define ATOMIC_ACQUIRE memory_order_acquire
#define ATOMIC_RELEASE memory_order_release
#define ATOMIC_SEQ_CST memory_order_seq_cst

#define ATOMIC_LOAD(ptr, order) \
    atomic_load_explicit((_Atomic __typeof__(*(ptr)) *) (ptr), order)
#define ATOMIC_STORE(ptr, val, order) \
    atomic_store_explicit((_Atomic __typeof__(*(ptr)) *) (ptr), val, order)
#define ATOMIC_FETCH_ADD(ptr, val, order) \
    atomic_fetch_add_explicit((_Atomic __typeof__(*(ptr)) *) (ptr), val, order)
#define ATOMIC_FETCH_SUB(ptr, val, order) \
    atomic_fetch_sub_explicit((_Atomic __typeof__(*(ptr)) *) (ptr), val, order)
#define ATOMIC_EXCHANGE(ptr, val, order) \
    atomic_exchange_explicit((_Atomic __typeof__(*(ptr)) *) (ptr), val, order)
#define ATOMIC_COMPARE_EXCHANGE_WEAK(ptr, expected, desired, succ, fail) \
    atomic_compare_exchange_weak_explicit(                               \
        (_Atomic __typeof__(*(ptr)) *) (ptr), expected, desired, succ, fail)

#else
/* 无原子操作支持：退回单线程实现（T2C 需要原子操作）。 */
#define HAVE_C11_ATOMICS 0

#if defined(_MSC_VER)
#pragma message("没有可用的原子操作。T2C JIT 将被关闭。")
#else
#warning "没有可用的原子操作。T2C JIT 将被关闭。"
#endif

#define ATOMIC_RELAXED 0
#define ATOMIC_ACQUIRE 0
#define ATOMIC_RELEASE 0
#define ATOMIC_SEQ_CST 0

/* 简单非原子退化实现：仅在单线程场景安全。
 * 警告：无扩展时 ATOMIC_EXCHANGE 返回新值而不是旧值。T2C/GDBSTUB 需要真实
 * 原子操作，否则在这些平台上会有数据竞争；缺少原子操作时应关闭它们。
 */
#define ATOMIC_LOAD(ptr, order) (*(ptr))
#define ATOMIC_STORE(ptr, val, order) ((void) (*(ptr) = (val)))
#define ATOMIC_FETCH_ADD(ptr, val, order) \
    ((*(ptr) += (val)) - (val)) /* 返回旧值。 */
#define ATOMIC_FETCH_SUB(ptr, val, order) \
    ((*(ptr) -= (val)) + (val)) /* 返回旧值。 */
/* 没有 statement expression 时，ATOMIC_EXCHANGE 无法返回旧值。
 * 这里返回新值，调用者不能依赖返回值。 */
#define ATOMIC_EXCHANGE(ptr, val, order) (*(ptr) = (val))
/* ATOMIC_COMPARE_EXCHANGE_WEAK：非原子实现，只能用于单线程。 */
#define ATOMIC_COMPARE_EXCHANGE_WEAK(ptr, expected, desired, succ, fail) \
    ((*(ptr) == *(expected)) ? (*(ptr) = (desired), 1)                   \
                             : (*(expected) = *(ptr), 0))
#endif

/* C 宏模式匹配技巧。
 * https://github.com/pfultz2/Cloak/wiki/C-Preprocessor-tricks,-tips,-and-idioms
 */

/* Visual Studio 会把 __VA_ARGS__ 当成独立参数处理。 */
#define FIX_VC_BUG(x) x

/* 拼接 token。 */
#define PRIMITIVE_CAT(a, ...) FIX_VC_BUG(a##__VA_ARGS__)

#define IIF(c) PRIMITIVE_CAT(IIF_, c)
/* 选择并展开第二个参数。 */
#define IIF_0(t, ...) __VA_ARGS__
/* 选择并展开第一个参数。 */
#define IIF_1(t, ...) t

/* 接受不少于 N 个参数，但只展开第 N 个。调用方宏仍只支持 4 个参数；由于可能
 * 返回的值集合多 1 个，因此 N 增加到 6。
 */
#define _GET_NTH_ARG(_1, _2, _3, _4, _5, N, ...) N

/* 统计可变参数宏中有多少参数。这里用 GCC/Clang 扩展处理 ... 展开为空的场景。
 * ##VA_ARGS 前的占位参数值无关紧要，但需要它保持偏移正确。N 位置额外加入 0，
 * 作为合法返回值。
 */
#define COUNT_VARARGS(...) _GET_NTH_ARG("ignored", ##__VA_ARGS__, 4, 3, 2, 1, 0)

/* 从 C23 开始，typeof 已纳入 C 标准。 */
#if defined(__GNUC__) || defined(__clang__) ||         \
    (defined(__STDC__) && defined(__STDC_VERSION__) && \
     (__STDC_VERSION__ >= 202000L)) /* C2x/C23 ?*/
#define __HAVE_TYPEOF 1
#endif

/**
 * container_of() - 根据成员指针反推出包含它的对象地址。
 * @ptr: 成员变量指针。
 * @type: 包含该成员的结构体类型。
 * @member: @type 中的成员名。
 *
 * Return: 包含 ptr 的对象指针，类型为 @type*。
 */
#ifndef container_of
#ifdef __HAVE_TYPEOF
#define container_of(ptr, type, member)                            \
    __extension__({                                                \
        const __typeof__(((type *) 0)->member) *__pmember = (ptr); \
        (type *) ((char *) __pmember - offsetof(type, member));    \
    })
#else
#define container_of(ptr, type, member) \
    ((type *) ((char *) (ptr) - (offsetof(type, member))))
#endif
#endif
