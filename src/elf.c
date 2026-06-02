/*
 * rv32emu 可依据 MIT 许可证自由再分发。使用和再分发规则见 LICENSE 文件。
 */

/*
 * 最小 ELF32 读取和装载器。
 *
 * 用户态运行时通过本文件打开 ELF、校验头部、装载 PT_LOAD 段、查询符号表，
 * 并为架构测试读取 begin_signature/end_signature/tohost 等符号。实现只覆盖
 * 模拟器需要的 ELF32 小端场景，不追求完整链接器/加载器能力。
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "elf.h"
#include "io.h"
#include "utils.h"

#if HAVE_MMAP
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#else
/* 回退到标准 I/O 文件流。 */
#include <stdio.h>
#endif

enum {
    EM_RISCV = 243,
};

enum {
    ELFCLASS32 = 1,
};

enum {
    PT_NULL = 0,
    PT_LOAD = 1,
    PT_DYNAMIC = 2,
    PT_INTERP = 3,
    PT_NOTE = 4,
    PT_SHLIB = 5,
    PT_PHDR = 6,
    PT_TLS = 7,
};

enum {
    STT_NOTYPE = 0,
    STT_OBJECT = 1,
    STT_FUNC = 2,
    STT_SECTION = 3,
    STT_FILE = 4,
    STT_COMMON = 5,
    STT_TLS = 6,
};

#define ELF_ST_TYPE(x) (((unsigned int) x) & 0xf)

struct elf_internal {
    const struct Elf32_Ehdr *hdr;
    uint32_t raw_size;
    uint8_t *raw_data;

    /* 符号表映射：uint32_t -> const char *。 */
    map_t symbols;
};

#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

elf_t *elf_new(void)
{
    elf_t *e = malloc(sizeof(elf_t));
    assert(e);
    e->hdr = NULL;
    e->raw_size = 0;
    e->symbols = map_init(int, char *, map_cmp_uint);
    e->raw_data = NULL;
    return e;
}

void elf_delete(elf_t *e)
{
    if (!e)
        return;

    map_delete(e->symbols);
#if HAVE_MMAP
    if (e->raw_data)
        munmap(e->raw_data, e->raw_size);
#else
    free(e->raw_data);
#endif
    free(e);
}

/* 释放已加载 ELF 文件占用的资源。 */
static void release(elf_t *e)
{
#if !HAVE_MMAP
    free(e->raw_data);
#endif

    e->raw_data = NULL;
    e->raw_size = 0;
    e->hdr = NULL;
}

/* 校验 ELF 文件头是否有效。 */
static bool is_valid(elf_t *e)
{
    /* 校验 ELF magic。 */
    if (memcmp(e->hdr->e_ident, "\177ELF", 4))
        return false;

    /* 只接受 32 位 ELF。 */
    if (e->hdr->e_ident[EI_CLASS] != ELFCLASS32)
        return false;

    /* 校验机器类型是否为 RISC-V。 */
    if (e->hdr->e_machine != EM_RISCV)
        return false;

    return true;
}

/* 获取 section header 字符串表中的字符串。 */
static const char *get_sh_string(elf_t *e, int index)
{
    uint32_t offset =
        e->hdr->e_shoff + e->hdr->e_shstrndx * e->hdr->e_shentsize;
    const struct Elf32_Shdr *shdr =
        (const struct Elf32_Shdr *) (e->raw_data + offset);
    return (const char *) (e->raw_data + shdr->sh_offset + index);
}

/* 按段名获取 section header。 */
static const struct Elf32_Shdr *get_section_header(elf_t *e, const char *name)
{
    for (int s = 0; s < e->hdr->e_shnum; ++s) {
        uint32_t offset = e->hdr->e_shoff + s * e->hdr->e_shentsize;
        const struct Elf32_Shdr *shdr =
            (const struct Elf32_Shdr *) (e->raw_data + offset);
        const char *sname = get_sh_string(e, shdr->sh_name);
        if (!strcmp(name, sname))
            return shdr;
    }
    return NULL;
}

/* 获取 ELF 符号字符串表。 */
static const char *get_strtab(elf_t *e)
{
    const struct Elf32_Shdr *shdr = get_section_header(e, ".strtab");
    if (!shdr)
        return NULL;

    return (const char *) (e->raw_data + shdr->sh_offset);
}

/* 查找指定名称的符号条目。 */
const struct Elf32_Sym *elf_get_symbol(elf_t *e, const char *name)
{
    const char *strtab = get_strtab(e); /* 获取字符串表。 */
    if (!strtab)
        return NULL;

    /* 获取符号表。 */
    const struct Elf32_Shdr *shdr = get_section_header(e, ".symtab");
    if (!shdr)
        return NULL;

    /* 计算符号表范围。 */
    const struct Elf32_Sym *sym =
        (const struct Elf32_Sym *) (e->raw_data + shdr->sh_offset);
    const struct Elf32_Sym *end =
        (const struct Elf32_Sym *) (e->raw_data + shdr->sh_offset +
                                    shdr->sh_size);

    for (; sym < end; ++sym) { /* 尝试查找目标符号。 */
        const char *sym_name = strtab + sym->st_name;
        if (!strcmp(name, sym_name))
            return sym;
    }

    /* 未找到符号。 */
    return NULL;
}

static void fill_symbols(elf_t *e)
{
    /* 初始化内部符号表。 */
    map_clear(e->symbols);
    map_insert(e->symbols, &(int) {0}, &(char *) {NULL});

    /* 获取字符串表。 */
    const char *strtab = get_strtab(e);
    if (!strtab)
        return;

    /* 获取符号表。 */
    const struct Elf32_Shdr *shdr = get_section_header(e, ".symtab");
    if (!shdr)
        return;

    /* 计算符号表范围。 */
    const struct Elf32_Sym *sym =
        (const struct Elf32_Sym *) (e->raw_data + shdr->sh_offset);
    const struct Elf32_Sym *end =
        (const struct Elf32_Sym *) (e->raw_data + shdr->sh_offset +
                                    shdr->sh_size);

    for (; sym < end; ++sym) { /* 尝试收集符号。 */
        const char *sym_name = strtab + sym->st_name;
        switch (ELF_ST_TYPE(sym->st_info)) { /* 加入内部符号表。 */
        case STT_NOTYPE:
        case STT_OBJECT:
        case STT_FUNC:
            map_insert(e->symbols, (void *) &(sym->st_value), &sym_name);
        }
    }
}

const char *elf_find_symbol(elf_t *e, uint32_t addr)
{
    if (map_empty(e->symbols))
        fill_symbols(e);
    map_iter_t it;
    map_find(e->symbols, &it, &addr);
    return map_at_end(e->symbols, &it) ? NULL : map_iter_value(&it, char *);
}

bool elf_get_data_section_range(elf_t *e, uint32_t *start, uint32_t *end)
{
    const struct Elf32_Shdr *shdr = get_section_header(e, ".data");
    if (!shdr || shdr->sh_type == SHT_NOBITS)
        return false;

    *start = shdr->sh_addr;
    *end = *start + shdr->sh_size;
    return true;
}

/* ELF 快速结构说明：
 *    +--------------------------------+
 *    | ELF 头                         |--+
 *    +--------------------------------+  |
 *    | Program Header                 |  |
 *    +--------------------------------+  |
 * +->| Sections: .text、.strtab 等    |  |
 * |  +--------------------------------+  |
 * +--| Section Headers                |<-+
 *    +--------------------------------+
 *
 * 查找 section header table（SHT）：
 *   文件起点 + ELF_header.shoff -> section_header table。
 * 查找 section header 名称字符串表：
 *   section_header table[ELF_header.shstrndx] -> 名称表对应 section header。
 * 查找 section 数据：
 *   文件起点 + section_header.offset -> section 数据。
 */
bool elf_load(elf_t *e, memory_t *mem)
{
    /* 遍历所有 program header。 */
    for (int p = 0; p < e->hdr->e_phnum; ++p) {
        /* 取得下一个 program header。 */
        uint32_t offset = e->hdr->e_phoff + (p * e->hdr->e_phentsize);
        const struct Elf32_Phdr *phdr =
            (const struct Elf32_Phdr *) (e->raw_data + offset);

        /* 只装载 PT_LOAD 段。 */
        if (phdr->p_type != PT_LOAD)
            continue;

        /* 复制文件中实际存在的字节范围。 */
        const int to_copy = min(phdr->p_memsz, phdr->p_filesz);
        if (to_copy && !memory_write(mem, phdr->p_vaddr,
                                     e->raw_data + phdr->p_offset, to_copy))
            return false;

        /* 对 memsz 超过 filesz 的部分补零。 */
        const int to_zero = max(phdr->p_memsz, phdr->p_filesz) - to_copy;
        if (to_zero && !memory_fill(mem, phdr->p_vaddr + to_copy, to_zero, 0))
            return false;
    }

    return true;
}

bool elf_open(elf_t *e, const char *input)
{
    /* 释放上一次打开的 ELF 数据。 */
    if (e->raw_data)
        release(e);

    char *path = sanitize_path(input);
    if (!path)
        return false;

#if HAVE_MMAP
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        goto free_path;

    /* 获取文件大小。 */
    struct stat st;
    fstat(fd, &st);
    e->raw_size = st.st_size;

    /* 把文件映射到内存；文件开头就是 ELF header。
     */
    e->raw_data = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (e->raw_data == MAP_FAILED)
        goto free_fd;
    close(fd);

#else  /* 回退到标准 I/O 文件流。 */
    FILE *f = fopen(path, "rb");
    if (!f)
        goto free_path;

    /* 获取文件大小。 */
    fseek(f, 0, SEEK_END);
    e->raw_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (!e->raw_size)
        goto free_fd;

    /* 分配缓冲区。 */
    free(e->raw_data);
    e->raw_data = malloc(e->raw_size);
    assert(e->raw_data);

    /* 读取文件内容到内存。 */
    const size_t r = fread(e->raw_data, 1, e->raw_size, f);
    fclose(f);
    if (r != e->raw_size)
        goto free_path;
#endif /* HAVE_MMAP */

    /* 指向 ELF 文件头。 */
    e->hdr = (const struct Elf32_Ehdr *) e->raw_data;

    /* 校验是否为有效 ELF 文件。 */
    if (!is_valid(e))
        goto free_path;

    free(path);
    return true;

free_fd:
#if HAVE_MMAP
    close(fd);
#else
    fclose(f);
#endif

free_path:
    free(path);

    release(e);
    return false;
}

struct Elf32_Ehdr *get_elf_header(elf_t *e)
{
    return (struct Elf32_Ehdr *) e->hdr;
}

uint8_t *get_elf_first_byte(elf_t *e)
{
    return (uint8_t *) e->raw_data;
}
