/*
 * rv32emu 可依据 MIT 许可证自由再分发。使用和再分发规则见 LICENSE 文件。
 */

/*
 * 一级 JIT 对外接口。
 *
 * 解释器热点路径调用 jit_translate 生成宿主机器码；系统模式可通过 clear_hot、
 * jit_mmu_handler 等接口响应页表和 MMIO 行为。具体指令发射细节在 jit.c 和
 * rv32_jit.c 中实现。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "riscv_private.h"
#include "utils.h"

/* 分支指令代码生成使用的跳转条件码。
 * 数值与 x86-64 Jcc opcode 一致；在 Arm64 上，emit_jcc_offset() 会把它们映射
 * 到等价条件码。两种架构都把这些值当作条件/无条件跳转的符号常量。
 */
#define JCC_JE 0x84  /* 相等则跳转，条件跳转。 */
#define JCC_JNE 0x85 /* 不相等则跳转，条件跳转。 */
#define JCC_JL 0x8c  /* 有符号小于则跳转，条件跳转。 */
#define JCC_JGE 0x8d /* 有符号大于等于则跳转，条件跳转。 */
#define JCC_JB 0x82  /* 无符号小于则跳转，条件跳转。 */
#define JCC_JAE 0x83 /* 无符号大于等于则跳转，条件跳转。 */
#define JCC_JMP 0xe9 /* 无条件跳转。 */

struct jump {
    uint32_t offset_loc;
    uint32_t target_pc;
    uint32_t target_offset;
#if RV32_HAS(SYSTEM)
    uint32_t target_satp;
#endif
};

struct offset_map {
    uint32_t pc;
    uint32_t offset;
#if RV32_HAS(SYSTEM)
    uint32_t satp;
#endif
};

struct jit_state {
    set_t set;
    uint8_t *buf;
    uint32_t offset;
    uint32_t stack_size;
    uint32_t size;
    uint32_t entry_loc;
    uint32_t exit_loc;
    uint32_t org_size; /* prologue 和 epilogue 总大小。 */
    uint32_t retpoline_loc;
    struct offset_map *offset_map;
    int n_blocks;
    struct jump *jumps;
    int n_jumps;
};

struct host_reg {
    uint8_t reg_idx : 5;   /* 宿主寄存器文件索引。 */
    int8_t vm_reg_idx : 6; /* VM 寄存器索引。 */
    bool dirty : 1;        /* 该寄存器内容是否已被覆盖。 */
    bool alive : 1;        /* 当前基本块后续是否仍会使用该寄存器。 */
};

struct jit_state *jit_state_init(size_t size);
void jit_state_exit(struct jit_state *state);
void jit_translate(riscv_t *rv, block_t *block);
typedef void (*exec_block_func_t)(riscv_t *rv, uintptr_t);

/* JIT 非对齐内存访问处理器。
 * 通过字节级内存访问完成非对齐 load/store。
 */
void jit_misaligned_handler(riscv_t *rv,
                            uint32_t addr,
                            uint32_t vreg_idx,
                            uint32_t type,
                            bool is_store);

#if RV32_HAS(T2C)
void t2c_compile(riscv_t *, block_t *, pthread_mutex_t *);
typedef void (*exec_t2c_func_t)(riscv_t *);

/* jit-cache 记录 T2C 生成的可执行代码入口和程序计数器。类似硬件缓存，同槽位的
 * 旧条目会被新条目替换。
 */

/* jit-cache 表大小应为 2 的幂，这样可以通过屏蔽 PC 低位快速索引条目。
 */
#define N_JIT_CACHE_ENTRIES (1 << 12)

/* 用于间接跳转快速解析的 inline cache。
 * 每个调用点保存最近使用的 (target, entry) 对。对于返回、vtable 等稳定分支模式，
 * 命中率通常高于 90%。
 *
 * 线程模型：inline cache 属于单个 riscv_t 实例，只由主执行线程访问，不需要同步；
 * 它是单写/单读模式。T2C 编译线程只更新受 seqlock 保护的 jit_cache，不访问
 * inline cache。
 *
 * 内存模型：这里使用非原子 load/store，原因是：
 * 1. 每个 riscv_t 实例内都是单线程访问。
 * 2. T2C 要求 64 位宿主（下方 static_assert），可保证 key 字段 64 位加载不会撕裂。
 *
 * satp 稳定性：系统模式下 key 包含 satp。satp 只会通过 CSR 写入改变，而 CSR 写入
 * 与执行串行化；inline cache 也始终使用当前 satp 检查，因此是安全的。
 *
 * 失效场景：
 * - 基本块淘汰（emulate.c 中的 inline_cache_clear_key）。
 * - SFENCE.VMA（rv32_template.c 中的 inline_cache_clear_page）。
 * - FENCE.I / code_cache_flush（jit.c 中的 inline_cache_clear）。
 *
 * ARM64 上命中时跳过 ISB，因为该目标此前已经成功执行，当时指令缓存已保持一致。
 */
#define N_INLINE_CACHE_ENTRIES (1 << 10)

struct inline_cache {
    uint64_t key; /* 目标 PC；系统模式下加上 satp<<32；0 表示空。 */
    void *entry;  /* 缓存的函数指针。 */
};

/* 校验 inline_cache 结构布局，供 LLVM IR 生成使用。
 * LLVM 类型：{ i64 key, ptr entry }
 * 偏移：key=0, entry=8
 *
 * T2C 要求 64 位宿主，以确保 64 位 key 加载是原子的，不会撕裂。32 位宿主上的
 * 非原子 64 位加载可能读到部分更新。
 */
static_assert(sizeof(void *) == 8,
              "T2C inline cache 需要 64 位宿主以原子加载 key");
static_assert(offsetof(struct inline_cache, key) == 0,
              "inline_cache.key 必须位于偏移 0");
static_assert(offsetof(struct inline_cache, entry) == 8,
              "inline_cache.entry 必须位于偏移 8");
static_assert(sizeof(struct inline_cache) == 16,
              "inline_cache 必须为 16 字节");

struct inline_cache *inline_cache_init(void);
void inline_cache_exit(struct inline_cache *cache);
void inline_cache_clear(struct inline_cache *cache);
void inline_cache_clear_key(struct inline_cache *cache, uint64_t key);
void inline_cache_clear_page(struct inline_cache *cache,
                             uint32_t va,
                             uint32_t satp);

/* T2C 编译代码查找使用的 jit_cache 条目。
 * 线程安全：使用 seqlock 模式支持无锁读者。
 * - 写者（持 cache_lock）：先把 seq 递增为奇数，写入 entry+key，再递增为偶数。
 * - 无锁读者（LLVM 生成）：读取 seq1，若为奇数则重试；读取 entry+key；读取
 *   seq2；若 seq1 != seq2 则重试。seqlock 保证读者看到一致的 (key, entry) 对，
 *   且不会遇到 ABA 问题。
 */
struct jit_cache {
    uint32_t seq; /* 序列计数器：奇数表示写入中，偶数表示稳定。 */
    uint64_t key; /* 程序计数器；系统模拟中会组合 satp。 */
    void *entry;  /* JIT 后代码入口。 */
};

/* 校验无锁 seqlock 读取和 LLVM IR 所需的结构布局。
 *
 * jit_cache 结构布局必须匹配 t2c.c 中的 LLVM 类型定义：
 *   LLVM: { i32 seq, i32 pad, i64 key, ptr entry }
 *   偏移：seq=0, pad=4, key=8, entry=16（64 位）。
 *
 * key 字段必须 8 字节对齐，以便不依赖 libatomic 就能原子加载 64 位值。entry 字段
 * 必须指针对齐。
 *
 * 如果此结构布局变化，必须同步更新 t2c.c 中的 LLVM 类型（搜索 jit_cache_memb）。
 */
static_assert(offsetof(struct jit_cache, seq) == 0,
              "jit_cache.seq 必须位于偏移 0 以匹配 LLVM IR");
static_assert(offsetof(struct jit_cache, key) == 8,
              "jit_cache.key 必须位于偏移 8 以匹配 LLVM IR");
static_assert(offsetof(struct jit_cache, entry) == 16,
              "jit_cache.entry 必须位于偏移 16 以匹配 LLVM IR");
static_assert(offsetof(struct jit_cache, key) % 8 == 0,
              "jit_cache.key 必须 8 字节对齐以支持原子加载");
static_assert(offsetof(struct jit_cache, entry) % sizeof(void *) == 0,
              "jit_cache.entry 必须指针对齐以支持原子加载");

struct jit_cache *jit_cache_init();
void jit_cache_exit(struct jit_cache *cache);
void jit_cache_update(struct jit_cache *cache, uint64_t key, void *entry);
void jit_cache_clear(struct jit_cache *cache);
void jit_cache_clear_page(struct jit_cache *cache, uint32_t va, uint32_t satp);

/* T2C 编译块释放时销毁 LLVM execution engine。 */
void t2c_dispose_engine(void *engine);

/* 缓存清理包装函数：从基本块中销毁 LLVM engine。 */
void t2c_dispose_block_engine(void *block);
#endif
