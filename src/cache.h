/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/*
 * JIT 基本块缓存接口。
 *
 * cache_t 管理 PC/SATP 到 block_t 的映射、热点计数和系统模式下的页级失效索引。
 * JIT、T2C 和 SFENCE.VMA/FENCE.I 路径通过这些 API 查询、插入、清理缓存块。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* THRESHOLD 用于判定热点基本块。
 * 某个基本块的使用频次超过该阈值后，会触发一级 JIT 编译流程。
 */
#define THRESHOLD 4096

struct cache;

/** cache_create - 创建一个新的缓存对象
 * @size_bits: 缓存容量为 2^size_bits
 * @return: 新缓存对象指针；创建失败时返回 NULL
 */
struct cache *cache_create(uint32_t size_bits);

/**
 * cache_get - 从缓存中查询指定条目
 * @cache: 目标缓存指针
 * @key: 待查询条目的键
 * @update: 是否更新命中频次
 * @return: 命中的条目值；未命中时返回 NULL
 */
void *cache_get(const struct cache *cache, uint32_t key, bool update);

/**
 * cache_put - 向缓存插入新条目
 * @cache: 目标缓存指针
 * @key: 待插入条目的键
 * @value: 待插入条目的值
 * @return: 被替换的旧条目值；没有替换项时返回 NULL
 */
void *cache_put(struct cache *cache, uint32_t key, void *value);

/**
 * cache_free - 释放缓存对象
 * @cache: 目标缓存指针
 */
void cache_free(struct cache *cache);

#if RV32_HAS(JIT)
/**
 * cache_hot - 检查缓存条目的访问频次是否已经超过热点阈值
 * @cache: 目标缓存指针
 * @key: 待查询条目的键
 */
bool cache_hot(const struct cache *cache, uint32_t key);

typedef void (*prof_func_t)(void *, uint32_t, FILE *);
void cache_profile(const struct cache *cache,
                   FILE *output_file,
                   prof_func_t func);
typedef void (*clear_func_t)(void *);
void clear_cache_hot(const struct cache *cache, clear_func_t func);
#endif

uint32_t cache_freq(const struct cache *cache, uint32_t key);

#if RV32_HAS(JIT) && RV32_HAS(SYSTEM)

/**
 * cache_invalidate_satp - 失效所有匹配指定 SATP 的基本块
 * @cache: 目标缓存指针
 * @satp: 用于匹配的 SATP 值
 * @return: 被失效的基本块数量
 *
 * 该接口用于处理 rs1=0 的 SFENCE.VMA（全局刷新），清理可能包含过期
 * VA 到 PA 映射的 JIT 编译基本块。
 */
uint32_t cache_invalidate_satp(struct cache *cache, uint32_t satp);

/**
 * cache_invalidate_va - 失效匹配 SATP 且落在指定虚拟页内的基本块
 * @cache: 目标缓存指针
 * @va: 虚拟地址（函数内部会提取页地址）
 * @satp: 用于匹配的 SATP 值
 * @return: 被失效的基本块数量
 *
 * 该接口用于处理 rs1!=0 的 SFENCE.VMA（按地址刷新），清理指定虚拟页内的
 * JIT 编译基本块。启用 BLOCK_CHAINING 时优先使用 O(1) 页索引查找，
 * 否则退回到 O(n) 全量扫描。
 */
uint32_t cache_invalidate_va(struct cache *cache, uint32_t va, uint32_t satp);

#if RV32_HAS(BLOCK_CHAINING)
/* 按虚拟地址做 O(1) 缓存失效的页索引。
 * 基本块被限制在页边界内时，每个块都完整落在一个 4KB 页中，因此可以按页地址
 * 直接定位，而不必执行 O(n) 扫描。该机制依赖 BLOCK_CHAINING 产生的页终止块
 * （见 emulate.c）。
 */
#define PAGE_INDEX_BITS 10
#define PAGE_INDEX_SIZE (1 << PAGE_INDEX_BITS)
#endif /* RV32_HAS(BLOCK_CHAINING) */

#endif /* RV32_HAS(JIT) && RV32_HAS(SYSTEM) */
