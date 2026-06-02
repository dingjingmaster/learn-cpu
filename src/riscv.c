/*
 * rv32emu 可依据 MIT 许可证自由再分发。使用和再分发规则见 LICENSE 文件。
 */

/*
 * RISC-V 虚拟机实例管理。
 *
 * 本文件实现 rv_create/rv_run/rv_delete 等生命周期函数，负责初始化寄存器、
 * 内存、ELF 或系统镜像、设备、GDB stub、JIT 缓存和 T2C 后台线程。系统模式下
 * 还会构造 DTB、映射内核/initrd、设置终端输入模式并在退出时同步设备状态。
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#if RV32_HAS(SYSTEM_MMIO)
#include <termios.h>
#include "dtc/libfdt/libfdt.h"
#endif

#if !defined(_WIN32) && !defined(_WIN64)
#include <unistd.h>
#define FILENO(x) fileno(x)
#else
#define FILENO(x) _fileno(x)
#define STDIN_FILENO FILENO(stdin)
#define STDOUT_FILENO FILENO(stdout)
#define STDERR_FILENO FILENO(stderr)
#endif

#if defined(__EMSCRIPTEN__)
#include "em_runtime.h"
#endif

#include "elf.h"
#include "mpool.h"
#include "riscv.h"
#include "riscv_private.h"
#include "utils.h"
#if RV32_HAS(JIT)
#if RV32_HAS(T2C)
#include <pthread.h>
#endif
#include "cache.h"
#include "jit.h"
#define CODE_CACHE_SIZE (4 * 1024 * 1024)
#endif

#define BLOCK_IR_MAP_CAPACITY_BITS 10

#if !RV32_HAS(JIT)
/* 初始化基本块哈希表。 */
static void block_map_init(block_map_t *map, const uint8_t bits)
{
    map->block_capacity = 1 << bits;
    map->size = 0;
    map->map = calloc(map->block_capacity, sizeof(struct block *));
    assert(map->map);
}

/* 清空基本块哈希表中的所有基本块和 IR 节点。 */
void block_map_clear(riscv_t *rv)
{
    block_map_t *map = &rv->block_map;
    for (uint32_t i = 0; i < map->block_capacity; i++) {
        block_t *block = map->map[i];
        if (!block)
            continue;

        uint32_t idx;
        rv_insn_t *ir, *next;
        for (idx = 0, ir = block->ir_head; idx < block->n_insn;
             idx++, ir = next) {
            if (ir->fuse)
                mpool_free(rv->fuse_mp, ir->fuse);
            free(ir->branch_table);
            next = ir->next;
            mpool_free(rv->block_ir_mp, ir);
        }
        mpool_free(rv->block_mp, block);
        map->map[i] = NULL;
    }
    map->size = 0;

    /* 清空 L1 直接映射基本块缓存。使用无效 tag 避免 PC=0 边界场景误命中；
     * tag 和指针分离存放，tag 优先用于高效 miss 检测，ptrs 用 memset 清零。
     */
    for (int i = 0; i < BLOCK_L1_SIZE; i++)
        rv->block_l1.tags[i] = BLOCK_L1_INVALID_TAG;
    memset(rv->block_l1.ptrs, 0, sizeof(rv->block_l1.ptrs));
}

static void block_map_destroy(riscv_t *rv)
{
    block_map_clear(rv);
    free(rv->block_map.map);

    mpool_destroy(rv->block_mp);
    mpool_destroy(rv->block_ir_mp);
    mpool_destroy(rv->fuse_mp);
}
#endif

bool rv_set_pc(riscv_t *rv, riscv_word_t pc)
{
    assert(rv);
#if RV32_HAS(EXT_C)
    if (pc & 1)
#else
    if (pc & 3)
#endif
        return false;

    rv->PC = pc;
    return true;
}

riscv_word_t rv_get_pc(riscv_t *rv)
{
    assert(rv);
    return rv->PC;
}

void rv_set_reg(riscv_t *rv, uint32_t reg, riscv_word_t in)
{
    assert(rv);
    if (reg < N_RV_REGS && reg != rv_reg_zero)
        rv->X[reg] = in;
}

riscv_word_t rv_get_reg(riscv_t *rv, uint32_t reg)
{
    assert(rv);
    if (reg < N_RV_REGS)
        return rv->X[reg];

    return ~0U;
}

/* 重映射标准流。
 *
 * @rv: RISC-V 实例。
 * @fsp: fd 到 FILE* 的映射列表。
 * @fsp_size: 列表长度。
 *
 * 注意：fsp 中的 fd 只应为 0、1、2。
 */
void rv_remap_stdstream(riscv_t *rv, fd_stream_pair_t *fsp, uint32_t fsp_size)
{
    assert(rv);

    vm_attr_t *attr = PRIV(rv);
    assert(attr && attr->fd_map);

    for (uint32_t i = 0; i < fsp_size; i++) {
        int fd = fsp[i].fd;
        FILE *file = fsp[i].file;

        if (!file)
            continue;
        if (fd != STDIN_FILENO && fd != STDOUT_FILENO && fd != STDERR_FILENO)
            continue;

        /* 检查该标准 fd 是否已经存在映射。 */
        map_iter_t it;
        map_find(attr->fd_map, &it, &fd);
        if (it.node) /* 已存在则先删除旧映射。 */
            map_erase(attr->fd_map, &it);
        map_insert(attr->fd_map, &fd, &file);

        /* 保存新的宿主 fd，保持 vm_attr_t 一致。 */
        int new_fd = FILENO(file);
        assert(new_fd != -1);

        if (fd == STDIN_FILENO)
            attr->fd_stdin = new_fd;
        else if (fd == STDOUT_FILENO) {
            attr->fd_stdout = new_fd;
            rv_log_set_stdout_stream(file);
        } else
            attr->fd_stderr = new_fd;
    }
}

#define MEMIO(op) on_mem_##op
#define IO_HANDLER_IMPL(type, op, RW)                                  \
    static IIF(RW)(                                                    \
        /* 写 */ void MEMIO(op)(UNUSED riscv_t * rv, riscv_word_t addr, \
                               riscv_##type##_t data),                 \
        /* 读 */ riscv_##type##_t MEMIO(op)(UNUSED riscv_t * rv,        \
                                           riscv_word_t addr))         \
    {                                                                  \
        IIF(RW)(memory_##op(addr, (uint8_t *) &data),                  \
                return memory_##op(addr));                             \
    }

#if !RV32_HAS(SYSTEM)
#define R 0
#define W 1

IO_HANDLER_IMPL(word, ifetch, R)
IO_HANDLER_IMPL(word, read_w, R)
IO_HANDLER_IMPL(half, read_s, R)
IO_HANDLER_IMPL(byte, read_b, R)

IO_HANDLER_IMPL(word, write_w, W)
IO_HANDLER_IMPL(half, write_s, W)
IO_HANDLER_IMPL(byte, write_b, W)

#undef R
#undef W
#endif

#if RV32_HAS(T2C)
static pthread_t t2c_thread;
static void *t2c_runloop(void *arg)
{
    riscv_t *rv = (riscv_t *) arg;
    pthread_mutex_lock(&rv->wait_queue_lock);
    while (!rv->quit) {
        /* 等待编译任务或退出信号。 */
        while (list_empty(&rv->wait_queue) && !rv->quit)
            pthread_cond_wait(&rv->wait_queue_cond, &rv->wait_queue_lock);

        if (rv->quit)
            break;

        /* 持锁取出一个待编译任务。 */
        queue_entry_t *entry =
            list_last_entry(&rv->wait_queue, queue_entry_t, list);
        list_del_init(&entry->list);
        pthread_mutex_unlock(&rv->wait_queue_lock);

        /* 尽量降低锁竞争地执行编译。
         *
         * 锁策略：仅在访问共享数据时持有 cache_lock：
         * 1. 初始查找和校验（短临界区）。
         * 2. 最终更新 jit_cache（短临界区）。
         *
         * 成本较高的 LLVM 编译阶段不持有 cache_lock，使 SFENCE.VMA/FENCE.I 可以
         * 以较低延迟继续推进。如果基本块在编译期间失效，会通过 invalidated 标志
         * 检测并丢弃编译结果。
         */
        pthread_mutex_lock(&rv->cache_lock);
        /* 用 key 从缓存查找基本块；它可能已被淘汰。 */
        uint32_t pc = (uint32_t) entry->key;
        block_t *block = (block_t *) cache_get(rv->block_cache, pc, false);
#if RV32_HAS(SYSTEM)
        /* 系统模式下还要确认 SATP 匹配。 */
        uint32_t satp = (uint32_t) (entry->key >> 32);
        if (block && block->satp != satp)
            block = NULL;
#endif
        /* 仅当基本块仍存在于缓存中时才继续编译。 */
        if (block)
            t2c_compile(rv, block, &rv->cache_lock);
        else
            pthread_mutex_unlock(&rv->cache_lock);
        free(entry);

        pthread_mutex_lock(&rv->wait_queue_lock);
    }
    pthread_mutex_unlock(&rv->wait_queue_lock);
    return NULL;
}
#endif

#if RV32_HAS(SYSTEM_MMIO)
/* 把文件映射到指定内存位置。
 * max_size > 0 时校验文件大小不得超过 max_size。成功时返回实际文件大小；超过
 * max_size 时返回 -1，由调用者负责输出更具体的错误信息；其他错误直接退出。
 */
static off_t map_file(char **ram_loc, const char *name, off_t max_size)
{
    int fd = open(name, O_RDONLY);
    if (fd < 0)
        goto fail;

    /* 获取文件大小。 */
    struct stat st;
    if (fstat(fd, &st) < 0)
        goto cleanup;

    /* 如果指定了 max_size，先校验文件大小。 */
    if (max_size > 0 && st.st_size > max_size) {
        close(fd);
        return -1; /* 调用者负责输出错误信息。 */
    }

#if HAVE_MMAP
    /* 把文件重新映射到目标内存区域。Emscripten/Windows 不支持带位置提示的 mmap，
     * 因此使用 read 回退路径。
     */
    *ram_loc = mmap(*ram_loc, st.st_size, PROT_READ | PROT_WRITE,
                    MAP_FIXED | MAP_PRIVATE, fd, 0);
    if (*ram_loc == MAP_FAILED)
        goto cleanup;
#else
    if (read(fd, *ram_loc, st.st_size) != st.st_size) {
        free(*ram_loc);
        goto cleanup;
    }
#endif

    /*
     * 内核会选择附近的页边界并尝试创建映射；ram_loc 随实际文件长度向后推进。
     */
    *ram_loc += st.st_size;
    close(fd);
    return st.st_size;

cleanup:
    close(fd);
fail:
    rv_log_fatal("map_file() 处理 %s 失败：%s", name, strerror(errno));
    exit(EXIT_FAILURE);
}

#define ALIGN_FDT(x) (((x) + (FDT_TAGSIZE) - 1) & ~((FDT_TAGSIZE) - 1))
static char *realloc_property(char *fdt,
                              int nodeoffset,
                              const char *name,
                              int newlen)
{
    int delta = 0;
    int oldlen = 0;

    if (!fdt_get_property(fdt, nodeoffset, name, &oldlen))
        /* 字符串表条目和 property 头。 */
        delta = sizeof(struct fdt_property) + strlen(name) + 1;

    if (newlen > oldlen)
        /* off_struct 中的实际属性值。 */
        delta += ALIGN_FDT(newlen) - ALIGN_FDT(oldlen);

    int new_sz = fdt_totalsize(fdt) + delta;
    /* 这里假设预留 RAM 足够容纳扩展后的 FDT，因此不重新分配 fdt 内存。 */
    fdt_open_into(fdt, fdt, new_sz);
    return fdt;
}

static void load_dtb(char **ram_loc, vm_attr_t *attr)
{
#include "minimal_dtb.h"
    char *bootargs = attr->data.system.bootargs;
    char **vblk = attr->data.system.vblk_device;
    char *blob = *ram_loc;
    char *buf;
    size_t len;
    int node, err;
    int totalsize;

#define DTB_EXPAND_SIZE 1024 /* 必要时可以继续增大。 */

    /* 为 DTB 和额外扩展空间分配缓冲区。 */
    size_t minimal_len = ARRAY_SIZE(minimal);
    void *dtb_buf = calloc(minimal_len + DTB_EXPAND_SIZE, sizeof(uint8_t));
    assert(dtb_buf);

    /* 展开为可修改的 DTB blob。 */
    err = fdt_open_into(minimal, dtb_buf, minimal_len + DTB_EXPAND_SIZE);
    if (err < 0) {
        rv_log_error("fdt_open_into 失败\n");
        exit(EXIT_FAILURE);
    }

    if (bootargs) {
        node = fdt_path_offset(dtb_buf, "/chosen");
        assert(node > 0);

        len = strlen(bootargs);
        buf = malloc(len + 1);
        assert(buf);
        memcpy(buf, bootargs, len);
        buf[len] = 0;
        err = fdt_setprop(dtb_buf, node, "bootargs", buf, len + 1);
        if (err == -FDT_ERR_NOSPACE) {
            dtb_buf = realloc_property(dtb_buf, node, "bootargs", len);
            err = fdt_setprop(dtb_buf, node, "bootargs", buf, len);
        }
        free(buf);
        assert(!err);
    }

/* 编译期未启用 RTC 时，从 DTB 删除 rtc 节点。 */
#if !RV32_HAS(GOLDFISH_RTC)
    const char *rtc_path = fdt_get_alias(dtb_buf, "rtc0");
    assert(rtc_path);

    node = fdt_path_offset(dtb_buf, rtc_path);
    assert(node > 0);

    err = fdt_del_node(dtb_buf, node);
    if (err < 0)
        rv_log_warn("从 DTB 删除 rtc 节点失败");
#endif

    if (vblk) {
        int node = fdt_path_offset(dtb_buf, "/soc@F0000000");
        assert(node >= 0);

        uint32_t base_addr = 0x4000000;
        uint32_t addr_offset = 0x100000;
        uint32_t size = 0x200;

        uint32_t next_addr = base_addr;
        uint32_t next_irq = 1;

        /* 扫描已有节点，计算下一个可用地址和 IRQ。 */
        int subnode;
        fdt_for_each_subnode(subnode, dtb_buf, node)
        {
            const char *name = fdt_get_name(dtb_buf, subnode, NULL);
            assert(name);

            char *at_pos = strchr(name, '@');
            assert(at_pos);

            char *endptr;
            uint32_t addr = strtoul(at_pos + 1, &endptr, 16);
            if (endptr == at_pos + 1) {
                attr->vblk_cnt = 0;
                rv_log_error(
                    "节点 %s 的 unit-address 无效，跳过 virtio-blk MMIO",
                    name);
                goto dtb_end;
            }
            if (addr == next_addr)
                next_addr = addr + addr_offset;

            const fdt32_t *irq_prop =
                fdt_getprop(dtb_buf, subnode, "interrupts", NULL);
            if (irq_prop) {
                uint32_t irq = fdt32_to_cpu(*irq_prop);
                if (irq == next_irq)
                    next_irq = irq + 1;
            }
        }
        /* 设置 virtio-blk IRQ 基址，定义见 devices/virtio.h。 */
        attr->vblk_irq_base = next_irq;

        /* 设置 VBLK MMIO 有效范围。 */
        attr->vblk_mmio_base_hi = next_addr >> 20;
        attr->vblk_mmio_max_hi = attr->vblk_mmio_base_hi + attr->vblk_cnt;

        /* 添加新的 virtio-blk DTB 节点。 */
        for (int i = 0; i < attr->vblk_cnt; i++) {
            uint32_t new_addr = next_addr + i * addr_offset;
            uint32_t new_irq = next_irq + i;

            char node_name[32];
            snprintf(node_name, sizeof(node_name), "virtio@%x", new_addr);

            int subnode = fdt_add_subnode(dtb_buf, node, node_name);
            if (subnode == -FDT_ERR_NOSPACE) {
                rv_log_warn("添加子节点失败：DTB 空间不足\n");
            }
            assert(subnode >= 0);

            /* compatible = "virtio,mmio"。 */
            assert(fdt_setprop_string(dtb_buf, subnode, "compatible",
                                      "virtio,mmio") == 0);

            /* reg = <new_addr size>。 */
            uint32_t reg[2] = {cpu_to_fdt32(new_addr), cpu_to_fdt32(size)};
            assert(fdt_setprop(dtb_buf, subnode, "reg", reg, sizeof(reg)) == 0);

            /* interrupts = <new_irq>。 */
            uint32_t irq = cpu_to_fdt32(new_irq);
            assert(fdt_setprop(dtb_buf, subnode, "interrupts", &irq,
                               sizeof(irq)) == 0);
        }
    }

dtb_end:
    memcpy(blob, dtb_buf, minimal_len + DTB_EXPAND_SIZE);
    free(dtb_buf);

    totalsize = fdt_totalsize(blob);
    *ram_loc += totalsize;
    return;
}

/*
 * 键盘控制模式标志。
 *
 * ICANON：启用规范模式。
 * ECHO：回显输入字符。
 * ISIG：收到 INTR、QUIT、SUSP 或 DSUSP 等控制字符时产生对应信号。
 *
 * 退出时必须重新启用 ISIG；否则默认信号处理器无法捕获信号，例如 CTRL+c 产生的
 * SIGINT。
 *
 */
#define TERMIOS_C_CFLAG (ICANON | ECHO | ISIG)
static void reset_keyboard_input()
{
    struct termios term;
    tcgetattr(0, &term);
    term.c_lflag |= TERMIOS_C_CFLAG;
    tcsetattr(0, TCSANOW, &term);
}

/* 切换终端模式，让 VM 异步捕获所有键盘输入。 */
static void capture_keyboard_input()
{
    /* 注册退出钩子，退出时恢复默认控制模式。 */
    atexit(reset_keyboard_input);

    struct termios term;
    tcgetattr(0, &term);
    term.c_lflag &= ~TERMIOS_C_CFLAG;
    tcsetattr(0, TCSANOW, &term);
}

#endif

#if RV32_HAS(SYSTEM_MMIO)
/*
 *
 * atexit() 注册的是 void (*)(void) 回调，不能传参。运行期仍需要释放内存，而
 * block_map_clear() 需要 RISC-V 实例且仅解释器模式使用。这里不修改其函数签名，
 * 而是通过 main.c 暴露的全局 RISC-V 实例访问运行时。
 *
 */
extern riscv_t *rv;
static void rv_async_block_clear()
{
#if !RV32_HAS(JIT)
    if (rv && rv->block_map.size)
        block_map_clear(rv);
#else  /* TODO：JIT 模式。 */
    return;
#endif /* !RV32_HAS(JIT) */
}

static void rv_fsync_device()
{
    if (!rv)
        return;

    vm_attr_t *attr = PRIV(rv);
    /*
     * mmap 回退路径下可能需要手动写回并同步设备。
     *
     * vblk 是可选设备，可能为 NULL。
     */
    if (attr->vblk_cnt) {
        for (int i = 0; i < attr->vblk_cnt; i++) {
            virtio_blk_state_t *vblk = attr->vblk[i];
            if (vblk->disk_fd >= 3) {
                if (vblk->device_features & VIRTIO_BLK_F_RO) /* 只读。 */
                    goto end;

                if (pwrite(vblk->disk_fd, vblk->disk, vblk->disk_size, 0) ==
                    -1) {
                    rv_log_error("写回块设备失败：%s",
                                 strerror(errno));
                    return;
                }

                if (fsync(vblk->disk_fd) == -1) {
                    rv_log_error("同步块设备失败：%s",
                                 strerror(errno));
                    return;
                }
                rv_log_info("块设备同步完成");

            end:
                close(vblk->disk_fd);
            }

            vblk_delete(vblk);
        }

        free(attr->vblk);
        free(attr->disk);
    }
}
#endif /* RV32_HAS(SYSTEM_MMIO) */

riscv_t *rv_create(riscv_user_t rv_attr)
{
    assert(rv_attr);

    riscv_t *rv = calloc(1, sizeof(riscv_t));
    if (!rv)
        return NULL;
    assert(rv);

#if RV32_HAS(SYSTEM_MMIO)
    /* 注册 CTRL+a+x 退出时的清理回调。 */
    atexit(rv_async_block_clear);
    /* 注册 CTRL+a+x 退出时的设备同步回调。 */
    atexit(rv_fsync_device);
#endif

    /* 保存调用者传入的属性指针。 */
    rv->data = rv_attr;

    vm_attr_t *attr = PRIV(rv);
    attr->mem = memory_new(attr->mem_size);
    assert(attr->mem);
    assert(!(((uintptr_t) attr->mem) & 0b11));

    /* 初始化寄存器和栈。 */
    rv_reset(rv, 0U);

    /*
     * 默认标准流。
     * 调用者可通过 rv_remap_stdstream() 覆盖它们。
     *
     * 日志 stdout 流也会一起重映射。
     *
     */
    attr->fd_map = map_init(int, FILE *, map_cmp_int);
    rv_remap_stdstream(rv,
                       (fd_stream_pair_t[]) {
                           {STDIN_FILENO, stdin},
                           {STDOUT_FILENO, stdout},
                           {STDERR_FILENO, stderr},
                       },
                       3);

    rv_log_set_level(attr->log_level);
    rv_log_info("日志级别：%s", rv_log_level_string(attr->log_level));

#if !RV32_HAS(SYSTEM_MMIO)
    elf_t *elf = elf_new();
    assert(elf);

    if (!elf_open(elf, attr->data.user.elf_program)) {
        rv_log_fatal("elf_open() 失败");
        map_delete(attr->fd_map);
        memory_delete(attr->mem);
        free(rv);
        exit(EXIT_FAILURE);
    }
    rv_log_info("%s ELF 已装载", attr->data.user.elf_program);

    const struct Elf32_Sym *end;
    if ((end = elf_get_symbol(elf, "_end")))
        attr->break_addr = end->st_value;

#if !RV32_HAS(SYSTEM)
    /* 初始状态不是退出中。 */
    attr->on_exit = false;
    attr->exit_addr = 0;

    /* 尝试从符号表查找退出函数地址。不同工具链/libc 可能使用不同符号名，
     * 因此按多个名称兜底查找。
     */
    const struct Elf32_Sym *exit_sym;
    if ((exit_sym = elf_get_symbol(elf, "exit")))
        attr->exit_addr = exit_sym->st_value;
    else if ((exit_sym = elf_get_symbol(elf, "_exit")))
        attr->exit_addr = exit_sym->st_value;
#endif

    assert(elf_load(elf, attr->mem));

    /* 设置入口 PC。 */
    const struct Elf32_Ehdr UNUSED *hdr = get_elf_header(elf);
    assert(rv_set_pc(rv, hdr->e_entry));

    elf_delete(elf);

/* 结合 USE_ELF 支持系统测试套件。 */
#if RV32_HAS(SYSTEM)
    /* mmu_io 定义在 system.c 中，具有外部链接。 */
    extern riscv_io_t mmu_io;
    /* 安装 MMU I/O 处理器。 */
    memcpy(&rv->io, &mmu_io, sizeof(riscv_io_t));
#else
    /* 安装用户态直接内存 I/O 处理器。 */
    const riscv_io_t io = {
        /* 内存读取接口。 */
        .mem_ifetch = MEMIO(ifetch),
        .mem_read_w = MEMIO(read_w),
        .mem_read_s = MEMIO(read_s),
        .mem_read_b = MEMIO(read_b),

        /* 内存写入接口。 */
        .mem_write_w = MEMIO(write_w),
        .mem_write_s = MEMIO(write_s),
        .mem_write_b = MEMIO(write_b),

        /* 系统服务和必要运行时例程。 */
        .on_ecall = ecall_handler,
        .on_ebreak = ebreak_handler,
        .on_memcpy = memcpy_handler,
        .on_memset = memset_handler,
        .on_trap = trap_handler,
    };
    memcpy(&rv->io, &io, sizeof(riscv_io_t));
#endif /* RV32_HAS(SYSTEM) */

#else
    /* *-----------------------------------------*
     * |              内存布局                   |
     * *----------------*----------------*-------*
     * |  kernel image  |  initrd image  |  dtb  |
     * *----------------*----------------*-------*
     */

    /* load_dtb 需要设备数量，用于动态添加 virtio-blk 子节点。 */
    attr->vblk_cnt = attr->data.system.vblk_device_cnt;

    char *ram_loc = (char *) attr->mem->mem_base;
    map_file(&ram_loc, attr->data.system.kernel, 0);
    rv_log_info("内核已装载");

    uint32_t dtb_addr = attr->mem->mem_size - DTB_SIZE;
    ram_loc = ((char *) attr->mem->mem_base) + dtb_addr;
    load_dtb(&ram_loc, attr);
    rv_log_info("DTB 已装载");
    /* 将可选 initrd 镜像加载到 dtb 区域之前。
     * initrd 区域大小由编译期 INITRD_SIZE 定义。
     */
    if (attr->data.system.initrd) {
        /* 确保内存足够容纳 initrd 区域。 */
        if (dtb_addr < INITRD_SIZE) {
            rv_log_fatal(
                "内存过小，无法容纳 INITRD_SIZE（%u MiB）。请增大 MEM_SIZE。",
                INITRD_SIZE / (1024 * 1024));
            exit(EXIT_FAILURE);
        }
        uint32_t initrd_addr = dtb_addr - INITRD_SIZE;
        ram_loc = ((char *) attr->mem->mem_base) + initrd_addr;
        off_t initrd_size =
            map_file(&ram_loc, attr->data.system.initrd, INITRD_SIZE);
        if (initrd_size < 0) {
            /* 文件超过 max_size 时 map_file 返回 -1。 */
            rv_log_fatal(
                "Initrd 文件超过 INITRD_SIZE（%u MiB）。\n"
                "请使用更大的 INITRD_SIZE 重新构建，例如：\n"
                "  make ENABLE_SYSTEM=1 INITRD_SIZE=64 system",
                INITRD_SIZE / (1024 * 1024));
            exit(EXIT_FAILURE);
        }
        rv_log_info("Rootfs 已装载（%ld 字节）", (long) initrd_size);
    }

    /* mmu_io 定义在 system.c 中，具有外部链接。 */
    extern riscv_io_t mmu_io;
    memcpy(&rv->io, &mmu_io, sizeof(riscv_io_t));

    /* 设置 RISC-V hart 启动参数。 */
    rv_set_reg(rv, rv_reg_a0, 0);
    rv_set_reg(rv, rv_reg_a1, dtb_addr);

    /* 设置定时器。 */
    attr->timer = 0xFFFFFFFFFFFFFFF;

    /* 设置 PLIC。 */
    attr->plic = plic_new();
    assert(attr->plic);
    attr->plic->rv = rv;

    /* 设置 UART。 */
    attr->uart = u8250_new();
    assert(attr->uart);
    attr->uart->in_fd = attr->fd_stdin;
    attr->uart->out_fd = attr->fd_stdout;

    /* 设置 RTC。 */
#if RV32_HAS(GOLDFISH_RTC)
    attr->rtc = rtc_new();
    assert(attr->rtc);
#endif /* RV32_HAS(GOLDFISH_RTC) */

    attr->vblk = calloc(attr->vblk_cnt, sizeof(virtio_blk_state_t *));
    assert(attr->vblk);
    attr->disk = calloc(attr->vblk_cnt, sizeof(uint32_t *));
    assert(attr->disk);

    if (attr->vblk_cnt) {
        for (int i = 0; i < attr->vblk_cnt; i++) {
/* 当前只用于块镜像路径和权限选项。 */
#define MAX_OPTS 2
            char *vblk_device_str = attr->data.system.vblk_device[i];
            if (!vblk_device_str[0]) {
                rv_log_error("磁盘路径不能为空");
                exit(EXIT_FAILURE);
            }

            char *vblk_opts[MAX_OPTS] = {NULL};
            int vblk_opt_idx = 0;
            char *opt = strtok(vblk_device_str, ",");
            while (opt) {
                if (vblk_opt_idx == MAX_OPTS) {
                    rv_log_error("vblk 参数过多");
                    break;
                }
                vblk_opts[vblk_opt_idx++] = opt;
                opt = strtok(NULL, ",");
            }

            char *vblk_device;
            char *vblk_readonly = vblk_opts[1];
            bool readonly = false;

            if (vblk_opts[0][0] == '~') {
                /* macOS 和 Linux 发行版通常都会由登录程序设置 HOME 环境变量。 */
                const char *home = getenv("HOME");
                if (!home) {
                    rv_log_error(
                        "HOME 环境变量未设置，无法访问磁盘 %s",
                        vblk_opts[0]);
                    exit(EXIT_FAILURE);
                }

                const char *suffix = vblk_opts[0] + 1; /* 跳过 "~"。 */
                size_t home_len = strlen(home);
                size_t suffix_len = strlen(suffix);
                if (home_len > SIZE_MAX - suffix_len - 1) {
                    rv_log_error("磁盘路径过长");
                    exit(EXIT_FAILURE);
                }
                size_t path_len = home_len + suffix_len + 1;
                vblk_device = malloc(path_len);
                if (!vblk_device) {
                    rv_log_error("为磁盘路径分配内存失败");
                    exit(EXIT_FAILURE);
                }
                snprintf(vblk_device, path_len, "%s%s", home, suffix);
            } else {
                vblk_device = vblk_opts[0];
            }

            if (vblk_readonly) {
                if (strcmp(vblk_readonly, "readonly") != 0) {
                    rv_log_error("未知 vblk 选项：%s", vblk_readonly);
                    exit(EXIT_FAILURE);
                }
                readonly = true;
            }

            attr->vblk[i] = vblk_new();
            attr->vblk[i]->ram = (uint32_t *) attr->mem->mem_base;
            attr->disk[i] =
                virtio_blk_init(attr->vblk[i], vblk_device, readonly);

            if (vblk_opts[0][0] == '~')
                free(vblk_device);
        }
    }

    capture_keyboard_input();
#endif /* !RV32_HAS(SYSTEM_MMIO) */

    /* 创建基本块和 IR 节点内存池。 */
    rv->block_mp = mpool_create(sizeof(block_t) << BLOCK_MAP_CAPACITY_BITS,
                                sizeof(block_t));
    rv->block_ir_mp = mpool_create(
        sizeof(rv_insn_t) << BLOCK_IR_MAP_CAPACITY_BITS, sizeof(rv_insn_t));
    /* 融合池：用于宏操作融合数组的固定大小槽位。
     * 每个槽位最多容纳 FUSE_MAX_ENTRIES 个 opcode_fuse_t 结构。
     */
    rv->fuse_mp = mpool_create(FUSE_SLOT_SIZE << BLOCK_IR_MAP_CAPACITY_BITS,
                               FUSE_SLOT_SIZE);
    if (!rv->block_mp || !rv->block_ir_mp || !rv->fuse_mp) {
        rv_log_fatal("创建内存池失败");
        goto fail_mpool;
    }

#if !RV32_HAS(JIT)
    /* 初始化基本块哈希表。 */
    block_map_init(&rv->block_map, BLOCK_MAP_CAPACITY_BITS);

    /* 用无效 tag 初始化 L1 基本块缓存。 */
    for (int i = 0; i < BLOCK_L1_SIZE; i++)
        rv->block_l1.tags[i] = BLOCK_L1_INVALID_TAG;
    memset(rv->block_l1.ptrs, 0, sizeof(rv->block_l1.ptrs));
#else
    INIT_LIST_HEAD(&rv->block_list);
    rv->jit_state = jit_state_init(CODE_CACHE_SIZE);
    if (!rv->jit_state) {
        rv_log_fatal("初始化 JIT 状态失败");
        goto fail_jit_state;
    }
    rv->block_cache = cache_create(BLOCK_MAP_CAPACITY_BITS);
    if (!rv->block_cache) {
        rv_log_fatal("创建基本块缓存失败");
        goto fail_block_cache;
    }
#if RV32_HAS(T2C)
    rv->quit = false;
    rv->jit_cache = jit_cache_init();
    if (!rv->jit_cache) {
        rv_log_fatal("初始化 JIT 缓存失败");
        goto fail_jit_cache;
    }
    rv->inline_cache = inline_cache_init();
    if (!rv->inline_cache) {
        rv_log_fatal("初始化 inline cache 失败");
        goto fail_inline_cache;
    }
    /* 准备后台编译等待队列。 */
    pthread_mutex_init(&rv->wait_queue_lock, NULL);
    pthread_mutex_init(&rv->cache_lock, NULL);
    pthread_cond_init(&rv->wait_queue_cond, NULL);
    INIT_LIST_HEAD(&rv->wait_queue);
    /* 启动后台编译线程。
     * 使用较大的 8MB 栈，以容纳 t2c_trace_ebb 的深递归和 LLVM 编译期内部栈使用。
     */
    pthread_attr_t t2c_attr;
    pthread_attr_init(&t2c_attr);
    pthread_attr_setstacksize(&t2c_attr, 8 * 1024 * 1024); /* 8MB 栈。 */
    pthread_create(&t2c_thread, &t2c_attr, t2c_runloop, rv);
    pthread_attr_destroy(&t2c_attr);
#endif
#endif

    return rv;

#if RV32_HAS(JIT)
#if RV32_HAS(T2C)
fail_inline_cache:
    jit_cache_exit(rv->jit_cache);
fail_jit_cache:
    cache_free(rv->block_cache);
#endif
fail_block_cache:
    if (rv->jit_state)
        jit_state_exit(rv->jit_state);
fail_jit_state:
#endif
fail_mpool:
    mpool_destroy(rv->block_ir_mp);
    mpool_destroy(rv->block_mp);
    mpool_destroy(rv->fuse_mp);
#if RV32_HAS(SYSTEM_MMIO)
    if (attr->uart)
        u8250_delete(attr->uart);
    if (attr->plic)
        plic_delete(attr->plic);
#if RV32_HAS(GOLDFISH_RTC)
    if (attr->rtc)
        rtc_delete(attr->rtc);
#endif
    if (attr->vblk) {
        for (int i = 0; i < attr->vblk_cnt; i++) {
            if (attr->vblk[i])
                vblk_delete(attr->vblk[i]);
        }
        free(attr->vblk);
    }
    free(attr->disk);
#endif
    map_delete(attr->fd_map);
    memory_delete(attr->mem);
    free(rv);
    return NULL;
}

#if !RV32_HAS(SYSTEM_MMIO)
/*
 * TODO：支持跟踪 Linux 内核符号。
 */
static void rv_run_and_trace(riscv_t *rv)
{
    assert(rv);

    vm_attr_t *attr = PRIV(rv);
    assert(attr && attr->data.user.elf_program);
    attr->cycle_per_step = 1;

    const char UNUSED *prog_name = attr->data.user.elf_program;
    elf_t *elf = elf_new();
    assert(elf && elf_open(elf, prog_name));

    for (; !rv_has_halted(rv);) { /* 持续运行直到 halt 标志置位。 */
        /* 跟踪执行位置。 */
        uint32_t pc = rv_get_pc(rv);
        const char *sym = elf_find_symbol(elf, pc);
        rv_log_trace("%08x  %s", pc, (sym ? sym : ""));

        rv_step(rv); /* 执行一批指令。 */
    }

    elf_delete(elf);
}
#endif

#if RV32_HAS(GDBSTUB)
/* 以 gdbstub 模式运行 RISC-V 模拟器。 */
void rv_debug(riscv_t *rv);
#endif

void rv_profile(riscv_t *rv, char *out_file_path);

void rv_run(riscv_t *rv)
{
    assert(rv);

    vm_attr_t *attr = PRIV(rv);
    assert(attr &&
#if RV32_HAS(SYSTEM_MMIO)
           attr->data.system.kernel && attr->data.system.initrd
#else
           attr->data.user.elf_program
#endif
    );

    if (!(attr->run_flag & (RV_RUN_TRACE | RV_RUN_GDBSTUB))) {
#ifdef __EMSCRIPTEN__
        emscripten_set_main_loop_arg(rv_step, (void *) rv, 0, 1);
#else
        /* 默认主循环。 */
        for (; !rv_has_halted(rv);) /* 持续运行直到 halt 标志置位。 */
            rv_step(rv);            /* 执行一批指令。 */
#endif
    }
#if !RV32_HAS(SYSTEM_MMIO)
    else if (attr->run_flag & RV_RUN_TRACE)
        rv_run_and_trace(rv);
#endif
#if RV32_HAS(GDBSTUB)
    else if (attr->run_flag & RV_RUN_GDBSTUB)
        rv_debug(rv);
#endif

    if (attr->run_flag & RV_RUN_PROFILE) {
        assert(attr->profile_output_file);
        rv_profile(rv, attr->profile_output_file);
    }
}

void rv_halt(riscv_t *rv)
{
    rv->halt = true;
}

bool rv_has_halted(riscv_t *rv)
{
    return rv->halt;
}

#if RV32_HAS(ARCH_TEST)
void rv_set_tohost_addr(riscv_t *rv, uint32_t addr)
{
    rv->tohost_addr = addr;
}

void rv_set_fromhost_addr(riscv_t *rv, uint32_t addr)
{
    rv->fromhost_addr = addr;
}
#endif

void rv_delete(riscv_t *rv)
{
    assert(rv);
#if !RV32_HAS(JIT) || (RV32_HAS(SYSTEM_MMIO))
    vm_attr_t *attr = PRIV(rv);
#endif
#if !RV32_HAS(JIT)
    map_delete(attr->fd_map);
    memory_delete(attr->mem);
    block_map_destroy(rv);
#else
#if RV32_HAS(T2C)
    /* 通知后台线程退出。 */
    pthread_mutex_lock(&rv->wait_queue_lock);
    rv->quit = true;
    pthread_cond_signal(&rv->wait_queue_cond);
    pthread_mutex_unlock(&rv->wait_queue_lock);

    pthread_join(t2c_thread, NULL);

    /* 清理等待队列中尚未处理的条目。 */
    queue_entry_t *entry, *safe;
    list_for_each_entry_safe (entry, safe, &rv->wait_queue, list) {
        list_del(&entry->list);
        free(entry);
    }

    pthread_mutex_destroy(&rv->wait_queue_lock);
    pthread_mutex_destroy(&rv->cache_lock);
    pthread_cond_destroy(&rv->wait_queue_cond);
    jit_cache_exit(rv->jit_cache);
    inline_cache_exit(rv->inline_cache);

    /* 释放缓存前，先销毁所有剩余基本块上的 LLVM engine。 */
    clear_cache_hot(rv->block_cache, t2c_dispose_block_engine);
#endif
    jit_state_exit(rv->jit_state);
    cache_free(rv->block_cache);
    mpool_destroy(rv->block_ir_mp);
    mpool_destroy(rv->block_mp);
    mpool_destroy(rv->fuse_mp);
#endif
#if RV32_HAS(SYSTEM_MMIO)
    u8250_delete(attr->uart);
    plic_delete(attr->plic);
#if RV32_HAS(GOLDFISH_RTC)
    rtc_delete(attr->rtc);
#endif /* RV32_HAS(GOLDFISH_RTC) */
    /* 同步设备；具体清理由被调函数完成。 */
    rv_fsync_device();
#endif
    free(rv);
}

void rv_reset(riscv_t *rv, riscv_word_t pc)
{
    assert(rv);
    memset(rv->X, 0, sizeof(uint32_t) * N_RV_REGS);

    vm_attr_t *attr = PRIV(rv);
#if !RV32_HAS(SYSTEM_MMIO)
    int argc = attr->argc;
    char **args = attr->argv;
    memory_t *mem = attr->mem;
#endif

    /* 设置复位地址。 */
    rv->PC = pc;

    /* 设置默认栈指针。 */
    rv->X[rv_reg_sp] =
        attr->mem_size - attr->stack_size - attr->args_offset_size;

    /* 用户态：把目标程序的 argc 和 args 写入客体内存。
     * 系统态：跳过这一步，内核启动不使用 argc/argv。
     *
     * 参数内存布局如下：
     * -----------------------
     * |    NULL            |
     * -----------------------
     * |    envp[n]         |
     * -----------------------
     * |    envp[n - 1]     |
     * -----------------------
     * |    ...             |
     * -----------------------
     * |    envp[0]         |
     * -----------------------
     * |    NULL            |
     * -----------------------
     * |    args[n]         |
     * -----------------------
     * |    args[n - 1]     |
     * -----------------------
     * |    ...             |
     * -----------------------
     * |    args[0]         |
     * -----------------------
     * |    argc            |
     * -----------------------
     *
     * TODO：支持访问 envp。
     */
#if !RV32_HAS(SYSTEM_MMIO)
    /* 把参数字符串复制到 RAM。 */
    uintptr_t args_size = (1 + argc + 1) * sizeof(uint32_t);
    uintptr_t args_bottom = attr->mem_size - attr->stack_size;
    uintptr_t args_top = args_bottom - args_size;
    args_top &= -16;

    /* argc */
    uintptr_t *args_p = (uintptr_t *) args_top;
    assert(memory_write(mem, (uintptr_t) args_p, (void *) &argc, sizeof(int)));
    args_p++;

    /* args */
    /* 记录每个参数长度，供后续压栈时计算偏移。 */
    size_t args_space[256];
    size_t args_space_idx = 0;
    size_t args_len;
    size_t args_len_total = 0;
    for (int i = 0; i < argc; i++) {
        const char *arg = args[i];
        args_len = strlen(arg);
        assert(memory_write(mem, (uintptr_t) args_p, (void *) arg,
                            (args_len + 1) * sizeof(uint8_t)));
        args_space[args_space_idx++] = args_len + 1;
        args_p = (uintptr_t *) ((uintptr_t) args_p + args_len + 1);
        args_len_total += args_len + 1;
    }
    args_p = (uintptr_t *) ((uintptr_t) args_p - args_len_total);
    args_p--; /* 指回 argc。 */

    /* 准备把 argc 和 argv 指针压到栈上。 */
    int stack_size = (1 + argc + 1) * sizeof(uint32_t);
    uintptr_t stack_bottom = (uintptr_t) rv->X[rv_reg_sp];
    uintptr_t stack_top = stack_bottom - stack_size;
    stack_top &= -16;

    /* argc */
    uintptr_t *sp = (uintptr_t *) stack_top;
    assert(memory_write(mem, (uintptr_t) sp,
                        (void *) (mem->mem_base + (uintptr_t) args_p),
                        sizeof(int)));
    args_p++;
    /* 按 RV32 ABI，让 argc 和 args[0] 各占一个字。 */
    sp = (uintptr_t *) ((uint32_t *) sp + 1);

    /* args */
    for (int i = 0; i < argc; i++) {
        uintptr_t offset = (uintptr_t) args_p;
        assert(memory_write(mem, (uintptr_t) sp, (void *) &offset,
                            sizeof(uintptr_t)));
        args_p = (uintptr_t *) ((uintptr_t) args_p + args_space[i]);
        sp = (uintptr_t *) ((uint32_t *) sp + 1);
    }
    assert(memory_fill(mem, (uintptr_t) sp, sizeof(uint32_t), 0));

    /* 重置 sp，使其指向 argc。 */
    rv->X[rv_reg_sp] = stack_top;
#endif /* !RV32_HAS(SYSTEM_MMIO) */

    /* 重置特权模式。 */
#if RV32_HAS(SYSTEM)
    /*
     * 系统模拟默认进入 S 模式，因为该启动路径不依赖 OpenSBI 等 M 模式软件。
     */
    rv->priv_mode = RV_PRIV_S_MODE;

    /* 初始状态未处于 trap 中。 */
    rv->is_trapped = false;

    /* 重置地址转换：清空 SATP 并刷新两个 TLB，避免复用上一次执行遗留的转换结果。
     */
    rv->csr_satp = 0;
    memset(rv->dtlb, 0, sizeof(rv->dtlb));
    memset(rv->itlb, 0, sizeof(rv->itlb));
#else
    /* ISA 用户态模拟默认进入 M 模式。 */
    rv->priv_mode = RV_PRIV_M_MODE;
#endif

    /* 重置 CSR。 */
    rv->csr_mtvec = 0;
    rv->csr_cycle = 0;
#if RV32_HAS(SYSTEM)
    rv->timer_offset = 0;
#endif
    rv->csr_mstatus = 0;
    rv->csr_misa |= MISA_SUPER | MISA_USER;
    rv->csr_mvendorid = RV_MVENDORID;
    rv->csr_marchid = RV_MARCHID;
    rv->csr_mimpid = RV_MIMPID;
#if !RV32_HAS(RV32E)
    rv->csr_misa |= MISA_I;
#else
    rv->csr_misa |= MISA_E;
#endif
#if RV32_HAS(EXT_A)
    rv->csr_misa |= MISA_A;
#endif
#if RV32_HAS(EXT_C)
    rv->csr_misa |= MISA_C;
#endif
#if RV32_HAS(EXT_F)
    rv->csr_misa |= MISA_F;
    /* 重置浮点寄存器。 */
    for (int i = 0; i < N_RV_REGS; i++)
        rv->F[i].v = 0;
    rv->csr_fcsr = 0;
#endif
#if RV32_HAS(EXT_M)
    rv->csr_misa |= MISA_M;
#endif

    rv->halt = false;
}

static const char *insn_name_table[] = {
#define _(inst, can_branch, insn_len, translatable, reg_mask) \
    [rv_insn_##inst] = #inst,
    RV_INSN_LIST
#undef _
#define _(inst) [rv_insn_##inst] = #inst,
        FUSE_INSN_LIST
#undef _
};

#if RV32_HAS(JIT)
static void profile(block_t *block, uint32_t freq, FILE *output_file)
{
    fprintf(output_file, "%#-9x|", block->pc_start);
    fprintf(output_file, "%#-8x|", block->pc_end);
    fprintf(output_file, " %-10u|", freq);
    fprintf(output_file, " %-5s |", block->hot ? "true" : "false");
    fprintf(output_file, " %-6s |", block->has_loops ? "true" : "false");
    rv_insn_t *taken = block->ir_tail->branch_taken,
              *untaken = block->ir_tail->branch_untaken;
    if (untaken)
        fprintf(output_file, "%#-9x|", untaken->pc);
    else
        fprintf(output_file, "%-9s|", "NULL");
    if (taken)
        fprintf(output_file, "%#-8x|", taken->pc);
    else
        fprintf(output_file, "%-8s|", "NULL");
    rv_insn_t *ir = block->ir_head;
    while (1) {
        assert(ir);
        fprintf(output_file, "%s", insn_name_table[ir->opcode]);
        if (!ir->next)
            break;
        ir = ir->next;
        fprintf(output_file, " - ");
    }
    fprintf(output_file, "\n");
}
#endif

void rv_profile(riscv_t *rv, char *out_file_path)
{
    if (!out_file_path) {
        rv_log_warn("profiling 数据输出文件为空");
        return;
    }
    FILE *f = fopen(out_file_path, "w");
    if (!f) {
        rv_log_error("无法打开 profiling 数据输出文件");
        return;
    }
#if RV32_HAS(JIT)
    fprintf(f,
            "PC start |PC end  | frequency |  hot  | loop  | untaken | taken | "
            "IR list \n");
    cache_profile(rv->block_cache, f, (prof_func_t) profile);
#else
    fprintf(f, "PC start |PC end  | untaken | taken  | IR list \n");
    block_map_t *map = &rv->block_map;
    for (uint32_t i = 0; i < map->block_capacity; i++) {
        block_t *block = map->map[i];
        if (!block)
            continue;
        fprintf(f, "%#-9x|", block->pc_start);
        fprintf(f, "%#-8x|", block->pc_end);
        rv_insn_t *taken = block->ir_tail->branch_taken,
                  *untaken = block->ir_tail->branch_untaken;
        if (untaken)
            fprintf(f, "%#-9x|", untaken->pc);
        else
            fprintf(f, "%-9s|", "NULL");
        if (taken)
            fprintf(f, "%#-8x|", taken->pc);
        else
            fprintf(f, "%-8s|", "NULL");
        rv_insn_t *ir = block->ir_head;
        while (1) {
            assert(ir);
            fprintf(f, "%s", insn_name_table[ir->opcode]);
            if (!ir->next)
                break;
            ir = ir->next;
            fprintf(f, " - ");
        }
        fprintf(f, "\n");
    }
#endif
}
