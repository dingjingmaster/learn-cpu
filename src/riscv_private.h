/*
 * rv32emu 可依据 MIT 许可证自由再分发。使用和再分发规则见 LICENSE 文件。
 */

/*
 * RISC-V 模拟器内部状态。
 *
 * 该头文件只供内部实现使用，集中定义 block_t、IR 链表、运行时属性、JIT/T2C
 * 缓存状态和系统模式私有字段。外部代码应优先依赖 riscv.h 中的公共接口。
 */

#pragma once
#include <stdbool.h>
#include <string.h>

#if RV32_HAS(GDBSTUB)
#include "breakpoint.h"
#include "mini-gdbstub/include/gdbstub.h"
#endif
#include "decode.h"
#include "riscv.h"
#include "utils.h"
#if RV32_HAS(JIT)
#if RV32_HAS(T2C)
#include <pthread.h>
#endif
#include "cache.h"
#endif

#define PRIV(x) ((vm_attr_t *) x->data)

/* 每个融合槽位的最大条目数，把单次融合限制在 16 条连续指令内。
 * 更长序列较少见，收益也会递减。
 */
#define FUSE_MAX_ENTRIES 16
#define FUSE_SLOT_SIZE (FUSE_MAX_ENTRIES * sizeof(opcode_fuse_t))

/* CSR 编号。 */
enum {
    /* 浮点 CSR。 */
    CSR_FFLAGS = 0x001, /* 浮点累计异常标志。 */
    CSR_FRM = 0x002,    /* 浮点动态舍入模式。 */
    CSR_FCSR = 0x003,   /* 浮点控制和状态寄存器。 */

    /* S 模式 trap 设置。 */
    CSR_SSTATUS = 0x100,    /* S 模式状态寄存器。 */
    CSR_SIE = 0x104,        /* S 模式中断使能寄存器。 */
    CSR_STVEC = 0x105,      /* S 模式 trap 处理器基址。 */
    CSR_SCOUNTEREN = 0x106, /* S 模式计数器使能。 */

    /* S 模式 trap 处理。 */
    CSR_SSCRATCH = 0x140, /* S 模式 scratch 寄存器。 */
    CSR_SEPC = 0x141,     /* S 模式异常 PC。 */
    CSR_SCAUSE = 0x142,   /* S 模式 trap 原因。 */
    CSR_STVAL = 0x143,    /* S 模式异常地址或指令。 */
    CSR_SIP = 0x144,      /* S 模式待处理中断。 */

    /* S 模式地址转换和保护。 */
    CSR_SATP = 0x180, /* S 模式地址转换与保护寄存器。 */

    /* M 模式信息寄存器。 */
    CSR_MVENDORID = 0xF11, /* 厂商 ID。 */
    CSR_MARCHID = 0xF12,   /* 架构 ID。 */
    CSR_MIMPID = 0xF13,    /* 实现 ID。 */
    CSR_MHARTID = 0xF14,   /* 硬件线程 ID。 */

    /* M 模式 trap 设置。 */
    CSR_MSTATUS = 0x300,    /* M 模式状态寄存器。 */
    CSR_MISA = 0x301,       /* ISA 与扩展位。 */
    CSR_MEDELEG = 0x302,    /* M 模式异常委托寄存器。 */
    CSR_MIDELEG = 0x303,    /* M 模式中断委托寄存器。 */
    CSR_MIE = 0x304,        /* M 模式中断使能寄存器。 */
    CSR_MTVEC = 0x305,      /* M 模式 trap 处理器基址。 */
    CSR_MCOUNTEREN = 0x306, /* M 模式计数器使能。 */

    /* M 模式 trap 处理。 */
    CSR_MSCRATCH = 0x340, /* M 模式 scratch 寄存器。 */
    CSR_MEPC = 0x341,     /* M 模式异常 PC。 */
    CSR_MCAUSE = 0x342,   /* M 模式 trap 原因。 */
    CSR_MTVAL = 0x343,    /* M 模式异常地址或指令。 */
    CSR_MIP = 0x344,      /* M 模式待处理中断。 */

    /* 低 32 位计数器。 */
    CSR_CYCLE = 0xC00, /* RDCYCLE 使用的周期计数器。 */
    CSR_TIME = 0xC01,  /* RDTIME 使用的时间计数器。 */
    CSR_INSTRET = 0xC02,

    /* 高 32 位计数器。 */
    CSR_CYCLEH = 0xC80,
    CSR_TIMEH = 0xC81,
    CSR_INSTRETH = 0xC82,
};

/* SYSTEM_MMIO 模式下的内存操作懒融合候选。
 * 解码阶段先把连续 LW/SW 序列标记为候选；首次执行时确认地址属于 RAM 而非 MMIO；
 * 只有已验证安全的序列才会在后续执行中真正融合。
 */
#if RV32_HAS(SYSTEM_MMIO) && RV32_HAS(MOP_FUSION)
typedef struct {
    rv_insn_t *ir;  /**< 候选序列第一条指令。 */
    uint8_t count;  /**< 连续指令数量。 */
    uint8_t opcode; /**< rv_insn_lw or rv_insn_sw */
    bool verified;  /**< 首次执行时已确认地址属于 RAM。 */
    bool failed;    /**< 已发现至少一个 MMIO 地址。 */
} lazy_fusion_candidate_t;

#define MAX_LAZY_CANDIDATES 8
#endif

/* 已翻译基本块。 */
typedef struct block {
    uint32_t n_insn;           /**< 基本块包含的指令数量。 */
    uint32_t pc_start, pc_end; /**< 基本块地址范围。 */
    uint32_t cycle_cost;       /**< 块级计数使用的周期成本。 */

    rv_insn_t *ir_head, *ir_tail; /**< 本基本块首尾 IR。 */

#if RV32_HAS(BLOCK_CHAINING)
    bool page_terminated; /**< 基本块在页边界结束，而非分支结束。 */
#endif

#if RV32_HAS(SYSTEM_MMIO) && RV32_HAS(MOP_FUSION)
    uint8_t n_lazy_candidates; /**< 懒融合候选数量。 */
    lazy_fusion_candidate_t lazy_candidates[MAX_LAZY_CANDIDATES];
    bool lazy_fusion_done; /**< 是否已尝试懒融合。 */
#endif

#if RV32_HAS(JIT)
    bool hot;          /**< 是否是潜在热点块。 */
    bool hot2;         /**< 是否是强热点块。 */
    bool translatable; /**< 是否可翻译为 JIT 代码。 */
    bool has_loops;    /**< 是否包含循环。 */
#if RV32_HAS(SYSTEM)
    uint32_t satp;
    bool invalidated; /**< 被 SFENCE.VMA 失效，需要重新编译。 */
#endif
#if RV32_HAS(T2C)
    bool compiled;     /**< T2C 请求是否已入队。 */
    bool is_compiling; /**< T2C 线程是否正在处理该块。 */
    bool should_free;  /**< 编译期间被淘汰，需由 T2C 释放。 */
#endif
    uint32_t offset;   /**< T1 代码缓存中的机器码偏移。 */
    uint32_t n_invoke; /**< T1 机器码调用次数。 */
    void *func;        /**< T2 机器码函数指针。 */
#if RV32_HAS(T2C)
    void *llvm_engine; /**< LLVM execution engine，用于保持 func 内存存活。 */
#endif
    struct list_head list;
#endif
} block_t;

/* T2C 蕴含 JIT，该约束由 Kconfig 和 feature.h 保证。 */
#if RV32_HAS(T2C)
typedef struct {
    uint64_t key; /**< 查找基本块的缓存 key（PC 或 PC|SATP）。 */
    struct list_head list;
} queue_entry_t;
#endif

#if RV32_HAS(SYSTEM)
/* 缓存 VA 到 PA 转换结果的 TLB，可降低系统模拟模式下的页表遍历开销。
 *
 * TLB 设计：
 * - 直接映射缓存，用 VPN 低位索引。
 * - 64 个条目，约覆盖 256KB 工作集。
 * - 每个条目缓存 VPN、PPN、权限和页级别。
 * - 分离 dTLB（数据）和 iTLB（指令），提升命中率。
 * - 通过 level 字段跟踪超级页，支持选择性 SFENCE.VMA。
 * - SFENCE.VMA 或 SATP 变化时失效。
 */
#define TLB_SIZE 64
#define TLB_MASK (TLB_SIZE - 1)

/* Sv32 页级别：1 = 4MB 超级页，2 = 4KB 普通页。 */
#define TLB_PAGE_LEVEL_SUPER 1
#define TLB_PAGE_LEVEL_4K 2

typedef struct {
    uint32_t vpn, ppn; /* 虚拟/物理页号。 */
    uint32_t pte_addr; /* PTE 物理地址，用于更新 A/D 位。 */
    uint8_t perm;      /* 权限位：R(1)、W(2)、X(4)、U(16)。 */
    uint8_t valid;     /* 条目有效标志。 */
    uint8_t dirty;     /* 缓存 dirty 状态，避免重复写 PTE。 */
    uint8_t level;     /* 页级别：1=超级页，2=4KB 页。 */
} tlb_entry_t;
#endif

typedef struct {
    uint32_t block_capacity; /**< 基本块表最大条目数。 */
    uint32_t size;           /**< 当前条目数。 */
    block_t **map;           /**< 基本块表。 */
} block_map_t;

/* 用于快速基本块查找的 L1 直接映射缓存。
 * 设计参考 rvdbt 的 tcache.h。通过避免热点循环中的哈希表查找，预期提升 5-15%。
 *
 * 设计：tag 和指针数组分离以提升缓存效率。
 * - tag 数组约 1KB，优先检查，适合 miss 路径。
 * - 指针数组约 2KB，仅命中时加载。
 * - x86-64 基准中比分散交错布局更快（+11.9% vs +8.8%）。
 */
#define BLOCK_L1_SIZE 256
#define BLOCK_L1_MASK (BLOCK_L1_SIZE - 1)

/* L1 缓存查找使用的索引位移。
 * 启用 EXT_C 时 PC 可能半字对齐，右移 1 使用 bit 1；未启用 EXT_C 时 PC 字对齐，
 * 右移 2。正确位移可减少压缩指令代码中的冲突 miss。
 */
#if RV32_HAS(EXT_C)
#define BLOCK_L1_INDEX_SHIFT 1
#else
#define BLOCK_L1_INDEX_SHIFT 2
#endif

/* 用于对齐的缓存行大小，典型 x86/Arm64 为 64 字节。 */
#define CACHE_LINE_SIZE 64

/* 无效 tag 哨兵值，保证不会匹配任何有效 PC。
 * 有效 RISC-V PC 按字对齐（启用 C 扩展时按半字对齐），因此低位被置位的值一定无效。
 */
#define BLOCK_L1_INVALID_TAG 0xFFFFFFFFu

/* tag 和指针分离的 L1 基本块缓存，用于提升缓存效率。
 * 先检查约 1KB 的 tag 数组，命中后才加载约 2KB 的指针数组；x86-64 基准中分离
 * 布局比分散交错布局更快。
 */
typedef struct {
    uint32_t tags[BLOCK_L1_SIZE]; /**< 用于快速比较的 PC tag。 */
    block_t *ptrs[BLOCK_L1_SIZE]; /**< 基本块指针，tag 命中后加载。 */
} block_l1_cache_t;

/* 清空基本块哈希表中的所有基本块。 */
void block_map_clear(riscv_t *rv);

struct riscv_internal {
    bool halt; /**< 核心是否已经停止。 */

    /* 整数寄存器；AArch64 编码器需要能用 9 位有符号偏移访问。 */
    riscv_word_t X[N_RV_REGS];
    riscv_word_t PC;

    uint64_t timer; /**< 单调递增定时器。 */

#if RV32_HAS(SYSTEM)
    /* is_trapped 必须位于 256 字节偏移内，供 ARM64 JIT 访问。 */
    bool is_trapped;
#endif

#if !RV32_HAS(JIT)
    /* L1 基本块缓存：tag/指针分离以提升缓存效率。
     * 先检查 tag（约 1KB），命中后才加载指针（约 2KB）；放在热点字段附近，服务
     * 解释器快路径。
     */
    block_l1_cache_t block_l1 __ALIGNED(CACHE_LINE_SIZE);
#endif

#if RV32_HAS(JIT) && RV32_HAS(SYSTEM)
    /*
     * AArch64 编码器只接受 9 位有符号偏移，不要把该结构体移到更靠后的位置。
     */
    struct {
        uint8_t is_mmio; /* 是否为 MMIO（0=RAM，1=MMIO/trap）。 */
        uint32_t type;   /* MMIO 处理器需要的指令类型。 */
        uint32_t vaddr;
        uint32_t paddr;
        uint32_t pc; /* 指令 PC，用于 trap 返回地址。 */
    } jit_mmu;
#endif
    /* 用户提供的属性数据。 */
    riscv_user_t data;

    /* I/O 接口。 */
    riscv_io_t io;

#if RV32_HAS(EXT_F)
    /* 浮点寄存器。 */
    riscv_float_t F[32];
    uint32_t csr_fcsr;
#endif

    /* CSR 寄存器。 */
    uint64_t csr_cycle;     /* M 模式周期计数器。 */
    uint32_t csr_time[2];   /* 性能/时间计数器。 */
    uint32_t csr_mstatus;   /* M 模式状态寄存器。 */
    uint32_t csr_mtvec;     /* M 模式 trap 处理器基址。 */
    uint32_t csr_misa;      /* ISA 与扩展位。 */
    uint32_t csr_mtval;     /* M 模式异常地址或指令。 */
    uint32_t csr_mcause;    /* M 模式 trap 原因。 */
    uint32_t csr_mscratch;  /* M 模式 trap 处理器 scratch。 */
    uint32_t csr_mepc;      /* M 模式异常 PC。 */
    uint32_t csr_mip;       /* M 模式待处理中断。 */
    uint32_t csr_mie;       /* M 模式中断使能。 */
    uint32_t csr_mideleg;   /* M 模式中断委托寄存器。 */
    uint32_t csr_medeleg;   /* M 模式异常委托寄存器。 */
    uint32_t csr_mvendorid; /* 厂商 ID。 */
    uint32_t csr_marchid;   /* 架构 ID。 */
    uint32_t csr_mimpid;    /* 实现 ID。 */
    uint32_t csr_mbadaddr;

    uint32_t csr_sstatus;    /* S 模式状态寄存器。 */
    uint32_t csr_stvec;      /* S 模式 trap 向量基址。 */
    uint32_t csr_sip;        /* S 模式待处理中断。 */
    uint32_t csr_sie;        /* S 模式中断使能。 */
    uint32_t csr_scounteren; /* S 模式计数器使能。 */
    uint32_t csr_sscratch;   /* S 模式 scratch 寄存器。 */
    uint32_t csr_sepc;       /* S 模式异常 PC。 */
    uint32_t csr_scause;     /* S 模式 trap 原因。 */
    uint32_t csr_stval;      /* S 模式 trap 值。 */
    uint32_t csr_satp;       /* S 模式地址转换和保护。 */

    uint32_t priv_mode; /* U/S/M 特权模式。 */

    bool compressed; /**< 当前指令是否为压缩指令。 */
#if !RV32_HAS(JIT)
    block_map_t block_map; /**< 基本块表，L1 miss 时回退查找。 */
#else
    struct cache *block_cache;
    struct list_head block_list; /**< 已翻译基本块链表。 */
#if RV32_HAS(T2C)
    struct list_head wait_queue;
    pthread_mutex_t wait_queue_lock, cache_lock;
    pthread_cond_t wait_queue_cond;
    bool quit; /**< 退出标志，由 wait_queue_lock 保护。 */
#endif
    void *jit_state;
    void *jit_cache;
#if RV32_HAS(T2C)
    void *inline_cache; /* 用于快速解析间接跳转的 inline cache。 */
#endif
#endif
    struct mpool *block_mp, *block_ir_mp, *fuse_mp;

#if RV32_HAS(GDBSTUB)
    /* gdbstub 实例。 */
    gdbstub_t gdbstub;

    bool debug_mode;

    /* GDB 指令断点表。 */
    breakpoint_map_t breakpoint_map;

    /* GDB 客户端中断通知标志；启动 GDBSTUB 后应通过原子操作访问。
     */
    bool is_interrupted;
#endif

#if RV32_HAS(SYSTEM)
    /* 保存 trap 点的 SEPC CSR，用于正确信号处理。
     */
    uint32_t last_csr_sepc;

    /* 数据 TLB，缓存虚拟地址到物理地址的转换结果，降低重复内存访问的页表遍历开销。
     */
    tlb_entry_t dtlb[TLB_SIZE];

    /* 指令 TLB，缓存取指地址转换。与 dTLB 分离可提升命中率并简化权限检查。
     */
    tlb_entry_t itlb[TLB_SIZE];

    /* 从 cycle 计数器派生 timer 的偏移。
     * timer = csr_cycle + timer_offset
     * 这样主循环无需每条指令都递增 timer。
     *
     * 注意：RISC-V 规范把 TIME 定义为独立于 MTIME 硬件的实时时钟计数器。本模拟器
     * 通过 CYCLE 近似派生 TIME，适合模拟用途，但与真实硬件不同；真实 TIME 不应
     * 受 CPU 频率缩放或睡眠状态影响。
     */
    uint64_t timer_offset;
#endif

#if RV32_HAS(ARCH_TEST)
    /* RISC-V 架构测试支持：tohost/fromhost 地址。 */
    uint32_t tohost_addr;
    uint32_t fromhost_addr;
#endif
};

/* 对 16 位值做符号扩展。 */
FORCE_INLINE uint32_t sign_extend_h(const uint32_t x)
{
    return (int32_t) ((int16_t) x);
}

/* 对 8 位值做符号扩展。 */
FORCE_INLINE uint32_t sign_extend_b(const uint32_t x)
{
    return (int32_t) ((int8_t) x);
}

/* 判断指令是否为 RV32C 压缩指令。 */
FORCE_INLINE bool is_compressed(uint32_t insn)
{
    return (insn & FC_OPCODE) != 3;
}

/* RAM 快路径内存访问。
 *
 * 用户态模拟（非 SYSTEM 模式）中，内存访问总是落到 RAM；此时 I/O 回调间接调用
 * 只是额外开销。这些 inline 函数直接访问内存，绕过函数指针分派。
 *
 * 性能收益：消除间接调用开销；在现代 CPU 上，分支预测惩罚通常会让每次内存访问
 * 多花约 5-10 个周期。
 *
 * 这些函数不做边界检查，假设 addr 一定位于 mem_base 内。该不变量由以下条件保证：
 * 1. ELF 加载器校验：所有程序段必须位于 RAM 范围内；见 elf.c 的 elf_load()。
 * 2. 内存布局：mem_base 按配置内存映射分配了足够空间；见 riscv.c 的 memory_new()。
 * 3. 栈/堆边界：初始 SP 和 brk 上限都位于 RAM 内。
 *
 * 如果放宽 ELF 加载器校验或内存分配逻辑，必须在这里补充显式边界检查。越界访问会
 * 导致未定义行为，可能破坏宿主内存。
 *
 * 注意：仅在关闭 SYSTEM 模式时使用。SYSTEM 模式需要 MMU/TLB 转换，必须经过
 * I/O 回调。
 */
#if !RV32_HAS(SYSTEM)
FORCE_INLINE uint32_t ram_read_w(const riscv_t *rv, uint32_t addr)
{
    vm_attr_t *attr = PRIV(rv);
    uint32_t val;
    memcpy(&val, attr->mem->mem_base + addr, sizeof(val));
    return val;
}

FORCE_INLINE uint16_t ram_read_s(const riscv_t *rv, uint32_t addr)
{
    vm_attr_t *attr = PRIV(rv);
    uint16_t val;
    memcpy(&val, attr->mem->mem_base + addr, sizeof(val));
    return val;
}

FORCE_INLINE uint8_t ram_read_b(const riscv_t *rv, uint32_t addr)
{
    vm_attr_t *attr = PRIV(rv);
    return attr->mem->mem_base[addr];
}

FORCE_INLINE void ram_write_w(const riscv_t *rv, uint32_t addr, uint32_t val)
{
    vm_attr_t *attr = PRIV(rv);
    memcpy(attr->mem->mem_base + addr, &val, sizeof(val));
}

FORCE_INLINE void ram_write_s(const riscv_t *rv, uint32_t addr, uint16_t val)
{
    vm_attr_t *attr = PRIV(rv);
    memcpy(attr->mem->mem_base + addr, &val, sizeof(val));
}

FORCE_INLINE void ram_write_b(const riscv_t *rv, uint32_t addr, uint8_t val)
{
    vm_attr_t *attr = PRIV(rv);
    attr->mem->mem_base[addr] = val;
}
#endif /* !RV32_HAS(SYSTEM) */
