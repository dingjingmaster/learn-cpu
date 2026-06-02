#pragma once

/*
 * 通用工具接口。
 *
 * 包含宿主时间查询、路径规范化和轻量集合工具。系统调用层和测试路径逻辑会复用
 * 这些函数，避免在业务代码中重复处理跨平台差异。
 */

#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

/* 获取宿主系统当前格林尼治时间。
 * TODO：补充当前时区处理。
 */
void rv_gettimeofday(struct timeval *tv);

/* 获取指定 clock_id 对应的宿主时钟值。 */
void rv_clock_gettime(struct timespec *tp);

#if RV32_HAS(JIT) && RV32_HAS(SYSTEM)

typedef uint64_t rv_hash_key_t;

#define HASH_FUNC_IMPL(name, size_bits, size)                      \
    FORCE_INLINE rv_hash_key_t name(rv_hash_key_t val)             \
    {                                                              \
        /* 0x61c8864680b583eb 是 64 位黄金比例常数。 */            \
        return (val * 0x61c8864680b583ebull >> (64 - size_bits)) & \
               ((size) - (1));                                     \
    }
#else

typedef uint32_t rv_hash_key_t;

/* 该哈希例程改编自 Linux 内核。
 * 参考：
 * https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tree/include/linux/hash.h
 */
#define HASH_FUNC_IMPL(name, size_bits, size)                           \
    FORCE_INLINE rv_hash_key_t name(rv_hash_key_t val)                  \
    {                                                                   \
        /* 0x61C88647 是 32 位黄金比例常数。 */                         \
        return (val * 0x61C88647 >> (32 - size_bits)) & ((size) - (1)); \
    }
#endif

/* sanitize_path 通过纯词法处理，返回与输入路径等价的最短路径名。
 * 它会反复应用以下规则，直到无法继续简化：
 *
 *  1. 将连续多个斜杠替换为单个斜杠。
 *  2. 删除每个 . 路径元素（当前目录）。
 *  3. 删除内部的 .. 路径元素（父目录），同时删除它前面的非 .. 元素。
 *  4. 删除根路径开头的 .. 元素，也就是把路径开头的 "/.." 替换为 "/"。
 *
 * 返回路径只有在根目录 "/" 时才会以斜杠结尾。
 *
 * 如果处理结果为空字符串，则返回 "."。
 *
 * 另见 Rob Pike 的文章 “Lexical File Names in Plan 9 or
 * Getting Dot-Dot Right”：
 * https://9p.io/sys/doc/lexnames.html
 *
 * 参考：
 * https://cs.opensource.google/go/go/+/refs/tags/go1.21.4:src/path/path.go;l=51
 */
char *sanitize_path(const char *input);

static inline uintptr_t align_up(uintptr_t sz, size_t alignment)
{
    uintptr_t mask = alignment - 1;
    if (likely((alignment & mask) == 0))
        return ((sz + mask) & ~mask);
    return (((sz + mask) / alignment) * alignment);
}

/* 类 Linux 内核风格的双向链表 API。 */

struct list_head {
    struct list_head *prev, *next;
};

static inline void INIT_LIST_HEAD(struct list_head *head)
{
    head->next = head->prev = head;
}

static inline bool list_empty(const struct list_head *head)
{
    return head->next == head;
}

static inline void list_add(struct list_head *node, struct list_head *head)
{
    struct list_head *next = head->next;

    next->prev = node;
    node->next = next;
    node->prev = head;
    head->next = node;
}

static inline void list_del(struct list_head *node)
{
    struct list_head *next = node->next, *prev = node->prev;

    next->prev = prev;
    prev->next = next;
}

static inline void list_del_init(struct list_head *node)
{
    list_del(node);
    INIT_LIST_HEAD(node);
}

#define list_entry(node, type, member) container_of(node, type, member)

#define list_first_entry(head, type, member) \
    list_entry((head)->next, type, member)

#define list_last_entry(head, type, member) \
    list_entry((head)->prev, type, member)

#ifdef __HAVE_TYPEOF
#define list_for_each_entry(entry, head, member)                       \
    for (entry = list_entry((head)->next, __typeof__(*entry), member); \
         &entry->member != (head);                                     \
         entry = list_entry(entry->member.next, __typeof__(*entry), member))

#define list_for_each_entry_safe(entry, safe, head, member)                \
    for (entry = list_entry((head)->next, __typeof__(*entry), member),     \
        safe = list_entry(entry->member.next, __typeof__(*entry), member); \
         &entry->member != (head); entry = safe,                           \
        safe = list_entry(safe->member.next, __typeof__(*entry), member))
#else
#define list_for_each_entry(entry, head, member, type)   \
    for (entry = list_entry((head)->next, type, member); \
         &entry->member != (head);                       \
         entry = list_entry(entry->member.next, type, member))

#define list_for_each_entry_safe(entry, safe, head, member, type) \
    for (entry = list_entry((head)->next, type, member),          \
        safe = list_entry(entry->member.next, type, member);      \
         &entry->member != (head);                                \
         entry = safe, safe = list_entry(safe->member.next, type, member))
#endif

#define SET_SIZE_BITS 10
#define SET_SIZE (1 << SET_SIZE_BITS)
#define SET_SLOTS_SIZE 32

#if RV32_HAS(JIT) && RV32_HAS(SYSTEM)
/*
 * JIT 使用组合 key：高 32 位保存 supervisor address translation and protection
 * (SATP) 寄存器值，低 32 位保存程序计数器 (PC)，与用户态模拟中的 key 语义一致。
 */
#define RV_HASH_KEY(block) \
    ((((rv_hash_key_t) block->satp) << 32) | (rv_hash_key_t) block->pc_start)
#else
#define RV_HASH_KEY(block) ((rv_hash_key_t) block->pc_start)
#endif

/* set 由 SET_SIZE 个桶组成，每个桶包含 SET_SLOTS_SIZE 个槽位。 */
typedef struct {
    rv_hash_key_t table[SET_SIZE][SET_SLOTS_SIZE];
} set_t;

/**
 * set_reset - 清空集合
 * @set: 目标集合指针
 */
void set_reset(set_t *set);

/**
 * set_add - 向集合插入新元素
 * @set: 目标集合指针
 * @key: 待插入元素的 key
 */
bool set_add(set_t *set, rv_hash_key_t key);

/**
 * set_has - 检查集合中是否存在指定元素
 * @set: 目标集合指针
 * @key: 待查询元素的 key
 */
bool set_has(set_t *set, rv_hash_key_t key);
