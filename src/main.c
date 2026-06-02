/*
 * rv32emu 可依据 MIT 许可证自由再分发。使用和再分发规则见 LICENSE 文件。
 */

/*
 * 命令行入口。
 *
 * main.c 负责解析模拟器参数、组装 vm_attr_t、创建 riscv_t 实例并调用 rv_run。
 * 运行结束后，它根据选项导出寄存器 JSON、架构测试 signature 和 profile 数据，
 * 最后释放模拟器资源并返回客体程序的退出码。
 */

#include <assert.h>
#include <inttypes.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__EMSCRIPTEN__)
#include "em_runtime.h"
#endif

#include "elf.h"
#include "io.h"
#include "riscv.h"
#include "utils.h"

/* 是否启用程序 trace 模式。 */
#if !RV32_HAS(SYSTEM_MMIO)
static bool opt_trace = false;
#endif

#if RV32_HAS(GDBSTUB)
/* 是否启用 gdbstub 远程调试模式。 */
static bool opt_gdbstub = false;
#endif

/* 是否以 JSON 导出寄存器。 */
static bool opt_dump_regs = false;
static char *registers_out_file;

/* RISC-V 架构测试 signature 输出。 */
static bool opt_arch_test = false;
static char *signature_out_file;

/* 是否静默普通输出。 */
static bool opt_quiet_outputs = false;

/* 客体目标可执行文件。 */
static char *opt_prog_name;

/* 传给客体程序的 argc/argv。 */
static int prog_argc;
static char **prog_args;
static const char *optstr = "tgqmhpd:a:k:i:b:x:";

/* 是否允许非对齐内存访问。 */
static bool opt_misaligned = false;

/* 是否导出 profiling 数据。 */
static bool opt_prof_data = false;
static char *prof_out_file;

#if RV32_HAS(SYSTEM_MMIO)
/* Linux 内核引导数据。 */
static char *opt_kernel_img;
static char *opt_rootfs_img;
static char *opt_bootargs;
/* FIXME：后续应更完整地处理设备数量溢出。 */
#define VBLK_DEV_MAX 100
static char *opt_virtio_blk_img[VBLK_DEV_MAX];
static int opt_virtio_blk_idx = 0;
#endif

static void print_usage(const char *filename)
{
    rv_log_error(
        "\nRV32I[MAFC] 模拟器，可加载 ELF 文件并执行。\n"
        "用法：%s [选项] [文件名] [程序参数]\n"
        "选项：\n"
#if !RV32_HAS(SYSTEM_MMIO)
        "  -t : 打印执行 trace\n"
#endif
#if RV32_HAS(GDBSTUB)
        "  -g : 允许远程 GDB 连接（gdbstub）\n"
#endif
#if RV32_HAS(SYSTEM_MMIO)
        "  -k <image> : 使用 <image> 作为内核镜像\n"
        "  -i <image> : 使用 <image> 作为 rootfs/initrd\n"
        "  -x vblk:<image>[,readonly]: 使用 <image> 作为 virtio-blk "
        "磁盘镜像（默认可读写）。该选项可重复指定多个块设备\n"
        "  -b <bootargs> : 为内核指定自定义 <bootargs>\n"
#endif
        "  -d [filename]: 将寄存器以 JSON 写入指定文件；`-` 表示标准输出\n"
        "  -q : 静默除 dump-registers 之外的普通输出\n"
        "  -a [filename] : 将 signature 写入指定文件，arch-test 需要该输出\n"
        "  -m : 启用非对齐内存访问\n"
        "  -p : 生成 profiling 数据\n"
        "  -h : 显示本帮助信息",
        filename);
}

static bool parse_args(int argc, char **args)
{
    int opt;
    int emu_argc = 0;

    while ((opt = getopt(argc, args, optstr)) != -1) {
        emu_argc++;

        switch (opt) {
#if !RV32_HAS(SYSTEM_MMIO)
        case 't':
            opt_trace = true;
            break;
#endif
#if RV32_HAS(GDBSTUB)
        case 'g':
            opt_gdbstub = true;
            break;
#endif
#if RV32_HAS(SYSTEM_MMIO)
        case 'k':
            opt_kernel_img = optarg;
            emu_argc++;
            break;
        case 'i':
            opt_rootfs_img = optarg;
            emu_argc++;
            break;
        case 'b':
            opt_bootargs = optarg;
            emu_argc++;
            break;
        case 'x':
            if (opt_virtio_blk_idx >= VBLK_DEV_MAX) {
                rv_log_error("virtio-blk 设备过多，最大数量为 %d。\n",
                             VBLK_DEV_MAX);
                return false;
            }
            if (!strncmp("vblk:", optarg, 5))
                opt_virtio_blk_img[opt_virtio_blk_idx++] =
                    optarg + 5; /* strlen("vblk:") */
            else
                return false;
            emu_argc++;
            break;
#endif
        case 'q':
            opt_quiet_outputs = true;
            break;
        case 'h':
            return false;
        case 'm':
            opt_misaligned = true;
            break;
        case 'p':
            opt_prof_data = true;
            break;
        case 'd':
            opt_dump_regs = true;
            registers_out_file = optarg;
            emu_argc++;
            break;
        case 'a':
            opt_arch_test = true;
            signature_out_file = optarg;
            emu_argc++;
            break;
        default:
            return false;
        }
    }

    prog_argc = argc - emu_argc - 1;
    /* optind 指向第一个非选项字符串，也就是客体目标程序。
     */
    prog_args = &args[optind];
    opt_prog_name = prog_args[0];

    if (opt_prof_data) {
        char cwd_path[PATH_MAX] = {0};
        assert(getcwd(cwd_path, PATH_MAX));

        char rel_path[PATH_MAX] = {0};
        size_t args0_len = strlen(args[0]);
        /* 先确认 args[0] 足够长，再截掉结尾的 "rv32emu"。 */
        if (args0_len > 7) { /* strlen("rv32emu") */
            size_t copy_len = args0_len - 7;
            if (copy_len >= PATH_MAX)
                copy_len = PATH_MAX - 1;
            memcpy(rel_path, args[0], copy_len);
            rel_path[copy_len] = '\0';
        }

        char *prog_basename = basename(opt_prog_name);
        size_t total_len = strlen(cwd_path) + 1 + strlen(rel_path) +
                           strlen(prog_basename) + 5 + 1;
        prof_out_file = malloc(total_len);
        if (!prof_out_file) {
            rv_log_error("分配 profiling 输出文件名失败");
            return false;
        }
        assert(prof_out_file);

        snprintf(prof_out_file, total_len, "%s/%s%s.prof", cwd_path, rel_path,
                 prog_basename);
    }
    return true;
}

static void dump_test_signature(const char UNUSED *prog_name)
{
    elf_t *elf = elf_new();
    assert(elf && elf_open(elf, prog_name));

    uint32_t start = 0, end = 0;
    const struct Elf32_Sym *sym;
    FILE *f = fopen(signature_out_file, "w");
    if (!f) {
        rv_log_fatal("无法打开 signature 输出文件：%s",
                     signature_out_file);
        return;
    }

    /* 默认使用整个 .data 段作为 signature 区间。 */
    elf_get_data_section_range(elf, &start, &end);

    /* 如果 ELF 暴露 begin_signature/end_signature，则使用精确区间。 */
    if ((sym = elf_get_symbol(elf, "begin_signature")))
        start = sym->st_value;
    if ((sym = elf_get_symbol(elf, "end_signature")))
        end = sym->st_value;

    /* 以 32 位字为单位导出 signature。 */
    for (uint32_t addr = start; addr < end; addr += 4)
        fprintf(f, "%08x\n", memory_read_w(addr));

    fclose(f);
    elf_delete(elf);
}

/* 不同运行时可以覆盖 CYCLE_PER_STEP；默认每轮执行 100 个周期。 */
#ifndef CYCLE_PER_STEP
#define CYCLE_PER_STEP 100
#endif
/* MEM_SIZE 由 Makefile 传入：
 * - SYSTEM 模式（Linux 内核）：可配置，默认 512 MiB。
 * - 用户态模式：可通过 USER_MEM_SIZE 配置，默认 256 MiB。
 * 启用按需分页后，物理内存只在真正访问时分配。
 */
#ifndef MEM_SIZE
#define MEM_SIZE (256ULL * 1024 * 1024) /* 默认 256 MiB。 */
#endif
#define STACK_SIZE 0x1000       /* 4096 */
#define ARGS_OFFSET_SIZE 0x1000 /* 4096 */

/* wasm 运行时需要能间接调用 rv_halt 来停止主循环。
 * 这里保留全局 rv 指针，并通过一层 indirect_rv_halt 暴露停止能力，避免把
 * riscv_t 实例直接交给外部 JS 调用侧。更多细节见 emulate.c 的 rv_step。
 */
riscv_t *rv;
#ifdef __EMSCRIPTEN__
void indirect_rv_halt()
{
    rv_halt(rv);
}
#endif

int main(int argc, char **args)
{
    if (argc == 1 || !parse_args(argc, args)) {
        print_usage(args[0]);
        return 1;
    }

    int run_flag = 0;
#if !RV32_HAS(SYSTEM_MMIO)
    run_flag |= opt_trace;
#endif
#if RV32_HAS(GDBSTUB)
    run_flag |= opt_gdbstub << 1;
#endif
    run_flag |= opt_prof_data << 2;

    vm_attr_t attr = {
        .mem_size = MEM_SIZE,
        .stack_size = STACK_SIZE,
        .args_offset_size = ARGS_OFFSET_SIZE,
        .argc = prog_argc,
        .argv = prog_args,
        .log_level = LOG_WARN,
        .run_flag = run_flag,
        .profile_output_file = prof_out_file,
        .cycle_per_step = CYCLE_PER_STEP,
        .allow_misalign = opt_misaligned,
        .fd_stdin = STDIN_FILENO,
        .fd_stdout = STDOUT_FILENO,
        .fd_stderr = STDERR_FILENO,
    };
#if RV32_HAS(SYSTEM_MMIO)
    attr.data.system.kernel = opt_kernel_img;
    attr.data.system.initrd = opt_rootfs_img;
    attr.data.system.bootargs = opt_bootargs;
    if (opt_virtio_blk_idx) {
        attr.data.system.vblk_device = opt_virtio_blk_img;
        attr.data.system.vblk_device_cnt = opt_virtio_blk_idx;
    } else {
        attr.data.system.vblk_device = NULL;
    }
#else
    attr.data.user.elf_program = opt_prog_name;
#endif

    /* 根据 -q 开关启用或静默普通日志输出。 */
    rv_log_set_quiet(opt_quiet_outputs);

    /* 创建 RISC-V 运行时实例。 */
    rv = rv_create(&attr);
    if (!rv) {
        rv_log_fatal("无法创建 RISC-V 模拟器");
        attr.exit_code = 1;
        goto end;
    }
    rv_log_info("RISC-V 模拟器已创建，可以运行");

#if RV32_HAS(ARCH_TEST)
    /* 架构测试模式下提取 tohost/fromhost 地址。 */
    if (opt_arch_test && opt_prog_name) {
        elf_t *elf = elf_new();
        if (elf && elf_open(elf, opt_prog_name)) {
            const struct Elf32_Sym *sym;
            if ((sym = elf_get_symbol(elf, "tohost"))) {
                rv_set_tohost_addr(rv, sym->st_value);
                rv_log_info("找到 tohost：0x%08x", sym->st_value);
            }
            if ((sym = elf_get_symbol(elf, "fromhost"))) {
                rv_set_fromhost_addr(rv, sym->st_value);
                rv_log_info("找到 fromhost：0x%08x", sym->st_value);
            }
        }
        elf_delete(elf);
    }
#endif

#if defined(__EMSCRIPTEN__)
    disable_run_button();
#endif

    rv_run(rv);

    /* 按需导出寄存器 JSON。 */
    if (opt_dump_regs)
        dump_registers(rv, registers_out_file);

    /* 架构测试模式下导出 signature 结果。 */
    if (opt_arch_test)
        dump_test_signature(opt_prog_name);

    /* 释放 RISC-V 运行时。 */
    rv_delete(rv);
    /*
     * 其他编译单元无法更新这个指针，因此在这里置空，避免 atexit 回调重复访问。
     */
    rv = NULL;
    uint64_t mem_usage = memory_get_usage();
    rv_log_info("峰值内存使用：%" PRIu64 " KB (%" PRIu64 " MB)",
                mem_usage / 1024, mem_usage / (1024 * 1024));
    rv_log_info("RISC-V 模拟器已销毁");

end:
    free(prof_out_file);
    return attr.exit_code;
}
