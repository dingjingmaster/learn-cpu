/*
 * rv32emu 可依据 MIT 许可证自由再分发。使用和再分发规则见 LICENSE 文件。
 */

/*
 * 系统模式支持。
 *
 * 该文件实现特权态运行所需的中断更新、SV32 页表遍历、TLB 缓存、MMIO 分派和
 * 地址转换。Linux 内核访问 UART、PLIC、RTC、virtio-blk 等设备时，会通过这里
 * 的 MMU 和 MMIO 路径进入对应设备模型。
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "system.h"

#if !RV32_HAS(ELF_LOADER)
void emu_update_uart_interrupts(riscv_t *rv)
{
    vm_attr_t *attr = PRIV(rv);
    u8250_update_interrupts(attr->uart);
    if (attr->uart->pending_intrs)
        attr->plic->active |= IRQ_UART_BIT;
    else
        attr->plic->active &= ~IRQ_UART_BIT;
    plic_update_interrupts(attr->plic);
}

void emu_update_vblk_interrupts(riscv_t *rv)
{
    vm_attr_t *attr = PRIV(rv);
    for (int i = 0; i < attr->vblk_cnt; i++) {
        if (attr->vblk[i]->interrupt_status)
            attr->plic->active |= IRQ_VBLK_BIT(attr->vblk_irq_base, i);
        else
            attr->plic->active &= ~IRQ_VBLK_BIT(attr->vblk_irq_base, i);
        plic_update_interrupts(attr->plic);
    }
}

#if RV32_HAS(GOLDFISH_RTC)
void emu_update_rtc_interrupts(riscv_t *rv)
{
    vm_attr_t *attr = PRIV(rv);
    if (attr->rtc->interrupt_status)
        attr->plic->active |= IRQ_RTC_BIT;
    else
        attr->plic->active &= ~IRQ_RTC_BIT;
    plic_update_interrupts(attr->plic);
}
#endif /* RV32_HAS(GOLDFISH_RTC) */
#endif

static bool ppn_is_valid(riscv_t *rv, uint32_t ppn)
{
    vm_attr_t *attr = PRIV(rv);
    const uint32_t nr_pg_max = attr->mem_size / RV_PG_SIZE;
    return ppn < nr_pg_max;
}

void mmu_tlb_flush_all(riscv_t *rv)
{
    memset(rv->dtlb, 0, sizeof(rv->dtlb));
    memset(rv->itlb, 0, sizeof(rv->itlb));
}

void mmu_tlb_flush(riscv_t *rv, uint32_t vaddr)
{
    uint32_t vpn = vaddr >> RV_PG_SHIFT;
    uint32_t idx = vpn & TLB_MASK;

    if (rv->dtlb[idx].valid && rv->dtlb[idx].vpn == vpn)
        rv->dtlb[idx].valid = 0;

    if (rv->itlb[idx].valid && rv->itlb[idx].vpn == vpn)
        rv->itlb[idx].valid = 0;
}

/* 数据访问（读/写）的 TLB 查询。
 * 命中时返回物理地址，未命中返回 0；写访问命中时同步更新 PTE 的 A/D 位。
 */
static inline uint32_t dtlb_lookup(riscv_t *rv,
                                   uint32_t vaddr,
                                   bool write,
                                   bool *hit)
{
    uint32_t vpn = vaddr >> RV_PG_SHIFT;
    uint32_t idx = vpn & TLB_MASK;
    tlb_entry_t *entry = &rv->dtlb[idx];

    if (entry->valid && entry->vpn == vpn) {
        /* 检查访问权限。 */
        uint8_t needed = write ? PTE_W : PTE_R;
        if (!(entry->perm & needed)) {
            *hit = false;
            return 0;
        }

        /* 写访问需要处理 dirty 位。 */
        if (write && !entry->dirty) {
            /* 同步更新内存中的 PTE dirty 位。 */
            vm_attr_t *attr = PRIV(rv);
            pte_t *pte = (pte_t *) (attr->mem->mem_base + entry->pte_addr);
            *pte |= PTE_D;
            entry->dirty = 1;
        }

        *hit = true;
        /* ppn 保存页对齐的物理地址基址。 */
        uint32_t offset = (entry->level == TLB_PAGE_LEVEL_SUPER)
                              ? (vaddr & MASK(RV_PG_SHIFT + 10))
                              : (vaddr & MASK(RV_PG_SHIFT));
        return entry->ppn | offset;
    }

    *hit = false;
    return 0;
}

/* 指令取指的 TLB 查询。
 * 命中时返回物理地址，未命中返回 0。
 */
static inline uint32_t itlb_lookup(riscv_t *rv, uint32_t vaddr, bool *hit)
{
    uint32_t vpn = vaddr >> RV_PG_SHIFT;
    uint32_t idx = vpn & TLB_MASK;
    tlb_entry_t *entry = &rv->itlb[idx];

    if (entry->valid && entry->vpn == vpn) {
        /* 检查执行权限。 */
        if (!(entry->perm & PTE_X)) {
            *hit = false;
            return 0;
        }

        *hit = true;
        /* ppn 保存页对齐的物理地址基址。 */
        uint32_t offset = (entry->level == TLB_PAGE_LEVEL_SUPER)
                              ? (vaddr & MASK(RV_PG_SHIFT + 10))
                              : (vaddr & MASK(RV_PG_SHIFT));
        return entry->ppn | offset;
    }

    *hit = false;
    return 0;
}

/* 页表遍历成功后填充 dTLB 条目。 */
static inline void dtlb_populate(riscv_t *rv,
                                 uint32_t vaddr,
                                 pte_t *pte,
                                 uint32_t level)
{
    vm_attr_t *attr = PRIV(rv);
    uint32_t vpn = vaddr >> RV_PG_SHIFT;
    uint32_t idx = vpn & TLB_MASK;
    tlb_entry_t *entry = &rv->dtlb[idx];

    entry->vpn = vpn;
    /* 保存页对齐物理地址基址：从 PTE [31:10] 提取 PPN 后左移 12 位。 */
    entry->ppn = *pte >> (RV_PG_SHIFT - 2) << RV_PG_SHIFT;
    entry->pte_addr = (uint8_t *) pte - attr->mem->mem_base;
    entry->perm = *pte & (PTE_R | PTE_W | PTE_X | PTE_U);
    entry->dirty = (*pte & PTE_D) ? 1 : 0;
    entry->level = level;
    entry->valid = 1;
}

/* 页表遍历成功后填充 iTLB 条目。 */
static inline void itlb_populate(riscv_t *rv,
                                 uint32_t vaddr,
                                 pte_t *pte,
                                 uint32_t level)
{
    vm_attr_t *attr = PRIV(rv);
    uint32_t vpn = vaddr >> RV_PG_SHIFT;
    uint32_t idx = vpn & TLB_MASK;
    tlb_entry_t *entry = &rv->itlb[idx];

    entry->vpn = vpn;
    /* 保存页对齐物理地址基址：从 PTE [31:10] 提取 PPN 后左移 12 位。 */
    entry->ppn = *pte >> (RV_PG_SHIFT - 2) << RV_PG_SHIFT;
    entry->pte_addr = (uint8_t *) pte - attr->mem->mem_base;
    entry->perm = *pte & (PTE_R | PTE_W | PTE_X | PTE_U);
    entry->dirty = (*pte & PTE_D) ? 1 : 0;
    entry->level = level;
    entry->valid = 1;
}

#define PAGE_TABLE(ppn)                                               \
    ppn_is_valid(rv, ppn)                                             \
        ? (uint32_t *) (attr->mem->mem_base + (ppn << (RV_PG_SHIFT))) \
        : NULL

/* 遍历页表，按虚拟地址查找对应 PTE。
 * @rv: RISC-V 模拟器实例。
 * @vaddr: 虚拟地址。
 * @level: 输出 PTE 所在页表层级。
 * @return: 未找到或遇到异常时返回 NULL，否则返回对应 PTE。
 */
pte_t *mmu_walk(riscv_t *rv, const uint32_t vaddr, uint32_t *level)
{
    vm_attr_t *attr = PRIV(rv);
    uint32_t ppn = rv->csr_satp & MASK(22);

    /* 根页表。 */
    uint32_t *page_table = PAGE_TABLE(ppn);
    if (!page_table)
        return NULL;

    for (int i = 1; i >= 0; i--) {
        *level = 2 - i;
        uint32_t vpn =
            (vaddr >> RV_PG_SHIFT >> (i * (RV_PG_SHIFT - 2))) & MASK(10);
        pte_t *pte = page_table + vpn;

        uint8_t XWRV_bit = (*pte & MASK(4));
        switch (XWRV_bit) {
        case NEXT_PG_TBL: /* 下一层页表。 */
            ppn = (*pte >> (RV_PG_SHIFT - 2));
            page_table = PAGE_TABLE(ppn);
            if (!page_table)
                return NULL;
            break;
        case RO_PAGE:
        case RW_PAGE:
        case EO_PAGE:
        case RX_PAGE:
        case RWX_PAGE:
            ppn = (*pte >> (RV_PG_SHIFT - 2));
            if (*level == 1 &&
                unlikely(ppn & MASK(10))) /* 超级页未对齐。 */
                return NULL;
            return pte; /* 叶子 PTE。 */
        case RESRV_PAGE1:
        case RESRV_PAGE2:
        default:
            return NULL;
        }
    }

    return NULL;
}

/* 校验 PTE，必要时产生对应页异常。
 * @op: 访问操作。
 * @rv: RISC-V 模拟器实例。
 * @pte: 待校验 PTE。
 * @vaddr: 触发异常的虚拟地址。
 * @return: 因权限违规产生异常时返回 false，否则返回 true。
 */
/* FIXME：补充 access fault 和地址越界检查。 */
#define MMU_FAULT_CHECK(op, rv, pte, vaddr, access_bits) \
    mmu_##op##_fault_check(rv, pte, vaddr, access_bits)
#define MMU_FAULT_CHECK_IMPL(op, pgfault)                                     \
    bool mmu_##op##_fault_check(riscv_t *rv, pte_t *pte, uint32_t vaddr,      \
                                uint32_t access_bits)                         \
    {                                                                         \
        uint32_t scause;                                                      \
        uint32_t stval = vaddr;                                               \
        switch (access_bits) {                                                \
        case PTE_R:                                                           \
            scause = PAGEFAULT_LOAD;                                          \
            break;                                                            \
        case PTE_W:                                                           \
            scause = PAGEFAULT_STORE;                                         \
            break;                                                            \
        case PTE_X:                                                           \
            scause = PAGEFAULT_INSN;                                          \
            break;                                                            \
        default:                                                              \
            __UNREACHABLE;                                                    \
            break;                                                            \
        }                                                                     \
        if (pte && (!(*pte & PTE_V))) {                                       \
            SET_CAUSE_AND_TVAL_THEN_TRAP(rv, scause, stval);                  \
            return false;                                                     \
        }                                                                     \
        if (!(pte && (*pte & access_bits))) {                                 \
            SET_CAUSE_AND_TVAL_THEN_TRAP(rv, scause, stval);                  \
            return false;                                                     \
        }                                                                     \
        /*                                                                    \
         * (1) MXR=0 时，只允许从标记为可读（R=1）的页面加载。              \
         *                                                                    \
         * (2) MXR=1 时，标记为可读或可执行（R=1 或 X=1）的页面都可加载。   \
         */                                                                   \
        if (pte && ((!(SSTATUS_MXR & rv->csr_sstatus) && !(*pte & PTE_R) &&   \
                     (access_bits == PTE_R)) ||                               \
                    ((SSTATUS_MXR & rv->csr_sstatus) &&                       \
                     !((*pte & PTE_R) | (*pte & PTE_X)) &&                    \
                     (access_bits == PTE_R)))) {                              \
            SET_CAUSE_AND_TVAL_THEN_TRAP(rv, scause, stval);                  \
            return false;                                                     \
        }                                                                     \
        /*                                                                    \
         * SUM=0 时，S 模式访问 U 模式可访问页面会触发异常。                \
         */                                                                   \
        if (pte && rv->priv_mode == RV_PRIV_S_MODE &&                         \
            !(SSTATUS_SUM & rv->csr_sstatus) && (*pte & PTE_U)) {             \
            SET_CAUSE_AND_TVAL_THEN_TRAP(rv, scause, stval);                  \
            return false;                                                     \
        }                                                                     \
        /* 没有找到 PTE，交给异常处理器建立映射。 */                         \
        if (!pte) {                                                           \
            SET_CAUSE_AND_TVAL_THEN_TRAP(rv, scause, stval);                  \
            return false;                                                     \
        }                                                                     \
        /* PTE 有效。 */                                                     \
        return true;                                                          \
    }

MMU_FAULT_CHECK_IMPL(ifetch, pagefault_insn)
MMU_FAULT_CHECK_IMPL(read, pagefault_load)
MMU_FAULT_CHECK_IMPL(write, pagefault_store)

/* 系统模拟启用 MMU 后，I/O 处理器负责管理地址转换后的取指、读写和 MMIO 分派。
 * 这些回调实现 riscv_io_t 接口，因此解释器、JIT 和 T2C 都能复用同一组入口。
 *
 * I/O 处理器包括：
 * - mmu_ifetch
 * - mmu_read_w
 * - mmu_read_s
 * - mmu_read_b
 * - mmu_write_w
 * - mmu_write_s
 * - mmu_write_b
 */
extern bool need_retranslate;
static uint32_t mmu_ifetch(riscv_t *rv, const uint32_t vaddr)
{
    /*
     * 这里不要调用 rv->io.mem_translate()。基本块可能正在重翻译，对应 PTE 可能
     * 为 NULL，而 get_ppn_and_offset() 无法处理 NULL PTE。
     */

    if (!rv->csr_satp)
        return memory_ifetch(vaddr);

    if (need_retranslate)
        return 0;

    /* 快路径先查 iTLB。 */
    bool hit;
    uint32_t paddr = itlb_lookup(rv, vaddr, &hit);
    if (hit)
        return memory_ifetch(paddr);

    /* TLB 未命中，执行完整页表遍历。 */
    uint32_t level;
    pte_t *pte = mmu_walk(rv, vaddr, &level);
    bool ok = MMU_FAULT_CHECK(ifetch, rv, pte, vaddr, PTE_X);
    if (unlikely(!ok)) {
#if RV32_HAS(SYSTEM_MMIO)
        CHECK_PENDING_SIGNAL(rv, need_handle_signal);
        if (need_handle_signal)
            return 0;
#endif
        /* 异常处理器建立页面后重试页表遍历。 */
        pte = mmu_walk(rv, vaddr, &level);
        /* 重试后再次校验权限。 */
        if (!pte || !MMU_FAULT_CHECK(ifetch, rv, pte, vaddr, PTE_X)) {
            need_retranslate = true;
            /* 同时设置 need_handle_signal，让 RVOP 宏返回解释器重试。 */
            need_handle_signal = true;
            return 0;
        }
    }

    /* 按 RISC-V Sv32 规范更新 Accessed 位。 */
    if (!(*pte & PTE_A))
        *pte |= PTE_A;

    /* 填充 iTLB，服务后续访问。 */
    itlb_populate(rv, vaddr, pte, level);

    get_ppn_and_offset();
    return memory_ifetch(ppn | offset);
}

uint32_t mmu_read_w(riscv_t *rv, const uint32_t vaddr)
{
    uint32_t addr = rv->io.mem_translate(rv, vaddr, R);

#if RV32_HAS(SYSTEM) && RV32_HAS(ELF_LOADER)
    if (need_retranslate)
        return 0;
#elif RV32_HAS(SYSTEM_MMIO)
    if (need_handle_signal)
        return 0;
#endif

    if (GUEST_RAM_CONTAINS(PRIV(rv)->mem, addr, 4))
        return memory_read_w(addr);

#if RV32_HAS(SYSTEM_MMIO)
    MMIO_READ();
#endif

    __UNREACHABLE;
}

uint16_t mmu_read_s(riscv_t *rv, const uint32_t vaddr)
{
    uint32_t addr = rv->io.mem_translate(rv, vaddr, R);

#if RV32_HAS(SYSTEM) && RV32_HAS(ELF_LOADER)
    if (need_retranslate)
        return 0;
#elif RV32_HAS(SYSTEM_MMIO)
    if (need_handle_signal)
        return 0;
#endif

    if (GUEST_RAM_CONTAINS(PRIV(rv)->mem, addr, 2))
        return memory_read_s(addr);

#if RV32_HAS(SYSTEM_MMIO)
    MMIO_READ();
#endif

    __UNREACHABLE;
}

uint8_t mmu_read_b(riscv_t *rv, const uint32_t vaddr)
{
    uint32_t addr = rv->io.mem_translate(rv, vaddr, R);

#if RV32_HAS(SYSTEM) && RV32_HAS(ELF_LOADER)
    if (need_retranslate)
        return 0;
#elif RV32_HAS(SYSTEM_MMIO)
    if (need_handle_signal)
        return 0;
#endif

    if (GUEST_RAM_CONTAINS(PRIV(rv)->mem, addr, 1))
        return memory_read_b(addr);

#if RV32_HAS(SYSTEM_MMIO)
    MMIO_READ();
#endif

    __UNREACHABLE;
}

void mmu_write_w(riscv_t *rv, const uint32_t vaddr, const uint32_t val)
{
    uint32_t addr = rv->io.mem_translate(rv, vaddr, W);

#if RV32_HAS(SYSTEM) && RV32_HAS(ELF_LOADER)
    if (need_retranslate)
        return;
#elif RV32_HAS(SYSTEM_MMIO)
    if (need_handle_signal)
        return;
#endif

    if (GUEST_RAM_CONTAINS(PRIV(rv)->mem, addr, 4)) {
        memory_write_w(addr, (uint8_t *) &val);
        return;
    }

#if RV32_HAS(SYSTEM_MMIO)
    MMIO_WRITE();
#endif

    __UNREACHABLE;
}

void mmu_write_s(riscv_t *rv, const uint32_t vaddr, const uint16_t val)
{
    uint32_t addr = rv->io.mem_translate(rv, vaddr, W);

#if RV32_HAS(SYSTEM) && RV32_HAS(ELF_LOADER)
    if (need_retranslate)
        return;
#elif RV32_HAS(SYSTEM_MMIO)
    if (need_handle_signal)
        return;
#endif

    if (GUEST_RAM_CONTAINS(PRIV(rv)->mem, addr, 2)) {
        memory_write_s(addr, (uint8_t *) &val);
        return;
    }

#if RV32_HAS(SYSTEM_MMIO)
    MMIO_WRITE();
#endif

    __UNREACHABLE;
}

void mmu_write_b(riscv_t *rv, const uint32_t vaddr, const uint8_t val)
{
    uint32_t addr = rv->io.mem_translate(rv, vaddr, W);

#if RV32_HAS(SYSTEM) && RV32_HAS(ELF_LOADER)
    if (need_retranslate)
        return;
#elif RV32_HAS(SYSTEM_MMIO)
    if (need_handle_signal)
        return;
#endif

    if (GUEST_RAM_CONTAINS(PRIV(rv)->mem, addr, 1)) {
        memory_write_b(addr, (uint8_t *) &val);
        return;
    }

#if RV32_HAS(SYSTEM_MMIO)
    MMIO_WRITE();
#endif

    __UNREACHABLE;
}

uint32_t mmu_translate(riscv_t *rv, uint32_t vaddr, bool rw)
{
    if (!rv->csr_satp)
        return vaddr;

    /* 快路径先查 dTLB。 */
    bool hit;
    uint32_t paddr = dtlb_lookup(rv, vaddr, !rw, &hit);
    if (hit)
        return paddr;

    /* TLB 未命中，执行完整页表遍历。 */
    uint32_t level;
    pte_t *pte = mmu_walk(rv, vaddr, &level);
    bool ok = rw ? MMU_FAULT_CHECK(read, rv, pte, vaddr, PTE_R)
                 : MMU_FAULT_CHECK(write, rv, pte, vaddr, PTE_W);
    if (unlikely(!ok)) {
#if RV32_HAS(SYSTEM_MMIO)
        CHECK_PENDING_SIGNAL(rv, need_handle_signal);
        if (need_handle_signal)
            return 0;
#endif
        /* 异常处理器建立页面后重试页表遍历。 */
        pte = mmu_walk(rv, vaddr, &level);
        /* 重试后再次校验权限。 */
        ok = rw ? MMU_FAULT_CHECK(read, rv, pte, vaddr, PTE_R)
                : MMU_FAULT_CHECK(write, rv, pte, vaddr, PTE_W);
        if (!pte || !ok) {
#if RV32_HAS(ELF_LOADER)
            need_retranslate = true;
            /* 同时设置 need_handle_signal，让 RVOP 宏返回解释器重试。 */
            need_handle_signal = true;
#else
            need_handle_signal = true;
#endif
            return 0;
        }
    }

    /* 按 RISC-V Sv32 规范更新 A/D 位。 */
    if (!(*pte & PTE_A))
        *pte |= PTE_A;
    if (!rw && !(*pte & PTE_D)) /* 写访问需要 Dirty 位。 */
        *pte |= PTE_D;

    /* 填充 dTLB，服务后续访问。 */
    dtlb_populate(rv, vaddr, pte, level);

    get_ppn_and_offset();
    return ppn | offset;
}

riscv_io_t mmu_io = {
    /* 内存读取接口。 */
    .mem_ifetch = mmu_ifetch,
    .mem_read_w = mmu_read_w,
    .mem_read_s = mmu_read_s,
    .mem_read_b = mmu_read_b,

    /* 内存写入接口。 */
    .mem_write_w = mmu_write_w,
    .mem_write_s = mmu_write_s,
    .mem_write_b = mmu_write_b,

    /* 虚拟地址到物理地址转换入口。 */
    .mem_translate = mmu_translate,

    /* T2C 运行时绑定使用的 MMU 内存访问函数。 */
    .mmu_read_w = mmu_read_w,
    .mmu_read_s = mmu_read_s,
    .mmu_read_b = mmu_read_b,
    .mmu_write_w = mmu_write_w,
    .mmu_write_s = mmu_write_s,
    .mmu_write_b = mmu_write_b,

    /* 系统服务和必要运行时例程。 */
    .on_ecall = ecall_handler,
    .on_ebreak = ebreak_handler,
    .on_memcpy = memcpy_handler,
    .on_memset = memset_handler,
    .on_trap = trap_handler,
};
