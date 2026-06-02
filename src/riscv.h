/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/*
 * RISC-V 模拟器公共状态和 API。
 *
 * 这里定义寄存器编号、CSR/异常常量、页表位、设备状态、vm_attr_t 配置和 riscv_t
 * 生命周期接口。它是模拟器外部调用者和各内部模块共享的主要类型边界。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "io.h"
#include "log.h"
#include "map.h"

#if RV32_HAS(SYSTEM)
#include "devices/plic.h"
#if RV32_HAS(GOLDFISH_RTC)
#include "devices/rtc.h"
#endif /* RV32_HAS(GOLDFISH_RTC) */
#include "devices/uart.h"
#include "devices/virtio.h"
#endif /* RV32_HAS(SYSTEM) */

#if RV32_HAS(EXT_F)
#define float16_t softfloat_float16_t
#define bfloat16_t softfloat_bfloat16_t
#define float32_t softfloat_float32_t
#define float64_t softfloat_float64_t
#include "softfloat/source/include/softfloat.h"
#undef float16_t
#undef bfloat16_t
#undef float32_t
#undef float64_t
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* clang-format off */
#define RV_REGS_LIST                                   \
    _(zero) /* 硬连线为零，忽略所有写入。 */           \
    _(ra)   /* 返回地址。 */                           \
    _(sp)   /* 栈指针。 */                             \
    _(gp)   /* 全局指针。 */                           \
    _(tp)   /* 线程指针。 */                           \
    _(t0)   /* 临时寄存器/备用链接寄存器。 */          \
    _(t1)   /* 临时寄存器。 */                         \
    _(t2)                                              \
    _(s0) /* 被调用者保存寄存器/帧指针。 */            \
    _(s1)                                              \
    _(a0) /* 函数参数/返回值。 */                      \
    _(a1)                                              \
    _(a2) /* 函数参数。 */                             \
    _(a3)                                              \
    _(a4)                                              \
    _(a5)                                              \
    IIF(RV32_HAS(RV32E))(,                             \
        _(a6)                                          \
        _(a7)                                          \
        _(s2) /* 被调用者保存寄存器。 */               \
        _(s3)                                          \
        _(s4)                                          \
        _(s5)                                          \
        _(s6)                                          \
        _(s7)                                          \
        _(s8)                                          \
        _(s9)                                          \
        _(s10)                                         \
        _(s11)                                         \
        _(t3) /* 临时寄存器。 */                       \
        _(t4)                                          \
        _(t5)                                          \
        _(t6)                                          \
    )
/* clang-format on */

/* RISC-V 寄存器（助记符和 ABI 名称）。
 *
 * RISC-V 有 32 个通用寄存器，此外还有程序计数器 "pc"。
 *
 * 指令编码并不强制指定某个寄存器必须作为栈指针或子程序返回地址；任意 x 寄存器
 * 都可用于这些用途。
 *
 * 不过标准调用约定使用 "x1" 保存调用返回地址，"x5" 作为备用链接寄存器，
 * "x2" 作为栈指针。
 */
/* clang-format off */
enum {
#define _(r) rv_reg_##r,
    RV_REGS_LIST
#undef _
    N_RV_REGS
};
/* clang-format on */

#define RV_PG_SHIFT 12
#define RV_PG_SIZE (1 << RV_PG_SHIFT)

typedef uint32_t pte_t;
#define PTE_V (1U)
#define PTE_R (1U << 1)
#define PTE_W (1U << 2)
#define PTE_X (1U << 3)
#define PTE_U (1U << 4)
#define PTE_G (1U << 5)
#define PTE_A (1U << 6)
#define PTE_D (1U << 7)

/* PTE XWRV 位的有序组合。 */
enum SV32_PTE_PERM {
    NEXT_PG_TBL = 0b0001,
    RO_PAGE = 0b0011,
    RW_PAGE = 0b0111,
    EO_PAGE = 0b1001,
    RX_PAGE = 0b1011,
    RWX_PAGE = 0b1111,
    RESRV_PAGE1 = 0b0101,
    RESRV_PAGE2 = 0b1101,
};

#define MISA_SUPER (1 << ('S' - 'A'))
#define MISA_USER (1 << ('U' - 'A'))
#define MISA_I (1 << ('I' - 'A'))
#define MISA_E (1 << ('E' - 'A'))
#define MISA_M (1 << ('M' - 'A'))
#define MISA_A (1 << ('A' - 'A'))
#define MISA_F (1 << ('F' - 'A'))
#define MISA_C (1 << ('C' - 'A'))

/*
 * mstatus 寄存器跟踪并控制 hart 当前运行状态。使用 enum 可确保这些值是真正的
 * 编译期常量，可用于 switch case、数组大小和 static_assert。
 */
/* clang-format off */
enum {
    /* mstatus 字段：移位值和掩码。 */
    MSTATUS_SIE_SHIFT  = 1,  MSTATUS_SIE  = (1U << 1),
    MSTATUS_MIE_SHIFT  = 3,  MSTATUS_MIE  = (1U << 3),
    MSTATUS_SPIE_SHIFT = 5,  MSTATUS_SPIE = (1U << 5),
    MSTATUS_UBE_SHIFT  = 6,  MSTATUS_UBE  = (1U << 6),
    MSTATUS_MPIE_SHIFT = 7,  MSTATUS_MPIE = (1U << 7),
    MSTATUS_SPP_SHIFT  = 8,  MSTATUS_SPP  = (1U << 8),
    MSTATUS_MPP_SHIFT  = 11, MSTATUS_MPP  = (3U << 11),  /* 2 位字段。 */
    MSTATUS_MPRV_SHIFT = 17, MSTATUS_MPRV = (1U << 17),
    MSTATUS_SUM_SHIFT  = 18, MSTATUS_SUM  = (1U << 18),
    MSTATUS_MXR_SHIFT  = 19, MSTATUS_MXR  = (1U << 19),
    MSTATUS_TVM_SHIFT  = 20, MSTATUS_TVM  = (1U << 20),
    MSTATUS_TW_SHIFT   = 21, MSTATUS_TW   = (1U << 21),
    MSTATUS_TSR_SHIFT  = 22, MSTATUS_TSR  = (1U << 22),
};
/* clang-format on */

/* sstatus：mstatus 的受限视图，位位置相同。 */
#define SSTATUS_SIE_SHIFT MSTATUS_SIE_SHIFT
#define SSTATUS_SPIE_SHIFT MSTATUS_SPIE_SHIFT
#define SSTATUS_UBE_SHIFT MSTATUS_UBE_SHIFT
#define SSTATUS_SPP_SHIFT MSTATUS_SPP_SHIFT
#define SSTATUS_SUM_SHIFT MSTATUS_SUM_SHIFT
#define SSTATUS_MXR_SHIFT MSTATUS_MXR_SHIFT
#define SSTATUS_SIE MSTATUS_SIE
#define SSTATUS_SPIE MSTATUS_SPIE
#define SSTATUS_UBE MSTATUS_UBE
#define SSTATUS_SPP MSTATUS_SPP
#define SSTATUS_SUM MSTATUS_SUM
#define SSTATUS_MXR MSTATUS_MXR
#define SIP_SSIP_SHIFT 1
#define SIP_STIP_SHIFT 5
#define SIP_SEIP_SHIFT 9
#define SIP_SSIP (1 << SIP_SSIP_SHIFT)
#define SIP_STIP (1 << SIP_STIP_SHIFT)
#define SIP_SEIP (1 << SIP_SEIP_SHIFT)

#define RV_PRIV_U_MODE 0
#define RV_PRIV_S_MODE 1
#define RV_PRIV_M_MODE 3
#define RV_PRIV_IS_U_OR_S_MODE() (rv->priv_mode <= RV_PRIV_S_MODE)

#define RV_MVENDORID 0x12345678
#define RV_MARCHID ((1ULL << 31) | 1)
#define RV_MIMPID 1

#define RV_INT_STI_SHIFT 5
#define RV_INT_STI (1 << RV_INT_STI_SHIFT)

/* clang-format off */
enum TRAP_CODE {
#if !RV32_HAS(EXT_C)
    INSN_MISALIGNED = 0,                       /* 指令地址未对齐。 */
#endif /* !RV32_HAS(EXT_C) */
    ILLEGAL_INSN = 2,                          /* 非法指令。 */
    BREAKPOINT = 3,                            /* 断点。 */
    LOAD_MISALIGNED = 4,                       /* 加载地址未对齐。 */
    STORE_MISALIGNED = 6,                      /* 存储/AMO 地址未对齐。 */
#if RV32_HAS(SYSTEM)
    PAGEFAULT_INSN = 12,                       /* 指令页错误。 */
    PAGEFAULT_LOAD = 13,                       /* 加载页错误。 */
    PAGEFAULT_STORE = 15,                      /* 存储页错误。 */
    SUPERVISOR_SW_INTR = (1U << 31) | 1,       /* Supervisor 软件中断。 */
    SUPERVISOR_TIMER_INTR = (1U << 31) | 5,    /* Supervisor 定时器中断。 */
    SUPERVISOR_EXTERNAL_INTR = (1U << 31) | 9, /* Supervisor 外部中断。 */
    ECALL_U = 8,                               /* 来自 U-mode 的环境调用。 */
#endif /* RV32_HAS(SYSTEM) */
#if !RV32_HAS(SYSTEM)
    ECALL_M = 11,              /* 来自 M-mode 的环境调用。 */
#endif /* !RV32_HAS(SYSTEM) */
};
/* clang-format on */

/*
 * 为简化表达，把 m/scause 和 m/stval 分别抽象为 cause 与 tval 标识符。
 */
/* clang-format off */
#define SET_CAUSE_AND_TVAL_THEN_TRAP(rv, cause, tval)                          \
    {                                                                          \
        /*                                                                     \
         * 使 rv32emu 行为与 Spike 对齐。                                      \
         *                                                                     \
         * 非系统模式下应调用 __trap_handler。                                \
         *                                                                     \
         * FIXME：ECALL_U 当前不能直接 trap 到 __trap_handler。                \
         */                                                                    \
        IIF(RV32_HAS(SYSTEM))(if (cause != ECALL_U) rv->is_trapped = true;, ); \
        if (RV_PRIV_IS_U_OR_S_MODE()) {                                        \
            rv->csr_scause = cause;                                            \
            rv->csr_stval = tval;                                              \
        } else {                                                               \
            rv->csr_mcause = cause;                                            \
            rv->csr_mtval = tval;                                              \
        }                                                                      \
        rv->io.on_trap(rv);                                                    \
    }
/* clang-format on */

/*
 * SBI 函数必须返回一对值：
 *
 * struct sbiret {
 *     long error;
 *     long value;
 * };
 *
 * SBI 函数返回后，error 和 value 字段会分别写入寄存器 a0 和 a1。error 字段表示
 * SBI 调用是否成功；SBI_SUCCESS 表示成功，SBI_ERR_NOT_SUPPORTED 表示不支持。
 * value 字段携带由扩展 ID（EID）和 SBI 函数 ID（FID）决定的信息。
 *
 * SBI 参考：https://github.com/riscv-non-isa/riscv-sbi-doc
 */
#define SBI_SUCCESS 0
#define SBI_ERR_NOT_SUPPORTED -2

/*
 * base extension 中的所有函数都必须被所有 SBI 实现支持。
 */
#define SBI_EID_BASE 0x10
#define SBI_BASE_GET_SBI_SPEC_VERSION 0
#define SBI_BASE_GET_SBI_IMPL_ID 1
#define SBI_BASE_GET_SBI_IMPL_VERSION 2
#define SBI_BASE_PROBE_EXTENSION 3
#define SBI_BASE_GET_MVENDORID 4
#define SBI_BASE_GET_MARCHID 5
#define SBI_BASE_GET_MIMPID 6

/* 让 supervisor 调度下一次定时器事件。 */
#define SBI_EID_TIMER 0x54494D45
#define SBI_TIMER_SET_TIMER 0

/* 允许 supervisor 请求系统级重启或关机。 */
#define SBI_EID_RST 0x53525354
#define SBI_RST_SYSTEM_RESET 0

#define BLOCK_MAP_CAPACITY_BITS 10

/* 内部结构的前置声明。 */
typedef struct riscv_internal riscv_t;
typedef void *riscv_user_t;

typedef uint32_t riscv_word_t;
typedef uint16_t riscv_half_t;
typedef uint8_t riscv_byte_t;
typedef uint32_t riscv_exception_t;
#if RV32_HAS(EXT_F)
typedef softfloat_float32_t riscv_float_t;
#endif

/* 内存读取处理器。 */
typedef riscv_word_t (*riscv_mem_ifetch)(riscv_t *rv, riscv_word_t addr);
typedef riscv_word_t (*riscv_mem_read_w)(riscv_t *rv, riscv_word_t addr);
typedef riscv_half_t (*riscv_mem_read_s)(riscv_t *rv, riscv_word_t addr);
typedef riscv_byte_t (*riscv_mem_read_b)(riscv_t *rv, riscv_word_t addr);

/* 内存写入处理器。 */
typedef void (*riscv_mem_write_w)(riscv_t *rv,
                                  riscv_word_t addr,
                                  riscv_word_t data);
typedef void (*riscv_mem_write_s)(riscv_t *rv,
                                  riscv_word_t addr,
                                  riscv_half_t data);
typedef void (*riscv_mem_write_b)(riscv_t *rv,
                                  riscv_word_t addr,
                                  riscv_byte_t data);
#if RV32_HAS(SYSTEM)
/*
 * VA 到 PA 转换处理器。
 * MMU 页表遍历器和 fault 检查器定义在 system.c 中，因此通过函数指针导出该处理器
 * 可以保持 MMU 翻译封装。
 *
 * ifetch 不使用该翻译函数，因为基本块可能被重新翻译，此时对应 PTE 可能为 NULL。
 */
typedef riscv_word_t (*riscv_mem_translate_t)(riscv_t *rv,
                                              riscv_word_t vaddr,
                                              bool rw);

/* T2C 运行时绑定使用的 MMU 内存访问函数指针。
 * 这能避免在 JIT 代码中嵌入编译期函数地址；在 x86-64 Linux 上，这种嵌入会被
 * ASLR 破坏。
 */
typedef riscv_word_t (*riscv_mmu_read_w_t)(riscv_t *rv, riscv_word_t vaddr);
typedef riscv_half_t (*riscv_mmu_read_s_t)(riscv_t *rv, riscv_word_t vaddr);
typedef riscv_byte_t (*riscv_mmu_read_b_t)(riscv_t *rv, riscv_word_t vaddr);
typedef void (*riscv_mmu_write_w_t)(riscv_t *rv,
                                    riscv_word_t vaddr,
                                    riscv_word_t val);
typedef void (*riscv_mmu_write_s_t)(riscv_t *rv,
                                    riscv_word_t vaddr,
                                    riscv_half_t val);
typedef void (*riscv_mmu_write_b_t)(riscv_t *rv,
                                    riscv_word_t vaddr,
                                    riscv_byte_t val);
#endif

/* 系统指令处理器。 */
typedef void (*riscv_on_ecall)(riscv_t *rv);
typedef void (*riscv_on_ebreak)(riscv_t *rv);
typedef void (*riscv_on_memset)(riscv_t *rv);
typedef void (*riscv_on_memcpy)(riscv_t *rv);
typedef void (*riscv_on_trap)(riscv_t *rv);
/* RISC-V 模拟器 I/O 接口。 */
typedef struct {
    /* 内存读取接口。 */
    riscv_mem_ifetch mem_ifetch;
    riscv_mem_read_w mem_read_w;
    riscv_mem_read_s mem_read_s;
    riscv_mem_read_b mem_read_b;

    /* 内存写入接口。 */
    riscv_mem_write_w mem_write_w;
    riscv_mem_write_s mem_write_s;
    riscv_mem_write_b mem_write_b;

#if RV32_HAS(SYSTEM)
    riscv_mem_translate_t mem_translate;

    /* T2C 运行时绑定使用的 MMU 内存访问函数。 */
    riscv_mmu_read_w_t mmu_read_w;
    riscv_mmu_read_s_t mmu_read_s;
    riscv_mmu_read_b_t mmu_read_b;
    riscv_mmu_write_w_t mmu_write_w;
    riscv_mmu_write_s_t mmu_write_s;
    riscv_mmu_write_b_t mmu_write_b;
#endif

    /* 系统回调。 */
    riscv_on_ecall on_ecall;
    riscv_on_ebreak on_ebreak;
    riscv_on_memset on_memset;
    riscv_on_memcpy on_memcpy;
    riscv_on_trap on_trap;
} riscv_io_t;

/* 运行模拟器。 */
void rv_run(riscv_t *rv);

/* 创建 RISC-V 模拟器。 */
riscv_t *rv_create(riscv_user_t attr);

/* 删除 RISC-V 模拟器。 */
void rv_delete(riscv_t *rv);

/* 重置 RISC-V 处理器。 */
void rv_reset(riscv_t *rv, riscv_word_t pc);

#if RV32_HAS(GDBSTUB)
/* 以 gdbstub 模式运行 RISC-V 模拟器。 */
void rv_debug(riscv_t *rv);
#endif

/* 单步推进 RISC-V 模拟器。 */
void rv_step(void *arg);

/* 调试模式下单步推进 RISC-V 模拟器。 */
void rv_step_debug(void *arg);

/* 设置 RISC-V 模拟器的程序计数器。 */
bool rv_set_pc(riscv_t *rv, riscv_word_t pc);

/* 获取 RISC-V 模拟器的程序计数器。 */
riscv_word_t rv_get_pc(riscv_t *rv);

/* 设置 RISC-V 模拟器寄存器。 */
void rv_set_reg(riscv_t *rv, uint32_t reg, riscv_word_t in);

typedef struct {
    int fd;
    FILE *file;
} fd_stream_pair_t;

/* 将标准流重映射到其他流。 */
void rv_remap_stdstream(riscv_t *rv, fd_stream_pair_t *fsp, uint32_t fsp_size);

/* 获取 RISC-V 模拟器寄存器。 */
riscv_word_t rv_get_reg(riscv_t *rv, uint32_t reg);

/* 系统调用处理器。 */
void syscall_handler(riscv_t *rv);

/* 环境调用处理器。 */
void ecall_handler(riscv_t *rv);

/* trap 处理器。 */
void trap_handler(riscv_t *rv);

/* memset 处理器。 */
void memset_handler(riscv_t *rv);

/* memcpy 处理器。 */
void memcpy_handler(riscv_t *rv);

/* 将寄存器以 JSON 形式转储到 out_file_path。 */
void dump_registers(riscv_t *rv, char *out_file_path);

/* 断点异常处理器。 */
void ebreak_handler(riscv_t *rv);

/*
 * 基本块模拟期间可能发生 trap，例如页错误。
 * 为了处理 trap，必须先退出基本块并执行已注册的 trap handler。该 trap_handler
 * 函数会逐 PC 执行已注册的处理器；trap 处理完成后，再恢复到触发 trap 的原执行流。
 *
 * 目前 rv32emu 支持非对齐访问和页错误处理。
 */
void trap_handler(riscv_t *rv);

/* 停止核心。 */
void rv_halt(riscv_t *rv);

/* 返回停止状态。 */
bool rv_has_halted(riscv_t *rv);

#if RV32_HAS(ARCH_TEST)
/* 设置架构测试使用的 tohost/fromhost 地址。 */
void rv_set_tohost_addr(riscv_t *rv, uint32_t addr);
void rv_set_fromhost_addr(riscv_t *rv, uint32_t addr);
#endif

#if RV32_HAS(SYSTEM)
/* SFENCE.VMA 指令和 SATP 变化使用的 TLB 管理函数。
 * 页表被修改时，失效已缓存的地址转换。
 */
void mmu_tlb_flush_all(riscv_t *rv);
void mmu_tlb_flush(riscv_t *rv, uint32_t vaddr);
#endif

enum {
    /* 运行并跟踪指令，在模拟期间打印指令。 */
    RV_RUN_TRACE = 1,

    /* 模拟期间以 gdbstub 模式运行。 */
    RV_RUN_GDBSTUB = 2,

    /* 模拟期间运行并统计基本块关系，保存到 prof_output_file。 */
    RV_RUN_PROFILE = 4,
};

typedef struct {
    char *elf_program;
} vm_user_t;

#if RV32_HAS(SYSTEM)
typedef struct {
    char *kernel;
    char *initrd;
    char *bootargs;
    char **vblk_device;
    int vblk_device_cnt;
} vm_system_t;
#endif /* RV32_HAS(SYSTEM) */

typedef struct {
#if !RV32_HAS(SYSTEM_MMIO)
    vm_user_t user;
#else
    vm_system_t system;
#endif /* !RV32_HAS(SYSTEM_MMIO) */

} vm_data_t;

typedef struct {
#if RV32_HAS(SYSTEM_MMIO)
    /* UART 对象。 */
    u8250_state_t *uart;

    /* PLIC 对象。 */
    plic_t *plic;

#if RV32_HAS(GOLDFISH_RTC)
    /* RTC 对象。 */
    rtc_t *rtc;
#endif /* RV32_HAS(GOLDFISH_RTC) */

    /* virtio-blk 设备。 */
    uint32_t **disk;
    virtio_blk_state_t **vblk;
    virtio_blk_state_t *vblk_curr;
    uint32_t vblk_mmio_base_hi;
    uint32_t vblk_mmio_max_hi;
    int vblk_irq_base;
    int vblk_cnt;
#endif /* RV32_HAS(SYSTEM_MMIO) */

    /* VM 内存对象。 */
    memory_t *mem;

    /* 最大内存大小为 2^32 字节（4GB）。
     * 使用 uint64_t 支持完整 4GB 地址空间和按需分页。物理内存按需分配，保持实际
     * 占用尽量小。
     */
    uint64_t mem_size;

    /* VM 主栈大小。 */
    uint32_t stack_size;

    /* 为了按 RV32 ABI 访问参数列表，需要保存参数数据偏移。
     *
     * args_offset_size 是保存这些偏移所需的内存大小。
     */
    uint32_t args_offset_size;

    /* 模拟程序参数。 */
    int argc;
    char **argv;
    /* FIXME：当前还不能访问 envp。 */

    /* 模拟程序退出码。 */
    int exit_code;

    /* 模拟程序错误码。 */
    int error;

    /* 日志等级。 */
    int log_level;

    /* 用户态或系统模拟数据。 */
    vm_data_t data;

    /* 一次 rv_step 调用中执行的周期（指令）数量。 */
    int cycle_per_step;

    /* 是否允许非对齐内存访问。 */
    bool allow_misalign;

    /* 运行标志，由 RV_RUN_TRACE、RV_RUN_GDBSTUB 和 RV_RUN_PROFILE 按位或组成。
     */
    uint8_t run_flag;

    /* run_flag 设置 RV_RUN_PROFILE 时使用的 profiling 输出文件。 */
    char *profile_output_file;

    /* rv_create 初始化期间设置。
     * 可通过 rv_remap_stdstream 覆盖。
     */
    int fd_stdin, fd_stdout, fd_stderr;

    /* VM 文件描述符映射：int -> (FILE *)。 */
    map_t fd_map;

    /* 数据段 break 地址。 */
    riscv_word_t break_addr;

#if !RV32_HAS(SYSTEM)
    /* 退出入口地址。 */
    riscv_word_t exit_addr;

    /* 标记模拟器是否退出目标程序。 */
    bool on_exit;
#endif

#if RV32_HAS(SDL) && RV32_HAS(SYSTEM_MMIO)
    /* 标记 guestOS 中是否正在运行 SDL 程序。 */
    bool running_sdl;
#endif /* SDL */

    /* SBI 定时器。 */
    uint64_t timer;
} vm_attr_t;

#ifdef __cplusplus
};
#endif
