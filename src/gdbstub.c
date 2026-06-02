/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/*
 * GDB 远程调试适配层。
 *
 * mini-gdbstub 通过 target_ops 回调读写寄存器、内存、单步执行和管理断点。
 * 本文件把这些回调映射到 riscv_t 状态与 breakpoint_map_t，使外部 GDB 可以
 * 通过 target remote 调试客体 RISC-V 程序。
 */

#if !RV32_HAS(GDBSTUB)
#error "只有启用 gdbstub 支持时才能构建此文件。"
#endif

#include <assert.h>
#include <errno.h>

#include "mini-gdbstub/include/gdbstub.h"

#include "breakpoint.h"
#include "riscv.h"
#include "riscv_private.h"

static size_t rv_get_reg_bytes(UNUSED int regno)
{
    return 4;
}

static int rv_read_reg(void *args, int regno, void *data)
{
    riscv_t *rv = (riscv_t *) args;

    if (unlikely(regno > 32))
        return EFAULT;

    if (regno == 32)
        *(riscv_word_t *) data = rv_get_pc(rv);
    else
        *(riscv_word_t *) data = rv_get_reg(rv, regno);

    return 0;
}

static int rv_write_reg(void *args, int regno, void *data)
{
    if (unlikely(regno > 32))
        return EFAULT;

    riscv_t *rv = (riscv_t *) args;
    if (regno == 32)
        rv_set_pc(rv, *(riscv_word_t *) data);
    else
        rv_set_reg(rv, regno, *(riscv_word_t *) data);

    return 0;
}

static int rv_read_mem(void *args, size_t addr, size_t len, void *val)
{
    riscv_t *rv = (riscv_t *) args;

    int err = 0;
    for (size_t i = 0; i < len; i++) {
        /* FIXME：这里用简单回退方式处理无效地址读取。
         * 后续可能需要直接在 mem_read_* 函数中做错误处理。
         */
        *((uint8_t *) val + i) = rv->io.mem_read_b(rv, addr + i);
    }

    return err;
}

static int rv_write_mem(void *args, size_t addr, size_t len, void *val)
{
    riscv_t *rv = (riscv_t *) args;

    for (size_t i = 0; i < len; i++)
        rv->io.mem_write_b(rv, addr + i, *((uint8_t *) val + i));

    return 0;
}

static inline bool rv_is_interrupt(riscv_t *rv)
{
    return ATOMIC_LOAD(&rv->is_interrupted, ATOMIC_RELAXED);
}

static gdb_action_t rv_cont(void *args)
{
    riscv_t *rv = (riscv_t *) args;
    assert(rv);

    for (; !rv_has_halted(rv) && !rv_is_interrupt(rv);) {
        if (breakpoint_map_find(rv->breakpoint_map, rv_get_pc(rv)))
            break;

        rv_step_debug(rv);
    }

    /* 若中断处于 pending 状态，则清除它。 */
    ATOMIC_STORE(&rv->is_interrupted, false, ATOMIC_RELAXED);

    return ACT_RESUME;
}

static gdb_action_t rv_stepi(void *args)
{
    riscv_t *rv = (riscv_t *) args;
    assert(rv);

    rv_step_debug(rv);
    return ACT_RESUME;
}

static bool rv_set_bp(void *args, size_t addr, bp_type_t type)
{
    riscv_t *rv = (riscv_t *) args;
    if (type != BP_SOFTWARE)
        return false;

    return breakpoint_map_insert(rv->breakpoint_map, addr);
}

static bool rv_del_bp(void *args, size_t addr, bp_type_t type)
{
    riscv_t *rv = (riscv_t *) args;
    if (type != BP_SOFTWARE)
        return false;

    /* 没有匹配断点时，不执行额外动作。 */
    breakpoint_map_del(rv->breakpoint_map, addr);
    return true;
}

static void rv_on_interrupt(void *args)
{
    riscv_t *rv = (riscv_t *) args;

    /* 通知模拟器跳出 rv_cont 中的 for 循环。 */
    ATOMIC_STORE(&rv->is_interrupted, true, ATOMIC_RELAXED);
}

const struct target_ops gdbstub_ops = {
    .get_reg_bytes = rv_get_reg_bytes,
    .read_reg = rv_read_reg,
    .write_reg = rv_write_reg,
    .read_mem = rv_read_mem,
    .write_mem = rv_write_mem,
    .cont = rv_cont,
    .stepi = rv_stepi,
    .set_bp = rv_set_bp,
    .del_bp = rv_del_bp,
    .on_interrupt = rv_on_interrupt,
};
