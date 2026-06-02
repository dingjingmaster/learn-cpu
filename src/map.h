/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/* 节省内存的红黑树 map 实现。
 *
 * 该实现以较小内存开销提供 O(log n) 插入、删除和查找操作。设计借鉴 Linux
 * 内核侵入式数据结构和 jemalloc 的 rb.h。
 *
 * 关键特性：
 * - 颜色位存放在指针最低有效位，每个节点只需两个指针
 * - 不保存父指针，遍历时使用栈维护路径
 * - 通过通用 key/value 存储支持任意数据类型
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 红黑树节点结构。
 *
 * 内存布局针对低开销优化：
 * - 颜色位编码在 right_red 指针最低位
 * - 不保存父指针，改用遍历栈
 * - key 和 data 以灵活指针保存
 * - node+key+data 单次分配以改善缓存局部性
 *
 * 该设计在保持 O(log n) 操作的同时，把节点开销压到 2 个指针加 payload。
 */
typedef struct map_node {
    void *key, *data;           /* 指向 key/value 数据。 */
    struct map_node *left;      /* 左子节点。 */
    struct map_node *right_red; /* 右子节点 + 最低位颜色位。 */
} map_node_t;

/* 校验指针对齐是否足以存放颜色位。 */
#ifdef __STDC_VERSION__
#if __STDC_VERSION__ >= 201112L
_Static_assert(_Alignof(void *) >= 2, "指针对齐不足，无法存储颜色位");
#endif
#endif

/* 比较结果枚举。 */
typedef enum {
    MAP_CMP_LESS = -1,
    MAP_CMP_EQUAL = 0,
    MAP_CMP_GREATER = 1
} map_cmp_t;

/* 不透明 map 句柄。 */
typedef struct map_internal *map_t;

/* 树遍历迭代器。 */
typedef struct {
    map_node_t *node; /* 当前节点。 */
    map_node_t *prev; /* 上一个节点，用于删除安全性。 */
    size_t count;     /* 迭代计数。 */
} map_iter_t;

#define map_iter_value(it, type) (*(type *) (it)->node->data)
#define map_iter_key(it, type) (*(type *) (it)->node->key)

/* 整数比较：优化过的无分支版本。 */
FORCE_INLINE map_cmp_t map_cmp_int(const void *arg0, const void *arg1)
{
    const int a = *(const int *) arg0;
    const int b = *(const int *) arg1;
    return (map_cmp_t) ((a > b) - (a < b));
}

/* 无符号整数比较：优化过的无分支版本。 */
FORCE_INLINE map_cmp_t map_cmp_uint(const void *arg0, const void *arg1)
{
    const unsigned int a = *(const unsigned int *) arg0;
    const unsigned int b = *(const unsigned int *) arg1;
    return (map_cmp_t) ((a > b) - (a < b));
}

/* 构造函数：创建新的 map 实例。
 * @param key_size: key 类型的字节大小
 * @param data_size: value 类型的字节大小
 * @param cmp: 用于排序 key 的比较函数
 * @return: 新 map 实例；分配失败时返回 NULL
 */
map_t map_new(size_t key_size,
              size_t data_size,
              map_cmp_t (*cmp)(const void *, const void *));

/* 向 map 插入 key/value 对。
 * @param obj: map 实例
 * @param key: 指向 key 数据的指针
 * @param val: 指向 value 数据的指针
 * @return: 插入成功返回 true；key 已存在返回 false
 */
bool map_insert(map_t obj, const void *key, const void *val);

/* 在 map 中查找 key。
 * @param obj: map 实例
 * @param it: 保存结果的迭代器
 * @param key: 要查找的 key
 */
void map_find(map_t obj, map_iter_t *it, const void *key);

/* 检查 map 是否为空。
 * @param obj: map 实例
 * @return: 为空返回 true，否则返回 false
 */
bool map_empty(map_t obj);

/* 检查迭代器是否位于末尾。
 * @param m: map 实例
 * @param it: 待检查迭代器
 * @return: 位于末尾返回 true；仍有效返回 false
 */
bool map_at_end(map_t m, const map_iter_t *it);

/* 移除迭代器当前位置的节点。
 * @param obj: map 实例
 * @param it: 指向待移除节点的迭代器
 */
void map_erase(map_t obj, map_iter_t *it);

/* 移除 map 中所有节点。
 * @param obj: map 实例
 */
void map_clear(map_t obj);

/* 销毁 map 并释放所有资源。
 * @param obj: 待销毁的 map 实例
 */
void map_delete(map_t obj);

/* 带类型安全的 map 初始化便利宏。 */
#define map_init(key_type, element_type, cmp_func) \
    map_new(sizeof(key_type), sizeof(element_type), cmp_func)

/* 获取 map 大小（元素数量）。
 * @param obj: map 实例
 * @return: map 中的元素数量
 */
size_t map_size(map_t obj);

/* 获取第一个元素（最小 key）的迭代器。
 * @param map: map 实例
 * @param it: 待初始化迭代器
 */
void map_first(map_t map, map_iter_t *it);

/* 获取最后一个元素（最大 key）的迭代器。
 * @param map: map 实例
 * @param it: 待初始化迭代器
 */
void map_last(map_t map, map_iter_t *it);

/* 将迭代器移动到下一个元素（中序遍历）。
 * @param map: map 实例
 * @param it: 待前进的迭代器
 */
void map_next(map_t map, map_iter_t *it);

/* 将迭代器移动到上一个元素（反向中序遍历）。
 * @param map: map 实例
 * @param it: 待后退的迭代器
 */
void map_prev(map_t map, map_iter_t *it);
