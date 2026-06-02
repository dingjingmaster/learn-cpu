/*
 * rv32emu 可依据 MIT 许可证自由再分发。使用和再分发规则见 LICENSE 文件。
 */

/*
 * 一级模板 JIT 实现。
 *
 * 本文件负责宿主机器码缓冲区、x86-64/Arm64 指令发射、寄存器分配、跳转修补、
 * MMIO/MMU 运行时辅助、宏操作融合和代码缓存刷新。rv32_jit.c 通过 GEN 宏包含
 * 具体 RISC-V 指令的发射逻辑，而这里提供公共的底层发射 API 和状态管理。
 */

/* 该 JIT 实现经过了大量改造，底层发射器很大程度参考 ubpf 的
 * ubpf_jit_[x86_64|arm64].[c|h]。原始 ubpf JIT 文件为本项目适配一级 JIT 提供
 * 了基础和灵感，特此致谢。
 *
 * 参考：
 *   https://github.com/iovisor/ubpf/blob/main/vm/ubpf_jit_x86_64.c
 *   https://github.com/iovisor/ubpf/blob/main/vm/ubpf_jit_arm64.c
 */

#if !RV32_HAS(JIT)
#error "只有启用 JIT 支持时才能构建此文件。"
#endif

#if !defined(__x86_64__) && !defined(__aarch64__)
#error "此实现仅支持 x64 和 arm64。"
#endif

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#if defined(__aarch64__)
#include <pthread.h>
#endif
#endif

#include "cache.h"
#include "decode.h"
#include "io.h"
#include "jit.h"
#include "riscv.h"
#include "riscv_private.h"
#include "utils.h"

#if RV32_HAS(SYSTEM)
#include "system.h"
#endif

#define JIT_CLS_MASK 0x07
#define JIT_ALU_OP_MASK 0xf0
#define JIT_CLS_ALU 0x04
#define JIT_CLS_ALU64 0x07
#define JIT_SRC_IMM 0x00
#define JIT_SRC_REG 0x08
#define JIT_OP_MUL_IMM (JIT_CLS_ALU | JIT_SRC_IMM | 0x20)
#define JIT_OP_MUL_REG (JIT_CLS_ALU | JIT_SRC_REG | 0x20)
#define JIT_OP_DIV_IMM (JIT_CLS_ALU | JIT_SRC_IMM | 0x30)
#define JIT_OP_DIV_REG (JIT_CLS_ALU | JIT_SRC_REG | 0x30)
#define JIT_OP_MOD_IMM (JIT_CLS_ALU | JIT_SRC_IMM | 0x90)
#define JIT_OP_MOD_REG (JIT_CLS_ALU | JIT_SRC_REG | 0x90)

#define STACK_SIZE 512
#define MAX_JUMPS 1024
#define MAX_BLOCKS 8192
#define IN_JUMP_THRESHOLD 256

/* 判断分支历史表条目是否应触发 JIT 翻译。 */
static inline bool bht_should_translate(const branch_history_table_t *bt,
                                        int idx
#if RV32_HAS(SYSTEM)
                                        ,
                                        uint32_t csr_satp
#endif
)
{
    if (!bt->PC[idx] || bt->times[idx] < IN_JUMP_THRESHOLD)
        return false;
#if RV32_HAS(SYSTEM)
    if (bt->satp[idx] != csr_satp)
        return false;
#endif
    return true;
}

#if defined(__x86_64__)
/* 标出已发射跳转指令中立即数所在位置。
 * 条件跳转（JNE、JE 等）需加 2 跳过 2 字节 opcode（0x0f + condition）；
 * 无条件跳转（JMP）需加 1 跳过 1 字节 opcode（0xe9）。
 */
#define JUMP_LOC_0 jump_loc_0 + 2
#define JUMP_TRAP jump_trap + 2
#define JUMP_NORMAL jump_normal + 1
#if RV32_HAS(SYSTEM)
#define JUMP_LOC_1 jump_loc_1 + 1
#endif
/* struct jump 中 target_pc 的特殊值。 */
#define TARGET_PC_EXIT -1U
#define TARGET_PC_RETPOLINE -3U
enum x64_reg {
    RAX,
    RCX,
    RDX,
    RBX,
    RSP,
    RBP,
    RIP = 5,
    RSI,
    RDI,
    R8,
    R9,
    R10,
    R11,
    R12,
    R13,
    R14,
    R15,
};

#elif defined(__aarch64__)
/* 标出已发射跳转指令中立即数所在位置。ARM64 的偏移嵌入指令字本身，因此不需要
 * 额外偏移调整。
 */
#define JUMP_LOC_0 jump_loc_0
#define JUMP_TRAP jump_trap
#define JUMP_NORMAL jump_normal
#if RV32_HAS(SYSTEM)
#define JUMP_LOC_1 jump_loc_1
#endif
/* struct jump 中 target_pc 的特殊值。 */
#define TARGET_PC_EXIT ~UINT32_C(0)
#define TARGET_PC_ENTER (~UINT32_C(0) & 0x0101)
/* 该值保证是一条非法 A64 指令。 */
#define BAD_OPCODE ~UINT32_C(0)

enum a64_reg {
    R0,
    R1,
    R2,
    R3,
    R4,
    R5,
    R6,
    R7,
    R8,
    R9,
    R10,
    R11,
    R12,
    R13,
    R14,
    R15,
    R16,
    R17,
    R18,
    R19,
    R20,
    R21,
    R22,
    R23,
    R24,
    R25,
    R26,
    R27,
    R28,
    R29,
    R30,
    SP,
    RZ = 31
};

typedef enum {
    /* AddSubOpcode */
    AS_ADD = 0,
    AS_SUB = 2,
    AS_SUBS = 3,
    /* LogicalOpcode */
    LOG_AND = 0x00000000U, /* 0000_0000_0000_0000_0000_0000_0000_0000 */
    LOG_ORR = 0x20000000U, /* 0010_0000_0000_0000_0000_0000_0000_0000 */
    LOG_ORN = 0x20200000U, /* 0010_0000_0010_0000_0000_0000_0000_0000 */
    LOG_EOR = 0x40000000U, /* 0100_0000_0000_0000_0000_0000_0000_0000 */
    /* LoadStoreOpcode */
    LS_STRB = 0x00000000U,   /* 0000_0000_0000_0000_0000_0000_0000_0000 */
    LS_LDRB = 0x00400000U,   /* 0000_0000_0100_0000_0000_0000_0000_0000 */
    LS_LDRSBW = 0x00c00000U, /* 0000_0000_1100_0000_0000_0000_0000_0000 */
    LS_STRH = 0x40000000U,   /* 0100_0000_0000_0000_0000_0000_0000_0000 */
    LS_LDRH = 0x40400000U,   /* 0100_0000_0100_0000_0000_0000_0000_0000 */
    LS_LDRSHW = 0x40c00000U, /* 0100_0000_1100_0000_0000_0000_0000_0000 */
    LS_STRW = 0x80000000U,   /* 1000_0000_0000_0000_0000_0000_0000_0000 */
    LS_LDRW = 0x80400000U,   /* 1000_0000_0100_0000_0000_0000_0000_0000 */
    LS_LDRSW = 0x80800000U,  /* 1000_0000_1000_0000_0000_0000_0000_0000 */
    LS_STRX = 0xc0000000U,   /* 1100_0000_0000_0000_0000_0000_0000_0000 */
    LS_LDRX = 0xc0400000U,   /* 1100_0000_0100_0000_0000_0000_0000_0000 */
    /* LoadStorePairOpcode */
    LSP_STPX = 0xa9000000U, /* 1010_1001_0000_0000_0000_0000_0000_0000 */
    LSP_LDPX = 0xa9400000U, /* 1010_1001_0100_0000_0000_0000_0000_0000 */
    /* UnconditionalBranchOpcode */
    BR_BR = 0xd61f0000U,  /* 1101_0110_0001_1111_0000_0000_0000_0000 */
    BR_BLR = 0xd63f0000U, /* 1101_0110_0011_1111_0000_0000_0000_0000 */
    BR_RET = 0xd65f0000U, /* 1101_0110_0101_1111_0000_0000_0000_0000 */
    /* UnconditionalBranchImmediateOpcode */
    UBR_B = 0x14000000U, /* 0001_0100_0000_0000_0000_0000_0000_0000 */
    /* ConditionalBranchImmediateOpcode */
    BR_Bcond = 0x54000000U,
    /* DP2Opcode */
    DP2_UDIV = 0x1ac00800U, /* 0001_1010_1100_0000_0000_1000_0000_0000 */
    DP2_SDIV = 0x1ac00c00U, /* 0001_1010_1100_0000_0000_1100_0000_0000 */
    DP2_LSLV = 0x1ac02000U, /* 0001_1010_1100_0000_0010_0000_0000_0000 */
    DP2_LSRV = 0x1ac02400U, /* 0001_1010_1100_0000_0010_0100_0000_0000 */
    DP2_ASRV = 0x1ac02800U, /* 0001_1010_1100_0000_0010_1000_0000_0000 */
    /* DP3Opcode */
    DP3_MADD = 0x1b000000U, /* 0001_1011_0000_0000_0000_0000_0000_0000 */
    DP3_MSUB = 0x1b008000U, /* 0001_1011_0000_0000_1000_0000_0000_0000 */
    /* MoveWideOpcode */
    MW_MOVN = 0x12800000U, /* 0001_0010_1000_0000_0000_0000_0000_0000 */
    MW_MOVZ = 0x52800000U, /* 0101_0010_1000_0000_0000_0000_0000_0000 */
    MW_MOVK = 0x72800000U, /* 0111_0010_1000_0000_0000_0000_0000_0000 */
} a64opcode_t;

enum condition {
    COND_EQ,
    COND_NE,
    COND_HS,
    COND_LO,
    COND_GE = 10,
    COND_LT = 11,
    COND_AL = 14,
};

enum {
    temp_imm_reg = R24, /* 生成立即数使用的临时寄存器。 */
    temp_div_reg = R25, /* 保存除法结果使用的临时寄存器。 */
};
#endif

enum operand_size {
    S8,
    S16,
    S32,
    S64,
};

#if defined(__x86_64__)
/* x86-64 常见调用约定有两种，参考：
 * https://en.wikipedia.org/wiki/X64_calling_conventions#x86-64_calling_conventions
 *
 * 注意：R12 是例外，本实现不使用它。因此虽然 R12 是非易失寄存器，但两个平台的
 * nonvolatile_reg 列表都故意省略它。
 */
#if defined(_WIN32)
static const int nonvolatile_reg[] = {RBP, RBX, RDI, RSI, R13, R14, R15};
static const int parameter_reg[] = {RCX, RDX, R8, R9};
static struct host_reg register_map[] = {
    {RAX, -1, 0, 0}, {R10, -1, 0, 0}, {RDX, -1, 0, 0}, {R8, -1, 0, 0},
    {R9, -1, 0, 0},  {R14, -1, 0, 0}, {R15, -1, 0, 0}, {RDI, -1, 0, 0},
    {RSI, -1, 0, 0}, {RBX, -1, 0, 0}, {RBP, -1, 0, 0},
};
static int temp_reg = RCX;
#else
static const int nonvolatile_reg[] = {RBP, RBX, R13, R14, R15};
static const int parameter_reg[] = {RDI, RSI, RDX, RCX, R8, R9};
static struct host_reg register_map[] = {
    {RAX, -1, 0, 0}, {RBX, -1, 0, 0}, {RDX, -1, 0, 0}, {R8, -1, 0, 0},
    {R9, -1, 0, 0},  {R10, -1, 0, 0}, {R11, -1, 0, 0}, {R13, -1, 0, 0},
    {R14, -1, 0, 0}, {R15, -1, 0, 0},
};
static int temp_reg = RCX;
#endif
#elif defined(__aarch64__)
/* callee_reg 的数量必须是 2 的倍数，因为后续栈保存逻辑按寄存器对处理。 */
static const int callee_reg[] = {R19, R20, R21, R22, R23, R24, R25, R26};
/* parameter_reg：调用者保存寄存器。 */
static const int parameter_reg[] = {R0, R1, R2, R3, R4};
static int temp_reg = R8;

/* 寄存器分配：
 * Arm64       用途
 *   r0 - r4   函数参数，调用者保存。
 *   r6 - r8   临时寄存器，保存执行期间计算出的值。
 *   r19-r23   被调用者保存寄存器。
 *   r24       临时寄存器，用于生成 32 位立即数。
 *   r25       临时寄存器，用于取模计算。
 *
 * 注意：Apple 和 Windows 平台保留 R18（platform register），不能使用。R16/R17
 *（IP0/IP1）是过程内调用临时寄存器，跨 BLR 调用时可能被链接器 veneer 破坏；
 * 它们可用于直线 JIT 代码，但不能假设其值在函数调用后仍然保留。
 */
static struct host_reg register_map[] = {
    {R5, -1, 0, 0},  {R6, -1, 0, 0},  {R7, -1, 0, 0},  {R9, -1, 0, 0},
    {R11, -1, 0, 0}, {R12, -1, 0, 0}, {R13, -1, 0, 0}, {R14, -1, 0, 0},
    {R15, -1, 0, 0}, {R16, -1, 0, 0}, {R17, -1, 0, 0}, {R26, -1, 0, 0},
};
#endif

static const int n_host_regs =
    ARRAY_SIZE(register_map); /* 可用宿主寄存器数量。 */

static inline void set_dirty(int reg_idx, bool is_dirty)
{
    for (int i = 0; i < n_host_regs; i++) {
        /* 忽略非易失寄存器和参数寄存器。 */
        if (register_map[i].reg_idx != reg_idx)
            continue;

        register_map[i].dirty = is_dirty;
        return;
    }
}

static inline void offset_map_insert(struct jit_state *state, block_t *block)
{
    assert(state->n_blocks < MAX_BLOCKS);

    struct offset_map *map_entry = &state->offset_map[state->n_blocks++];
    map_entry->pc = block->pc_start;
    map_entry->offset = state->offset;
#if RV32_HAS(SYSTEM)
    map_entry->satp = block->satp;
#endif
}

#if !defined(__APPLE__)
#define sys_icache_invalidate(addr, size) \
    __builtin___clear_cache((char *) (addr), (char *) (addr) + (size));
#endif

static bool should_flush = false;

#if defined(__APPLE__) && defined(__aarch64__)
/* 跟踪 JIT 写入模式，以批量切换写保护。
 * Apple Silicon 上频繁切换写保护可能导致缓存一致性问题。翻译开始时启用写模式，
 * 所有代码生成和跳转修补完成后才关闭。
 *
 * 该标志必须是线程局部的，因为 pthread_jit_write_protect_np 按线程生效。若使用
 * 共享标志，多个线程同时翻译时会产生竞态。
 */
static __thread bool jit_write_mode = false;

static inline void jit_enter_write_mode(void)
{
    if (!jit_write_mode) {
        pthread_jit_write_protect_np(false);
        jit_write_mode = true;
    }
}

static inline void jit_exit_write_mode(void)
{
    if (jit_write_mode) {
        pthread_jit_write_protect_np(true);
        jit_write_mode = false;
    }
}
#endif

static void emit_bytes(struct jit_state *state, void *data, uint32_t len)
{
    if (unlikely((state->offset + len) > state->size)) {
        should_flush = true;
        return;
    }
    if (unlikely(state->n_blocks == MAX_BLOCKS)) {
        should_flush = true;
        return;
    }
#if defined(__APPLE__) && defined(__aarch64__)
    /* 若当前不在写模式（例如初始设置阶段），临时切换写保护。常规翻译期间由
     * jit_translate 保持写模式，以避免频繁切换导致缓存一致性问题。
     */
    bool need_toggle = !jit_write_mode;
    if (need_toggle)
        pthread_jit_write_protect_np(false);
    memcpy(state->buf + state->offset, data, len);
    if (need_toggle) {
        sys_icache_invalidate(state->buf + state->offset, len);
        pthread_jit_write_protect_np(true);
    }
#else
    memcpy(state->buf + state->offset, data, len);
    sys_icache_invalidate(state->buf + state->offset, len);
#endif
    state->offset += len;
}

#if defined(__x86_64__)
static inline void emit1(struct jit_state *state, uint8_t x)
{
    emit_bytes(state, &x, sizeof(x));
}

static inline void emit4(struct jit_state *state, uint32_t x)
{
    emit_bytes(state, &x, sizeof(x));
}

static inline void emit8(struct jit_state *state, uint64_t x)
{
    emit_bytes(state, &x, sizeof(x));
}

static inline void emit_modrm(struct jit_state *state, int mod, int r, int m)
{
    assert(!(mod & ~0xc0));
    emit1(state, (mod & 0xc0) | ((r & 7) << 3) | (m & 7));
}

static inline void emit_modrm_reg2reg(struct jit_state *state, int r, int m)
{
    emit_modrm(state, 0xc0, r, m);
}

static inline void emit_modrm_and_displacement(struct jit_state *state,
                                               int r,
                                               int m,
                                               int32_t d)
{
    if (d == 0 && (m & 7) != RBP) {
        emit_modrm(state, 0x00, r, m);
    } else if ((int8_t) d == d) {
        emit_modrm(state, 0x40, r, m);
        emit1(state, d);
    } else {
        emit_modrm(state, 0x80, r, m);
        emit4(state, d);
    }
}

static inline void emit_rex(struct jit_state *state, int w, int r, int x, int b)
{
    assert(!(w & ~1));
    assert(!(r & ~1));
    assert(!(x & ~1));
    assert(!(b & ~1));
    emit1(state, 0x40 | (w << 3) | (r << 2) | (x << 1) | b);
}

/* 发射 REX 前缀，并把 src/dst 的高位编码进去。
 * 若没有任何高位需要设置，则跳过该步骤。
 */
static inline void emit_basic_rex(struct jit_state *state,
                                  int w,
                                  int src,
                                  int dst)
{
    if (w || (src & 8) || (dst & 8))
        emit_rex(state, w, !!(src & 8), 0, !!(dst & 8));
}

static inline void emit_push(struct jit_state *state, int r)
{
    if (r & 8)
        emit_basic_rex(state, 0, 0, r);
    emit1(state, 0x50 | (r & 7));
}

static inline void emit_pop(struct jit_state *state, int r)
{
    if (r & 8)
        emit_basic_rex(state, 0, 0, r);
    emit1(state, 0x58 | (r & 7));
}

static inline void emit_jump_target_address(struct jit_state *state,
                                            int32_t target_pc,
                                            uint32_t target_satp UNUSED)
{
    assert(state->n_jumps < MAX_JUMPS);

    struct jump *jump = &state->jumps[state->n_jumps++];
    jump->offset_loc = state->offset;
    jump->target_pc = target_pc;
#if RV32_HAS(SYSTEM)
    jump->target_satp = target_satp;
#endif
    emit4(state, 0);
}
#elif defined(__aarch64__)
static inline void emit_load_imm(struct jit_state *state,
                                 int dst,
                                 uint32_t imm);

static void emit_a64(struct jit_state *state, uint32_t insn)
{
    assert(insn != BAD_OPCODE);
    emit_bytes(state, &insn, 4);
}

/* 获取多数指令编码中的 size 位（第 31 位）。 */
static inline uint32_t sz(bool is64)
{
    return (is64 ? UINT32_C(1) : UINT32_C(0)) << 31;
}

/* Arm 指令细节可参考：
 * https://developer.arm.com/documentation/ddi0487/ha (Arm Architecture
 * Reference Manual for A-profile architecture).
 */

/* [ARM-A] C4.1.64：Add/subtract (immediate)。 */
static void emit_addsub_imm(struct jit_state *state,
                            bool is64,
                            a64opcode_t op,
                            int rd,
                            int rn,
                            uint32_t imm12)
{
    const uint32_t imm_op_base = 0x11000000;
    emit_a64(state, sz(is64) | (op << 29) | imm_op_base | (0 << 22) |
                        (imm12 << 10) | (rn << 5) | rd);
    set_dirty(rd, true);
}

/* [ARM-A] C4.1.67：Logical (shifted register)。 */
static void emit_logical_register(struct jit_state *state,
                                  bool is64,
                                  a64opcode_t op,
                                  int rd,
                                  int rn,
                                  int rm)
{
    emit_a64(state, sz(is64) | op | (1 << 27) | (1 << 25) | (rm << 16) |
                        (rn << 5) | rd);
    set_dirty(rd, true);
}

/* [ARM-A] C4.1.67：Add/subtract (shifted register)。 */
static inline void emit_addsub_register(struct jit_state *state,
                                        bool is64,
                                        a64opcode_t op,
                                        int rd,
                                        int rn,
                                        int rm)
{
    const uint32_t reg_op_base = 0x0b000000;
    emit_a64(state,
             sz(is64) | (op << 29) | reg_op_base | (rm << 16) | (rn << 5) | rd);
    set_dirty(rd, true);
}

/* [ARM-A] C4.1.64：Move wide (Immediate)。 */
static inline void emit_movewide_imm(struct jit_state *state,
                                     bool is64,
                                     int rd,
                                     uint64_t imm)
{
    /* 先发射 MOVZ 或 MOVN，再追加一串 MOVK 以生成 imm 中的 64 位常量。
     * 通过比较立即数中 0x0000 与 0xffff 模式哪个更常见，选择能生成最少立即数
     * 片段的编码。
     */
    unsigned count0000 = is64 ? 0 : 2;
    unsigned countffff = 0;
    for (unsigned i = 0; i < (is64 ? 64 : 32); i += 16) {
        uint64_t block = (imm >> i) & 0xffff;
        if (block == 0xffff) {
            ++countffff;
        } else if (block == 0) {
            ++count0000;
        }
    }

    /* 遍历 imm 的 16 位片段，输出相应的 move 指令。 */
    bool invert = (count0000 < countffff);
    a64opcode_t op = invert ? MW_MOVN : MW_MOVZ;
    uint64_t skip_pattern = invert ? 0xffff : 0;
    for (unsigned i = 0; i < (is64 ? 4 : 2); ++i) {
        uint64_t imm16 = (imm >> (i * 16)) & 0xffff;
        if (imm16 != skip_pattern) {
            if (invert) {
                imm16 = ~imm16;
                imm16 &= 0xffff;
            }
            emit_a64(state, sz(is64) | op | (i << 21) | (imm16 << 5) | rd);
            op = MW_MOVK;
            invert = false;
        }
    }

    /* 处理 imm = 0 或 imm == -1 的特殊情况。 */
    if (op != MW_MOVK)
        emit_a64(state, sz(is64) | op | (0 << 21) | (0 << 5) | rd);

    set_dirty(rd, true);
}

/* [ARM-A] C4.1.66：Load/store register (unscaled immediate)。 */
static void emit_loadstore_imm(struct jit_state *state,
                               a64opcode_t op,
                               int rt,
                               int rn,
                               int16_t imm9)
{
    const uint32_t imm_op_base = 0x38000000U;
    assert(imm9 >= -256 && imm9 < 256);
    imm9 &= 0x1ff;
    emit_a64(state, imm_op_base | op | (imm9 << 12) | (rn << 5) | rt);
}

/* [ARM-A] C4.1.66：Load/store register pair (offset)。 */
static void emit_loadstorepair_imm(struct jit_state *state,
                                   a64opcode_t op,
                                   int rt,
                                   int rt2,
                                   int rn,
                                   int32_t imm7)
{
    int32_t imm_div = ((op == LSP_STPX) || (op == LSP_LDPX)) ? 8 : 4;
    assert(imm7 % imm_div == 0);
    imm7 /= imm_div;
    emit_a64(state, op | (imm7 << 15) | (rt2 << 10) | (rn << 5) | rt);
}

/* [ARM-A] C4.1.65：Unconditional branch (register)。 */
static void emit_uncond_branch_reg(struct jit_state *state,
                                   a64opcode_t op,
                                   int rn)
{
    emit_a64(state, op | (rn << 5));
}

/* [ARM-A] C4.1.67：Data-processing (2 source)。 */
static void emit_dataproc_2source(struct jit_state *state,
                                  bool is64,
                                  a64opcode_t op,
                                  int rd,
                                  int rn,
                                  int rm)
{
    emit_a64(state, sz(is64) | op | (rm << 16) | (rn << 5) | rd);
    set_dirty(rd, true);
}


#if RV32_HAS(EXT_M)
/* [ARM-A] C4.1.67：Data-processing (3 source)。 */
static void emit_dataproc_3source(struct jit_state *state,
                                  bool is64,
                                  a64opcode_t op,
                                  int rd,
                                  int rn,
                                  int rm,
                                  int ra)
{
    emit_a64(state, sz(is64) | op | (rm << 16) | (ra << 10) | (rn << 5) | rd);
    set_dirty(rd, true);
}
#endif

/* 在不切换写保护的情况下修补分支指令。
 * 调用方必须负责写保护和 cache 维护。
 */
static void patch_branch_imm(struct jit_state *state,
                             uint32_t offset,
                             int32_t imm)
{
    assert((imm & 3) == 0);
    uint32_t insn;
    imm >>= 2;
    memcpy(&insn, state->buf + offset, sizeof(uint32_t));
    if ((insn & 0xfe000000U) == 0x54000000U /* 条件立即数分支。 */
        || (insn & 0x7e000000U) ==
               0x34000000U) { /* 比较并按立即数分支。 */
        assert((imm >> 19) == INT64_C(-1) || (imm >> 19) == 0);
        insn |= (imm & 0x7ffff) << 5;
    } else if ((insn & 0x7c000000U) == 0x14000000U) {
        /* 无条件立即数分支。 */
        assert((imm >> 26) == INT64_C(-1) || (imm >> 26) == 0);
        insn |= (imm & 0x03ffffffU) << 0;
    } else {
        assert(false);
        insn = BAD_OPCODE;
    }
    memcpy(state->buf + offset, &insn, sizeof(uint32_t));
}

#endif

static inline void emit_jump_target_offset(struct jit_state *state,
                                           uint32_t jump_loc_0,
                                           uint32_t jump_state_offset)
{
    assert(state->n_jumps < MAX_JUMPS);

    struct jump *jump = &state->jumps[state->n_jumps++];
    jump->offset_loc = jump_loc_0;
    jump->target_offset = jump_state_offset;
}

static inline void emit_alu32(struct jit_state *state, int op, int src, int dst)
{
#if defined(__x86_64__)
    /* 发射 REX 前缀和 ModRM 字节。
     * 有编码选择时优先使用 MR 编码；src 经常作为 opcode 扩展位使用。
     */
    if (src & 8 || dst & 8)
        emit_basic_rex(state, 0, src, dst);
    emit1(state, op);
    emit_modrm_reg2reg(state, src, dst);

    set_dirty(dst, true);
#elif defined(__aarch64__)
    switch (op) {
    case 1: /* ADD */
        emit_addsub_register(state, false, AS_ADD, dst, dst, src);
        break;
    case 0x29: /* SUB */
        emit_addsub_register(state, false, AS_SUB, dst, dst, src);
        break;
    case 0x31: /* XOR */
        emit_logical_register(state, false, LOG_EOR, dst, dst, src);
        break;
    case 9: /* OR */
        emit_logical_register(state, false, LOG_ORR, dst, dst, src);
        break;
    case 0x21: /* AND */
        emit_logical_register(state, false, LOG_AND, dst, dst, src);
        break;
    case 0xd3:
        if (src == 4) /* SLL */
            emit_dataproc_2source(state, false, DP2_LSLV, dst, dst, temp_reg);
        else if (src == 5) /* SRL */
            emit_dataproc_2source(state, false, DP2_LSRV, dst, dst, temp_reg);
        else if (src == 7) /* SRA */
            emit_dataproc_2source(state, false, DP2_ASRV, dst, dst, temp_reg);
        break;
    default:
        __UNREACHABLE;
        break;
    }
    set_dirty(dst, true);
#endif
}

static inline void emit_alu32_imm32(struct jit_state *state,
                                    int op UNUSED,
                                    int src,
                                    int dst,
                                    int32_t imm)
{
#if defined(__x86_64__)
    /* REX 前缀、ModRM 字节和 32 位立即数。 */
    emit_alu32(state, op, src, dst);
    emit4(state, imm);
#elif defined(__aarch64__)
    switch (src) {
    case 0:
        emit_load_imm(state, R10, imm);
        emit_addsub_register(state, false, AS_ADD, dst, dst, R10);
        break;
    case 1:
        emit_load_imm(state, R10, imm);
        emit_logical_register(state, false, LOG_ORR, dst, dst, R10);
        break;
    case 4:
        emit_load_imm(state, R10, imm);
        emit_logical_register(state, false, LOG_AND, dst, dst, R10);
        break;
    case 6:
        emit_load_imm(state, R10, imm);
        emit_logical_register(state, false, LOG_EOR, dst, dst, R10);
        break;
    default:
        __UNREACHABLE;
        break;
    }
    set_dirty(dst, true);
#endif
}

static inline void emit_alu32_imm8(struct jit_state *state,
                                   int op UNUSED,
                                   int src,
                                   int dst,
                                   int8_t imm)
{
#if defined(__x86_64__)
    /* REX 前缀、ModRM 字节和 8 位立即数。 */
    emit_alu32(state, op, src, dst);
    emit1(state, imm);
#elif defined(__aarch64__)
    switch (src) {
    case 4:
        emit_load_imm(state, R10, imm);
        emit_dataproc_2source(state, false, DP2_LSLV, dst, dst, R10);
        break;
    case 5:
        emit_load_imm(state, R10, imm);
        emit_dataproc_2source(state, false, DP2_LSRV, dst, dst, R10);
        break;
    case 7:
        emit_load_imm(state, R10, imm);
        emit_dataproc_2source(state, false, DP2_ASRV, dst, dst, R10);
        break;
    default:
        __UNREACHABLE;
        break;
    }
    set_dirty(dst, true);
#endif
}

static inline void emit_alu64(struct jit_state *state, int op, int src, int dst)
{
#if defined(__x86_64__)
    /* 发射 REX.W 前缀和 ModRM 字节。
     * 有编码选择时优先使用 MR 编码；src 经常作为 opcode 扩展位使用。
     */
    emit_basic_rex(state, 1, src, dst);
    emit1(state, op);
    emit_modrm_reg2reg(state, src, dst);

    set_dirty(dst, true);
#elif defined(__aarch64__)
    if (op == 0x01)
        emit_addsub_register(state, true, AS_ADD, dst, dst, src);
#endif
}

#if RV32_HAS(EXT_M)
static inline void emit_alu64_imm8(struct jit_state *state,
                                   int op,
                                   int src UNUSED,
                                   int dst,
                                   int8_t imm)
{
#if defined(__x86_64__)
    /* REX.W 前缀、ModRM 字节和 8 位立即数。 */
    emit_alu64(state, op, src, dst);
    emit1(state, imm);
#elif defined(__aarch64__)
    if (op == 0xc1) {
        emit_load_imm(state, R10, imm);
        emit_dataproc_2source(state, true, DP2_LSRV, dst, dst, R10);
    } else if (src == 0) {
        emit_load_imm(state, R10, imm);
        emit_addsub_register(state, true, AS_ADD, dst, dst, R10);
    }
#endif
}
#endif

/* 寄存器到寄存器 mov：保留全部 64 位，包括符号扩展位。 */
static inline void emit_mov(struct jit_state *state, int src, int dst)
{
#if defined(__x86_64__)
    emit_alu64(state, 0x89, src, dst);
#elif defined(__aarch64__)
    /* 使用零寄存器上的 64 位 ORR：MOV Xd, Xm = ORR Xd, XZR, Xm。
     * 这样可保留全部 64 位，包括高 32 位上的符号扩展。
     * 旧实现使用 32 位 ADD，会把结果零扩展。
     */
    emit_logical_register(state, true, LOG_ORR, dst, RZ, src);
    set_dirty(dst, true);
#endif
}

#if defined(__x86_64__)
/* REX.W 前缀、ModRM 字节和 32 位立即数。 */
static inline void emit_alu64_imm32(struct jit_state *state,
                                    int op,
                                    int src,
                                    int dst,
                                    int32_t imm)
{
    emit_alu64(state, op, src, dst);
    emit4(state, imm);
}
#endif

static inline void emit_cmp_imm32(struct jit_state *state, int dst, int32_t imm)
{
#if defined(__x86_64__)
    emit_alu32_imm32(state, 0x81, 7, dst, imm);
#elif defined(__aarch64__)
    emit_load_imm(state, R10, imm);
    emit_addsub_register(state, false, AS_SUBS, RZ, dst, R10);
#endif
}

static inline void emit_cmp32(struct jit_state *state, int src, int dst)
{
#if defined(__x86_64__)
    emit_alu32(state, 0x39, src, dst);
#elif defined(__aarch64__)
    emit_addsub_register(state, false, AS_SUBS, RZ, dst, src);
#endif
}

static inline void emit_jcc_offset(struct jit_state *state, int code)
{
#if defined(__x86_64__)
    /* 无条件跳转指令没有 0x0f 前缀。 */
    if (code != JCC_JMP)
        emit1(state, 0x0f);
    emit1(state, code);
    emit4(state, 0);
#elif defined(__aarch64__)
    switch (code) {
    case JCC_JE: /* BEQ */
        code = COND_EQ;
        break;
    case JCC_JNE: /* BNE */
        code = COND_NE;
        break;
    case JCC_JL: /* BLT */
        code = COND_LT;
        break;
    case JCC_JGE: /* BGE */
        code = COND_GE;
        break;
    case JCC_JB: /* BLTU */
        code = COND_LO;
        break;
    case JCC_JAE: /* BGEU */
        code = COND_HS;
        break;
    case JCC_JMP: /* AL */
        code = COND_AL;
        break;
    default:
        assert(NULL);
        __UNREACHABLE;
    }
    emit_a64(state, BR_Bcond | (0 << 5) | code);
#endif
}

static inline void emit_load_imm(struct jit_state *state,
                                 int dst,
                                 uint32_t imm);

/* 将 [src + offset] 加载到 dst。
 *
 * 若 offset 非零，表示从栈中把 VM 寄存器恢复到宿主寄存器。
 * 否则这是一个 `read` 伪指令，把 [src] 处内容加载到目标寄存器。
 */
static inline void emit_load(struct jit_state *state,
                             enum operand_size size,
                             int src,
                             int dst,
                             int32_t offset)
{
    for (int i = 0; i < n_host_regs; i++) {
        if (register_map[i].reg_idx != dst)
            continue;
        if (register_map[i].vm_reg_idx != 0)
            continue;

        /* 若 dst 是 x0，则向宿主寄存器加载 0x0。 */
        emit_load_imm(state, dst, 0x0);
        set_dirty(dst, true);
        return;
    }

#if defined(__x86_64__)
    if (src & 8 || dst & 8)
        emit_basic_rex(state, 0, dst, src);
    if (size == S8 || size == S16) {
        /* movzx */
        emit1(state, 0x0f);
        emit1(state, size == S8 ? 0xb6 : 0xb7);
    } else if (size == S32) {
        /* mov */
        emit1(state, 0x8b);
    } else {
        assert(NULL);
        __UNREACHABLE;
    }

    emit_modrm_and_displacement(state, dst, src, offset);
#elif defined(__aarch64__)
    switch (size) {
    case S8:
        emit_loadstore_imm(state, LS_LDRB, dst, src, offset);
        break;
    case S16:
        emit_loadstore_imm(state, LS_LDRH, dst, src, offset);
        break;
    case S32:
        emit_loadstore_imm(state, LS_LDRW, dst, src, offset);
        break;
    case S64:
        emit_loadstore_imm(state, LS_LDRX, dst, src, offset);
        break;
    default:
        assert(NULL);
        __UNREACHABLE;
    }
#endif

    set_dirty(dst, !offset);
}

static inline void emit_load_sext(struct jit_state *state,
                                  enum operand_size size,
                                  int src,
                                  int dst,
                                  int32_t offset)
{
    for (int i = 0; i < n_host_regs; i++) {
        if (register_map[i].reg_idx != dst)
            continue;
        if (register_map[i].vm_reg_idx != 0)
            continue;

        /* 若 dst 是 x0，则向宿主寄存器加载 0x0。 */
        emit_load_imm(state, dst, 0x0);
        set_dirty(dst, true);
        return;
    }

#if defined(__x86_64__)
    if (size == S8 || size == S16) {
        if (src & 8 || dst & 8)
            emit_basic_rex(state, 0, dst, src);
        /* movsx */
        emit1(state, 0x0f);
        emit1(state, size == S8 ? 0xbe : 0xbf);
    } else if (size == S32) {
        emit_basic_rex(state, 1, dst, src);
        emit1(state, 0x63);
    }

    emit_modrm_and_displacement(state, dst, src, offset);
#elif defined(__aarch64__)
    switch (size) {
    case S8:
        emit_loadstore_imm(state, LS_LDRSBW, dst, src, offset);
        break;
    case S16:
        emit_loadstore_imm(state, LS_LDRSHW, dst, src, offset);
        break;
    case S32:
        emit_loadstore_imm(state, LS_LDRSW, dst, src, offset);
        break;
    default:
        __UNREACHABLE;
        break;
    }
#endif

    set_dirty(dst, !offset);
}

/* 将寄存器中的 32 位值原地符号扩展为 64 位。 */
static inline void UNUSED emit_sxtw(struct jit_state *state, int reg)
{
#if defined(__x86_64__)
    /* MOVSXD reg, reg：将 32 位符号扩展为 64 位。 */
    emit_basic_rex(state, 1, reg, reg);
    emit1(state, 0x63);
    emit_modrm_reg2reg(state, reg, reg);
#elif defined(__aarch64__)
    /* SXTW Xd, Wn 等价于 SBFM Xd, Xn, #0, #31。
     * 编码：sf=1, opc=00, N=1, immr=0, imms=31
     * = 0x93407C00 | (Rn << 5) | Rd
     */
    uint32_t insn = 0x93407C00 | ((uint32_t) reg << 5) | (uint32_t) reg;
    emit_a64(state, insn);
#endif
}

/* 将 32 位立即数加载到寄存器，并进行零扩展。 */
static inline void emit_load_imm(struct jit_state *state, int dst, uint32_t imm)
{
#if defined(__x86_64__)
    if (dst & 8)
        emit_basic_rex(state, 0, 0, dst);
    emit1(state, 0xb8 | (dst & 7));
    emit4(state, imm);

    set_dirty(dst, true);
#elif defined(__aarch64__)
    emit_movewide_imm(state, true, dst, imm);
#endif
}

/* 将符号扩展后的立即数加载到寄存器。 */
static inline void emit_load_imm_sext(struct jit_state *state,
                                      int dst,
                                      int64_t imm)
{
#if defined(__x86_64__)
    if ((int32_t) imm == imm)
        emit_alu64_imm32(state, 0xc7, 0, dst, imm);
    else {
        /* movabs $imm, dst */
        emit_basic_rex(state, 1, 0, dst);
        emit1(state, 0xb8 | (dst & 7));
        emit8(state, imm);
    }

    set_dirty(dst, true);
#elif defined(__aarch64__)
    if ((int32_t) imm == imm)
        emit_movewide_imm(state, false, dst, imm);
    else
        emit_movewide_imm(state, true, dst, imm);
#endif
}

static inline bool jit_store_x0(struct jit_state *state,
                                enum operand_size size,
                                int src,
                                int dst,
                                int32_t offset)
{
    for (int i = 0; i < n_host_regs; i++) {
        if (register_map[i].reg_idx != src)
            continue;
        if (register_map[i].vm_reg_idx != 0)
            continue;

#if defined(__x86_64__)
        /* 若 src 是 x0，则向目标位置写入 0x0。 */
        if (size == S16)
            emit1(state, 0x66); /* 16 位操作数覆盖前缀。 */
        if (dst & 8)
            emit_rex(state, 0, 0, 0, !!(dst & 8));
        emit1(state, size == S8 ? 0xc6 : 0xc7);
        emit1(state, 0x80 | (dst & 0x7));
        emit4(state, offset);
        switch (size) {
        case S8:
            emit1(state, 0x0);
            break;
        case S16:
            emit1(state, 0x0);
            emit1(state, 0x0);
            break;
        case S32:
            emit4(state, 0x0);
            break;
        default:
            assert(NULL);
            __UNREACHABLE;
        }
#elif defined(__aarch64__)
        switch (size) {
        case S8:
            emit_loadstore_imm(state, LS_STRB, RZ, dst, offset);
            break;
        case S16:
            emit_loadstore_imm(state, LS_STRH, RZ, dst, offset);
            break;
        case S32:
            emit_loadstore_imm(state, LS_STRW, RZ, dst, offset);
            break;
        default:
            assert(NULL);
            __UNREACHABLE;
        }
#endif
        set_dirty(src, false);
        return true;
    }
    return false;
}

/* 将寄存器 src 写入 [dst + offset]。
 *
 * 若 offset 非零，表示把宿主寄存器写回映射 VM 寄存器文件的栈槽。
 * 否则这是一个 `write` 伪指令，把 src 内容写入 [dst]。
 */
static inline void emit_store(struct jit_state *state,
                              enum operand_size size,
                              int src,
                              int dst,
                              int32_t offset)
{
    if (jit_store_x0(state, size, src, dst, offset))
        return;

#if defined(__x86_64__)
    if (size == S16)
        emit1(state, 0x66); /* 16 位操作数覆盖前缀。 */
    if (src & 8 || dst & 8 || size == S8)
        emit_rex(state, 0, !!(src & 8), 0, !!(dst & 8));
    emit1(state, size == S8 ? 0x88 : 0x89);
    emit_modrm_and_displacement(state, src, dst, offset);
#elif defined(__aarch64__)
    switch (size) {
    case S8:
        emit_loadstore_imm(state, LS_STRB, src, dst, offset);
        break;
    case S16:
        emit_loadstore_imm(state, LS_STRH, src, dst, offset);
        break;
    case S32:
        emit_loadstore_imm(state, LS_STRW, src, dst, offset);
        break;
    case S64:
        emit_loadstore_imm(state, LS_STRX, src, dst, offset);
        break;
    default:
        assert(NULL);
        __UNREACHABLE;
    }
#endif

    if (offset)
        set_dirty(src, false);
}

static inline void emit_jmp(struct jit_state *state,
                            uint32_t target_pc,
                            uint32_t target_satp UNUSED)
{
#if defined(__x86_64__)
    emit1(state, JCC_JMP);
    emit_jump_target_address(state, target_pc, target_satp);
#elif defined(__aarch64__)
    assert(state->n_jumps < MAX_JUMPS);

    struct jump *jump = &state->jumps[state->n_jumps++];
    jump->offset_loc = state->offset;
    jump->target_pc = target_pc;
    emit_a64(state, UBR_B);
#if RV32_HAS(SYSTEM)
    jump->target_satp = target_satp;
#endif
#endif
}

static inline void save_reg(struct jit_state *, int);
static inline void unmap_vm_reg(int);

static inline void emit_call(struct jit_state *state, intptr_t target)
{
#if defined(__x86_64__)
    emit_load_imm_sext(state, RAX, target);
    /* callq *%rax */
    emit1(state, 0xff);
    /* ModR/M 字节：b11010000b = xd0，其中 rax 是寄存器 0。 */
    emit1(state, 0xd0);
#elif defined(__aarch64__)
    uint32_t stack_movement = align_up(8, 16);
    emit_addsub_imm(state, true, AS_SUB, SP, SP, stack_movement);
    emit_loadstore_imm(state, LS_STRX, R30, SP, 0);

    emit_movewide_imm(state, true, temp_imm_reg, target);
    emit_uncond_branch_reg(state, BR_BLR, temp_imm_reg);

    save_reg(state, 0); /* R5 */
    unmap_vm_reg(0);    /* R5 */
    emit_logical_register(state, true, LOG_ORR, R5, RZ, R0);

    emit_loadstore_imm(state, LS_LDRX, R30, SP, 0);
    emit_addsub_imm(state, true, AS_ADD, SP, SP, stack_movement);
#endif
}

static inline void emit_exit(struct jit_state *state)
{
#if defined(__x86_64__)
    emit1(state, JCC_JMP);
    emit_jump_target_address(state, TARGET_PC_EXIT, 0);
#elif defined(__aarch64__)
    emit_jmp(state, TARGET_PC_EXIT, 0);
#endif
}

#if RV32_HAS(EXT_M)
#if defined(__x86_64__)
static inline void emit_conditional_move(struct jit_state *state,
                                         int src,
                                         int dst)
{
    emit1(state, 0x48);
    emit1(state, 0x0f);
    emit1(state, 0x44);
    emit_modrm_reg2reg(state, dst, src);
}
#elif defined(__aarch64__)
static inline void emit_conditional_move(struct jit_state *state,
                                         int rd,
                                         int rn,
                                         int rm,
                                         int cond)
{
    emit_a64(state, 0x1a800000 | (rm << 16) | (cond << 12) | (rn << 5) | rd);
    set_dirty(rd, true);
}

static void divmod(struct jit_state *state,
                   uint8_t opcode,
                   int rd,
                   int rn,
                   int rm,
                   bool sign)
{
    bool mod = (opcode & JIT_ALU_OP_MASK) == (JIT_OP_MOD_IMM & JIT_ALU_OP_MASK);
    bool is64 = (opcode & JIT_CLS_MASK) == JIT_CLS_ALU64;
    int div_dest = mod ? temp_div_reg : rd;

    if (sign)
        emit_cmp_imm32(state, rd, 0x80000000); /* 溢出检查。 */

    /* 有符号操作使用 SDIV，无符号操作使用 UDIV。 */
    emit_dataproc_2source(state, is64, sign ? DP2_SDIV : DP2_UDIV, div_dest, rn,
                          rm);
    if (mod)
        emit_dataproc_3source(state, is64, DP3_MSUB, rd, rm, div_dest, rn);

    if (sign) {
        /* 处理溢出。 */
        uint32_t jump_loc_0 = state->offset;
        emit_jcc_offset(state, JCC_JNE);
        emit_cmp_imm32(state, rm, -1);
        if (mod)
            emit_load_imm(state, R10, 0);
        else
            emit_load_imm(state, R10, 0x80000000);
        emit_conditional_move(state, rd, R10, rd, COND_EQ);
        emit_jump_target_offset(state, JUMP_LOC_0, state->offset);
    }
    if (!mod) {
        /* 处理除零。 */
        emit_cmp_imm32(state, rm, 0);
        emit_load_imm(state, temp_reg, -1);
        emit_conditional_move(state, rd, temp_reg, rd, COND_EQ);
    }
}
#endif

static void muldivmod(struct jit_state *state,
                      uint8_t opcode,
                      int src,
                      int dst,
                      bool sign)
{
#if defined(__x86_64__)
    bool mul = (opcode & JIT_ALU_OP_MASK) == (JIT_OP_MUL_IMM & JIT_ALU_OP_MASK);
    bool div = (opcode & JIT_ALU_OP_MASK) == (JIT_OP_DIV_IMM & JIT_ALU_OP_MASK);
    bool mod = (opcode & JIT_ALU_OP_MASK) == (JIT_OP_MOD_IMM & JIT_ALU_OP_MASK);
    bool is64 = (opcode & JIT_CLS_MASK) == JIT_CLS_ALU64;

    /* 寄存器被挪作他用前先记录映射状态，弹栈后再恢复该状态。
     */
    int d1 = register_map[0].dirty, d2 = register_map[2].dirty;
    int r1 = register_map[0].vm_reg_idx, r2 = register_map[2].vm_reg_idx;

    if (dst != RAX) {
        unmap_vm_reg(0); /* RAX */
        emit_push(state, RAX);
    }

    if (dst != RDX) {
        unmap_vm_reg(2); /* RDX */
        emit_push(state, RDX);
    }

    /* 将除数加载到 RCX。 */
    emit_mov(state, src, RCX);

    /* 将被除数加载到 RAX。 */
    emit_mov(state, dst, RAX);

    /* JIT 对除法和取模采用不同语义：除法中除数为 0 时结果为 -1；
     * 取模中除数为 0 时结果为被除数。为统一处理，先在原除数为 0 时把除数改为
     * 1，再按原除数是否为 0 修正结果：除法修正为 -1，取模修正为被除数。
     */

    if (div || mod) {
        if (sign) {
            emit_load_imm_sext(state, RDX, -1);
            /* 与 -1 比较，用于溢出检查。 */
            emit_cmp32(state, RDX, RCX);
            /* 保存比较结果。 */
            emit1(state, 0x9c); /* pushfq */
        }
        if (mod || (div && sign))
            emit_push(state, RAX); /* 保存被除数。 */

        emit_alu32(state, 0x85, RCX, RCX);
        /* 保存 test 结果。 */
        emit1(state, 0x9c); /* pushfq */

        /* 若除数为 0，则先把除数设为 1。 */
        emit_load_imm(state, RDX, 1);
        emit_conditional_move(state, RDX, RCX);
        /* xor %edx,%edx */
        emit_alu32(state, 0x31, RDX, RDX);
    }

    if (is64)
        emit_rex(state, 1, 0, 0, 0);
    /* 执行乘法或除法。 */
    emit_alu32(state, 0xf7, mul ? 4 : 6, RCX);

    /* 除法操作会把余数放入 RDX，把商放入 RAX。
     */
    if (div || mod) {
        /* 恢复 test 结果。 */
        emit1(state, 0x9d); /* popfq */

        /* 若 zero flag 被设置，则原除数为 0。 */

        if (div) {
            /* 若原除数为 0，则把结果修正为 -1。 */
            emit_load_imm_sext(state, RCX, -1);

            /* 使用 conditional move 避免分支。 */
            emit_conditional_move(state, RCX, RAX);
            if (sign) {
                emit_pop(state, RCX);
                /* 处理 DIV 溢出。 */
                emit1(state, 0x9d); /* popfq */
                uint32_t jump_loc_0 = state->offset;
                emit_jcc_offset(state, JCC_JNE);
                emit_cmp_imm32(state, RCX, 0x80000000);
                emit_conditional_move(state, RCX, RAX);
                emit_jump_target_offset(state, JUMP_LOC_0, state->offset);
            }
        } else {
            /* 将被除数恢复到 RCX。 */
            emit_pop(state, RCX);
            /* 若原除数为 0，则把被除数写入 RDX；使用 conditional move 避免分支。 */
            emit_conditional_move(state, RCX, RDX);
            if (sign) {
                /* 处理 REM 溢出。 */
                emit1(state, 0x9d); /* popfq */
                uint32_t jump_loc_0 = state->offset;
                emit_jcc_offset(state, JCC_JNE);
                emit_cmp_imm32(state, RCX, 0x80000000);
                emit_load_imm(state, RCX, 0);
                emit_conditional_move(state, RCX, RDX);
                emit_jump_target_offset(state, JUMP_LOC_0, state->offset);
            }
        }
    }

    if (dst != RDX) {
        if (mod)
            emit_mov(state, RDX, dst);
        emit_pop(state, RDX);
        register_map[2].vm_reg_idx = r2;
        register_map[2].dirty = d2;
    }
    if (dst != RAX) {
        if (div || mul)
            emit_mov(state, RAX, dst);
        emit_pop(state, RAX);
        register_map[0].vm_reg_idx = r1;
        register_map[0].dirty = d1;
    }
#elif defined(__aarch64__)
    switch (opcode) {
    case 0x28:
        emit_dataproc_3source(state, false, DP3_MADD, dst, dst, src, RZ);
        break;
    case 0x2f:
        emit_dataproc_3source(state, true, DP3_MADD, dst, dst, src, RZ);
        break;
    case 0x38:
        divmod(state, JIT_OP_DIV_REG, dst, dst, src, sign);
        break;
    case 0x98:
        divmod(state, JIT_OP_MOD_REG, dst, dst, src, sign);
        break;
    default:
        __UNREACHABLE;
        break;
    }
#endif
}
#endif /* RV32_HAS(EXT_M) */

/* JIT 非对齐内存访问处理器。
 * 该函数用字节级访问完成非对齐 load/store，行为与解释器默认非对齐 trap 处理保持
 * 一致。
 * @rv: RISC-V 模拟器状态。
 * @addr: 非对齐内存地址。
 * @vreg_idx: 寄存器索引；load 时为 rd，store 时为 rs2。
 * @type: 指令类型（rv_insn_lw、rv_insn_lh 等）。
 * @is_store: true 表示 store，false 表示 load。
 *
 * 注意：JIT 生成代码检测到非对齐内存访问，且模拟器配置为由软件处理非对齐访问
 *（allow_misalign 为 false）时，会调用该处理器。
 */
void jit_misaligned_handler(riscv_t *rv,
                            uint32_t addr,
                            uint32_t vreg_idx,
                            uint32_t type,
                            bool is_store)
{
    assert(vreg_idx < 32);

    if (is_store) {
        /* 非对齐 store。 */
        uint32_t value = rv->X[vreg_idx];
        switch (type) {
        case rv_insn_sw:
#if RV32_HAS(EXT_C)
        case rv_insn_csw:
        case rv_insn_cswsp:
#endif
            /* 2 字节对齐走快路径，奇数地址走慢路径。 */
            if ((addr & 1) == 0) {
                rv->io.mem_write_s(rv, addr, value & 0xFFFF);
                rv->io.mem_write_s(rv, addr + 2, (value >> 16) & 0xFFFF);
            } else {
                for (int i = 0; i < 4; i++)
                    rv->io.mem_write_b(rv, addr + i, (value >> (i * 8)) & 0xFF);
            }
            break;
        case rv_insn_sh:
            for (int i = 0; i < 2; i++)
                rv->io.mem_write_b(rv, addr + i, (value >> (i * 8)) & 0xFF);
            break;
        default:
            break;
        }
    } else {
        /* 非对齐 load。 */
        uint32_t value = 0;
        switch (type) {
        case rv_insn_lw:
#if RV32_HAS(EXT_C)
        case rv_insn_clw:
        case rv_insn_clwsp:
#endif
            /* 2 字节对齐走快路径，奇数地址走慢路径。 */
            if ((addr & 1) == 0) {
                value = (uint32_t) rv->io.mem_read_s(rv, addr);
                value |= ((uint32_t) rv->io.mem_read_s(rv, addr + 2)) << 16;
            } else {
                for (int i = 0; i < 4; i++)
                    value |= ((uint32_t) rv->io.mem_read_b(rv, addr + i))
                             << (i * 8);
            }
            rv->X[vreg_idx] = value;
            break;
        case rv_insn_lh:
            for (int i = 0; i < 2; i++)
                value |= ((uint32_t) rv->io.mem_read_b(rv, addr + i))
                         << (i * 8);
            rv->X[vreg_idx] = (int32_t) ((int16_t) value); /* 符号扩展。 */
            break;
        case rv_insn_lhu:
            for (int i = 0; i < 2; i++)
                value |= ((uint32_t) rv->io.mem_read_b(rv, addr + i))
                         << (i * 8);
            rv->X[vreg_idx] = value;
            break;
        default:
            break;
        }
    }
}

#if RV32_HAS(SYSTEM_MMIO)
uint32_t jit_mmio_read_wrapper(riscv_t *rv, uint32_t addr)
{
    MMIO_READ();
    __UNREACHABLE;
}

void jit_mmu_handler(riscv_t *rv, uint32_t vreg_idx)
{
    assert(vreg_idx < 32);

    uint32_t addr;
    uint32_t access_size;

    /* 按指令类型确定访问大小。 */
    switch (rv->jit_mmu.type) {
    case rv_insn_lb:
    case rv_insn_lbu:
    case rv_insn_sb:
        access_size = 1;
        break;
    case rv_insn_lh:
    case rv_insn_lhu:
    case rv_insn_sh:
        access_size = 2;
        break;
    case rv_insn_lw:
    case rv_insn_sw:
        access_size = 4;
        break;
    default:
        /* 尽早捕获未处理的指令类型。 */
        assert(!"未处理的 JIT MMU 指令类型");
        __UNREACHABLE;
    }

    /* 调用 mem_translate 前，先把 rv->PC 设置为出错指令的 PC。
     * 这是必要的：如果发生缺页，mem_translate 内部会通过
     * SET_CAUSE_AND_TVAL_THEN_TRAP 调用 on_trap，而 trap 处理器使用 rv->PC 设置
     * sepc/mepc（返回地址）。如果不这样做，内核在 sret 后会从错误指令恢复。
     */
    rv->PC = rv->jit_mmu.pc;

    if (rv->jit_mmu.type == rv_insn_lb || rv->jit_mmu.type == rv_insn_lh ||
        rv->jit_mmu.type == rv_insn_lbu || rv->jit_mmu.type == rv_insn_lhu ||
        rv->jit_mmu.type == rv_insn_lw)
        addr = rv->io.mem_translate(rv, rv->jit_mmu.vaddr, R);
    else
        addr = rv->io.mem_translate(rv, rv->jit_mmu.vaddr, W);

    /* 检查地址转换期间是否触发 trap。
     * mem_translate 可能触发缺页并设置 is_trapped=true。此时把访问标记为 MMIO，
     * 让 JIT 代码跳过直接内存访问，但实际不执行任何 MMIO 操作。
     */
    if (rv->is_trapped) {
        rv->jit_mmu.is_mmio = 1;
        return;
    }

    /* 只有整个访问区间 [addr, addr+size) 都位于有效客体内存范围内时，才视为 RAM。
     * 这能防止靠近内存边界的多字节访问造成缓冲区溢出。
     */
    if (GUEST_RAM_CONTAINS(PRIV(rv)->mem, addr, access_size)) {
        rv->jit_mmu.is_mmio = 0;
        rv->jit_mmu.paddr = addr;
        return;
    }

    uint32_t val;
    rv->jit_mmu.is_mmio = 1;

    switch (rv->jit_mmu.type) {
    case rv_insn_sb:
        val = rv->X[vreg_idx] & 0xff;
        MMIO_WRITE();
        break;
    case rv_insn_sh:
        val = rv->X[vreg_idx] & 0xffff;
        MMIO_WRITE();
        break;
    case rv_insn_sw:
        val = rv->X[vreg_idx];
        MMIO_WRITE();
        break;
    case rv_insn_lb:
        rv->X[vreg_idx] = (int8_t) jit_mmio_read_wrapper(rv, addr);
        break;
    case rv_insn_lh:
        rv->X[vreg_idx] = (int16_t) jit_mmio_read_wrapper(rv, addr);
        break;
    case rv_insn_lw:
        rv->X[vreg_idx] = jit_mmio_read_wrapper(rv, addr);
        break;
    case rv_insn_lbu:
        rv->X[vreg_idx] = (uint8_t) jit_mmio_read_wrapper(rv, addr);
        break;
    case rv_insn_lhu:
        rv->X[vreg_idx] = (uint16_t) jit_mmio_read_wrapper(rv, addr);
        break;
    default:
        assert(NULL);
        __UNREACHABLE;
    }
}

void emit_jit_mmu_handler(struct jit_state *state, uint8_t vreg_idx)
{
    assert(vreg_idx < 32);

#if defined(__x86_64__)
    /* 压栈保存 $rdi。 */
    emit1(state, 0xff);
    emit_modrm(state, 0x3 << 6, 0x6, parameter_reg[0]);

    /* 将 vreg_idx 移入 %rsi。 */
    emit1(state, 0xbe);
    emit4(state, vreg_idx);

    /* 调用 jit_mmu_handler。 */
    emit_load_imm_sext(state, temp_reg, (uintptr_t) &jit_mmu_handler);
    emit1(state, 0xff);
    emit_modrm(state, 0x3 << 6, 0x2, temp_reg);

    /* 从栈中恢复 rv 到 $rdi。 */
    emit1(state, 0x8f);
    emit_modrm(state, 0x3 << 6, 0x0, parameter_reg[0]);
#elif defined(__aarch64__)
    uint32_t insn;

    /* 将 rv 压入栈。 */
    insn = (0xf81f0fe << 4) | R0;
    emit_a64(state, insn);

    /* 将 vreg_idx 移入 R1。 */
    emit_movewide_imm(state, false, R1, vreg_idx);

    /* 加载 &jit_mmu_handler。 */
    emit_movewide_imm(state, true, temp_reg, (uintptr_t) &jit_mmu_handler);
    /* 通过 blr 调用 jit_mmu_handler。 */
    insn = (0xd63f << 16) | (temp_reg << 5);
    emit_a64(state, insn);

    /* 从栈中恢复 rv。 */
    insn = (0xf84107e << 4) | R0;
    emit_a64(state, insn);
#endif
}
#endif

static void prepare_translate(struct jit_state *state)
{
#if defined(__x86_64__)
    /* 保存平台非易失寄存器。 */
    for (uint32_t i = 0; i < ARRAY_SIZE(nonvolatile_reg); i++)
        emit_push(state, nonvolatile_reg[i]);

    /* 假设进入本段代码的 call 指令执行前，栈已经按 16 字节对齐。
     * 开始执行 JIT 代码时需要恢复 16 字节对齐。STACK_SIZE 保证可被 16 整除。
     * 但若状态保存阶段（见上方）压入了偶数个寄存器，则需额外增加 8 字节以重新
     * 获得 16 字节对齐。
     */
    if (!(ARRAY_SIZE(nonvolatile_reg) % 2))
        emit_alu64_imm32(state, 0x81, 5, RSP, 0x8);

    /* 将 JIT R10（JIT 中访问 frame 的方式）设置为当前 RSP。 */
    emit_mov(state, RSP, RBP);

    /* 分配栈空间。 */
    emit_alu64_imm32(state, 0x81, 5, RSP, STACK_SIZE);

#if defined(_WIN32)
    /* Windows x64 ABI 要求 home register space。 */
    /* 分配 4 个寄存器大小的 home register space。 */
    emit_alu64_imm32(state, 0x81, 5, RSP, 4 * sizeof(uint64_t));
#endif

    /* 跳转到第二个参数中保存的入口点。 */
    emit1(state, 0xff);
    emit1(state, 0xe6);

    /* 函数尾声。 */
    state->exit_loc = state->offset;

    /* 从 JIT R10 恢复 RSP，以释放栈空间。 */
    emit_mov(state, RBP, RSP);

    if (!(ARRAY_SIZE(nonvolatile_reg) % 2))
        emit_alu64_imm32(state, 0x81, 0, RSP, 0x8);

    /* 恢复平台非易失寄存器。 */
    for (uint32_t i = 0; i < ARRAY_SIZE(nonvolatile_reg); i++)
        emit_pop(state, nonvolatile_reg[ARRAY_SIZE(nonvolatile_reg) - i - 1]);

    /* 返回调用方。 */
    emit1(state, 0xc3);
#elif defined(__aarch64__)
    uint32_t register_space = ARRAY_SIZE(callee_reg) * 8 + 2 * 8;
    state->stack_size = align_up(STACK_SIZE + register_space, 16);
    emit_addsub_imm(state, true, AS_SUB, SP, SP, state->stack_size);

    /* 建立栈帧。 */
    emit_loadstorepair_imm(state, LSP_STPX, R29, R30, SP, 0);
    /* ARM64 调用约定中，R29 是 frame pointer。 */
    emit_addsub_imm(state, true, AS_ADD, R29, SP, 0);

    /* 保存被调用者保存寄存器。 */
    for (size_t i = 0; i < ARRAY_SIZE(callee_reg); i += 2) {
        emit_loadstorepair_imm(state, LSP_STPX, callee_reg[i],
                               callee_reg[i + 1], SP, (i + 2) * 8);
    }

    emit_uncond_branch_reg(state, BR_BR, R1);
    /* 函数尾声。 */
    state->exit_loc = state->offset;

    /* 恢复被调用者保存寄存器。 */
    for (size_t i = 0; i < ARRAY_SIZE(callee_reg); i += 2) {
        emit_loadstorepair_imm(state, LSP_LDPX, callee_reg[i],
                               callee_reg[i + 1], SP, (i + 2) * 8);
    }
    emit_loadstorepair_imm(state, LSP_LDPX, R29, R30, SP, 0);
    emit_addsub_imm(state, true, AS_ADD, SP, SP, state->stack_size);
    emit_uncond_branch_reg(state, BR_RET, R30);
#endif
    state->org_size = state->offset;
}

static int liveness[N_RV_REGS];
/* VM 寄存器优先队列，后续最晚再使用的寄存器排在最前。 */
static uint8_t candidate_queue[N_RV_REGS];
static int vm_reg[3]; /* enum x64_reg/a64_reg */

static void reset_reg()
{
    for (int i = 0; i < n_host_regs; i++) {
        register_map[i].vm_reg_idx = -1;
        register_map[i].dirty = false;
        register_map[i].alive = false;
    }
}

/* 如果宿主寄存器为 dirty，则写回 VM 寄存器。 */
static inline void save_reg(struct jit_state *state, int idx)
{
    assert(idx > -1 && idx < n_host_regs);

    if (!register_map[idx].dirty)
        return;

    /* 永远不保存 x0，它硬连线为零。这样可把 rv_reg_zero 用作临时计算 scratch，
     * 而不会破坏零寄存器。
     */
    if (register_map[idx].vm_reg_idx == 0) {
        register_map[idx].dirty = 0;
        return;
    }

    emit_store(state, S32, register_map[idx].reg_idx, parameter_reg[0],
               offsetof(riscv_t, X) + 4 * register_map[idx].vm_reg_idx);
    register_map[idx].dirty = 0;
}

static void store_back(struct jit_state *state)
{
    for (int i = 0; i < n_host_regs; i++) {
        if (register_map[i].vm_reg_idx == -1)
            continue;
        save_reg(state, i);
    }
}

static inline void liveness_reset()
{
    memset(liveness, 0xff, sizeof(liveness));
}

static inline void candidate_queue_init()
{
    for (int i = 0; i < N_RV_REGS; i++) {
        candidate_queue[i] = i;
    }
}

static int liveness_cmp(const void *l, const void *r)
{
    int liveness_l = liveness[*(uint8_t *) l];
    int liveness_r = liveness[*(uint8_t *) r];

    /* 使用显式比较，避免减法比较可能溢出。 */
    if (liveness_l < liveness_r)
        return -1;
    if (liveness_l > liveness_r)
        return 1;

    /* 使用寄存器索引作为稳定排序的平局规则。 */
    uint8_t reg_l = *(uint8_t *) l;
    uint8_t reg_r = *(uint8_t *) r;
    if (reg_l < reg_r)
        return -1;
    if (reg_l > reg_r)
        return 1;
    return 0;
}

static inline void liveness_calc(block_t *block)
{
    uint32_t idx;
    rv_insn_t *ir;

    /* 按 src/rv32_template.c 中操作实现的顺序统计活跃度。 */
    for (idx = 0, ir = block->ir_head; idx < block->n_insn;
         idx++, ir = ir->next) {
        switch (ir->opcode) {
        case rv_insn_nop:
        case rv_insn_lui:
        case rv_insn_auipc:
        case rv_insn_jal:
            break;
        case rv_insn_jalr:
            liveness[ir->rs1] = idx;
            break;
        case rv_insn_beq:
        case rv_insn_bne:
        case rv_insn_blt:
        case rv_insn_bge:
        case rv_insn_bltu:
        case rv_insn_bgeu:
            liveness[ir->rs1] = idx;
            liveness[ir->rs2] = idx;
            break;
        case rv_insn_lb:
        case rv_insn_lh:
        case rv_insn_lw:
        case rv_insn_lbu:
        case rv_insn_lhu:
            liveness[ir->rs1] = idx;
            break;
        case rv_insn_sb:
        case rv_insn_sh:
        case rv_insn_sw:
            liveness[ir->rs1] = idx;
            liveness[ir->rs2] = idx;
            break;
        case rv_insn_addi:
        case rv_insn_slti:
        case rv_insn_sltiu:
        case rv_insn_xori:
        case rv_insn_ori:
        case rv_insn_andi:
        case rv_insn_slli:
        case rv_insn_srli:
        case rv_insn_srai:
            liveness[ir->rs1] = idx;
            break;
        case rv_insn_add:
        case rv_insn_sub:
        case rv_insn_sll:
        case rv_insn_slt:
        case rv_insn_sltu:
        case rv_insn_xor:
        case rv_insn_srl:
        case rv_insn_sra:
        case rv_insn_or:
        case rv_insn_and:
            liveness[ir->rs1] = idx;
            liveness[ir->rs2] = idx;
            break;
        case rv_insn_ecall:
        case rv_insn_ebreak:
            break;
#if RV32_HAS(EXT_M)
        case rv_insn_mul:
        case rv_insn_mulh:
        case rv_insn_mulhsu:
        case rv_insn_mulhu:
        case rv_insn_div:
        case rv_insn_divu:
        case rv_insn_rem:
        case rv_insn_remu:
            liveness[ir->rs1] = idx;
            liveness[ir->rs2] = idx;
            break;
#endif
#if RV32_HAS(EXT_C)
        case rv_insn_caddi4spn:
            liveness[rv_reg_sp] = idx;
            break;
        case rv_insn_clw:
            liveness[ir->rs1] = idx;
            break;
        case rv_insn_csw:
            liveness[ir->rs1] = idx;
            liveness[ir->rs2] = idx;
            break;
        case rv_insn_cnop:
            break;
        case rv_insn_caddi:
            liveness[ir->rd] = idx;
            break;
        case rv_insn_cjal:
        case rv_insn_cli:
        case rv_insn_clui:
            break;
        case rv_insn_caddi16sp:
            liveness[ir->rd] = idx;
            break;
        case rv_insn_csrli:
        case rv_insn_csrai:
        case rv_insn_candi:
            liveness[ir->rs1] = idx;
            break;
        case rv_insn_csub:
        case rv_insn_cxor:
        case rv_insn_cor:
        case rv_insn_cand:
            liveness[ir->rs1] = idx;
            liveness[ir->rs2] = idx;
            break;
        case rv_insn_cj:
            break;
        case rv_insn_cbeqz:
        case rv_insn_cbnez:
            liveness[ir->rs1] = idx;
            break;
        case rv_insn_cslli:
            liveness[ir->rd] = idx;
            break;
        case rv_insn_clwsp:
            liveness[rv_reg_sp] = idx;
            break;
        case rv_insn_cjr:
            liveness[ir->rs1] = idx;
            break;
        case rv_insn_cmv:
            liveness[ir->rs2] = idx;
            break;
        case rv_insn_cebreak:
            break;
        case rv_insn_cjalr:
            liveness[ir->rs1] = idx;
            break;
        case rv_insn_cadd:
            liveness[ir->rs1] = idx;
            liveness[ir->rs2] = idx;
            break;
        case rv_insn_cswsp:
            liveness[rv_reg_sp] = idx;
            liveness[ir->rs2] = idx;
            break;
#endif
        case rv_insn_fuse1:
            for (int i = 0; i < ir->imm2; i++) {
                liveness[ir->fuse[i].rd] = idx;
            }
            break;
        case rv_insn_fuse2:
            liveness[ir->rs1] = idx;
            break;
        case rv_insn_fuse3:
            for (int i = 0; i < ir->imm2; i++) {
                liveness[ir->fuse[i].rs1] = idx;
                liveness[ir->fuse[i].rs2] = idx;
            }
            break;
        case rv_insn_fuse4:
        case rv_insn_fuse5:
            for (int i = 0; i < ir->imm2; i++) {
                liveness[ir->fuse[i].rs1] = idx;
            }
            break;
        case rv_insn_fuse6:
            /* LI a7 + ECALL：无须跟踪寄存器，a7 在内部设置。 */
            break;
        case rv_insn_fuse7:
            /* 多条 ADDI：跟踪每个操作的 rs1。 */
            for (int i = 0; i < ir->imm2; i++) {
                liveness[ir->fuse[i].rs1] = idx;
            }
            break;
        case rv_insn_fuse8:
            /* LUI + ADDI：无源寄存器，rd = imm + imm2。 */
            break;
        case rv_insn_fuse9:
            /* LUI + LW：无源寄存器，绝对地址加载。 */
            break;
        case rv_insn_fuse10:
            /* LUI + SW：rs1 是待存储值来源。 */
            liveness[ir->rs1] = idx;
            break;
        case rv_insn_fuse11:
            /* LW + ADDI：rs1 是基址和递增来源。 */
            liveness[ir->rs1] = idx;
            break;
        case rv_insn_fuse12:
            /* ADDI + BNE：rs1 是源寄存器。 */
            liveness[ir->rs1] = idx;
            break;
        default:
            __UNREACHABLE;
        }
    }

    candidate_queue_init();
    qsort(candidate_queue, N_RV_REGS, sizeof(uint8_t), liveness_cmp);
}

static inline void regs_refresh(int idx)
{
    for (int i = 0; i < n_host_regs; i++) {
        if (register_map[i].vm_reg_idx == -1)
            continue;
        if (liveness[register_map[i].vm_reg_idx] < idx)
            register_map[i].alive = false;
    }
}

/* 返回 register_map 中的索引。 */
static inline int reg_pick(int reserved)
{
    /* 优先选择可用寄存器。 */
    for (int i = 0; i < n_host_regs; i++) {
        if (register_map[i].reg_idx == reserved)
            continue;
        if (!register_map[i].alive)
            return i;
    }

    /* 寄存器耗尽时，选择后续最晚再使用的那个。 */
    int idx = -1;
    for (int i = 0; i < N_RV_REGS; i++) {
        uint8_t candidate = candidate_queue[i];
        for (int j = 0; j < n_host_regs; j++) {
            if (register_map[j].reg_idx == reserved)
                continue;
            if (register_map[j].vm_reg_idx == candidate) {
                idx = j;
                goto end_pick_reg;
            }
        }
    }
    __UNREACHABLE;

end_pick_reg:
    assert(idx > -1 && idx < n_host_regs);
    return idx;
}

/* 返回 register_map 中的索引，同时避开两个保留寄存器。 */
static inline int reg_pick2(int reserved1, int reserved2)
{
    /* 优先选择可用寄存器。 */
    for (int i = 0; i < n_host_regs; i++) {
        if (register_map[i].reg_idx == reserved1 ||
            register_map[i].reg_idx == reserved2)
            continue;
        if (!register_map[i].alive)
            return i;
    }

    /* 寄存器耗尽时，选择后续最晚再使用的那个。 */
    int idx = -1;
    for (int i = 0; i < N_RV_REGS; i++) {
        uint8_t candidate = candidate_queue[i];
        for (int j = 0; j < n_host_regs; j++) {
            if (register_map[j].reg_idx == reserved1 ||
                register_map[j].reg_idx == reserved2)
                continue;
            if (register_map[j].vm_reg_idx == candidate) {
                idx = j;
                goto end_pick_reg2;
            }
        }
    }
    __UNREACHABLE;

end_pick_reg2:
    assert(idx > -1 && idx < n_host_regs);
    return idx;
}

/* 解除 VM 寄存器到宿主寄存器的映射。 */
static inline void unmap_vm_reg(int idx)
{
    /* 解除映射前应已处理 dirty 状态。 */
    assert(idx > -1 && idx < n_host_regs);
    register_map[idx].vm_reg_idx = -1;
}

static inline void set_vm_reg(int idx, int vm_reg_idx)
{
    assert(idx > -1 && idx < n_host_regs);
    register_map[idx].vm_reg_idx = vm_reg_idx;
    register_map[idx].alive = true;
}

/* 将 VM 寄存器映射到宿主寄存器。若宿主寄存器耗尽，则选择一个寄存器换出。
 */
static inline int map_vm_reg(struct jit_state *state, int vm_reg_idx)
{
    for (int i = 0; i < n_host_regs; i++) {
        if (register_map[i].vm_reg_idx != vm_reg_idx)
            continue;
        return register_map[i].reg_idx;
    }

    int idx = reg_pick(-1);
    int target_reg = register_map[idx].reg_idx;
    save_reg(state, idx);
    unmap_vm_reg(idx);
    set_vm_reg(idx, vm_reg_idx);
    return target_reg;
}

static int ra_load(struct jit_state *state, int vm_reg_idx)
{
    int origin = -1;
    for (int i = 0; i < n_host_regs; i++) {
        if (register_map[i].vm_reg_idx != vm_reg_idx)
            continue;
        origin = register_map[i].reg_idx;
    }

    int target_reg = map_vm_reg(state, vm_reg_idx);

    if (origin != target_reg)
        emit_load(state, S32, parameter_reg[0], target_reg,
                  offsetof(riscv_t, X) + 4 * vm_reg_idx);
    return target_reg;
}

/* 避免宿主寄存器冲突：第一个 VM 寄存器已经完成映射时，第二个 VM 寄存器若将映射
 * 到同一个宿主寄存器并触发交换，就需要保护已保留的宿主寄存器。
 */
static inline int map_vm_reg_reserved(struct jit_state *state,
                                      int vm_reg_idx,
                                      int reserved_reg_idx)
{
    for (int i = 0; i < n_host_regs; i++) {
        if (register_map[i].vm_reg_idx != vm_reg_idx)
            continue;
        return register_map[i].reg_idx;
    }

    int idx, target_reg;
    do {
        idx = reg_pick(reserved_reg_idx);
        target_reg = register_map[idx].reg_idx;
    } while (target_reg == reserved_reg_idx);

    save_reg(state, idx);
    unmap_vm_reg(idx);
    set_vm_reg(idx, vm_reg_idx);
    return target_reg;
}

/* 映射一个 VM 寄存器，同时保护两个已分配的宿主寄存器。
 * 这样在分配第三个寄存器时，寄存器分配器不会逐出两个保留寄存器之一
 * （例如加载 rs1、rs2 后再为 rd 分配寄存器）。
 */
static inline int map_vm_reg_reserved2(struct jit_state *state,
                                       int vm_reg_idx,
                                       int reserved_reg_idx1,
                                       int reserved_reg_idx2)
{
    for (int i = 0; i < n_host_regs; i++) {
        if (register_map[i].vm_reg_idx != vm_reg_idx)
            continue;
        return register_map[i].reg_idx;
    }

    int idx = reg_pick2(reserved_reg_idx1, reserved_reg_idx2);
    int target_reg = register_map[idx].reg_idx;

    save_reg(state, idx);
    unmap_vm_reg(idx);
    set_vm_reg(idx, vm_reg_idx);
    return target_reg;
}

static void ra_load2(struct jit_state *state, int vm_reg_idx1, int vm_reg_idx2)
{
    int origin1 = -1, origin2 = -1;
    for (int i = 0; i < n_host_regs; i++) {
        if (register_map[i].vm_reg_idx != vm_reg_idx1)
            continue;
        origin1 = register_map[i].reg_idx;
    }
    for (int i = 0; i < n_host_regs; i++) {
        if (register_map[i].vm_reg_idx != vm_reg_idx2)
            continue;
        origin2 = register_map[i].reg_idx;
    }

    if (vm_reg_idx1 == vm_reg_idx2) {
        vm_reg[0] = vm_reg[1] = map_vm_reg(state, vm_reg_idx1);
    } else {
        vm_reg[0] = map_vm_reg(state, vm_reg_idx1);
        vm_reg[1] = map_vm_reg_reserved(state, vm_reg_idx2, vm_reg[0]);
        assert(vm_reg[0] != vm_reg[1]);
    }

    if (origin1 != vm_reg[0])
        emit_load(state, S32, parameter_reg[0], vm_reg[0],
                  offsetof(riscv_t, X) + 4 * vm_reg_idx1);
    if (origin2 != vm_reg[1])
        emit_load(state, S32, parameter_reg[0], vm_reg[1],
                  offsetof(riscv_t, X) + 4 * vm_reg_idx2);
}

#if RV32_HAS(EXT_M)
static void ra_load2_sext(struct jit_state *state,
                          int vm_reg_idx1,
                          int vm_reg_idx2,
                          bool sext1,
                          bool sext2)
{
    int origin1 = -1, origin2 = -1;
    for (int i = 0; i < n_host_regs; i++) {
        if (register_map[i].vm_reg_idx != vm_reg_idx1)
            continue;
        origin1 = register_map[i].reg_idx;
    }
    for (int i = 0; i < n_host_regs; i++) {
        if (register_map[i].vm_reg_idx != vm_reg_idx2)
            continue;
        origin2 = register_map[i].reg_idx;
    }

    if (vm_reg_idx1 == vm_reg_idx2) {
        vm_reg[0] = vm_reg[1] = map_vm_reg(state, vm_reg_idx1);
    } else {
        vm_reg[0] = map_vm_reg(state, vm_reg_idx1);
        vm_reg[1] = map_vm_reg_reserved(state, vm_reg_idx2, vm_reg[0]);
        assert(vm_reg[0] != vm_reg[1]);
    }

    if (origin1 != vm_reg[0]) {
        if (sext1)
            emit_load_sext(state, S32, parameter_reg[0], vm_reg[0],
                           offsetof(riscv_t, X) + 4 * vm_reg_idx1);
        else
            emit_load(state, S32, parameter_reg[0], vm_reg[0],
                      offsetof(riscv_t, X) + 4 * vm_reg_idx1);
    } else if (sext1) {
        /* 寄存器已映射，但可能尚未符号扩展。
         * ARM64 上 emit_mov 使用 32 位操作，会把结果零扩展，因此有符号操作必须
         * 显式执行符号扩展。
         */
        emit_sxtw(state, vm_reg[0]);
    }
    if (origin2 != vm_reg[1]) {
        if (sext2)
            emit_load_sext(state, S32, parameter_reg[0], vm_reg[1],
                           offsetof(riscv_t, X) + 4 * vm_reg_idx2);
        else
            emit_load(state, S32, parameter_reg[0], vm_reg[1],
                      offsetof(riscv_t, X) + 4 * vm_reg_idx2);
    } else if (sext2) {
        /* 寄存器已映射，但可能尚未符号扩展。 */
        emit_sxtw(state, vm_reg[1]);
    }
}
#endif

void parse_branch_history_table(struct jit_state *state,
                                riscv_t *rv UNUSED,
                                rv_insn_t *ir)
{
    branch_history_table_t *bt = ir->branch_table;
    int max_idx = bht_find_max_idx(bt);
#if RV32_HAS(SYSTEM)
    if (!bht_should_translate(bt, max_idx, rv->csr_satp))
        return;
#else
    if (!bht_should_translate(bt, max_idx))
        return;
#endif
    save_reg(state, 0);
    unmap_vm_reg(0);
    emit_load_imm(state, register_map[0].reg_idx, bt->PC[max_idx]);
    emit_cmp32(state, temp_reg, register_map[0].reg_idx);
    uint32_t jump_loc_0 = state->offset;
    emit_jcc_offset(state, JCC_JNE);
#if RV32_HAS(SYSTEM)
    emit_jmp(state, bt->PC[max_idx], bt->satp[max_idx]);
#else
    emit_jmp(state, bt->PC[max_idx], 0);
#endif
    emit_jump_target_offset(state, JUMP_LOC_0, state->offset);
}

/* 已移除 timer 递增：现在 timer 在中断检查点（rv_check_interrupt）由
 * cycle counter 推导，而不是逐指令维护。这可消除 JIT 热路径上的逐指令内存操作。
 */

#define GEN(inst, code)                                                       \
    static void do_##inst(struct jit_state *state UNUSED, riscv_t *rv UNUSED, \
                          rv_insn_t *ir UNUSED)                               \
    {                                                                         \
        code;                                                                 \
    }
#include "rv32_jit.c"
#undef GEN

static void do_fuse1(struct jit_state *state, riscv_t *rv UNUSED, rv_insn_t *ir)
{
    opcode_fuse_t *fuse = ir->fuse;
    for (int i = 0; i < ir->imm2; i++) {
        vm_reg[0] = map_vm_reg(state, fuse[i].rd);
        emit_load_imm(state, vm_reg[0], fuse[i].imm);
    }
}

static void do_fuse2(struct jit_state *state, riscv_t *rv UNUSED, rv_insn_t *ir)
{
    vm_reg[0] = map_vm_reg(state, ir->rd);
    emit_load_imm(state, vm_reg[0], ir->imm);
    emit_mov(state, vm_reg[0], temp_reg);
    vm_reg[1] = ra_load(state, ir->rs1);
    vm_reg[2] = map_vm_reg_reserved(state, ir->rs2, vm_reg[1]);
    emit_mov(state, vm_reg[1], vm_reg[2]);
    emit_alu32(state, 0x01, temp_reg, vm_reg[2]);
}

static void do_fuse3(struct jit_state *state, riscv_t *rv, rv_insn_t *ir)
{
    memory_t *m = PRIV(rv)->mem;
    opcode_fuse_t *fuse = ir->fuse;
    for (int i = 0; i < ir->imm2; i++) {
        vm_reg[0] = ra_load(state, fuse[i].rs1);
        emit_load_imm_sext(state, temp_reg,
                           (intptr_t) (m->mem_base + fuse[i].imm));
        emit_alu64(state, 0x01, vm_reg[0], temp_reg);
        vm_reg[1] = ra_load(state, fuse[i].rs2);
        emit_store(state, S32, vm_reg[1], temp_reg, 0);
    }
}

static void do_fuse4(struct jit_state *state, riscv_t *rv, rv_insn_t *ir)
{
    memory_t *m = PRIV(rv)->mem;
    opcode_fuse_t *fuse = ir->fuse;
    for (int i = 0; i < ir->imm2; i++) {
        vm_reg[0] = ra_load(state, fuse[i].rs1);
        emit_load_imm_sext(state, temp_reg,
                           (intptr_t) (m->mem_base + fuse[i].imm));
        emit_alu64(state, 0x01, vm_reg[0], temp_reg);
        vm_reg[1] = map_vm_reg(state, fuse[i].rd);
        emit_load(state, S32, temp_reg, vm_reg[1], 0);
    }
}

static void do_fuse5(struct jit_state *state, riscv_t *rv UNUSED, rv_insn_t *ir)
{
    opcode_fuse_t *fuse = ir->fuse;
    for (int i = 0; i < ir->imm2; i++) {
        switch (fuse[i].opcode) {
        case rv_insn_slli:
            vm_reg[0] = ra_load(state, fuse[i].rs1);
            vm_reg[1] = map_vm_reg_reserved(state, fuse[i].rd, vm_reg[0]);
            if (vm_reg[0] != vm_reg[1])
                emit_mov(state, vm_reg[0], vm_reg[1]);
            emit_alu32_imm8(state, 0xc1, 4, vm_reg[1], fuse[i].imm & 0x1f);
            break;
        case rv_insn_srli:
            vm_reg[0] = ra_load(state, fuse[i].rs1);
            vm_reg[1] = map_vm_reg_reserved(state, fuse[i].rd, vm_reg[0]);
            if (vm_reg[0] != vm_reg[1])
                emit_mov(state, vm_reg[0], vm_reg[1]);
            emit_alu32_imm8(state, 0xc1, 5, vm_reg[1], fuse[i].imm & 0x1f);
            break;
        case rv_insn_srai:
            vm_reg[0] = ra_load(state, fuse[i].rs1);
            vm_reg[1] = map_vm_reg_reserved(state, fuse[i].rd, vm_reg[0]);
            if (vm_reg[0] != vm_reg[1])
                emit_mov(state, vm_reg[0], vm_reg[1]);
            emit_alu32_imm8(state, 0xc1, 7, vm_reg[1], fuse[i].imm & 0x1f);
            break;
        default:
            __UNREACHABLE;
            break;
        }
    }
}

/* 融合 LI a7, imm + ECALL。
 * 该融合只适用于标准 RV32I/M/A/F/C；RV32E 使用不同系统调用约定（t0 而非 a7）。
 */
#if !RV32_HAS(RV32E)
static void do_fuse6(struct jit_state *state, riscv_t *rv, rv_insn_t *ir)
{
    /* 设置 a7 = 系统调用号 imm。 */
    vm_reg[0] = map_vm_reg(state, rv_reg_a7);
    emit_load_imm(state, vm_reg[0], ir->imm);
    /* 写回所有寄存器并调用 ecall 处理器。
     * ECALL 位于 ir->pc + 4，即融合对的第二条指令。
     */
    store_back(state);
    emit_load_imm(state, temp_reg, ir->pc + 4);
    emit_store(state, S32, temp_reg, parameter_reg[0], offsetof(riscv_t, PC));
    emit_call(state, (intptr_t) rv->io.on_ecall);
    emit_exit(state);
}
#else
/* RV32E stub：RV32E 下不会生成 fuse6 模式。
 * 防御性回退：如果意外到达，则直接发射 exit。
 */
static void do_fuse6(struct jit_state *state,
                     riscv_t *rv UNUSED,
                     rv_insn_t *ir UNUSED)
{
    assert(!"RV32E 模式不应调用 fuse6");
    emit_exit(state);
}
#endif

/* 融合多条 ADDI。 */
static void do_fuse7(struct jit_state *state, riscv_t *rv UNUSED, rv_insn_t *ir)
{
    opcode_fuse_t *fuse = ir->fuse;
    for (int i = 0; i < ir->imm2; i++) {
        vm_reg[0] = ra_load(state, fuse[i].rs1);
        vm_reg[1] = map_vm_reg_reserved(state, fuse[i].rd, vm_reg[0]);
        if (vm_reg[0] != vm_reg[1])
            emit_mov(state, vm_reg[0], vm_reg[1]);
        emit_alu32_imm32(state, 0x81, 0, vm_reg[1], fuse[i].imm);
    }
}

/* 融合 LUI + ADDI：加载 32 位常量（li 伪指令）。
 * rd = (lui_imm << 12) + addi_imm = ir->imm + ir->imm2。
 */
static void do_fuse8(struct jit_state *state, riscv_t *rv UNUSED, rv_insn_t *ir)
{
    vm_reg[0] = map_vm_reg(state, ir->rd);
    /* 在 JIT 编译期计算合并立即数。转为 uint32_t 可避免有符号溢出 UB。
     */
    uint32_t combined_imm = (uint32_t) ir->imm + (uint32_t) ir->imm2;
    emit_load_imm(state, vm_reg[0], combined_imm);
}

/* 融合 LUI + LW：绝对地址加载。
 * addr = ir->imm（lui << 12）+ ir->imm2（lw 偏移）。
 * ir->rs2 是加载目的寄存器。
 */
static void do_fuse9(struct jit_state *state, riscv_t *rv, rv_insn_t *ir)
{
    memory_t *m = PRIV(rv)->mem;
    uint32_t addr = (uint32_t) ir->imm + (uint32_t) ir->imm2;
#if RV32_HAS(SYSTEM_MMIO)
    /* 写入 LUI 结果到 rd；当 rd != LW 目的寄存器时这是必须的。
     * LUI 在 LW 前完成，因此即使 LW fault，该写入也应发生。
     */
    vm_reg[0] = map_vm_reg(state, ir->rd);
    emit_load_imm(state, vm_reg[0], ir->imm);

    /* 保存虚拟地址和访问类型，供 MMU 转换使用。 */
    emit_load_imm(state, temp_reg, addr);
    emit_store(state, S32, temp_reg, parameter_reg[0],
               offsetof(riscv_t, jit_mmu.vaddr));
    emit_load_imm(state, temp_reg, rv_insn_lw);
    emit_store(state, S32, temp_reg, parameter_reg[0],
               offsetof(riscv_t, jit_mmu.type));
    /* 保存指令 PC，作为 trap 返回地址。 */
    emit_load_imm(state, temp_reg, ir->pc);
    emit_store(state, S32, temp_reg, parameter_reg[0],
               offsetof(riscv_t, jit_mmu.pc));

    store_back(state);
    emit_jit_mmu_handler(state, ir->rs2);
    reset_reg();

    /* 检查 MMU 转换期间是否发生 trap。若已 trap，则完全跳过加载，避免读入无效数据。
     */
    emit_load(state, S8, parameter_reg[0], temp_reg,
              offsetof(riscv_t, is_trapped));
    emit_cmp_imm32(state, temp_reg, 0);
    uint32_t jump_trap = state->offset;
    emit_jcc_offset(state, JCC_JNE); /* 已 trap 时跳到末尾。 */

    /* 若为 MMIO，值已在 X[rd]；否则从转换后的 paddr 加载。 */
    emit_load(state, S8, parameter_reg[0], temp_reg,
              offsetof(riscv_t, jit_mmu.is_mmio));
    emit_cmp_imm32(state, temp_reg, 0);
    vm_reg[0] = map_vm_reg(state, ir->rs2);
    uint32_t jump_loc_0 = state->offset;
    emit_jcc_offset(state, JCC_JE);

    /* MMIO 路径：从 X[rd] 加载。 */
    emit_load(state, S32, parameter_reg[0], vm_reg[0],
              offsetof(riscv_t, X) + 4 * ir->rs2);
    uint32_t jump_loc_1 = state->offset;
    emit_jcc_offset(state, JCC_JMP);

    /* RAM 路径：从 mem_base + paddr 加载。
     * 复用已映射到 ir->rs2 的 vm_reg[0] 做地址计算，然后加载到同一寄存器，与
     * GEN_LOAD 模式保持一致。
     */
    emit_jump_target_offset(state, JUMP_LOC_0, state->offset);
    emit_load(state, S32, parameter_reg[0], temp_reg,
              offsetof(riscv_t, jit_mmu.paddr));
    emit_load_imm_sext(state, vm_reg[0], (intptr_t) m->mem_base);
    emit_alu64(state, ALU_OP_ADD, temp_reg, vm_reg[0]);
    emit_load(state, S32, vm_reg[0], vm_reg[0], 0);
    emit_jump_target_offset(state, JUMP_LOC_1, state->offset);
    /* 跳过 trap exit，继续正常执行。 */
    uint32_t jump_normal = state->offset;
    emit_jcc_offset(state, JCC_JMP);
    /* trap 退出点：离开 JIT 基本块，交给 trap 处理。 */
    emit_jump_target_offset(state, JUMP_TRAP, state->offset);
    emit_exit(state);
    /* 正常继续执行点。 */
    emit_jump_target_offset(state, JUMP_NORMAL, state->offset);
#else
    /* 写入 LUI 结果到 rd；当 rd != LW 目的寄存器时这是必须的。 */
    vm_reg[0] = map_vm_reg(state, ir->rd);
    emit_load_imm(state, vm_reg[0], ir->imm);
    emit_load_imm_sext(state, temp_reg, (intptr_t) (m->mem_base + addr));
    vm_reg[1] = map_vm_reg(state, ir->rs2);
    emit_load(state, S32, temp_reg, vm_reg[1], 0);
#endif
}

/* 融合 LUI + SW：绝对地址存储。
 * addr = ir->imm（lui << 12）+ ir->imm2（sw 偏移）。
 * ir->rs1 是存储源寄存器。
 */
static void do_fuse10(struct jit_state *state, riscv_t *rv, rv_insn_t *ir)
{
    memory_t *m = PRIV(rv)->mem;
    uint32_t addr = (uint32_t) ir->imm + (uint32_t) ir->imm2;
#if RV32_HAS(SYSTEM_MMIO)
    /* 写入 LUI 结果到 rd。SW 不写寄存器，因此 rd 之后可能仍会被使用；LUI 在 SW
     * 前完成，所以即使 SW fault，该写入也应发生。
     */
    vm_reg[0] = map_vm_reg(state, ir->rd);
    emit_load_imm(state, vm_reg[0], ir->imm);

    /* 保存虚拟地址和访问类型，供 MMU 转换使用。 */
    emit_load_imm(state, temp_reg, addr);
    emit_store(state, S32, temp_reg, parameter_reg[0],
               offsetof(riscv_t, jit_mmu.vaddr));
    emit_load_imm(state, temp_reg, rv_insn_sw);
    emit_store(state, S32, temp_reg, parameter_reg[0],
               offsetof(riscv_t, jit_mmu.type));
    /* 保存指令 PC，作为 trap 返回地址。 */
    emit_load_imm(state, temp_reg, ir->pc);
    emit_store(state, S32, temp_reg, parameter_reg[0],
               offsetof(riscv_t, jit_mmu.pc));
    store_back(state);
    emit_jit_mmu_handler(state, ir->rs1);
    reset_reg();

    /* 检查是否发生 trap；已 trap 时跳过存储。 */
    emit_load(state, S8, parameter_reg[0], temp_reg,
              offsetof(riscv_t, is_trapped));
    emit_cmp_imm32(state, temp_reg, 0);
    uint32_t jump_trap = state->offset;
    emit_jcc_offset(state, JCC_JNE); /* 已 trap 时跳到末尾。 */

    /* 若为 MMIO，跳过这里的 store，已由 MMU handler 处理。 */
    emit_load(state, S8, parameter_reg[0], temp_reg,
              offsetof(riscv_t, jit_mmu.is_mmio));
    emit_cmp_imm32(state, temp_reg, 1);
    uint32_t jump_loc_0 = state->offset;
    emit_jcc_offset(state, JCC_JE);

    /* RAM 路径：存储到 mem_base + paddr。
     * SW 不写寄存器，因此 rd 可用作 scratch。注意不能使用 rv_reg_zero 作为 scratch，
     * 因为 emit_load 对映射到 x0 的目标寄存器有特殊处理，会直接返回 0。
     */
    emit_load(state, S32, parameter_reg[0], temp_reg,
              offsetof(riscv_t, jit_mmu.paddr));
    vm_reg[0] = map_vm_reg(state, ir->rd);
    emit_load_imm_sext(state, vm_reg[0], (intptr_t) m->mem_base);
    emit_alu64(state, ALU_OP_ADD, temp_reg, vm_reg[0]);
    vm_reg[1] = ra_load(state, ir->rs1);
    emit_store(state, S32, vm_reg[1], vm_reg[0], 0);
    emit_jump_target_offset(state, JUMP_LOC_0, state->offset);
    /* 跳过 trap exit，继续正常执行。 */
    uint32_t jump_normal = state->offset;
    emit_jcc_offset(state, JCC_JMP);
    /* trap 退出点：离开 JIT 基本块，交给 trap 处理。 */
    emit_jump_target_offset(state, JUMP_TRAP, state->offset);
    emit_exit(state);
    /* 正常继续执行点。 */
    emit_jump_target_offset(state, JUMP_NORMAL, state->offset);
    reset_reg();
#else
    /* 写入 LUI 结果到 rd；SW 不写寄存器，因此 rd 之后可能仍会使用。 */
    vm_reg[0] = map_vm_reg(state, ir->rd);
    emit_load_imm(state, vm_reg[0], ir->imm);
    vm_reg[1] = ra_load(state, ir->rs1);
    emit_load_imm_sext(state, temp_reg, (intptr_t) (m->mem_base + addr));
    emit_store(state, S32, vm_reg[1], temp_reg, 0);
#endif
}

/* 融合 LW + ADDI（后递增加载）。
 * addr = rv->X[ir->rs1] + ir->imm。
 * ir->rd 是加载目的寄存器。
 * ir->rs1 += ir->imm2（递增）。
 */
static void do_fuse11(struct jit_state *state, riscv_t *rv, rv_insn_t *ir)
{
    memory_t *m = PRIV(rv)->mem;
#if RV32_HAS(SYSTEM_MMIO)
    /* 计算虚拟地址：rs1 + imm。 */
    vm_reg[0] = ra_load(state, ir->rs1);
    emit_load_imm_sext(state, temp_reg, ir->imm);
    emit_alu32(state, ALU_OP_ADD, vm_reg[0], temp_reg);
    emit_store(state, S32, temp_reg, parameter_reg[0],
               offsetof(riscv_t, jit_mmu.vaddr));
    emit_load_imm(state, temp_reg, rv_insn_lw);
    emit_store(state, S32, temp_reg, parameter_reg[0],
               offsetof(riscv_t, jit_mmu.type));
    /* 保存指令 PC，作为 trap 返回地址。 */
    emit_load_imm(state, temp_reg, ir->pc);
    emit_store(state, S32, temp_reg, parameter_reg[0],
               offsetof(riscv_t, jit_mmu.pc));

    store_back(state);
    emit_jit_mmu_handler(state, ir->rd);
    reset_reg();

    /* 检查 MMU 转换期间是否发生 trap。若已 trap，则完全跳过加载和后递增。
     * mem_translate fault 时，jit_mmu_handler 会设置 is_trapped。
     */
    emit_load(state, S8, parameter_reg[0], temp_reg,
              offsetof(riscv_t, is_trapped));
    emit_cmp_imm32(state, temp_reg, 0);
    uint32_t jump_trap = state->offset;
    emit_jcc_offset(state, JCC_JNE); /* 已 trap 时跳到末尾。 */

    /* 若为 MMIO，值已在 X[rd]；否则从转换后的 paddr 加载。 */
    emit_load(state, S8, parameter_reg[0], temp_reg,
              offsetof(riscv_t, jit_mmu.is_mmio));
    emit_cmp_imm32(state, temp_reg, 0);
    vm_reg[0] = map_vm_reg(state, ir->rd);
    uint32_t jump_loc_0 = state->offset;
    emit_jcc_offset(state, JCC_JE);

    /* MMIO 路径：从 X[rd] 加载。 */
    emit_load(state, S32, parameter_reg[0], vm_reg[0],
              offsetof(riscv_t, X) + 4 * ir->rd);
    uint32_t jump_loc_1 = state->offset;
    emit_jcc_offset(state, JCC_JMP);

    /* RAM 路径：从 mem_base + paddr 加载。
     * 复用已映射到 ir->rd 的 vm_reg[0] 做地址计算，然后加载到同一寄存器，与
     * GEN_LOAD 模式保持一致。
     */
    emit_jump_target_offset(state, JUMP_LOC_0, state->offset);
    emit_load(state, S32, parameter_reg[0], temp_reg,
              offsetof(riscv_t, jit_mmu.paddr));
    emit_load_imm_sext(state, vm_reg[0], (intptr_t) m->mem_base);
    emit_alu64(state, ALU_OP_ADD, temp_reg, vm_reg[0]);
    emit_load(state, S32, vm_reg[0], vm_reg[0], 0);
    emit_jump_target_offset(state, JUMP_LOC_1, state->offset);

    /* rs1 按 imm2 后递增，仅在没有 trap 时执行。
     * store_back() 后调用了 reset_reg()，因此必须用 ra_load 从内存重新加载 rs1；
     * 否则会递增无效寄存器内容。
     */
    vm_reg[0] = ra_load(state, ir->rs1);
    emit_alu32_imm32(state, 0x81, 0, vm_reg[0], ir->imm2);
    /* 跳过 trap exit，继续正常执行。 */
    uint32_t jump_normal = state->offset;
    emit_jcc_offset(state, JCC_JMP);
    /* trap 退出点：离开 JIT 基本块，交给 trap 处理。 */
    emit_jump_target_offset(state, JUMP_TRAP, state->offset);
    emit_exit(state);
    /* 正常继续执行点。 */
    emit_jump_target_offset(state, JUMP_NORMAL, state->offset);
#else
    vm_reg[0] = ra_load(state, ir->rs1);
    /* 计算地址：mem_base + rs1 + imm。 */
    emit_load_imm_sext(state, temp_reg, (intptr_t) (m->mem_base + ir->imm));
    emit_alu64(state, 0x01, vm_reg[0], temp_reg);
    /* 加载值到 rd。 */
    vm_reg[1] = map_vm_reg(state, ir->rd);
    emit_load(state, S32, temp_reg, vm_reg[1], 0);
    /* rs1 按 imm2 递增。 */
    vm_reg[0] = map_vm_reg(state, ir->rs1);
    emit_alu32_imm32(state, 0x81, 0, vm_reg[0], ir->imm2);
    set_dirty(vm_reg[0], true); /* 标记 rs1 为 dirty，确保写回内存。 */
#endif
}

/* 融合 ADDI + BNE（循环计数递减并分支）。
 * rd = rs1 + imm。
 * 如果 rd != 0，则跳转到 PC + 4 + imm2。
 * 这是分支指令，必须写回寄存器并退出当前 JIT 块。
 */
static void do_fuse12(struct jit_state *state, riscv_t *rv, rv_insn_t *ir)
{
    /* 计算 rd = rs1 + imm。 */
    vm_reg[0] = ra_load(state, ir->rs1);
    vm_reg[1] = map_vm_reg_reserved(state, ir->rd, vm_reg[0]);
    if (vm_reg[0] != vm_reg[1])
        emit_mov(state, vm_reg[0], vm_reg[1]);
    emit_alu32_imm32(state, 0x81, 0, vm_reg[1], ir->imm);
    /* 比较 rd 与 0，决定是否分支。 */
    emit_cmp_imm32(state, vm_reg[1], 0);
    store_back(state);
    /* JNE 跳转到 taken 路径：0x85 = JNE。 */
    uint32_t jump_loc_0 = state->offset;
    emit_jcc_offset(state, 0x85);
    /* untaken 路径：rd == 0，落到 PC + 8。 */
    if (ir->branch_untaken) {
        emit_jmp(state, ir->pc + 8, rv->csr_satp);
    }
    emit_load_imm(state, temp_reg, ir->pc + 8);
    emit_store(state, S32, temp_reg, parameter_reg[0], offsetof(riscv_t, PC));
    emit_exit(state);
    /* taken 路径：rd != 0，跳转到 PC + 4 + imm2。 */
    emit_jump_target_offset(state, JUMP_LOC_0, state->offset);
    if (ir->branch_taken) {
        emit_jmp(state, ir->pc + 4 + ir->imm2, rv->csr_satp);
    }
    emit_load_imm(state, temp_reg, ir->pc + 4 + ir->imm2);
    emit_store(state, S32, temp_reg, parameter_reg[0], offsetof(riscv_t, PC));
    emit_exit(state);
}

/* clang-format off */
static const void *dispatch_table[] = {
    /* RV32 指令。 */
#define _(inst, can_branch, insn_len, translatable, reg_mask) [rv_insn_##inst] = do_##inst,
    RV_INSN_LIST
#undef _
    /* 宏操作融合指令。 */
#define _(inst) [rv_insn_##inst] = do_##inst,
    FUSE_INSN_LIST
#undef _
};
/* clang-format on */

void clear_hot(block_t *block)
{
    block->hot = false;
}

static void code_cache_flush(struct jit_state *state, riscv_t *rv)
{
    should_flush = false;
    state->offset = state->org_size;
    state->n_blocks = 0;
    set_reset(&state->set);
    clear_cache_hot(rv->block_cache, (clear_func_t) clear_hot);
#if RV32_HAS(T2C)
    jit_cache_clear(rv->jit_cache);
    inline_cache_clear(rv->inline_cache);
#endif
    return;
}

typedef void (*codegen_block_func_t)(struct jit_state *,
                                     riscv_t *,
                                     rv_insn_t *);

static void translate(struct jit_state *state, riscv_t *rv, block_t *block)
{
    uint32_t idx;
    rv_insn_t *ir, *next;
    reset_reg();
    liveness_reset();
    liveness_calc(block);
    for (idx = 0, ir = block->ir_head; idx < block->n_insn && !should_flush;
         idx++, ir = next) {
        next = ir->next;
        regs_refresh(idx);
        ((codegen_block_func_t) dispatch_table[ir->opcode])(state, rv, ir);
    }

#if RV32_HAS(BLOCK_CHAINING)
    /* 页边界终止基本块的 fallthrough：发射跳转到下一块或退出。不同于分支终止块，
     * 页边界终止块总是落到下一个顺序地址 pc_end。
     */
    if (block->page_terminated && !should_flush) {
        ir = block->ir_tail;
        store_back(state);
        if (ir->branch_taken) {
            /* fallthrough 链接已建立，跳转到下一基本块。 */
            emit_jmp(state, block->pc_end, rv->csr_satp);
        }
        /* 未链接路径保存 PC 并退出。 */
        emit_load_imm(state, temp_reg, block->pc_end);
        emit_store(state, S32, temp_reg, parameter_reg[0],
                   offsetof(riscv_t, PC));
        emit_exit(state);
    }
#endif
}

static void resolve_jumps(struct jit_state *state)
{
    if (state->n_jumps == 0)
        return;

#if defined(__APPLE__) && defined(__aarch64__)
    /* 翻译期间写模式由 jit_translate 维护。 */
#endif

    for (int i = 0; i < state->n_jumps; i++) {
        struct jump jump = state->jumps[i];
        int target_loc;
        if (jump.target_offset != 0)
            target_loc = jump.target_offset;
        else if (jump.target_pc == TARGET_PC_EXIT)
            target_loc = state->exit_loc;
#if defined(__x86_64__)
        else if (jump.target_pc == TARGET_PC_RETPOLINE)
            target_loc = state->retpoline_loc;
#elif defined(__aarch64__)
        else if (jump.target_pc == TARGET_PC_ENTER)
            target_loc = state->entry_loc;
#endif
        else {
            target_loc = jump.offset_loc + sizeof(uint32_t);
            for (int j = 0; j < state->n_blocks; j++) {
                if (jump.target_pc == state->offset_map[j].pc) {
                    IIF(RV32_HAS(SYSTEM))(
                        if (jump.target_satp == state->offset_map[j].satp), )
                    {
                        target_loc = state->offset_map[j].offset;
                        break;
                    }
                }
            }
        }
#if defined(__x86_64__)
        /* 假设跳转偏移位于指令末尾。 */
        uint32_t rel = target_loc - (jump.offset_loc + sizeof(uint32_t));

        uint8_t *offset_ptr = &state->buf[jump.offset_loc];
        memcpy(offset_ptr, &rel, sizeof(uint32_t));
#elif defined(__aarch64__)
        int32_t rel = target_loc - jump.offset_loc;
        patch_branch_imm(state, jump.offset_loc, rel);
#endif
    }
}

static void translate_chained_block(struct jit_state *state,
                                    riscv_t *rv,
                                    block_t *block)
{
    if (set_has(&state->set, RV_HASH_KEY(block)))
        return;

    if (state->n_blocks == MAX_BLOCKS)
        return;

    assert(set_add(&state->set, RV_HASH_KEY(block)));
    offset_map_insert(state, block);
    translate(state, rv, block);
    if (unlikely(should_flush))
        return;
    rv_insn_t *ir = block->ir_tail;
    if (ir->branch_untaken && !set_has(&state->set, ir->branch_untaken->pc)) {
        block_t *block1 =
            cache_get(rv->block_cache, ir->branch_untaken->pc, false);
        if (block1->translatable) {
            IIF(RV32_HAS(SYSTEM))(
                if (block1->satp == rv->csr_satp && !block1->invalidated), )
                translate_chained_block(state, rv, block1);
        }
    }
    if (ir->branch_taken && !set_has(&state->set, ir->branch_taken->pc)) {
        block_t *block1 =
            cache_get(rv->block_cache, ir->branch_taken->pc, false);
        if (block1->translatable) {
            IIF(RV32_HAS(SYSTEM))(
                if (block1->satp == rv->csr_satp && !block1->invalidated), )
                translate_chained_block(state, rv, block1);
        }
    }

    branch_history_table_t *bt = ir->branch_table;
    if (bt) {
        int max_idx = bht_find_max_idx(bt);
#if RV32_HAS(SYSTEM)
        if (bht_should_translate(bt, max_idx, rv->csr_satp) &&
            !set_has(&state->set, bt->PC[max_idx])) {
#else
        if (bht_should_translate(bt, max_idx) &&
            !set_has(&state->set, bt->PC[max_idx])) {
#endif
            block_t *block1 =
                cache_get(rv->block_cache, bt->PC[max_idx], false);
            if (block1 && block1->translatable) {
                IIF(RV32_HAS(SYSTEM))(
                    if (block1->satp == rv->csr_satp && !block1->invalidated), )
                    translate_chained_block(state, rv, block1);
            }
        }
    }
}

void jit_translate(riscv_t *rv, block_t *block)
{
    struct jit_state *state = rv->jit_state;
    if (set_has(&state->set, RV_HASH_KEY(block))) {
        /* 基本块已翻译，直接复用。 */
        for (int i = 0; i < state->n_blocks; i++) {
            if (block->pc_start == state->offset_map[i].pc
#if RV32_HAS(SYSTEM)
                && block->satp == state->offset_map[i].satp
#endif
            ) {
                block->offset = state->offset_map[i].offset;
                block->hot = true;
                return;
            }
        }
        assert(NULL);
        __UNREACHABLE;
    }
restart:
    memset(state->jumps, 0, MAX_JUMPS * sizeof(struct jump));
    state->n_jumps = 0;
    block->offset = state->offset;
#if defined(__APPLE__) && defined(__aarch64__)
    /* 整个翻译阶段进入写模式。
     * 将所有写保护切换合并为一次操作，避免快速切换造成潜在缓存一致性问题。
     */
    jit_enter_write_mode();
#endif
    translate_chained_block(state, rv, block);
    if (unlikely(should_flush)) {
#if defined(__APPLE__) && defined(__aarch64__)
        jit_exit_write_mode();
#endif
        code_cache_flush(state, rv);
        goto restart;
    }
    resolve_jumps(state);
#if defined(__aarch64__)
    /* 修补分支立即数后的缓存维护。
     * Apple 上 sys_icache_invalidate 执行 DC CVAU + DSB + IC IVAU + DSB + ISB；
     * Linux 上 __builtin___clear_cache 执行类似维护。
     */
#if defined(__APPLE__)
    __asm__ volatile("dmb ish" ::: "memory");
#endif
    sys_icache_invalidate(state->buf + block->offset,
                          state->offset - block->offset);
#if defined(__APPLE__)
    /* 退出写模式，页面恢复可执行。 */
    jit_exit_write_mode();
#endif
    /* 完整屏障序列，确保指令一致性。 */
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
#endif
    block->hot = true;
}

struct jit_state *jit_state_init(size_t size)
{
    struct jit_state *state = malloc(sizeof(struct jit_state));
    if (!state)
        return NULL;
    assert(state);

    state->offset = 0;
    state->size = size;
    state->buf = mmap(0, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS
#if defined(__APPLE__)
                          | MAP_JIT
#endif
                      ,
                      -1, 0);
    if (state->buf == MAP_FAILED) {
        free(state);
        return NULL;
    }
    assert(state->buf != MAP_FAILED);

    state->n_blocks = 0;
    set_reset(&state->set);
    reset_reg();
    prepare_translate(state);
#if defined(__APPLE__) && defined(__aarch64__)
    /* 对 prologue/epilogue 代码做最后一次 cache flush。
     * emit_bytes 已处理逐指令 cache 维护，但最终 flush 可确保整个区域一致。
     */
    __builtin___clear_cache((char *) state->buf,
                            (char *) (state->buf + state->offset));
#endif

    state->offset_map = calloc(MAX_BLOCKS, sizeof(struct offset_map));
    if (!state->offset_map) {
        munmap(state->buf, state->size);
        free(state);
        return NULL;
    }

    state->jumps = calloc(MAX_JUMPS, sizeof(struct jump));
    if (!state->jumps) {
        free(state->offset_map);
        munmap(state->buf, state->size);
        free(state);
        return NULL;
    }

    return state;
}

void jit_state_exit(struct jit_state *state)
{
    munmap(state->buf, state->size);
    free(state->offset_map);
    free(state->jumps);
    free(state);
}
