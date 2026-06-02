/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/*
 * JIT 基本块缓存。
 *
 * 缓存使用哈希桶、热点频次和 ghost list 管理翻译后的基本块；系统模式下还维护
 * 虚拟页到基本块的反向索引，用于 SFENCE.VMA/FENCE.I 后按 SATP 或 VA 精确失效。
 * 这里是解释器热点提升、一级 JIT 复用和二级 JIT 失效传播的共同基础。
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache.h"
#include "mpool.h"
#include "riscv.h"
#include "riscv_private.h"
#include "utils.h"

static uint32_t cache_size, cache_size_bits;

/* 缓存哈希函数。 */
HASH_FUNC_IMPL(cache_hash, cache_size_bits, cache_size)

struct hlist_head {
    struct hlist_node *first;
};

struct hlist_node {
    struct hlist_node *next, **pprev;
};

typedef struct {
    void *value;
    bool alive; /* true 表示活跃缓存；false 表示哈希表中保留的淘汰历史 */
    uint32_t key;
    uint32_t freq;
    struct list_head list;
    struct hlist_node ht_list;
} cache_entry_t;

typedef struct {
    struct hlist_head *ht_list_head;
} hashtable_t;

/*
 * 缓存采用退化版 ARC（Adaptive Replacement Cache，自适应替换缓存）策略。
 * 这里仅保留 LRU（最近最少使用）部分，忽略 LFU（最不常使用）部分。
 * 高频条目会被 JIT 编译为宿主平台机器码，之后不再需要长期保存在本缓存中。
 * 当缓存已满时，最近最少使用的活跃条目会被移动到 ghost list，作为淘汰历史。
 * 若新插入条目的 key 命中了 ghost list 中的历史记录，则释放旧历史条目，并将
 * 其中记录的频次等信息继承到新条目上。
 */

#if RV32_HAS(JIT) && RV32_HAS(SYSTEM) && RV32_HAS(BLOCK_CHAINING)
/* 页索引条目：把同一页桶中的基本块串起来。 */
typedef struct page_block_entry {
    void *block;                   /* 指向 block_t 的指针 */
    struct page_block_entry *next; /* 桶链表中的下一个条目 */
} page_block_entry_t;
#endif

typedef struct cache {
    struct list_head list;       /* 活跃缓存链表 */
    struct list_head ghost_list; /* 已淘汰缓存的历史链表 */
    hashtable_t map; /* 同时保存活跃条目和淘汰历史的哈希表 */
    uint32_t size;
    uint32_t ghost_list_size;
    uint32_t capacity;
#if RV32_HAS(JIT) && RV32_HAS(SYSTEM) && RV32_HAS(BLOCK_CHAINING)
    /* 按虚拟地址做 O(1) 失效的页索引。
     * 每个桶保存起始地址落在该页内的基本块链表。
     */
    page_block_entry_t *page_index[PAGE_INDEX_SIZE];
    /* malloc 失败会导致页索引不完整。
     * 该标志置位后，cache_invalidate_va 必须退回 O(n) 全量扫描。
     */
    bool page_index_incomplete;
#endif
} cache_t;

#if RV32_HAS(JIT) && RV32_HAS(SYSTEM) && RV32_HAS(BLOCK_CHAINING)
/* 页索引辅助函数的前置声明。 */
static void page_index_insert(cache_t *cache, block_t *block);
static void page_index_remove(cache_t *cache, block_t *block);
#endif

#define INIT_HLIST_HEAD(ptr) ((ptr)->first = NULL)

static inline void INIT_HLIST_NODE(struct hlist_node *h)
{
    h->next = NULL;
    h->pprev = NULL;
}

static inline int hlist_empty(const struct hlist_head *h)
{
    return !h->first;
}

static inline void hlist_add_head(struct hlist_node *n, struct hlist_head *h)
{
#ifndef __clang_analyzer__
    struct hlist_node *first = h->first;
    n->next = first;
    if (first)
        first->pprev = &n->next;

    h->first = n;
    n->pprev = &h->first;
#endif
}

static inline bool hlist_unhashed(const struct hlist_node *h)
{
    return !h->pprev;
}

static inline void hlist_del(struct hlist_node *n)
{
    struct hlist_node *next = n->next;
    struct hlist_node **pprev = n->pprev;

    *pprev = next;
    if (next)
        next->pprev = pprev;
}

static inline void hlist_del_init(struct hlist_node *n)
{
    if (hlist_unhashed(n))
        return;
    hlist_del(n);
    INIT_HLIST_NODE(n);
}

#define hlist_entry(ptr, type, member) container_of(ptr, type, member)

#ifdef __HAVE_TYPEOF
#define hlist_entry_safe(ptr, type, member)                  \
    ({                                                       \
        typeof(ptr) ____ptr = (ptr);                         \
        ____ptr ? hlist_entry(____ptr, type, member) : NULL; \
    })
#else
#define hlist_entry_safe(ptr, type, member) \
    (ptr) ? hlist_entry(ptr, type, member) : NULL
#endif

/* clang-format off */
#ifdef __HAVE_TYPEOF
#define hlist_for_each_entry(pos, head, member)                              \
    for (pos = hlist_entry_safe((head)->first, typeof(*(pos)), member); pos; \
         pos = hlist_entry_safe((pos)->member.next, typeof(*(pos)), member))

#define hlist_for_each_entry_safe(pos, n, head, member)               \
    for (pos = hlist_entry_safe((head)->first, typeof(*pos), member); \
         pos && ({ n = pos->member.next; 1; });                       \
         pos = hlist_entry_safe(n, typeof(*pos), member))
#else
#define hlist_for_each_entry(pos, head, member, type)              \
    for (pos = hlist_entry_safe((head)->first, type, member); pos; \
         pos = hlist_entry_safe((pos)->member.next, type, member))

#define hlist_for_each_entry_safe(pos, n, head, member, type) \
    for (pos = hlist_entry_safe((head)->first, type, member); \
         pos && ({ n = pos->member.next; 1; });               \
         pos = hlist_entry_safe(n, type, member))
#endif
/* clang-format on */

cache_t *cache_create(uint32_t size_bits)
{
    /* 避免 1 << size_bits 发生整数溢出。 */
    if (size_bits >= 32)
        return NULL;

    cache_t *cache = malloc(sizeof(cache_t));
    if (!cache)
        return NULL;

    cache_size_bits = size_bits;
    cache_size = 1 << size_bits;

    INIT_LIST_HEAD(&cache->list);
    INIT_LIST_HEAD(&cache->ghost_list);
    cache->size = 0;
    cache->ghost_list_size = 0;
    cache->capacity = cache_size;

    /* 检查哈希桶数组大小计算是否溢出。 */
    size_t alloc_size = cache_size * sizeof(struct hlist_head);
    if (alloc_size / sizeof(struct hlist_head) != cache_size)
        goto fail_cache;

    cache->map.ht_list_head = malloc(alloc_size);
    if (!cache->map.ht_list_head)
        goto fail_cache;

    for (uint32_t i = 0; i < cache_size; i++)
        INIT_HLIST_HEAD(&cache->map.ht_list_head[i]);

#if RV32_HAS(JIT) && RV32_HAS(SYSTEM) && RV32_HAS(BLOCK_CHAINING)
    /* 初始化用于 O(1) 失效查找的页索引。 */
    memset(cache->page_index, 0, sizeof(cache->page_index));
    cache->page_index_incomplete = false;
#endif

    return cache;

fail_cache:
    free(cache);
    return NULL;
}

void *cache_get(const cache_t *cache, uint32_t key, bool update)
{
    if (unlikely(!cache->capacity))
        return NULL;

    if (hlist_empty(&cache->map.ht_list_head[cache_hash(key)]))
        return NULL;

    cache_entry_t *entry = NULL;
#ifdef __HAVE_TYPEOF
    hlist_for_each_entry (entry, &cache->map.ht_list_head[cache_hash(key)],
                          ht_list)
#else
    hlist_for_each_entry (entry, &cache->map.ht_list_head[cache_hash(key)],
                          ht_list, cache_entry_t)
#endif
    {
        if (entry->key == key)
            break;
    }

    /* 缓存未命中时返回 NULL。 */
    if (!entry || entry->key != key || !entry->alive)
        return NULL;

    /*
     * FIXME：系统模式下，不同进程可能出现相同 PC。这里后续需要结合 SATP CSR
     * 判断并更新正确的缓存条目。
     */
    /* 某个基本块的使用频次超过预设 THRESHOLD 后，会被送入代码生成器生成 C 代码；
     * 生成的 C 代码随后由目标编译器编译为机器码。
     */
    if (update)
        entry->freq++;

    return entry->value;
}

/*
 * ghost list 超过容量上限时，丢弃最旧的历史记录；对应的历史频次信息也会永久丢失。
 */
FORCE_INLINE void cache_ghost_list_update(cache_t *cache)
{
    if (cache->ghost_list_size <= cache->capacity)
        return;

    cache_entry_t *entry =
        list_last_entry(&cache->ghost_list, cache_entry_t, list);
    assert(!entry->alive);
    hlist_del_init(&entry->ht_list);
    list_del_init(&entry->list);
    cache->ghost_list_size--;
    free(entry);
}

/*
 * 插入缓存时可能出现三种情况：
 * - 淘汰最近最少使用的活跃缓存
 * - 更新已有缓存
 * - 从 ghost list 的历史记录中恢复频次信息
 */
void *cache_put(cache_t *cache, uint32_t key, void *value)
{
    assert(cache->size <= cache->capacity);

    cache_entry_t *replaced = NULL, *revived = NULL, *entry;
#ifdef __HAVE_TYPEOF
    hlist_for_each_entry (entry, &cache->map.ht_list_head[cache_hash(key)],
                          ht_list)
#else
    hlist_for_each_entry (entry, &cache->map.ht_list_head[cache_hash(key)],
                          ht_list, cache_entry_t)
#endif
    {
        if (entry->key != key)
            continue;
        if (!entry->alive) {
            revived = entry;
            break;
        }
        /* 更新已有缓存。 */
        if (entry->value != value) {
            replaced = entry;
            break;
        }
        /* 不应向缓存重复放入完全相同的基本块。 */
        assert(NULL);
        __UNREACHABLE;
    }

    /* 缓存已满时，选出需要被替换的条目。 */
    if (!replaced && cache->size == cache->capacity) {
        replaced = list_last_entry(&cache->list, cache_entry_t, list);
        assert(replaced);
    }

    void *replaced_value = NULL;
    if (replaced) {
        assert(replaced->alive);

        replaced_value = replaced->value;
#if RV32_HAS(JIT) && RV32_HAS(SYSTEM) && RV32_HAS(BLOCK_CHAINING)
        /* 淘汰前先从页索引移除被替换的基本块。 */
        if (replaced_value)
            page_index_remove(cache, (block_t *) replaced_value);
#endif
        replaced->alive = false;
        list_del_init(&replaced->list);
        cache->size--;
        list_add(&replaced->list, &cache->ghost_list);
        cache->ghost_list_size++;
    }

    cache_entry_t *new_entry = calloc(1, sizeof(cache_entry_t));
    if (unlikely(!new_entry)) {
        /* 分配失败时，如果已经取出了替换条目，需要恢复原状。 */
        if (replaced) {
            replaced->alive = true;
            list_del_init(&replaced->list);
            list_add(&replaced->list, &cache->list);
            cache->size++;
            cache->ghost_list_size--;
        }
        return NULL;
    }
    assert(new_entry);

    INIT_LIST_HEAD(&new_entry->list);
    INIT_HLIST_NODE(&new_entry->ht_list);
    new_entry->key = key;
    new_entry->value = value;
    new_entry->alive = true;

    if (!revived) {
        new_entry->freq = 1;
    } else {
        new_entry->freq = revived->freq + 1;
        hlist_del_init(&revived->ht_list);
        list_del_init(&revived->list);
        cache->ghost_list_size--;
        free(revived);
    }

    list_add(&new_entry->list, &cache->list);
    hlist_add_head(&new_entry->ht_list,
                   &cache->map.ht_list_head[cache_hash(key)]);

    cache->size++;

#if RV32_HAS(JIT) && RV32_HAS(SYSTEM) && RV32_HAS(BLOCK_CHAINING)
    /* 用页索引支持 O(1) 失效。
     * 基本块在页边界处终止；非分支块边界通过顺序落入链继续执行。
     */
    page_index_insert(cache, (block_t *) value);
#endif

    cache_ghost_list_update(cache);

    assert(cache->size <= cache->capacity);
    assert(cache->ghost_list_size <= cache->capacity);
    return replaced_value;
}

void cache_free(cache_t *cache)
{
#if RV32_HAS(JIT) && RV32_HAS(SYSTEM) && RV32_HAS(BLOCK_CHAINING)
    /* 释放所有页索引条目。 */
    for (uint32_t i = 0; i < PAGE_INDEX_SIZE; i++) {
        page_block_entry_t *entry = cache->page_index[i];
        while (entry) {
            page_block_entry_t *next = entry->next;
            free(entry);
            entry = next;
        }
    }
#endif
    free(cache->map.ht_list_head);
    free(cache);
}

uint32_t cache_freq(const struct cache *cache, uint32_t key)
{
    if (unlikely(!cache->capacity))
        return 0;

    if (hlist_empty(&cache->map.ht_list_head[cache_hash(key)]))
        return 0;

    cache_entry_t *entry = NULL;
#ifdef __HAVE_TYPEOF
    hlist_for_each_entry (entry, &cache->map.ht_list_head[cache_hash(key)],
                          ht_list)
#else
    hlist_for_each_entry (entry, &cache->map.ht_list_head[cache_hash(key)],
                          ht_list, cache_entry_t)
#endif
    {
        if (entry->key == key && entry->alive)
            return entry->freq;
    }
    return 0;
}

#if RV32_HAS(JIT)
bool cache_hot(const struct cache *cache, uint32_t key)
{
    if (unlikely(!cache->capacity))
        return false;

    if (hlist_empty(&cache->map.ht_list_head[cache_hash(key)]))
        return false;

    cache_entry_t *entry = NULL;
#ifdef __HAVE_TYPEOF
    hlist_for_each_entry (entry, &cache->map.ht_list_head[cache_hash(key)],
                          ht_list)
#else
    hlist_for_each_entry (entry, &cache->map.ht_list_head[cache_hash(key)],
                          ht_list, cache_entry_t)
#endif
    {
        if (entry->key == key && entry->alive && entry->freq >= THRESHOLD) {
            return true;
        }
    }
    return false;
}
void cache_profile(const struct cache *cache,
                   FILE *output_file,
                   prof_func_t func)
{
    assert(cache);
    assert(func);
    assert(output_file);

    cache_entry_t *entry;
#ifdef __HAVE_TYPEOF
    list_for_each_entry (entry, &cache->list, list)
#else
    list_for_each_entry (entry, &cache->list, list, cache_entry_t)
#endif
    {
        func(entry->value, entry->freq, output_file);
    }
}

/* 对间接调用禁用 UBSAN 函数指针类型检查。启用 T2C 时，
 * t2c_dispose_block_engine 会使用 LLVM 的 cflags 编译，可能导致函数类型元数据
 * 不匹配；通过 clear_func_t 间接调用时，UBSAN 会因此产生误报。
 */
DISABLE_UBSAN_FUNC
void clear_cache_hot(const struct cache *cache, clear_func_t func)
{
    assert(cache);
    assert(func);

    cache_entry_t *entry = NULL;
#ifdef __HAVE_TYPEOF
    list_for_each_entry (entry, &cache->list, list)
#else
    list_for_each_entry (entry, &cache->list, list, cache_entry_t)
#endif
    {
        func(entry->value);
    }
}
#endif

#if RV32_HAS(JIT) && RV32_HAS(SYSTEM) && RV32_HAS(BLOCK_CHAINING)
/* 支持 O(1) 缓存失效的页索引函数。
 * 依赖 BLOCK_CHAINING 生成的页终止基本块。
 */

/* 页索引哈希函数，使用黄金比例乘法哈希。 */
HASH_FUNC_IMPL(page_index_hash, PAGE_INDEX_BITS, PAGE_INDEX_SIZE)

/* 将基本块插入页索引。 */
static void page_index_insert(cache_t *cache, block_t *block)
{
    uint32_t page = block->pc_start & ~(RV_PG_SIZE - 1);
    uint32_t bucket = page_index_hash(page >> RV_PG_SHIFT);

    page_block_entry_t *entry = malloc(sizeof(page_block_entry_t));
    if (!entry) {
        /* 标记页索引不完整。cache_invalidate_va 必须退回 O(n) 扫描，
         * 以确保 SFENCE.VMA 时不会漏掉任何基本块。
         */
        cache->page_index_incomplete = true;
        return;
    }

    entry->block = block;
    entry->next = cache->page_index[bucket];
    cache->page_index[bucket] = entry;
}

/* 从页索引移除基本块。 */
static void page_index_remove(cache_t *cache, block_t *block)
{
    uint32_t page = block->pc_start & ~(RV_PG_SIZE - 1);
    uint32_t bucket = page_index_hash(page >> RV_PG_SHIFT);

    page_block_entry_t **pp = &cache->page_index[bucket];
    while (*pp) {
        if ((*pp)->block == block) {
            page_block_entry_t *tmp = *pp;
            *pp = (*pp)->next;
            free(tmp);
            return;
        }
        pp = &(*pp)->next;
    }
}
#endif /* RV32_HAS(JIT) && RV32_HAS(SYSTEM) && RV32_HAS(BLOCK_CHAINING) */

#if RV32_HAS(JIT) && RV32_HAS(SYSTEM)
/* 线程安全说明：这些失效函数假设运行环境是单线程。
 * rv32emu 的 JIT 目前采用单线程模型，编译和执行不会并发发生。如果该假设未来改变，
 * cache->list 遍历周围必须补充合适的锁保护。
 */

uint32_t cache_invalidate_satp(cache_t *cache, uint32_t satp)
{
    if (unlikely(!cache->capacity))
        return 0;

    uint32_t count = 0;
    cache_entry_t *entry = NULL;
#ifdef __HAVE_TYPEOF
    list_for_each_entry (entry, &cache->list, list)
#else
    list_for_each_entry (entry, &cache->list, list, cache_entry_t)
#endif
    {
        block_t *block = (block_t *) entry->value;
        if (block && block->satp == satp && !block->invalidated) {
            block->invalidated = true;
#if RV32_HAS(T2C)
            /* 重置 hot2，防止 T2C 继续执行已失效基本块。
             * 这样 rv_step() 中的 T2C 执行路径会跳过该块并回落到重新翻译流程。
             */
            ATOMIC_STORE(&block->hot2, false, ATOMIC_RELEASE);
#endif
            count++;
        }
    }
    return count;
}

uint32_t cache_invalidate_va(cache_t *cache, uint32_t va, uint32_t satp)
{
    if (unlikely(!cache->capacity))
        return 0;

    uint32_t va_page = va & ~(RV_PG_SIZE - 1);
    uint32_t count = 0;

#if RV32_HAS(BLOCK_CHAINING)
    /* 页索引完整时使用 O(1) 查找；否则退回 O(n) 扫描，确保不会漏掉基本块。
     */
    if (!cache->page_index_incomplete) {
        /* 通过页索引执行 O(1) 查找。
         * 在页界限内的基本块会完整落在一个 4KB 页中，因此只需检查对应页桶。
         */
        uint32_t bucket = page_index_hash(va_page >> RV_PG_SHIFT);
        page_block_entry_t *pentry = cache->page_index[bucket];
        while (pentry) {
            block_t *block = (block_t *) pentry->block;
            if (block && block->satp == satp && !block->invalidated) {
                /* 确认基本块属于该页，用于处理哈希冲突。 */
                uint32_t block_page = block->pc_start & ~(RV_PG_SIZE - 1);
                if (block_page == va_page) {
                    block->invalidated = true;
#if RV32_HAS(T2C)
                    /* 重置 hot2，防止 T2C 继续执行已失效基本块。 */
                    ATOMIC_STORE(&block->hot2, false, ATOMIC_RELEASE);
#endif
                    count++;
                }
            }
            pentry = pentry->next;
        }
        return count;
    }
#endif /* RV32_HAS(BLOCK_CHAINING) */

    /* O(n) 回退路径：页索引不可用或不完整时扫描所有基本块。
     * 这能保证在 BLOCK_CHAINING 被禁用、基本块可能跨页，或 page_index_insert 中
     * malloc 失败时仍然保持正确性。
     */
    cache_entry_t *entry = NULL;
#ifdef __HAVE_TYPEOF
    list_for_each_entry (entry, &cache->list, list)
#else
    list_for_each_entry (entry, &cache->list, list, cache_entry_t)
#endif
    {
        block_t *block = (block_t *) entry->value;
        if (!block || block->satp != satp || block->invalidated)
            continue;

        /* 检查目标 VA 页是否与基本块地址范围重叠。
         * 基本块可能跨越多个页，因此需要判断 va_page 是否落在
         * [block_start_page, block_end_page] 范围内。
         *
         * 注意：pc_end 是排他边界（最后一条指令之后的地址），因此用
         * (pc_end - 1) 取得最后一个字节所在页。这样可以避免 pc_end 正好落在页边界
         * 时产生误失效。
         */
        uint32_t block_start_page = block->pc_start & ~(RV_PG_SIZE - 1);
        uint32_t last_byte = block->pc_end > block->pc_start ? block->pc_end - 1
                                                             : block->pc_start;
        uint32_t block_end_page = last_byte & ~(RV_PG_SIZE - 1);
        if (va_page >= block_start_page && va_page <= block_end_page) {
            block->invalidated = true;
#if RV32_HAS(T2C)
            /* 重置 hot2，防止 T2C 继续执行已失效基本块。 */
            ATOMIC_STORE(&block->hot2, false, ATOMIC_RELEASE);
#endif
            count++;
        }
    }

    return count;
}
#endif /* RV32_HAS(JIT) && RV32_HAS(SYSTEM) */
