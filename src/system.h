/*
 * rv32emu 可依据 MIT 许可证自由再分发。使用和再分发规则见 LICENSE 文件。
 */

/*
 * 系统模式接口。
 *
 * 该头文件声明 MMU、TLB、MMIO、PLIC/UART/RTC/virtio 中断更新和地址转换函数。
 * 只有 CONFIG_SYSTEM 启用时才应被构建。
 */

#pragma once

#if !RV32_HAS(SYSTEM)
#error "只有启用 system 支持时才能构建此文件。"
#endif

#include "devices/plic.h"
#include "devices/uart.h"
#include "riscv_private.h"

#define R 1
#define W 0

/* Linux 内核模拟使用的 MMIO 定义。
 * 仅在关闭 ELF_LOADER 时启用；内核启动路径需要 MMIO，ELF 测试模式不需要。
 */
#if !RV32_HAS(ELF_LOADER)

#define MMIO_R 1
#define MMIO_W 0

enum SUPPORTED_MMIO {
    MMIO_PLIC,
    MMIO_UART,
    MMIO_VIRTIOBLK,
#if RV32_HAS(GOLDFISH_RTC)
    MMIO_RTC,
#endif /* RV32_HAS(GOLDFISH_RTC) */
};

/* clang-format off */
#define MMIO_OP(io, rw)                                                               \
    switch(io){                                                                       \
        case MMIO_PLIC:                                                               \
            IIF(rw)( /* 读 */                                                         \
                mmio_read_val = plic_read(PRIV(rv)->plic, addr & 0x3FFFFFF);          \
                plic_update_interrupts(PRIV(rv)->plic);                               \
                return mmio_read_val;                                                 \
                ,    /* 写 */                                                         \
                plic_write(PRIV(rv)->plic, addr & 0x3FFFFFF, val);                    \
                plic_update_interrupts(PRIV(rv)->plic);                               \
                return;                                                               \
            )                                                                         \
            break;                                                                    \
        case MMIO_UART:                                                               \
            IIF(rw)( /* 读 */                                                         \
                mmio_read_val = u8250_read(PRIV(rv)->uart, addr & 0xFFFFF);           \
                emu_update_uart_interrupts(rv);                                       \
                return mmio_read_val;                                                 \
                ,    /* 写 */                                                         \
                u8250_write(PRIV(rv)->uart, addr & 0xFFFFF, val);                     \
                emu_update_uart_interrupts(rv);                                       \
                return;                                                               \
            )                                                                         \
            break;                                                                    \
        case MMIO_VIRTIOBLK:                                                          \
            IIF(rw)( /* 读 */                                                         \
                mmio_read_val = virtio_blk_read(PRIV(rv)->vblk_curr, addr & 0xFFFFF); \
                emu_update_vblk_interrupts(rv);                                       \
                return mmio_read_val;                                                 \
                ,    /* 写 */                                                         \
                virtio_blk_write(PRIV(rv)->vblk_curr, addr & 0xFFFFF, val);           \
                emu_update_vblk_interrupts(rv);                                       \
                return;                                                               \
            )                                                                         \
            break;                                                                    \
        IIF(RV32_FEATURE_GOLDFISH_RTC)(                                               \
        case MMIO_RTC:                                                                \
            IIF(rw)( /* 读 */                                                         \
                mmio_read_val = rtc_read(PRIV(rv)->rtc, addr & 0xFFFFF);              \
                emu_update_rtc_interrupts(rv);                                        \
                return mmio_read_val;                                                 \
                ,    /* 写 */                                                         \
                rtc_write(PRIV(rv)->rtc, addr & 0xFFFFF, val);                        \
                emu_update_rtc_interrupts(rv);                                        \
                return;                                                               \
            )                                                                         \
            break;                                                                    \
        ,)                                                                            \
        default:                                                                      \
            rv_log_error("未知 MMIO 类型 %d\n", io);                                  \
            break;                                                                    \
    }

#define MMIO_READ()                                                           \
    do {                                                                      \
        uint32_t mmio_read_val;                                               \
        if ((addr >> 28) == 0xF) { /* 位于 0xF_______ 的 MMIO。 */            \
            /* 256 个 1MiB 区域。 */                                          \
            uint32_t hi = (addr >> 20) & MASK(8);                             \
            if (PRIV(rv)->vblk_cnt && hi >= PRIV(rv)->vblk_mmio_base_hi &&    \
                hi <= PRIV(rv)->vblk_mmio_max_hi) {                           \
                PRIV(rv)->vblk_curr =                                         \
                    PRIV(rv)->vblk[hi - PRIV(rv)->vblk_mmio_base_hi];         \
                MMIO_OP(MMIO_VIRTIOBLK, MMIO_R);                              \
            } else {                                                          \
                switch (hi) {                                                 \
                case 0x0:                                                     \
                case 0x2: /* PLIC (0 - 0x3F) */                               \
                    MMIO_OP(MMIO_PLIC, MMIO_R);                               \
                    break;                                                    \
                case 0x40: /* UART */                                         \
                    MMIO_OP(MMIO_UART, MMIO_R);                               \
                    break;                                                    \
                IIF(RV32_FEATURE_GOLDFISH_RTC)(                               \
                case 0x41 : /* RTC */                                         \
                    MMIO_OP(MMIO_RTC, MMIO_R);                                \
                    break;                                                    \
                ,)                                                            \
                default:                                                      \
                    __UNREACHABLE;                                            \
                    break;                                                    \
                }                                                             \
            }                                                                 \
        }                                                                     \
    } while (0)

#define MMIO_WRITE()                                                          \
    do {                                                                      \
        if ((addr >> 28) == 0xF) { /* 位于 0xF_______ 的 MMIO。 */            \
            /* 256 个 1MiB 区域。 */                                          \
            uint32_t hi = (addr >> 20) & MASK(8);                             \
            if (PRIV(rv)->vblk_cnt && hi >= PRIV(rv)->vblk_mmio_base_hi &&    \
                hi <= PRIV(rv)->vblk_mmio_max_hi) {                           \
                PRIV(rv)->vblk_curr =                                         \
                    PRIV(rv)->vblk[hi - PRIV(rv)->vblk_mmio_base_hi];         \
                MMIO_OP(MMIO_VIRTIOBLK, MMIO_W);                              \
            } else {                                                          \
                switch (hi) {                                                 \
                case 0x0:                                                     \
                case 0x2: /* PLIC (0 - 0x3F) */                               \
                    MMIO_OP(MMIO_PLIC, MMIO_W);                               \
                    break;                                                    \
                case 0x40: /* UART */                                         \
                    MMIO_OP(MMIO_UART, MMIO_W);                               \
                    break;                                                    \
                IIF(RV32_FEATURE_GOLDFISH_RTC)(                               \
                case 0x41 : /* RTC */                                         \
                    MMIO_OP(MMIO_RTC, MMIO_W);                                \
                    break;                                                    \
                ,)                                                            \
                default:                                                      \
                    __UNREACHABLE;                                            \
                    break;                                                    \
                }                                                             \
            }                                                                 \
        }                                                                     \
    } while (0)
/* clang-format on */

void emu_update_uart_interrupts(riscv_t *rv);
void emu_update_vblk_interrupts(riscv_t *rv);
#if RV32_HAS(GOLDFISH_RTC)
void emu_update_rtc_interrupts(riscv_t *rv);
#endif /* RV32_HAS(GOLDFISH_RTC) */

#define CHECK_PENDING_SIGNAL(rv, signal_flag)              \
    do {                                                   \
        signal_flag = (rv->csr_sepc != rv->last_csr_sepc); \
    } while (0)

#endif /* !RV32_HAS(ELF_LOADER) */

/*
 * 通知 RVOP 宏：当前发生了内联 trap 处理。该标志置位时，指令应在不推进 PC 的
 * 情况下返回，以便重试。Linux 内核信号处理（修改 SEPC）和 ELF loader 模式的
 * 内联缺页处理（缺页已解决，需要重试指令）都会使用它。
 */
extern bool need_handle_signal;

/* 遍历页表，按虚拟地址查找对应 PTE。
 * @rv: RISC-V 模拟器实例。
 * @addr: 虚拟地址。
 * @level: 输出 PTE 所在页表层级。
 * @return: 未找到或遇到异常时返回 NULL，否则返回对应 PTE。
 */
uint32_t *mmu_walk(riscv_t *rv, const uint32_t addr, uint32_t *level);

/* 校验 PTE，必要时产生对应页异常。
 * @op: 访问操作。
 * @rv: RISC-V 模拟器实例。
 * @pte: 待校验 PTE。
 * @addr: 触发异常的虚拟地址。
 * @return: 因权限违规产生异常时返回 false，否则返回 true。
 */
/* FIXME：补充 access fault 和地址越界检查。 */
#define MMU_FAULT_CHECK_DECL(op)                                            \
    bool mmu_##op##_fault_check(riscv_t *rv, uint32_t *pte, uint32_t vaddr, \
                                uint32_t access_bits);

MMU_FAULT_CHECK_DECL(ifetch);
MMU_FAULT_CHECK_DECL(read);
MMU_FAULT_CHECK_DECL(write);

/*
 * 通过 TLB 缓存把虚拟地址转换为物理地址。
 */
uint32_t mmu_translate(riscv_t *rv, uint32_t vaddr, bool rw);

/*
 * SFENCE.VMA 和 SATP 变更使用的 TLB 管理函数。
 */
void mmu_tlb_flush_all(riscv_t *rv);
void mmu_tlb_flush(riscv_t *rv, uint32_t vaddr);

#define get_ppn_and_offset()                                   \
    uint32_t ppn;                                              \
    uint32_t offset;                                           \
    do {                                                       \
        ppn = *pte >> (RV_PG_SHIFT - 2) << RV_PG_SHIFT;        \
        offset = level == 1 ? vaddr & MASK((RV_PG_SHIFT + 10)) \
                            : vaddr & MASK(RV_PG_SHIFT);       \
    } while (0)

uint8_t mmu_read_b(riscv_t *rv, const uint32_t vaddr);
uint16_t mmu_read_s(riscv_t *rv, const uint32_t vaddr);
uint32_t mmu_read_w(riscv_t *rv, const uint32_t vaddr);
void mmu_write_b(riscv_t *rv, const uint32_t vaddr, const uint8_t val);
void mmu_write_s(riscv_t *rv, const uint32_t vaddr, const uint16_t val);
void mmu_write_w(riscv_t *rv, const uint32_t vaddr, const uint32_t val);
