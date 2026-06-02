/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

#pragma once

/* 最小 ELF 解析器接口。
 *
 * 只覆盖模拟器装载 RV32 ELF、查询符号和读取段范围所需的能力。
 */

#include <stdint.h>

#include "io.h"
#include "map.h"
#include "riscv.h"

typedef uint32_t Elf32_Addr;
typedef uint32_t Elf32_Off;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Word;

enum {
    EI_MAG0 = 0, /* ELF magic 值。 */
    EI_MAG1 = 1,
    EI_MAG2 = 2,
    EI_MAG3 = 3,
    EI_CLASS = 4,   /* ELF class，取值为 ELF_IDENT_CLASS_ 之一。 */
    EI_DATA = 5,    /* 文件剩余部分的数据类型。 */
    EI_VERSION = 6, /* 头部版本，ELF_IDENT_VERSION_CURRENT。 */
    EI_OSABI = 7,
    EI_ABIVERSION = 8,
    EI_PAD = 9, /* 未使用的 padding。 */
    EI_NIDENT = 16,
};

/* Section 类型。 */
enum {
    SHT_NULL = 0,     /* 未使用的 section header 表项。 */
    SHT_PROGBITS = 1, /* 程序数据。 */
    SHT_SYMTAB = 2,   /* 符号表。 */
    SHT_STRTAB = 3,   /* 字符串表。 */
    SHT_RELA = 4,     /* 带加数的重定位条目。 */
    SHT_HASH = 5,     /* 符号哈希表。 */
    SHT_DYNAMIC = 6,  /* 动态链接信息。 */
    SHT_NOTE = 7,     /* 注记。 */
    SHT_NOBITS = 8,   /* 无文件数据的程序空间（bss）。 */
    SHT_REL = 9,      /* 不带加数的重定位条目。 */
    SHT_SHLIB = 10,   /* 保留。 */
    SHT_DYNSYM = 11,  /* 动态链接器符号表。 */
    SHT_NUM = 12,
    SHT_LOPROC = 0x70000000, /* 处理器专用范围起点。 */
    SHT_HIPROC = 0x7fffffff, /* 处理器专用范围终点。 */
    SHT_LOUSER = 0x80000000, /* 应用专用范围起点。 */
    SHT_HIUSER = 0xffffffff, /* 应用专用范围终点。 */
};

/* Section 属性标志。 */
enum {
    SHF_WRITE = 0x1,
    SHF_ALLOC = 0x2,
    SHF_EXECINSTR = 0x4,
    SHF_MERGE = 0x10,
    SHF_STRINGS = 0x20,
    SHF_INFO_LINK = 0x40,
    SHF_LINK_ORDER = 0x80,
    SHF_OS_NONCONFORMING = 0x100,
    SHF_GROUP = 0x200,
    SHF_TLS = 0x400,
    SHF_COMPRESSED = 0x800,
    SHF_MASKOS = 0x0ff00000,
    SHF_MASKPROC = 0xf0000000,
};

/* Elf32 头部。 */
struct Elf32_Ehdr {
    uint8_t e_ident[EI_NIDENT];
    Elf32_Half e_type;      /* 目标文件类型。 */
    Elf32_Half e_machine;   /* 架构。 */
    Elf32_Word e_version;   /* 目标文件版本。 */
    Elf32_Addr e_entry;     /* 入口点虚拟地址。 */
    Elf32_Off e_phoff;      /* program header 表文件偏移。 */
    Elf32_Off e_shoff;      /* section header 表文件偏移。 */
    Elf32_Word e_flags;     /* 处理器专用标志。 */
    Elf32_Half e_ehsize;    /* ELF header 字节大小。 */
    Elf32_Half e_phentsize; /* program header 表项大小。 */
    Elf32_Half e_phnum;     /* program header 表项数量。 */
    Elf32_Half e_shentsize; /* section header 表项大小。 */
    Elf32_Half e_shnum;     /* section header 表项数量。 */
    Elf32_Half e_shstrndx;  /* section header 字符串表索引。 */
};

/* Elf32 program header 表。 */
struct Elf32_Phdr {
    Elf32_Word p_type;   /* 类型，ELF_PROGRAM_TYPE_ 的组合。 */
    Elf32_Off p_offset;  /* 程序映像在文件中的偏移。 */
    Elf32_Addr p_vaddr;  /* 内存中的虚拟地址。 */
    Elf32_Addr p_paddr;  /* 可选物理地址。 */
    Elf32_Word p_filesz; /* 文件中映像大小。 */
    Elf32_Word p_memsz;  /* 内存中映像大小。 */
    Elf32_Word p_flags;  /* 类型相关标志。 */
    Elf32_Word p_align;  /* 内存对齐字节数。 */
};

/* Elf32 section header 表。 */
struct Elf32_Shdr {
    Elf32_Word sh_name;      /* section 名称。 */
    Elf32_Word sh_type;      /* section 类型。 */
    Elf32_Word sh_flags;     /* section 标志。 */
    Elf32_Addr sh_addr;      /* 执行时 section 虚拟地址。 */
    Elf32_Off sh_offset;     /* section 文件偏移。 */
    Elf32_Word sh_size;      /* section 字节大小。 */
    Elf32_Word sh_link;      /* 指向另一个 section 的链接。 */
    Elf32_Word sh_info;      /* 额外 section 信息。 */
    Elf32_Word sh_addralign; /* section 对齐。 */
    Elf32_Word sh_entsize;   /* section 持有表时的表项大小。 */
};

struct Elf32_Sym {
    Elf32_Word st_name;
    Elf32_Addr st_value;
    Elf32_Word st_size;
    uint8_t st_info;
    uint8_t st_other;
    Elf32_Half st_shndx;
};

typedef struct elf_internal elf_t;

elf_t *elf_new(void);
void elf_delete(elf_t *e);

/* 从指定路径打开 ELF 文件。 */
bool elf_open(elf_t *e, const char *path);

/* 查找符号条目。 */
const struct Elf32_Sym *elf_get_symbol(elf_t *e, const char *name);

/* 从指定 ELF 文件中按地址查找符号。 */
const char *elf_find_symbol(elf_t *e, uint32_t addr);

/* 获取 ELF 文件中 .data section 的范围。 */
bool elf_get_data_section_range(elf_t *e, uint32_t *start, uint32_t *end);

/* 将 ELF 文件加载到内存抽象中。 */
bool elf_load(elf_t *e, memory_t *mem);

/* 获取 ELF 头部。 */
struct Elf32_Ehdr *get_elf_header(elf_t *e);

/* 获取 ELF 原始数据的首字节。 */
uint8_t *get_elf_first_byte(elf_t *e);
