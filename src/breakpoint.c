/*
 * rv32emu 可依据 MIT 许可证自由再分发。使用和再分发规则见 LICENSE 文件。
 */

/*
 * 断点表实现。
 *
 * GDB stub 需要按客体 PC 快速查找、插入和删除软件断点。本文件把通用 map
 * 容器包装成 breakpoint_map_t，key 使用 RISC-V 地址，value 保存断点描述。
 * 断点比较只关心地址，因此同一地址不会重复插入。
 */

#if !RV32_HAS(GDBSTUB)
#error "只有启用 gdbstub 支持时才能构建此文件。"
#endif

#include "breakpoint.h"

static inline int cmp(const void *arg0, const void *arg1)
{
    riscv_word_t *a = (riscv_word_t *) arg0, *b = (riscv_word_t *) arg1;
    return (*a < *b)   ? MAP_CMP_LESS
           : (*a > *b) ? MAP_CMP_GREATER
                       : MAP_CMP_EQUAL;
}

breakpoint_map_t breakpoint_map_new()
{
    return map_init(riscv_word_t, breakpoint_t, cmp);
}

bool breakpoint_map_insert(breakpoint_map_t map, riscv_word_t addr)
{
    breakpoint_t bp = (breakpoint_t) {.addr = addr, .orig_insn = 0};
    map_iter_t it;
    map_find(map, &it, &addr);
    /* 同一地址不应重复设置断点。 */
    if (!map_at_end(map, &it))
        return false;

    return map_insert(map, &addr, &bp);
}

static bool breakpoint_map_find_it(breakpoint_map_t map,
                                   riscv_word_t addr,
                                   map_iter_t *it)
{
    map_find(map, it, &addr);
    if (map_at_end(map, it))
        return false;

    return true;
}

breakpoint_t *breakpoint_map_find(breakpoint_map_t map, riscv_word_t addr)
{
    map_iter_t it;
    if (!breakpoint_map_find_it(map, addr, &it))
        return NULL;

    return (breakpoint_t *) map_iter_value(&it, breakpoint_t *);
}

bool breakpoint_map_del(breakpoint_map_t map, riscv_word_t addr)
{
    map_iter_t it;
    if (!breakpoint_map_find_it(map, addr, &it))
        return false;

    map_erase(map, &it);
    return true;
}

void breakpoint_map_destroy(breakpoint_map_t map)
{
    map_delete(map);
}
