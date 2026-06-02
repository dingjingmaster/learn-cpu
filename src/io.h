/*
 * rv32emu 可依据 MIT 许可证自由再分发。使用和再分发规则见 LICENSE 文件。
 */

/*
 * 客体内存接口。
 *
 * memory_t 描述连续的 RV32 客体物理/虚拟内存区域。这里声明直接内存读写、
 * 指令取指、内存使用量统计和按需分页 GC 接口；系统模式的 MMU 会在转换后调用
 * 这些底层访问函数。
 */

#pragma once

#include <stdint.h>
#include <string.h>

/* 客体主内存。 */
typedef struct {
    uint8_t *mem_base;
    uint64_t mem_size;
} memory_t;

/* 检查地址区间 [addr, addr+size) 是否位于客体 RAM 内。
 * 这能防止靠近内存边界的多字节访问越界；size=0 可用于仅校验地址本身。
 */
#define GUEST_RAM_CONTAINS(mem, addr, size) \
    ((uint64_t) (addr) < (mem)->mem_size && \
     (uint64_t) (size) <= (mem)->mem_size - (uint64_t) (addr))

/* 创建内存实例。 */
memory_t *memory_new(uint64_t size);

/* 销毁内存实例。 */
void memory_delete(memory_t *m);

/* 回收未使用内存页（增量 GC）。 */
void memory_gc(void);

/* 获取峰值物理内存使用量，单位字节。
 * 使用 mmap 时统计按需分页后的实际物理内存；不用 mmap 时统计已分配总量。
 */
uint64_t memory_get_usage(void);

/* 从内存读取一条指令。 */
uint32_t memory_ifetch(uint32_t addr);

/* 从内存读取 32 位字。 */
uint32_t memory_read_w(uint32_t addr);

/* 从内存读取 16 位半字。 */
uint16_t memory_read_s(uint32_t addr);

/* 从内存读取 8 位字节。 */
uint8_t memory_read_b(uint32_t addr);

/* 从内存读取指定长度数据。 */
void memory_read(const memory_t *m, uint8_t *dst, uint32_t addr, uint32_t size);

/* 向内存写入指定长度数据。 */
static inline bool memory_write(memory_t *m,
                                uint32_t addr,
                                const uint8_t *src,
                                uint32_t size)
{
    /* 边界检查，避免缓冲区溢出。 */
    if (addr >= m->mem_size || size > m->mem_size - addr)
        return false;
    memcpy(m->mem_base + addr, src, size);
    return true;
}

/* 向内存写入 32 位字。 */
void memory_write_w(uint32_t addr, const uint8_t *src);

/* 向内存写入 16 位半字。 */
void memory_write_s(uint32_t addr, const uint8_t *src);

/* 向内存写入 8 位字节。 */
void memory_write_b(uint32_t addr, const uint8_t *src);

/* 用指定字节值填充一段内存。 */
static inline bool memory_fill(memory_t *m,
                               uint32_t addr,
                               uint32_t size,
                               uint8_t val)
{
    /* 边界检查，避免缓冲区溢出。 */
    if (addr >= m->mem_size || size > m->mem_size - addr)
        return false;
    memset(m->mem_base + addr, val, size);
    return true;
}
