/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/*
 * 客体内存管理和基础内存访问。
 *
 * 本文件提供 memory_new/delete、指令取指、读写字/半字/字节等接口。支持 mmap
 * 的平台会使用 PROT_NONE + SIGSEGV/SIGBUS 处理器实现按需分页，首次访问时才
 * 激活 64 KiB 内存块，并可通过 memory_gc 回收全零块以降低物理内存占用。
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if HAVE_MMAP
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "io.h"
#include "log.h"

static uint8_t *data_memory_base;
static uint64_t data_memory_size;

#if HAVE_MMAP
/* 按需分页内存管理。
 *
 * 通过信号处理按块延迟分配内存：
 * 1. 初始映射使用 PROT_NONE（不可访问，也不占用物理内存）
 * 2. 访问触发 SIGSEGV/SIGBUS，处理器激活对应内存块
 * 3. 未使用的块可以通过 madvise(MADV_DONTNEED) 回收
 *
 * 这能以很小的初始占用实现客体内存的自动增长。
 */

/* 块大小：64KB 在粒度和管理开销之间提供较好的折中。 */
#define CHUNK_SHIFT 16
#define CHUNK_SIZE (1UL << CHUNK_SHIFT)
#define CHUNK_MASK (~(CHUNK_SIZE - 1))

/* 4GB 地址空间的最大块数：4GB / 64KB = 65536。 */
#define MAX_CHUNKS (0x100000000ULL >> CHUNK_SHIFT)
#define BITMAP_SIZE (MAX_CHUNKS / 8)

/* 记录哪些内存块已经激活的位图。 */
static uint8_t chunk_bitmap[BITMAP_SIZE];

/* GC 状态：循环扫描索引。 */
static uint32_t gc_scan_idx;

/* 统计信息：当前和峰值激活块数。为保证信号安全，这里使用原子变量。 */
static atomic_uint_fast32_t active_chunks;
static atomic_uint_fast32_t peak_chunks;

/* 记录旧信号处理器，必要时继续转发。 */
static struct sigaction prev_sigsegv_handler;
static struct sigaction prev_sigbus_handler;

static inline bool bitmap_test(uint32_t idx)
{
    return (chunk_bitmap[idx >> 3] & (1 << (idx & 7))) != 0;
}

static inline void bitmap_set(uint32_t idx)
{
    chunk_bitmap[idx >> 3] |= (1 << (idx & 7));
}

static inline void bitmap_clear(uint32_t idx)
{
    chunk_bitmap[idx >> 3] &= ~(1 << (idx & 7));
}

/* 检查一段内存是否全为零，用于判断是否可以回收。
 * 这里按字节扫描，避免按字访问带来的对齐问题；编译器在条件允许时可能优化为 SIMD。
 */
static bool is_region_zero(const uint8_t *ptr, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        if (ptr[i] != 0)
            return false;
    }
    return true;
}

/* 按需分页的信号处理器。
 *
 * 信号安全说明：
 * - POSIX.1-2008 及之后版本规定 mprotect() 是异步信号安全的
 * - 使用 memory_order_relaxed 的原子操作满足信号安全要求
 * - 位图操作作用于静态内存，不需要锁
 * - 处理器内不能进行堆分配或 stdio 调用
 */
static void memory_fault_handler(int sig, siginfo_t *si, void *context UNUSED)
{
    uintptr_t fault_addr = (uintptr_t) si->si_addr;
    uintptr_t base = (uintptr_t) data_memory_base;
    uintptr_t end = base + data_memory_size;

    /* 检查触发地址是否落在客体内存范围内。 */
    if (fault_addr >= base && fault_addr < end) {
        /* 根据基地址计算块边界。 */
        uintptr_t offset = fault_addr - base;
        uintptr_t aligned_offset = offset & CHUNK_MASK;
        uintptr_t chunk_start = base + aligned_offset;
        uint32_t chunk_idx = aligned_offset >> CHUNK_SHIFT;

        /* 计算块长度，最后一块可能不足 CHUNK_SIZE。 */
        size_t chunk_len = CHUNK_SIZE;
        if (chunk_start + CHUNK_SIZE > end) {
            /* 最后一块是不完整块。 */
            chunk_len = end - chunk_start;
        }

        /* 以可读写权限激活该块。 */
        if (mprotect((void *) chunk_start, chunk_len, PROT_READ | PROT_WRITE) ==
            0) {
            /* 只有尚未激活时才计数，用于处理重复触发的边界情况。 */
            if (!bitmap_test(chunk_idx)) {
                bitmap_set(chunk_idx);
                uint_fast32_t current =
                    atomic_fetch_add_explicit(&active_chunks, 1,
                                              memory_order_relaxed) +
                    1;
                uint_fast32_t peak =
                    atomic_load_explicit(&peak_chunks, memory_order_relaxed);
                while (current > peak) {
                    if (atomic_compare_exchange_weak_explicit(
                            &peak_chunks, &peak, current, memory_order_relaxed,
                            memory_order_relaxed))
                        break;
                }
            }
            return; /* 恢复执行。 */
        }
    }

    /* 非本内存区域故障，或 mprotect 失败时，转发给旧处理器。 */
    struct sigaction *prev =
        (sig == SIGSEGV) ? &prev_sigsegv_handler : &prev_sigbus_handler;

    if (prev->sa_flags & SA_SIGINFO) {
        prev->sa_sigaction(sig, si, context);
    } else if (prev->sa_handler == SIG_DFL) {
        /* 恢复默认处理器并重新抛出信号。 */
        struct sigaction dfl;
        dfl.sa_handler = SIG_DFL;
        sigemptyset(&dfl.sa_mask);
        dfl.sa_flags = 0;
        sigaction(sig, &dfl, NULL);
        raise(sig);
    } else if (prev->sa_handler != SIG_IGN) {
        prev->sa_handler(sig);
    }
}

static bool install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = memory_fault_handler;
    sa.sa_flags = SA_SIGINFO; /* 不使用 SA_NODEFER：避免重入。 */
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGSEGV, &sa, &prev_sigsegv_handler) == -1) {
        rv_log_error("安装按需分页 SIGSEGV 处理器失败");
        return false;
    }

    if (sigaction(SIGBUS, &sa, &prev_sigbus_handler) == -1) {
        rv_log_error("安装按需分页 SIGBUS 处理器失败");
        sigaction(SIGSEGV, &prev_sigsegv_handler, NULL);
        return false;
    }

    return true;
}

static void restore_signal_handlers(void)
{
    sigaction(SIGSEGV, &prev_sigsegv_handler, NULL);
    sigaction(SIGBUS, &prev_sigbus_handler, NULL);
}
#endif /* HAVE_MMAP */

memory_t *memory_new(uint64_t size)
{
    if (!size)
        return NULL;

    /* 最大支持 4GB（32 位地址空间）。
     * 按需分页位图也是按 4GB 空间大小设计的。
     */
    if (size > 0x100000000ULL)
        return NULL;

    memory_t *mem = malloc(sizeof(memory_t));
    if (!mem)
        return NULL;

#if HAVE_MMAP
    /* 安装按需分页信号处理器。 */
    if (!install_signal_handlers()) {
        free(mem);
        return NULL;
    }

    /* 初始以 PROT_NONE 映射内存，不立即分配物理页。
     * 访问时由信号处理器按需激活块；MAP_NORESERVE 避免提前预留交换空间。
     */
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
    data_memory_base = mmap(NULL, size, PROT_NONE,
                            MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    if (data_memory_base == MAP_FAILED) {
        restore_signal_handlers();
        free(mem);
        return NULL;
    }

    data_memory_size = size;
    memset(chunk_bitmap, 0, sizeof(chunk_bitmap));
    gc_scan_idx = 0;
    atomic_store_explicit(&active_chunks, 0, memory_order_relaxed);
    atomic_store_explicit(&peak_chunks, 0, memory_order_relaxed);
#else
    /* 不支持 mmap 的系统（如 Windows、Emscripten）使用回退路径。
     * 此时无法按需分页，物理内存会一次性分配。这里限制为 512MB，避免过度占用；
     * 需要更大的地址空间时应使用 HAVE_MMAP=1 构建。
     */
#define MALLOC_MAX_SIZE (512UL * 1024 * 1024) /* 512 MB */
    if (size > MALLOC_MAX_SIZE) {
        free(mem);
        return NULL;
    }
    data_memory_base = malloc(size);
    if (!data_memory_base) {
        free(mem);
        return NULL;
    }
    /* 清零初始化，保证行为一致。 */
    memset(data_memory_base, 0, size);
    data_memory_size = size;
#undef MALLOC_MAX_SIZE
#endif

    mem->mem_base = data_memory_base;
    mem->mem_size = data_memory_size; /* 使用实际分配大小。 */
    return mem;
}

void memory_delete(memory_t *mem)
{
#if HAVE_MMAP
    /* 先恢复信号处理器，避免处理器访问已释放内存。 */
    restore_signal_handlers();
    munmap(mem->mem_base, mem->mem_size);
#else
    free(mem->mem_base);
#endif
    free(mem);
}

/* 返回峰值物理内存使用量，单位为字节。
 * 启用 MMAP 时返回按需分页实际分配的物理内存。
 * 未启用 MMAP 时返回总分配大小，因为没有按需分页能力。
 */
uint64_t memory_get_usage(void)
{
#if HAVE_MMAP
    /* 限制到实际内存大小，防止溢出。 */
    uint32_t max_chunks = (data_memory_size + CHUNK_SIZE - 1) >> CHUNK_SHIFT;
    uint_fast32_t peak =
        atomic_load_explicit(&peak_chunks, memory_order_relaxed);
    uint32_t chunks = (peak < max_chunks) ? peak : max_chunks;
    return (uint64_t) chunks * CHUNK_SIZE;
#else
    return data_memory_size;
#endif
}

/* 增量垃圾回收：每次调用扫描一个内存块。
 * 对全零块，通过释放物理页（madvise）并重新设置故障处理（mprotect PROT_NONE）
 * 完成回收。
 */
void memory_gc(void)
{
#if HAVE_MMAP
    uint32_t idx = gc_scan_idx;

    /* 只处理实际内存大小范围内的块。 */
    uint32_t max_idx = (data_memory_size + CHUNK_SIZE - 1) >> CHUNK_SHIFT;
    if (max_idx > MAX_CHUNKS)
        max_idx = MAX_CHUNKS;

    if (idx >= max_idx) {
        gc_scan_idx = 0;
        return;
    }

    /* 位图操作期间阻塞信号，避免与信号处理器竞争。 */
    sigset_t block_set, old_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGSEGV);
    sigaddset(&block_set, SIGBUS);
    sigprocmask(SIG_BLOCK, &block_set, &old_set);

    /* 检查当前块是否已经激活。 */
    if (bitmap_test(idx)) {
        uint8_t *chunk_ptr =
            data_memory_base + ((uintptr_t) idx << CHUNK_SHIFT);

        /* 计算块长度，最后一块可能不足 CHUNK_SIZE。 */
        size_t chunk_len = CHUNK_SIZE;
        uintptr_t chunk_end = (uintptr_t) chunk_ptr + CHUNK_SIZE;
        uintptr_t mem_end = (uintptr_t) data_memory_base + data_memory_size;
        if (chunk_end > mem_end)
            chunk_len = mem_end - (uintptr_t) chunk_ptr;

        /* 块内容全零时回收。 */
        if (is_region_zero(chunk_ptr, chunk_len)) {
            /* 先改为不可访问，避免 madvise 与 mprotect 之间块又被写脏。
             * 只有保护成功后才继续。
             */
            if (mprotect(chunk_ptr, chunk_len, PROT_NONE) == 0) {
                /* 建议操作系统回收物理页。 */
                madvise(chunk_ptr, chunk_len, MADV_DONTNEED);
                bitmap_clear(idx);
                uint_fast32_t current =
                    atomic_load_explicit(&active_chunks, memory_order_relaxed);
                if (current > 0)
                    atomic_fetch_sub_explicit(&active_chunks, 1,
                                              memory_order_relaxed);
            }
        }
    }

    /* 恢复信号掩码。 */
    sigprocmask(SIG_SETMASK, &old_set, NULL);

    /* 推进到下一个块，循环扫描。 */
    gc_scan_idx = (idx + 1) % max_idx;
#endif
}

/*
 * 快速内存访问函数：出于性能考虑不做边界检查。
 * 调用方必须先验证地址。启用 MMAP 时，越界访问会触发 SIGSEGV，并继续转发到默认
 * 处理器。
 */

void memory_read(const memory_t *mem,
                 uint8_t *dst,
                 uint32_t addr,
                 uint32_t size)
{
    memcpy(dst, mem->mem_base + addr, size);
}

uint32_t memory_ifetch(uint32_t addr)
{
    uint32_t val;
    memcpy(&val, data_memory_base + addr, sizeof(val));
    return val;
}

/* 使用 memcpy 执行安全的非对齐访问，编译器通常会优化为普通 load/store。
 */
#define MEM_READ_IMPL(size, type)                            \
    type memory_read_##size(uint32_t addr)                   \
    {                                                        \
        type val;                                            \
        memcpy(&val, data_memory_base + addr, sizeof(type)); \
        return val;                                          \
    }

MEM_READ_IMPL(w, uint32_t)
MEM_READ_IMPL(s, uint16_t)
MEM_READ_IMPL(b, uint8_t)

#define MEM_WRITE_IMPL(size, type)                              \
    void memory_write_##size(uint32_t addr, const uint8_t *src) \
    {                                                           \
        memcpy(data_memory_base + addr, src, sizeof(type));     \
    }

MEM_WRITE_IMPL(w, uint32_t)
MEM_WRITE_IMPL(s, uint16_t)
MEM_WRITE_IMPL(b, uint8_t)
