/*
 * rv32emu 可依据 MIT 许可证自由再分发。使用和再分发规则见 LICENSE 文件。
 */

/* 固定大小对象内存池分配器。
 *
 * 为固定大小对象提供 O(1) 分配/释放。可用时使用 mmap，缺失时退回 malloc。
 * 内存池耗尽后会自动扩展；所有函数都允许传入 NULL 并安全返回。
 */

#pragma once

#include <stddef.h>

struct mpool;

/**
 * mpool_create - 创建内存池。
 * @pool_size: 初始内存池大小，单位字节。
 * @chunk_size: 每个分配单元大小。
 *
 * 返回内存池指针；失败时返回 NULL。
 */
struct mpool *mpool_create(size_t pool_size, size_t chunk_size);

/**
 * mpool_alloc - 从内存池分配一个 chunk。
 * @mp: 内存池，可为 NULL。
 *
 * 返回 chunk 指针；mp 为 NULL 或分配失败时返回 NULL。
 */
void *mpool_alloc(struct mpool *mp);

/**
 * mpool_calloc - 从内存池分配并清零一个 chunk。
 * @mp: 内存池，可为 NULL。
 *
 * 返回已清零 chunk 指针；mp 为 NULL 或分配失败时返回 NULL。
 */
void *mpool_calloc(struct mpool *mp);

/**
 * mpool_free - 把 chunk 归还给内存池。
 * @mp: 内存池，可为 NULL。
 * @target: 待释放 chunk，可为 NULL。
 */
void mpool_free(struct mpool *mp, void *target);

/**
 * mpool_destroy - 销毁内存池并释放所有内存。
 * @mp: 内存池，可为 NULL。
 */
void mpool_destroy(struct mpool *mp);
