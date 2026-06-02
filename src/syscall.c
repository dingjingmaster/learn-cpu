/*
 * rv32emu 可依据 MIT 许可证自由再分发。使用和再分发规则见 LICENSE 文件。
 */

/*
 * 用户态 ecall/系统调用处理。
 *
 * 模拟器实现 newlib 和简单 POSIX 程序常用的一小组系统调用，把客体寄存器 a0-a7
 * 中的参数转换为宿主系统调用或运行时操作。系统模式下也复用本文件处理 SBI
 * timer/base/reset 等调用。
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "riscv.h"
#include "riscv_private.h"
#include "utils.h"

#define PREALLOC_SIZE 4096

/* newlib 是可移植的 C 运行库，并非 RISC-V 专用。它实现了 printf(3) 以及
 * C 标准库中的其他函数，因此模拟器需要补齐配套的一小组系统调用。
 *
 * 系统调用：名称、编号。
 */
/* clang-format off */
#define SUPPORTED_SYSCALLS                 \
    _(close,                57)            \
    _(lseek,                62)            \
    _(read,                 63)            \
    _(write,                64)            \
    _(fstat,                80)            \
    _(exit,                 93)            \
    _(gettimeofday,         169)           \
    _(brk,                  214)           \
    _(clock_gettime,        403)           \
    _(open,                 1024)          \
    IIF(RV32_HAS(SYSTEM))(                 \
        _(sbi_base,         0x10)          \
        _(sbi_timer,        0x54494D45)    \
        _(sbi_rst,          0x53525354)    \
    )                                      \
    IIF(RV32_HAS(SDL))(                    \
        _(draw_frame,       0xBEEF)        \
        _(setup_queue,      0xC0DE)        \
        _(submit_queue,     0xFEED)        \
        _(setup_audio,      0xBABE)        \
        _(control_audio,    0xD00D)        \
    )
/* clang-format on */

enum {
#define _(name, number) SYS_##name = number,
    SUPPORTED_SYSCALLS
#undef _
};

enum {
    O_RDONLY = 0,
    O_WRONLY = 1,
    O_RDWR = 2,
    O_ACCMODE = 3,
};

static int find_free_fd(vm_attr_t *attr)
{
    for (int i = 3;; ++i) {
        map_iter_t it;
        map_find(attr->fd_map, &it, &i);
        if (map_at_end(attr->fd_map, &it))
            return i;
    }
}

static const char *get_mode_str(uint32_t flags, uint32_t mode UNUSED)
{
    switch (flags & O_ACCMODE) {
    case O_RDONLY:
        return "rb";
    case O_WRONLY:
        return "wb";
    case O_RDWR:
        return "a+";
    default:
        return NULL;
    }
}

static uint8_t tmp[PREALLOC_SIZE];
static void syscall_write(riscv_t *rv)
{
    vm_attr_t *attr = PRIV(rv);

    /* _write(fd, buffer, count) */
    riscv_word_t fd = rv_get_reg(rv, rv_reg_a0);
    riscv_word_t buffer = rv_get_reg(rv, rv_reg_a1);
    riscv_word_t count = rv_get_reg(rv, rv_reg_a2);

    /* 查询文件描述符映射。 */
    map_iter_t it;
    map_find(attr->fd_map, &it, &fd);
    if (map_at_end(attr->fd_map, &it))
        goto error_handler;

    uint32_t total_write = 0;
    FILE *handle = map_iter_value(&it, FILE *);

    while (count > PREALLOC_SIZE) {
        memory_read(attr->mem, tmp, buffer + total_write, PREALLOC_SIZE);
        /* 分块写出客体内存中的数据。 */
        size_t written = fwrite(tmp, 1, PREALLOC_SIZE, handle);
        if (written != PREALLOC_SIZE && ferror(handle))
            goto error_handler;
        total_write += written;
        count -= PREALLOC_SIZE;
    }

    memory_read(attr->mem, tmp, buffer + total_write, count);
    /* 写出剩余数据。 */
    size_t written = fwrite(tmp, 1, count, handle);
    if (written != count && ferror(handle))
        goto error_handler;
    total_write += written;
    assert(total_write == rv_get_reg(rv, rv_reg_a2));

    /* 返回实际写出的字节数。 */
    rv_set_reg(rv, rv_reg_a0, total_write);
    return;

error_handler:
    /* 出错时按 newlib 约定返回 -1。 */
    rv_set_reg(rv, rv_reg_a0, -1);
}

static void syscall_exit(riscv_t *rv)
{
    /* 退出系统调用只停止 CPU 并保存退出码，退出码语义由上层应用决定。
     */
    rv_halt(rv);

    vm_attr_t *attr = PRIV(rv);
    attr->exit_code = rv_get_reg(rv, rv_reg_a0);
}

/* brk(increment)
 * 说明：
 *   - malloc chunk 使用 8 字节对齐。
 *   - sbrk block 使用 4 KiB 对齐。
 */
static void syscall_brk(riscv_t *rv)
{
    vm_attr_t *attr = PRIV(rv);

    /* 读取新的 break 地址。 */
    riscv_word_t increment = rv_get_reg(rv, rv_reg_a0);
    if (increment)
        attr->break_addr = increment;

    /* 返回当前 break 地址。 */
    rv_set_reg(rv, rv_reg_a0, attr->break_addr);
}

static void syscall_gettimeofday(riscv_t *rv)
{
    /* 读取客体传入的 timeval/timezone 指针。 */
    riscv_word_t tv = rv_get_reg(rv, rv_reg_a0);
    riscv_word_t tz = rv_get_reg(rv, rv_reg_a1);

    /* 写回宿主时钟时间。 */
    if (tv) {
        struct timeval tv_s;
        rv_gettimeofday(&tv_s);
        memory_write_w(tv + 0, (const uint8_t *) &tv_s.tv_sec);
        memory_write_w(tv + 8, (const uint8_t *) &tv_s.tv_usec);
    }

    if (tz) {
        /* FIXME：newlib 的 syscall 处理器当前会忽略 timezone 参数。 */
    }

    /* 成功返回 0。 */
    rv_set_reg(rv, rv_reg_a0, 0);
}

static void syscall_clock_gettime(riscv_t *rv)
{
    /* 读取 clock id 和 timespec 输出地址。 */
    riscv_word_t id = rv_get_reg(rv, rv_reg_a0);
    riscv_word_t tp = rv_get_reg(rv, rv_reg_a1);

    switch (id) {
    case CLOCK_REALTIME:
#ifdef CLOCK_MONOTONIC
    case CLOCK_MONOTONIC:
#endif
        break;
    default:
        rv_set_reg(rv, rv_reg_a0, -1);
        return;
    }

    if (tp) {
        struct timespec tp_s;
        rv_clock_gettime(&tp_s);
        memory_write_w(tp + 0, (const uint8_t *) &tp_s.tv_sec);
        memory_write_w(tp + 8, (const uint8_t *) &tp_s.tv_nsec);
    }

    /* 成功返回 0。 */
    rv_set_reg(rv, rv_reg_a0, 0);
}

static void syscall_close(riscv_t *rv)
{
    vm_attr_t *attr = PRIV(rv);

    /* _close(fd); */
    uint32_t fd = rv_get_reg(rv, rv_reg_a0);

#if !RV32_HAS(SYSTEM)
    /*
     * crt0 会在进程退出时关闭标准文件描述符 0、1、2，这类操作不应视为错误。
     * 对于无法找到 exit_addr 的 stripped ELF，允许 close(fd < 3) 静默成功。
     */
    if (fd < 3 && !PRIV(rv)->on_exit && PRIV(rv)->exit_addr) {
        rv_set_reg(rv, rv_reg_a0, -1);
        rv_log_error(
            "尝试关闭小于 3 的标准文件描述符（fd=%u），该操作不受支持。",
            fd);
        return;
    }
#endif

    if (fd >= 3) { /* 查询文件描述符映射。 */
        map_iter_t it;
        map_find(attr->fd_map, &it, &fd);
        if (!map_at_end(attr->fd_map, &it)) {
            if (fclose(map_iter_value(&it, FILE *))) {
                /* fclose 失败。 */
                rv_set_reg(rv, rv_reg_a0, -1);
                return;
            }
            map_erase(attr->fd_map, &it);

            /* 成功关闭并移除映射。 */
            rv_set_reg(rv, rv_reg_a0, 0);
        }
    }

    /* 标准描述符或已关闭描述符也按成功处理。 */
    rv_set_reg(rv, rv_reg_a0, 0);
}

/* lseek() 根据 whence 把 fd 对应已打开文件的偏移移动到 offset 指定位置。 */
static void syscall_lseek(riscv_t *rv)
{
    vm_attr_t *attr = PRIV(rv);

    /* _lseek(fd, offset, whence); */
    uint32_t fd = rv_get_reg(rv, rv_reg_a0);
    uint32_t offset = rv_get_reg(rv, rv_reg_a1);
    uint32_t whence = rv_get_reg(rv, rv_reg_a2);

    /* 查询文件描述符映射。 */
    map_iter_t it;
    map_find(attr->fd_map, &it, &fd);
    if (map_at_end(attr->fd_map, &it)) {
        /* 未找到描述符。 */
        rv_set_reg(rv, rv_reg_a0, -1);
        return;
    }

    FILE *handle = map_iter_value(&it, FILE *);
    if (fseek(handle, offset, whence)) {
        /* fseek 失败。 */
        rv_set_reg(rv, rv_reg_a0, -1);
        return;
    }

    long pos = ftell(handle);
    if (pos == -1) {
        /* ftell 失败。 */
        rv_set_reg(rv, rv_reg_a0, -1);
        return;
    }

    /* 返回移动后的文件偏移。 */
    rv_set_reg(rv, rv_reg_a0, pos);
}

static void syscall_read(riscv_t *rv)
{
    vm_attr_t *attr = PRIV(rv);

    /* _read(fd, buf, count); */
    uint32_t fd = rv_get_reg(rv, rv_reg_a0);
    uint32_t buf = rv_get_reg(rv, rv_reg_a1);
    uint32_t count = rv_get_reg(rv, rv_reg_a2);

    /* 查询文件描述符映射。 */
    map_iter_t it;
    map_find(attr->fd_map, &it, &fd);
    if (map_at_end(attr->fd_map, &it)) {
        /* 未找到描述符。 */
        rv_set_reg(rv, rv_reg_a0, -1);
        return;
    }

    FILE *handle = map_iter_value(&it, FILE *);
    uint32_t total_read = 0;
    /* 从宿主文件读取数据，再写入客体内存。 */

    while (count > PREALLOC_SIZE) {
        size_t r = fread(tmp, 1, PREALLOC_SIZE, handle);
        if (!memory_write(attr->mem, buf + total_read, tmp, r)) {
            rv_set_reg(rv, rv_reg_a0, -1);
            return;
        }
        count -= r;
        total_read += r;
        if (r != PREALLOC_SIZE)
            break;
    }
    size_t r = fread(tmp, 1, count, handle);
    if (!memory_write(attr->mem, buf + total_read, tmp, r)) {
        rv_set_reg(rv, rv_reg_a0, -1);
        return;
    }
    total_read += r;
    if (total_read != rv_get_reg(rv, rv_reg_a2) && ferror(handle)) {
        /* 宿主读失败。 */
        rv_set_reg(rv, rv_reg_a0, -1);
        return;
    }
    /* 返回实际读取的字节数。 */
    rv_set_reg(rv, rv_reg_a0, total_read);
}

static void syscall_fstat(riscv_t *rv UNUSED)
{
    /* FIXME：补充真实 fstat 实现。 */
}

static void syscall_open(riscv_t *rv)
{
    vm_attr_t *attr = PRIV(rv);

    /* _open(name, flags, mode); */
    uint32_t name = rv_get_reg(rv, rv_reg_a0);
    uint32_t flags = rv_get_reg(rv, rv_reg_a1);
    uint32_t mode = rv_get_reg(rv, rv_reg_a2);

    /* 带边界检查地读取客体内存中的路径字符串。 */
    if (name >= attr->mem->mem_size) {
        rv_set_reg(rv, rv_reg_a0, -1);
        return;
    }

    /* 计算安全最大长度，避免越过客体内存末尾。 */
    const size_t max_len = attr->mem->mem_size - name;
    const char *name_ptr = (char *) attr->mem->mem_base + name;

    /* 在边界内查找字符串结尾。 */
    const size_t name_len = strnlen(name_ptr, max_len);
    if (name_len == max_len) {
        /* 边界内没有找到 NUL 终止符。 */
        rv_set_reg(rv, rv_reg_a0, -1);
        return;
    }
    char *name_str = malloc(name_len + 1);
    if (!name_str) {
        rv_set_reg(rv, rv_reg_a0, -1);
        return;
    }
    assert(name_str);
    name_str[name_len] = '\0';
    memory_read(attr->mem, (uint8_t *) name_str, name, name_len);

    /* 按 flags/mode 映射出的宿主模式打开文件。 */
    const char *mode_str = get_mode_str(flags, mode);
    if (!mode_str) {
        free(name_str);
        rv_set_reg(rv, rv_reg_a0, -1);
        return;
    }

    FILE *handle = fopen(name_str, mode_str);
    if (!handle) {
        free(name_str);
        rv_set_reg(rv, rv_reg_a0, -1);
        return;
    }

    free(name_str);

    const int fd = find_free_fd(attr); /* 分配一个空闲文件描述符。 */

    /* 插入文件描述符映射。 */
    map_insert(attr->fd_map, (void *) &fd, &handle);

    /* 返回分配的客体文件描述符。 */
    rv_set_reg(rv, rv_reg_a0, fd);
}

#if RV32_HAS(SDL)
extern void syscall_draw_frame(riscv_t *rv);
extern void syscall_setup_queue(riscv_t *rv);
extern void syscall_submit_queue(riscv_t *rv);
extern void syscall_setup_audio(riscv_t *rv);
extern void syscall_control_audio(riscv_t *rv);
#endif

#if RV32_HAS(SYSTEM)
/* SBI 相关调用。 */
static void syscall_sbi_timer(riscv_t *rv)
{
    vm_attr_t *attr = PRIV(rv);
    const riscv_word_t fid = rv_get_reg(rv, rv_reg_a6);
    const riscv_word_t a0 = rv_get_reg(rv, rv_reg_a0);
    const riscv_word_t a1 = rv_get_reg(rv, rv_reg_a1);

    switch (fid) {
    case SBI_TIMER_SET_TIMER:
        attr->timer = (((uint64_t) a1) << 32) | (uint64_t) (a0);
        rv_set_reg(rv, rv_reg_a0, SBI_SUCCESS);
        rv_set_reg(rv, rv_reg_a1, 0);
        break;
    default:
        rv_set_reg(rv, rv_reg_a0, SBI_ERR_NOT_SUPPORTED);
        rv_set_reg(rv, rv_reg_a1, 0);
        break;
    }
}

#define SBI_IMPL_ID 0x999
#define SBI_IMPL_VERSION 1

static void syscall_sbi_base(riscv_t *rv)
{
    const riscv_word_t fid = rv_get_reg(rv, rv_reg_a6);

    switch (fid) {
    case SBI_BASE_GET_SBI_IMPL_ID:
        rv_set_reg(rv, rv_reg_a0, SBI_SUCCESS);
        rv_set_reg(rv, rv_reg_a1, SBI_IMPL_ID);
        break;
    case SBI_BASE_GET_SBI_IMPL_VERSION:
        rv_set_reg(rv, rv_reg_a0, SBI_SUCCESS);
        rv_set_reg(rv, rv_reg_a1, SBI_IMPL_VERSION);
        break;
    case SBI_BASE_GET_MVENDORID:
        rv_set_reg(rv, rv_reg_a0, SBI_SUCCESS);
        rv_set_reg(rv, rv_reg_a1, rv->csr_mvendorid);
        break;
    case SBI_BASE_GET_MARCHID:
        rv_set_reg(rv, rv_reg_a0, SBI_SUCCESS);
        rv_set_reg(rv, rv_reg_a1, rv->csr_marchid);
        break;
    case SBI_BASE_GET_MIMPID:
        rv_set_reg(rv, rv_reg_a0, SBI_SUCCESS);
        rv_set_reg(rv, rv_reg_a1, rv->csr_mimpid);
        break;
    case SBI_BASE_GET_SBI_SPEC_VERSION:
        rv_set_reg(rv, rv_reg_a0, SBI_SUCCESS);
        rv_set_reg(rv, rv_reg_a1, (0 << 24) | 3); /* version 0.3 */
        break;
    case SBI_BASE_PROBE_EXTENSION: {
        const riscv_word_t eid = rv_get_reg(rv, rv_reg_a0);
        bool available =
            eid == SBI_EID_BASE || eid == SBI_EID_TIMER || eid == SBI_EID_RST;
        rv_set_reg(rv, rv_reg_a0, SBI_SUCCESS);
        rv_set_reg(rv, rv_reg_a1, available);
        break;
    }
    default:
        rv_set_reg(rv, rv_reg_a0, SBI_ERR_NOT_SUPPORTED);
        rv_set_reg(rv, rv_reg_a1, 0);
        break;
    }
}

static void syscall_sbi_rst(riscv_t *rv)
{
    const riscv_word_t fid = rv_get_reg(rv, rv_reg_a6);
    const riscv_word_t a0 = rv_get_reg(rv, rv_reg_a0);
    const riscv_word_t a1 = rv_get_reg(rv, rv_reg_a1);

    switch (fid) {
    case SBI_RST_SYSTEM_RESET:
        rv_log_info("系统复位：type=%u, reason=%u", a0, a1);
        rv_halt(rv);
        rv_set_reg(rv, rv_reg_a0, SBI_SUCCESS);
        rv_set_reg(rv, rv_reg_a1, 0);
        break;
    default:
        rv_set_reg(rv, rv_reg_a0, SBI_ERR_NOT_SUPPORTED);
        rv_set_reg(rv, rv_reg_a1, 0);
        break;
    }
}
#endif /* SYSTEM */

void syscall_handler(riscv_t *rv)
{
/* 读取系统调用号。 */
#if !RV32_HAS(RV32E)
    riscv_word_t syscall = rv_get_reg(rv, rv_reg_a7);
#else
    riscv_word_t syscall = rv_get_reg(rv, rv_reg_t0);
#endif

    switch (syscall) { /* 分派系统调用。 */
#define _(name, number)     \
    case SYS_##name:        \
        syscall_##name(rv); \
        break;
        SUPPORTED_SYSCALLS
#undef _
    default:
        rv_log_fatal("未知系统调用：%d", (int) syscall);
        break;
    }

    /* 保存返回码，返回码用途由上层应用决定。
     */
    vm_attr_t *attr = PRIV(rv);
    attr->error = rv_get_reg(rv, rv_reg_a0);
}
