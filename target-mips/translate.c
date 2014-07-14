/*
 *  MIPS32 emulation for qemu: main translation routines.
 *
 *  Copyright (c) 2004-2005 Jocelyn Mayer
 *  Copyright (c) 2006 Marius Groeger (FPU operations)
 *  Copyright (c) 2006 Thiemo Seufer (MIPS32R2 support)
 *  Copyright (c) 2009 CodeSourcery (MIPS16 and microMIPS support)
 *  Copyright (c) 2012 Jia Liu & Dongxue Zhang (MIPS ASE DSP support)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "cpu.h"
#include "disas/disas.h"
#include "tcg-op.h"

#include "helper.h"
#define GEN_HELPER 1
#include "helper.h"

//#define DISABLE_CHAINING_BRANCH
//#define DISABLE_CHAINING_JAL

#define MIPS_DEBUG_DISAS 0

static int csr_regno(int regno);

/* Get regno in our csr reg array from actual csr regno
 * Mapping:
 * [
 *  pos_in_array: regname (real reg number, input as int regno)
 *    0: sup0 (0x500),
 *    1: sup1 (0x501),
 *    2: epc (0x502),
 *    3: badvaddr (0x503),
 *    4: ptbr (0x504),
 *    5: asid (0x505),
 *    6: count (0x506),
 *    7: compare (0x507),
 *    8: evec (0x508),
 *    9: cause (0x509),
 *    A: status (0x50A),
 *    B: hartid (0x50B),
 *    C: impl (0x50C),
 *    D: fatc (0x50D),
 *    E: send_ipi (0x50E),
 *    F: clear_ipi (0x50F),
 *   10: cycle (0xC00),
 *   11: time (0xC01),
 *   12: instret (0xC02),
 *   13: fflags (0x1),
 *   14: frm (0x2),
 *   15: fcsr (0x3),
 *   ...
 *   1E: tohost (0x51E),
 *   1F: fromhost (0x51F)
 * ]
 */
static int csr_regno(int regno)
{
    if (regno < 0x4 && regno > 0x0) { //0x1, 0x2, 0x3
        return regno + 0x12;
    }
    if (regno < 0xC00) {
        // 0x5xx registers
        return 0xFF & regno;
    }
    // 0xC0x registers
    return (0xF & regno) + 0x10;
}


#define MASK_OP_MAJOR(op)  (op & 0x7F)
enum {
    /* rv32i, rv64i, rv32m */
    OPC_RISC_LUI    = (0x37),
    OPC_RISC_AUIPC  = (0x17),
    OPC_RISC_JAL    = (0x6F),
    OPC_RISC_JALR   = (0x67), // TODO
    OPC_RISC_BRANCH = (0x63),
    OPC_RISC_LOAD   = (0x03),
    OPC_RISC_STORE  = (0x23),
    OPC_RISC_ARITH_IMM  = (0x13),
    OPC_RISC_ARITH      = (0x33),
    OPC_RISC_FENCE      = (0x0F),
    OPC_RISC_SYSTEM     = (0x73), // TODO

    /* rv64i, rv64m */
    OPC_RISC_ARITH_IMM_W = (0x1B),
    OPC_RISC_ARITH_W = (0x3B),

    /* rv32a, rv64a */
    OPC_RISC_ATOMIC = (0x2F),

    /* floating point */
    OPC_RISC_FP_LOAD = (0x7),
    OPC_RISC_FP_STORE = (0x27),
    
    OPC_RISC_FMADD = (0x43),
    OPC_RISC_FMSUB = (0x47),
    OPC_RISC_FNMSUB = (0x4B),
    OPC_RISC_FNMADD = (0x4F),

    OPC_RISC_FP_ARITH = (0x53),
    // FP system instructions are under the normal system opcode

};

#define MASK_OP_ARITH(op)   (MASK_OP_MAJOR(op) | (op & ((0x7 << 12) | (0x7F << 25))))
enum {
    OPC_RISC_ADD   = OPC_RISC_ARITH | (0x0 << 12) | (0x00 << 25),
    OPC_RISC_SUB   = OPC_RISC_ARITH | (0x0 << 12) | (0x20 << 25),
    OPC_RISC_SLL   = OPC_RISC_ARITH | (0x1 << 12) | (0x00 << 25),
    OPC_RISC_SLT   = OPC_RISC_ARITH | (0x2 << 12) | (0x00 << 25),
    OPC_RISC_SLTU  = OPC_RISC_ARITH | (0x3 << 12) | (0x00 << 25),
    OPC_RISC_XOR   = OPC_RISC_ARITH | (0x4 << 12) | (0x00 << 25),
    OPC_RISC_SRL   = OPC_RISC_ARITH | (0x5 << 12) | (0x00 << 25), 
    OPC_RISC_SRA   = OPC_RISC_ARITH | (0x5 << 12) | (0x20 << 25),
    OPC_RISC_OR    = OPC_RISC_ARITH | (0x6 << 12) | (0x00 << 25),
    OPC_RISC_AND   = OPC_RISC_ARITH | (0x7 << 12) | (0x00 << 25),

    /* RV64M */
    OPC_RISC_MUL    = OPC_RISC_ARITH | (0x0 << 12) | (0x01 << 25),
    OPC_RISC_MULH   = OPC_RISC_ARITH | (0x1 << 12) | (0x01 << 25),
    OPC_RISC_MULHSU = OPC_RISC_ARITH | (0x2 << 12) | (0x01 << 25),
    OPC_RISC_MULHU  = OPC_RISC_ARITH | (0x3 << 12) | (0x01 << 25),

    OPC_RISC_DIV    = OPC_RISC_ARITH | (0x4 << 12) | (0x01 << 25),
    OPC_RISC_DIVU   = OPC_RISC_ARITH | (0x5 << 12) | (0x01 << 25),
    OPC_RISC_REM    = OPC_RISC_ARITH | (0x6 << 12) | (0x01 << 25),
    OPC_RISC_REMU   = OPC_RISC_ARITH | (0x7 << 12) | (0x01 << 25),
};


#define MASK_OP_ARITH_IMM(op)   (MASK_OP_MAJOR(op) | (op & (0x7 << 12)))
enum {
    OPC_RISC_ADDI   = OPC_RISC_ARITH_IMM | (0x0 << 12),
    OPC_RISC_SLTI   = OPC_RISC_ARITH_IMM | (0x2 << 12),
    OPC_RISC_SLTIU  = OPC_RISC_ARITH_IMM | (0x3 << 12),
    OPC_RISC_XORI   = OPC_RISC_ARITH_IMM | (0x4 << 12),
    OPC_RISC_ORI    = OPC_RISC_ARITH_IMM | (0x6 << 12),
    OPC_RISC_ANDI   = OPC_RISC_ARITH_IMM | (0x7 << 12),
    OPC_RISC_SLLI   = OPC_RISC_ARITH_IMM | (0x1 << 12), // additional part of IMM
    OPC_RISC_SHIFT_RIGHT_I = OPC_RISC_ARITH_IMM | (0x5 << 12) // SRAI, SRLI
};

#define MASK_OP_BRANCH(op)     (MASK_OP_MAJOR(op) | (op & (0x7 << 12)))
enum {
    OPC_RISC_BEQ  = OPC_RISC_BRANCH  | (0x0  << 12),
    OPC_RISC_BNE  = OPC_RISC_BRANCH  | (0x1  << 12),
    OPC_RISC_BLT  = OPC_RISC_BRANCH  | (0x4  << 12),
    OPC_RISC_BGE  = OPC_RISC_BRANCH  | (0x5  << 12),
    OPC_RISC_BLTU = OPC_RISC_BRANCH  | (0x6  << 12),
    OPC_RISC_BGEU = OPC_RISC_BRANCH  | (0x7  << 12)
};

#define MASK_OP_ARITH_IMM_W(op)   (MASK_OP_MAJOR(op) | (op & (0x7 << 12)))
enum {
    OPC_RISC_ADDIW   = OPC_RISC_ARITH_IMM_W | (0x0 << 12),
    OPC_RISC_SLLIW   = OPC_RISC_ARITH_IMM_W | (0x1 << 12), // additional part of IMM
    OPC_RISC_SHIFT_RIGHT_IW = OPC_RISC_ARITH_IMM_W | (0x5 << 12) // SRAI, SRLI
};

#define MASK_OP_ARITH_W(op)   (MASK_OP_MAJOR(op) | (op & ((0x7 << 12) | (0x7F << 25))))
enum {
    OPC_RISC_ADDW   = OPC_RISC_ARITH_W | (0x0 << 12) | (0x00 << 25),
    OPC_RISC_SUBW   = OPC_RISC_ARITH_W | (0x0 << 12) | (0x20 << 25),
    OPC_RISC_SLLW   = OPC_RISC_ARITH_W | (0x1 << 12) | (0x00 << 25),
    OPC_RISC_SRLW   = OPC_RISC_ARITH_W | (0x5 << 12) | (0x00 << 25), 
    OPC_RISC_SRAW   = OPC_RISC_ARITH_W | (0x5 << 12) | (0x20 << 25),

    /* RV64M */
    OPC_RISC_MULW   = OPC_RISC_ARITH_W | (0x0 << 12) | (0x01 << 25),
    OPC_RISC_DIVW   = OPC_RISC_ARITH_W | (0x4 << 12) | (0x01 << 25),
    OPC_RISC_DIVUW  = OPC_RISC_ARITH_W | (0x5 << 12) | (0x01 << 25),
    OPC_RISC_REMW   = OPC_RISC_ARITH_W | (0x6 << 12) | (0x01 << 25),
    OPC_RISC_REMUW  = OPC_RISC_ARITH_W | (0x7 << 12) | (0x01 << 25),
};

#define MASK_OP_LOAD(op)   (MASK_OP_MAJOR(op) | (op & (0x7 << 12)))
enum {
    OPC_RISC_LB   = OPC_RISC_LOAD | (0x0 << 12),
    OPC_RISC_LH   = OPC_RISC_LOAD | (0x1 << 12),
    OPC_RISC_LW   = OPC_RISC_LOAD | (0x2 << 12),
    OPC_RISC_LD   = OPC_RISC_LOAD | (0x3 << 12),
    OPC_RISC_LBU  = OPC_RISC_LOAD | (0x4 << 12),
    OPC_RISC_LHU  = OPC_RISC_LOAD | (0x5 << 12),
    OPC_RISC_LWU  = OPC_RISC_LOAD | (0x6 << 12),
};

#define MASK_OP_STORE(op)   (MASK_OP_MAJOR(op) | (op & (0x7 << 12)))
enum {
    OPC_RISC_SB   = OPC_RISC_STORE | (0x0 << 12),
    OPC_RISC_SH   = OPC_RISC_STORE | (0x1 << 12),
    OPC_RISC_SW   = OPC_RISC_STORE | (0x2 << 12),
    OPC_RISC_SD   = OPC_RISC_STORE | (0x3 << 12),
};

#define MASK_OP_JALR(op)   (MASK_OP_MAJOR(op) | (op & (0x7 << 12)))
// no enum since OPC_RISC_JALR is the actual value

#define MASK_OP_ATOMIC(op)   (MASK_OP_MAJOR(op) | (op & ((0x7 << 12) | (0x7F << 25))))
#define MASK_OP_ATOMIC_NO_AQ_RL(op)   (MASK_OP_MAJOR(op) | (op & ((0x7 << 12) | (0x1F << 27))))
enum {
    OPC_RISC_LR_W        = OPC_RISC_ATOMIC | (0x2 << 12) | (0x02 << 27),
    OPC_RISC_SC_W        = OPC_RISC_ATOMIC | (0x2 << 12) | (0x03 << 27),
    OPC_RISC_AMOSWAP_W   = OPC_RISC_ATOMIC | (0x2 << 12) | (0x01 << 27),
    OPC_RISC_AMOADD_W    = OPC_RISC_ATOMIC | (0x2 << 12) | (0x00 << 27),
    OPC_RISC_AMOXOR_W    = OPC_RISC_ATOMIC | (0x2 << 12) | (0x04 << 27),
    OPC_RISC_AMOAND_W    = OPC_RISC_ATOMIC | (0x2 << 12) | (0x0C << 27),
    OPC_RISC_AMOOR_W     = OPC_RISC_ATOMIC | (0x2 << 12) | (0x08 << 27),
    OPC_RISC_AMOMIN_W    = OPC_RISC_ATOMIC | (0x2 << 12) | (0x10 << 27),
    OPC_RISC_AMOMAX_W    = OPC_RISC_ATOMIC | (0x2 << 12) | (0x14 << 27),
    OPC_RISC_AMOMINU_W   = OPC_RISC_ATOMIC | (0x2 << 12) | (0x18 << 27),
    OPC_RISC_AMOMAXU_W   = OPC_RISC_ATOMIC | (0x2 << 12) | (0x1C << 27),

    OPC_RISC_LR_D        = OPC_RISC_ATOMIC | (0x3 << 12) | (0x02 << 27),
    OPC_RISC_SC_D        = OPC_RISC_ATOMIC | (0x3 << 12) | (0x03 << 27),
    OPC_RISC_AMOSWAP_D   = OPC_RISC_ATOMIC | (0x3 << 12) | (0x01 << 27),
    OPC_RISC_AMOADD_D    = OPC_RISC_ATOMIC | (0x3 << 12) | (0x00 << 27),
    OPC_RISC_AMOXOR_D    = OPC_RISC_ATOMIC | (0x3 << 12) | (0x04 << 27),
    OPC_RISC_AMOAND_D    = OPC_RISC_ATOMIC | (0x3 << 12) | (0x0C << 27),
    OPC_RISC_AMOOR_D     = OPC_RISC_ATOMIC | (0x3 << 12) | (0x08 << 27),
    OPC_RISC_AMOMIN_D    = OPC_RISC_ATOMIC | (0x3 << 12) | (0x10 << 27),
    OPC_RISC_AMOMAX_D    = OPC_RISC_ATOMIC | (0x3 << 12) | (0x14 << 27),
    OPC_RISC_AMOMINU_D   = OPC_RISC_ATOMIC | (0x3 << 12) | (0x18 << 27),
    OPC_RISC_AMOMAXU_D   = OPC_RISC_ATOMIC | (0x3 << 12) | (0x1C << 27),
};

#define MASK_OP_SYSTEM(op)   (MASK_OP_MAJOR(op) | (op & (0x7 << 12)))
enum {
    OPC_RISC_SCALL       = OPC_RISC_SYSTEM | (0x0 << 12),
    OPC_RISC_SBREAK      = OPC_RISC_SYSTEM | (0x0 << 12),
    OPC_RISC_SRET        = OPC_RISC_SYSTEM | (0x0 << 12),
    OPC_RISC_CSRRW       = OPC_RISC_SYSTEM | (0x1 << 12),
    OPC_RISC_CSRRS       = OPC_RISC_SYSTEM | (0x2 << 12),
    OPC_RISC_CSRRC       = OPC_RISC_SYSTEM | (0x3 << 12),
    OPC_RISC_CSRRWI      = OPC_RISC_SYSTEM | (0x5 << 12),
    OPC_RISC_CSRRSI      = OPC_RISC_SYSTEM | (0x6 << 12),
    OPC_RISC_CSRRCI      = OPC_RISC_SYSTEM | (0x7 << 12),
};

#define MASK_OP_FP_LOAD(op)   (MASK_OP_MAJOR(op) | (op & (0x7 << 12)))
enum {
    OPC_RISC_FLW   = OPC_RISC_FP_LOAD | (0x2 << 12),
    OPC_RISC_FLD   = OPC_RISC_FP_LOAD | (0x3 << 12),
};

#define MASK_OP_FP_STORE(op)   (MASK_OP_MAJOR(op) | (op & (0x7 << 12)))
enum {
    OPC_RISC_FSW   = OPC_RISC_FP_STORE | (0x2 << 12),
    OPC_RISC_FSD   = OPC_RISC_FP_STORE | (0x3 << 12),
};

/* global register indices */
static TCGv_ptr cpu_env;
static TCGv cpu_gpr[32], cpu_PC, cpu_fpr[32];


#include "exec/gen-icount.h"

#define gen_helper_0e0i(name, arg) do {                           \
    TCGv_i32 helper_tmp = tcg_const_i32(arg);                     \
    gen_helper_##name(cpu_env, helper_tmp);                       \
    tcg_temp_free_i32(helper_tmp);                                \
    } while(0)

typedef struct DisasContext {
    struct TranslationBlock *tb;
    target_ulong pc;
    uint32_t opcode;
    int singlestep_enabled;
    /* Routine used to access memory */
    int mem_idx;
    int bstate;
} DisasContext;


void kill_unknown(DisasContext *ctx, int excp);

enum {
    BS_NONE     = 0, /* We go out of the TB without reaching a branch or an
                      * exception condition */
    BS_STOP     = 1, /* We want to stop translation for any reason */
    BS_BRANCH   = 2, /* We reached a branch condition     */
    BS_EXCP     = 3, /* We reached an exception condition */
};

static const char * const regnames[] = {
    "zero", "ra", "s0", "s1",  "s2",  "s3",  "s4",  "s5",
    "s6",   "s7", "s8", "s9", "s10", "s11",  "sp",  "tp",
    "v0",   "v1", "a0", "a1",  "a2",  "a3",  "a4",  "a5",
    "a6",   "a7", "t0", "t1",  "t2",  "t3",  "t4",  "gp"
};

static const char * const cs_regnames[] = {
    "sup0", "sup1", "epc", "badvaddr", "ptbr", "asid", "count", "compare",
    "evec", "cause", "status", "hartid", "impl", "fatc", "send_ipi", "clear_ipi",
    "cycle", "time", "instret", "fflags", "frm", "fcsr", "junk", "junk",
    "junk", "junk", "junk", "junk", "junk", "junk", "tohost", "fromhost"
};

static const char * const fpr_regnames[] = {
    "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7",
    "f8", "f9", "f10", "f11", "f12", "f13", "f14", "f15",
    "f16", "f17", "f18", "f19", "f20", "f21", "f22", "f23",
    "f24", "f25", "f26", "f27", "f28", "f29", "f30", "f31",
};




#define MIPS_DEBUG(fmt, ...)                                                  \
    do {                                                                      \
        if (MIPS_DEBUG_DISAS) {                                               \
            qemu_log_mask(CPU_LOG_TB_IN_ASM,                                  \
                          TARGET_FMT_lx ": %08x " fmt "\n",                   \
                          ctx->pc, ctx->opcode , ## __VA_ARGS__);             \
        }                                                                     \
    } while (0)

#define LOG_DISAS(...)                                                        \
    do {                                                                      \
        if (MIPS_DEBUG_DISAS) {                                               \
            qemu_log_mask(CPU_LOG_TB_IN_ASM, ## __VA_ARGS__);                 \
        }                                                                     \
    } while (0)

static inline void gen_save_pc(target_ulong pc)
{
    tcg_gen_movi_tl(cpu_PC, pc);
}


// filling in for unknown instruction exception for now
void kill_unknown(DisasContext *ctx, int excp) {
    if (excp == RISCV_EXCP_FP_DISABLED) {
        printf("       FP INSTRUCTION at addr 0x" TARGET_FMT_lx ", opcode 0x%x\n", ctx->pc, ctx->opcode);
    } else {
        printf("       ILLEGAL INSTRUCTION at addr 0x" TARGET_FMT_lx ", opcode 0x%x\n", ctx->pc, ctx->opcode);
    }
    printf("killed due to unknown instruction\n");
    exit(0);
}

static inline void save_cpu_state (DisasContext *ctx, int do_save_pc)
{
    if (do_save_pc) {
        tcg_gen_movi_tl(cpu_PC, ctx->pc);
    }
}

static inline void restore_cpu_state (CPUMIPSState *env, DisasContext *ctx)
{
}

static inline void
generate_exception (DisasContext *ctx, int excp)
{
    tcg_gen_movi_tl(cpu_PC, ctx->pc);
    TCGv_i32 helper_tmp = tcg_const_i32(excp);
    gen_helper_raise_exception(cpu_env, helper_tmp);
    tcg_temp_free_i32(helper_tmp);
}

static inline void gen_goto_tb(DisasContext *ctx, int n, target_ulong dest)
{
    TranslationBlock *tb;
    tb = ctx->tb;
    if ((tb->pc & TARGET_PAGE_MASK) == (dest & TARGET_PAGE_MASK) &&
        likely(!ctx->singlestep_enabled)) {
        tcg_gen_goto_tb(n);
        gen_save_pc(dest);
        tcg_gen_exit_tb((uintptr_t)tb + n);
    } else {
        gen_save_pc(dest);
        if (ctx->singlestep_enabled) {
            save_cpu_state(ctx, 0);
            gen_helper_0e0i(raise_exception, EXCP_DEBUG);
        }
        tcg_gen_exit_tb(0);
    }
}

/* Wrapper for getting reg values - need to check of reg is zero since 
 * cpu_gpr[0] is not actually allocated 
 */
static inline void gen_get_gpr (TCGv t, int reg_num)
{
    if (reg_num == 0) {
        tcg_gen_movi_tl(t, 0);
    } else {
        tcg_gen_mov_tl(t, cpu_gpr[reg_num]);
    }
}

/* Wrapper for setting reg values - need to check of reg is zero since 
 * cpu_gpr[0] is not actually allocated. this is more for safety purposes,
 * since we usually avoid calling the OP_TYPE_gen function if we see a write to 
 * $zero
 */
static inline void gen_set_gpr (int reg_num_dst, TCGv t)
{
    if (reg_num_dst != 0) {
        tcg_gen_mov_tl(cpu_gpr[reg_num_dst], t);
    }
}

static void gen_arith(DisasContext *ctx, uint32_t opc, 
                      int rd, int rs1, int rs2)
{
    TCGv source1, source2;

    source1 = tcg_temp_new();
    source2 = tcg_temp_new();

    gen_get_gpr(source1, rs1);
    gen_get_gpr(source2, rs2);

    switch (opc) {

    case OPC_RISC_ADD:
        tcg_gen_add_tl(source1, source1, source2);
        break;
    case OPC_RISC_SUB:
        tcg_gen_sub_tl(source1, source1, source2);
        break;
    case OPC_RISC_SLL:
        tcg_gen_andi_tl(source2, source2, 0x3F);
        tcg_gen_shl_tl(source1, source1, source2);
        break;
    case OPC_RISC_SLT:
        tcg_gen_setcond_tl(TCG_COND_LT, source1, source1, source2);
        break;
    case OPC_RISC_SLTU:
        tcg_gen_setcond_tl(TCG_COND_LTU, source1, source1, source2);
        break;
    case OPC_RISC_XOR:
        tcg_gen_xor_tl(source1, source1, source2);
        break;
    case OPC_RISC_SRL:
        tcg_gen_andi_tl(source2, source2, 0x3F);
        tcg_gen_shr_tl(source1, source1, source2);
        break;
    case OPC_RISC_SRA:
        tcg_gen_andi_tl(source2, source2, 0x3F);
        tcg_gen_sar_tl(source1, source1, source2);
        break;
    case OPC_RISC_OR:
        tcg_gen_or_tl(source1, source1, source2);
        break;
    case OPC_RISC_AND:
        tcg_gen_and_tl(source1, source1, source2);
        break;
    case OPC_RISC_MUL:
        tcg_gen_muls2_tl(source1, source2, source1, source2);
        break;
    case OPC_RISC_MULH:
        tcg_gen_muls2_tl(source2, source1, source1, source2);
        break;
    case OPC_RISC_MULHSU:
        // TODO: better way to do this? currently implemented as a C helper
        gen_helper_mulhsu(source1, cpu_env, source1, source2);
        break;
    case OPC_RISC_MULHU:
        tcg_gen_mulu2_tl(source2, source1, source1, source2);
        break;
    case OPC_RISC_DIV:
        {
            TCGv spec_source1, spec_source2;
            TCGv cond1, cond2;
            int handle_zero = gen_new_label();
            int handle_overflow = gen_new_label();
            int done = gen_new_label();
            spec_source1 = tcg_temp_local_new();
            spec_source2 = tcg_temp_local_new();
            cond1 = tcg_temp_local_new();
            cond2 = tcg_temp_local_new();

            gen_get_gpr(spec_source1, rs1);
            gen_get_gpr(spec_source2, rs2);
            tcg_gen_brcondi_tl(TCG_COND_EQ, spec_source2, 0x0, handle_zero);

            // now, use temp reg to check if both overflow conditions satisfied
            tcg_gen_setcondi_tl(TCG_COND_EQ, cond2, spec_source2, 0xFFFFFFFFFFFFFFFF); // divisor = -1
            tcg_gen_setcondi_tl(TCG_COND_EQ, cond1, spec_source1, 0x8000000000000000);
            tcg_gen_and_tl(cond1, cond1, cond2);

            tcg_gen_brcondi_tl(TCG_COND_EQ, cond1, 1, handle_overflow);
            // normal case
            tcg_gen_div_tl(spec_source1, spec_source1, spec_source2);
            tcg_gen_br(done);
            // special zero case
            gen_set_label(handle_zero);
            tcg_gen_movi_tl(spec_source1, -1);
            tcg_gen_br(done);
            // special overflow case
            gen_set_label(handle_overflow);
            tcg_gen_movi_tl(spec_source1, 0x8000000000000000); 
            // done
            gen_set_label(done);
            tcg_gen_mov_tl(source1, spec_source1);
            tcg_temp_free(spec_source1);
            tcg_temp_free(spec_source2);
        }                
        break;
    case OPC_RISC_DIVU:
        {
            TCGv spec_source1, spec_source2;
            int handle_zero = gen_new_label();
            int done = gen_new_label();
            spec_source1 = tcg_temp_local_new();
            spec_source2 = tcg_temp_local_new();

            gen_get_gpr(spec_source1, rs1);
            gen_get_gpr(spec_source2, rs2);
            tcg_gen_brcondi_tl(TCG_COND_EQ, spec_source2, 0x0, handle_zero);

            // normal case
            tcg_gen_divu_tl(spec_source1, spec_source1, spec_source2);
            tcg_gen_br(done);
            // special zero case
            gen_set_label(handle_zero);
            tcg_gen_movi_tl(spec_source1, -1);
            tcg_gen_br(done);
            // done
            gen_set_label(done);
            tcg_gen_mov_tl(source1, spec_source1);
            tcg_temp_free(spec_source1);
            tcg_temp_free(spec_source2);
        }                
        break;
    case OPC_RISC_REM:
        {
            TCGv spec_source1, spec_source2;
            TCGv cond1, cond2;
            int handle_zero = gen_new_label();
            int handle_overflow = gen_new_label();
            int done = gen_new_label();
            spec_source1 = tcg_temp_local_new();
            spec_source2 = tcg_temp_local_new();
            cond1 = tcg_temp_local_new();
            cond2 = tcg_temp_local_new();

            gen_get_gpr(spec_source1, rs1);
            gen_get_gpr(spec_source2, rs2);
            tcg_gen_brcondi_tl(TCG_COND_EQ, spec_source2, 0x0, handle_zero);

            // now, use temp reg to check if both overflow conditions satisfied
            tcg_gen_setcondi_tl(TCG_COND_EQ, cond2, spec_source2, 0xFFFFFFFFFFFFFFFF); // divisor = -1
            tcg_gen_setcondi_tl(TCG_COND_EQ, cond1, spec_source1, 0x8000000000000000);
            tcg_gen_and_tl(cond1, cond1, cond2);

            tcg_gen_brcondi_tl(TCG_COND_EQ, cond1, 1, handle_overflow);
            // normal case
            tcg_gen_rem_tl(spec_source1, spec_source1, spec_source2);
            tcg_gen_br(done);
            // special zero case
            gen_set_label(handle_zero);
            tcg_gen_mov_tl(spec_source1, spec_source1); // even though it's a nop, just for clarity
            tcg_gen_br(done);
            // special overflow case
            gen_set_label(handle_overflow);
            tcg_gen_movi_tl(spec_source1, 0); 
            // done
            gen_set_label(done);
            tcg_gen_mov_tl(source1, spec_source1);
            tcg_temp_free(spec_source1);
            tcg_temp_free(spec_source2);
        }                
        break;
    case OPC_RISC_REMU:
        {
            TCGv spec_source1, spec_source2;
            int handle_zero = gen_new_label();
            int done = gen_new_label();
            spec_source1 = tcg_temp_local_new();
            spec_source2 = tcg_temp_local_new();

            gen_get_gpr(spec_source1, rs1);
            gen_get_gpr(spec_source2, rs2);
            tcg_gen_brcondi_tl(TCG_COND_EQ, spec_source2, 0x0, handle_zero);

            // normal case
            tcg_gen_remu_tl(spec_source1, spec_source1, spec_source2);
            tcg_gen_br(done);
            // special zero case
            gen_set_label(handle_zero);
            tcg_gen_mov_tl(spec_source1, spec_source1); // even though it's a nop, just for clarity
            tcg_gen_br(done);
            // done
            gen_set_label(done);
            tcg_gen_mov_tl(source1, spec_source1);
            tcg_temp_free(spec_source1);
            tcg_temp_free(spec_source2);
        }                
        break;
    default:
        // TODO EXCEPTION
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;

    }

    // set and free
    gen_set_gpr(rd, source1);
    tcg_temp_free(source1);
    tcg_temp_free(source2);
}

/* lower 12 bits of imm are valid */
static void gen_arith_imm(DisasContext *ctx, uint32_t opc, 
                      int rd, int rs1, int16_t imm)
{
    TCGv source1;

    source1 = tcg_temp_new();

    gen_get_gpr(source1, rs1);

    target_ulong uimm = (target_long)imm; /* sign ext 16->64 bits */

    switch (opc) {

    case OPC_RISC_ADDI:
        tcg_gen_addi_tl(source1, source1, uimm);
        break;
    case OPC_RISC_SLTI:
        tcg_gen_setcondi_tl(TCG_COND_LT, source1, source1, uimm);
        break;
    case OPC_RISC_SLTIU:
        tcg_gen_setcondi_tl(TCG_COND_LTU, source1, source1, uimm);
        break;
    case OPC_RISC_XORI:
        tcg_gen_xori_tl(source1, source1, uimm);
        break;
    case OPC_RISC_ORI:
        tcg_gen_ori_tl(source1, source1, uimm);
        break;
    case OPC_RISC_ANDI:
        tcg_gen_andi_tl(source1, source1, uimm);
        break;
    case OPC_RISC_SLLI: // TODO: add immediate upper bits check?
        tcg_gen_shli_tl(source1, source1, uimm);
        break;
    case OPC_RISC_SHIFT_RIGHT_I: // SRLI, SRAI, TODO: upper bits check
        // differentiate on IMM
        if (uimm & 0x400) {
            // SRAI
            tcg_gen_sari_tl(source1, source1, uimm ^ 0x400);
        } else {
            tcg_gen_shri_tl(source1, source1, uimm);
        }
        break;
    default:
        // TODO EXCEPTION
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;

    }

    gen_set_gpr(rd, source1);
    tcg_temp_free(source1);
}



/* lower 12 bits of imm are valid */
static void gen_arith_imm_w(DisasContext *ctx, uint32_t opc, 
                      int rd, int rs1, int16_t imm)
{

    TCGv source1;

    source1 = tcg_temp_new();

    gen_get_gpr(source1, rs1);

    target_ulong uimm = (target_long)imm; /* sign ext 16->64 bits */

    switch (opc) {

    case OPC_RISC_ADDIW:
        tcg_gen_addi_tl(source1, source1, uimm); // TODO: check this
        tcg_gen_ext32s_tl(source1, source1);
        break;
    case OPC_RISC_SLLIW: // TODO: add immediate upper bits check?
        tcg_gen_shli_tl(source1, source1, uimm);
        tcg_gen_ext32s_tl(source1, source1);
        break;
    case OPC_RISC_SHIFT_RIGHT_IW: // SRLIW, SRAIW, TODO: upper bits check
        // differentiate on IMM
        if (uimm & 0x400) {
            // SRAI
            // first, trick to get it to act like working on 32 bits:
            tcg_gen_shli_tl(source1, source1, 32);
            // now shift back to the right by shamt + 32 to get proper upper bits filling
            tcg_gen_sari_tl(source1, source1, (uimm ^ 0x400) + 32);
            tcg_gen_ext32s_tl(source1, source1);
        } else {
            // first, trick to get it to act like working on 32 bits (get rid of upper 32):
            tcg_gen_shli_tl(source1, source1, 32);
            // now shift back to the right by shamt + 32 to get proper upper bits filling
            tcg_gen_shri_tl(source1, source1, uimm + 32);
            tcg_gen_ext32s_tl(source1, source1);
        }
        break;
    default:
        // TODO EXCEPTION
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;

    }

    gen_set_gpr(rd, source1);
    tcg_temp_free(source1);

}

static void gen_arith_w(DisasContext *ctx, uint32_t opc, 
                      int rd, int rs1, int rs2)
{

    TCGv source1, source2;

    source1 = tcg_temp_new();
    source2 = tcg_temp_new();

    gen_get_gpr(source1, rs1);
    gen_get_gpr(source2, rs2);

    switch (opc) {

    case OPC_RISC_ADDW:
        tcg_gen_add_tl(source1, source1, source2);
        tcg_gen_ext32s_tl(source1, source1);
        break;
    case OPC_RISC_SUBW:
        tcg_gen_sub_tl(source1, source1, source2);
        tcg_gen_ext32s_tl(source1, source1);
        break;
    case OPC_RISC_SLLW:
        tcg_gen_andi_tl(source2, source2, 0x1F);
        tcg_gen_shl_tl(source1, source1, source2);
        tcg_gen_ext32s_tl(source1, source1);
        break;
    case OPC_RISC_SRLW:
        tcg_gen_andi_tl(source1, source1, 0x00000000FFFFFFFFLL); // clear upper 32
        tcg_gen_andi_tl(source2, source2, 0x1F);
        tcg_gen_shr_tl(source1, source1, source2); // do actual right shift
        tcg_gen_ext32s_tl(source1, source1); // sign ext
        break;
    case OPC_RISC_SRAW:
        // first, trick to get it to act like working on 32 bits (get rid of upper 32)
        tcg_gen_shli_tl(source1, source1, 32); // clear upper 32
        tcg_gen_sari_tl(source1, source1, 32); // smear the sign bit into upper 32
        tcg_gen_andi_tl(source2, source2, 0x1F);
        tcg_gen_sar_tl(source1, source1, source2); // do the actual right shift
        tcg_gen_ext32s_tl(source1, source1); // sign ext
        break;
    case OPC_RISC_MULW:
        tcg_gen_muls2_tl(source1, source2, source1, source2);
        tcg_gen_ext32s_tl(source1, source1);
        break;
    case OPC_RISC_DIVW:
        {
            TCGv spec_source1, spec_source2;
            TCGv cond1, cond2;
            int handle_zero = gen_new_label();
            int handle_overflow = gen_new_label();
            int done = gen_new_label();
            spec_source1 = tcg_temp_local_new();
            spec_source2 = tcg_temp_local_new();
            cond1 = tcg_temp_local_new();
            cond2 = tcg_temp_local_new();

            gen_get_gpr(spec_source1, rs1);
            gen_get_gpr(spec_source2, rs2);
            tcg_gen_ext32s_tl(spec_source1, spec_source1);
            tcg_gen_ext32s_tl(spec_source2, spec_source2);

            tcg_gen_brcondi_tl(TCG_COND_EQ, spec_source2, 0x0, handle_zero);

            // now, use temp reg to check if both overflow conditions satisfied
            tcg_gen_setcondi_tl(TCG_COND_EQ, cond2, spec_source2, 0xFFFFFFFFFFFFFFFF); // divisor = -1
            tcg_gen_setcondi_tl(TCG_COND_EQ, cond1, spec_source1, 0x8000000000000000);
            tcg_gen_and_tl(cond1, cond1, cond2);

            tcg_gen_brcondi_tl(TCG_COND_EQ, cond1, 1, handle_overflow);
            // normal case
            tcg_gen_div_tl(spec_source1, spec_source1, spec_source2);
            tcg_gen_br(done);
            // special zero case
            gen_set_label(handle_zero);
            tcg_gen_movi_tl(spec_source1, -1);
            tcg_gen_br(done);
            // special overflow case
            gen_set_label(handle_overflow);
            tcg_gen_movi_tl(spec_source1, 0x8000000000000000); 
            // done
            gen_set_label(done);
            tcg_gen_mov_tl(source1, spec_source1);
            tcg_temp_free(spec_source1);
            tcg_temp_free(spec_source2);
            tcg_gen_ext32s_tl(source1, source1);
        }                
        break;
    case OPC_RISC_DIVUW:
        {
            TCGv spec_source1, spec_source2;
            int handle_zero = gen_new_label();
            int done = gen_new_label();
            spec_source1 = tcg_temp_local_new();
            spec_source2 = tcg_temp_local_new();

            gen_get_gpr(spec_source1, rs1);
            gen_get_gpr(spec_source2, rs2);
            tcg_gen_ext32u_tl(spec_source1, spec_source1);
            tcg_gen_ext32u_tl(spec_source2, spec_source2);

            tcg_gen_brcondi_tl(TCG_COND_EQ, spec_source2, 0x0, handle_zero);

            // normal case
            tcg_gen_divu_tl(spec_source1, spec_source1, spec_source2);
            tcg_gen_br(done);
            // special zero case
            gen_set_label(handle_zero);
            tcg_gen_movi_tl(spec_source1, -1);
            tcg_gen_br(done);
            // done
            gen_set_label(done);
            tcg_gen_mov_tl(source1, spec_source1);
            tcg_temp_free(spec_source1);
            tcg_temp_free(spec_source2);
            tcg_gen_ext32s_tl(source1, source1);
        }                
        break;
    case OPC_RISC_REMW:
        {
            TCGv spec_source1, spec_source2;
            TCGv cond1, cond2;
            int handle_zero = gen_new_label();
            int handle_overflow = gen_new_label();
            int done = gen_new_label();
            spec_source1 = tcg_temp_local_new();
            spec_source2 = tcg_temp_local_new();
            cond1 = tcg_temp_local_new();
            cond2 = tcg_temp_local_new();

            gen_get_gpr(spec_source1, rs1);
            gen_get_gpr(spec_source2, rs2);
            tcg_gen_ext32s_tl(spec_source1, spec_source1);
            tcg_gen_ext32s_tl(spec_source2, spec_source2);

            tcg_gen_brcondi_tl(TCG_COND_EQ, spec_source2, 0x0, handle_zero);

            // now, use temp reg to check if both overflow conditions satisfied
            tcg_gen_setcondi_tl(TCG_COND_EQ, cond2, spec_source2, 0xFFFFFFFFFFFFFFFF); // divisor = -1
            tcg_gen_setcondi_tl(TCG_COND_EQ, cond1, spec_source1, 0x8000000000000000);
            tcg_gen_and_tl(cond1, cond1, cond2);

            tcg_gen_brcondi_tl(TCG_COND_EQ, cond1, 1, handle_overflow);
            // normal case
            tcg_gen_rem_tl(spec_source1, spec_source1, spec_source2);
            tcg_gen_br(done);
            // special zero case
            gen_set_label(handle_zero);
            tcg_gen_mov_tl(spec_source1, spec_source1); // even though it's a nop, just for clarity
            tcg_gen_br(done);
            // special overflow case
            gen_set_label(handle_overflow);
            tcg_gen_movi_tl(spec_source1, 0); 
            // done
            gen_set_label(done);
            tcg_gen_mov_tl(source1, spec_source1);
            tcg_temp_free(spec_source1);
            tcg_temp_free(spec_source2);
            tcg_gen_ext32s_tl(source1, source1);
        }                
        break;
    case OPC_RISC_REMUW:
        {
            TCGv spec_source1, spec_source2;
            int handle_zero = gen_new_label();
            int done = gen_new_label();
            spec_source1 = tcg_temp_local_new();
            spec_source2 = tcg_temp_local_new();

            gen_get_gpr(spec_source1, rs1);
            gen_get_gpr(spec_source2, rs2);
            tcg_gen_ext32u_tl(spec_source1, spec_source1);
            tcg_gen_ext32u_tl(spec_source2, spec_source2);

            tcg_gen_brcondi_tl(TCG_COND_EQ, spec_source2, 0x0, handle_zero);
// normal case
            tcg_gen_remu_tl(spec_source1, spec_source1, spec_source2);
            tcg_gen_br(done);
            // special zero case
            gen_set_label(handle_zero);
            tcg_gen_mov_tl(spec_source1, spec_source1); // even though it's a nop, just for clarity
            tcg_gen_br(done);
            // done
            gen_set_label(done);
            tcg_gen_mov_tl(source1, spec_source1);
            tcg_temp_free(spec_source1);
            tcg_temp_free(spec_source2);
            tcg_gen_ext32s_tl(source1, source1);
        }                
        break;
    default:
        // TODO EXCEPTION
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;

    }

    gen_set_gpr(rd, source1);
    tcg_temp_free(source1);
    tcg_temp_free(source2);
}

static void gen_branch(DisasContext *ctx, uint32_t opc, 
                       int rs1, int rs2, int16_t bimm) {

    int l = gen_new_label();

    TCGv source1, source2;

    source1 = tcg_temp_new();
    source2 = tcg_temp_new();

    gen_get_gpr(source1, rs1);
    gen_get_gpr(source2, rs2);

    target_ulong ubimm = (target_long)bimm; /* sign ext 16->64 bits */

    switch (opc) {

    case OPC_RISC_BEQ:
        tcg_gen_brcond_tl(TCG_COND_EQ, source1, source2, l);
        break;
    case OPC_RISC_BNE:
        tcg_gen_brcond_tl(TCG_COND_NE, source1, source2, l);
        break;
    case OPC_RISC_BLT:
        tcg_gen_brcond_tl(TCG_COND_LT, source1, source2, l);
        break;
    case OPC_RISC_BGE:
        tcg_gen_brcond_tl(TCG_COND_GE, source1, source2, l);
        break;
    case OPC_RISC_BLTU:
        tcg_gen_brcond_tl(TCG_COND_LTU, source1, source2, l);
        break;
    case OPC_RISC_BGEU:
        tcg_gen_brcond_tl(TCG_COND_GEU, source1, source2, l);
        break;
    default:
        /* TODO: exception here */
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;

    }

    // TODO: where do the frees go?

    tcg_gen_movi_tl(cpu_PC, ctx->pc + 4);
#ifdef DISABLE_CHAINING_BRANCH
    tcg_gen_exit_tb(0);
#else
    tcg_gen_goto_tb(1); // 1 is not taken, try chaining
    // TODO insn_bytes should be passed in here instead of hardcoded 4?
    tcg_gen_exit_tb((uintptr_t)ctx->tb | 0x1);
#endif
    gen_set_label(l); // branch taken
    tcg_gen_movi_tl(cpu_PC, ctx->pc + ubimm);
#ifdef DISABLE_CHAINING_BRANCH
    tcg_gen_exit_tb(0);
#else
    tcg_gen_goto_tb(0); // 0 is taken, try chaining
    tcg_gen_exit_tb((uintptr_t)ctx->tb | 0x0);
#endif
    ctx->bstate = BS_BRANCH;
}

static void gen_load(DisasContext *ctx, uint32_t opc, 
                      int rd, int rs1, int16_t imm)
{

    target_ulong uimm = (target_long)imm; /* sign ext 16->64 bits */

//    gen_helper_tlb_flush(cpu_env);

    TCGv t0 = tcg_temp_new();
    gen_get_gpr(t0, rs1);
    tcg_gen_addi_tl(t0, t0, uimm); // 
    
    switch (opc) {

    case OPC_RISC_LB:
        tcg_gen_qemu_ld8s(t0, t0, ctx->mem_idx); // TODO: is ctx->mem_idx right?
        break;
    case OPC_RISC_LH:
        tcg_gen_qemu_ld16s(t0, t0, ctx->mem_idx); // TODO: is ctx->mem_idx right?
        break;
    case OPC_RISC_LW:
        tcg_gen_qemu_ld32s(t0, t0, ctx->mem_idx); // TODO: is ctx->mem_idx right?
        break;
    case OPC_RISC_LD:
        tcg_gen_qemu_ld64(t0, t0, ctx->mem_idx); // TODO: is ctx->mem_idx right?
        break;
    case OPC_RISC_LBU:
        tcg_gen_qemu_ld8u(t0, t0, ctx->mem_idx); // TODO: is ctx->mem_idx right?
        break;
    case OPC_RISC_LHU:
        tcg_gen_qemu_ld16u(t0, t0, ctx->mem_idx); // TODO: is ctx->mem_idx right?
        break;
    case OPC_RISC_LWU:
        tcg_gen_qemu_ld32u(t0, t0, ctx->mem_idx); // TODO: is ctx->mem_idx right?
        break;
    default:
        // TODO Exception
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;

    }

    gen_set_gpr(rd, t0);
    tcg_temp_free(t0);
}


static void gen_store(DisasContext *ctx, uint32_t opc, 
                      int rs1, int rs2, int16_t imm)
{
    target_ulong uimm = (target_long)imm; /* sign ext 16->64 bits */

//    gen_helper_tlb_flush(cpu_env);

    TCGv t0 = tcg_temp_new();
    TCGv dat = tcg_temp_new();
    gen_get_gpr(t0, rs1);
    tcg_gen_addi_tl(t0, t0, uimm); // 
    gen_get_gpr(dat, rs2);

    switch (opc) {

    case OPC_RISC_SB:
        tcg_gen_qemu_st8(dat, t0, ctx->mem_idx); // TODO: is ctx->mem_idx right?
        break;
    case OPC_RISC_SH:
        tcg_gen_qemu_st16(dat, t0, ctx->mem_idx); // TODO: is ctx->mem_idx right?
        break;
    case OPC_RISC_SW:
        tcg_gen_qemu_st32(dat, t0, ctx->mem_idx); // TODO: is ctx->mem_idx right?
        break;
    case OPC_RISC_SD:
        tcg_gen_qemu_st64(dat, t0, ctx->mem_idx); // TODO: is ctx->mem_idx right?
        break;

    default:
        // TODO: exception
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;
    }

    tcg_temp_free(t0);
    tcg_temp_free(dat);
}

static void gen_jalr(DisasContext *ctx, uint32_t opc, 
                      int rd, int rs1, int16_t imm)
{
    target_ulong uimm = (target_long)imm; /* sign ext 16->64 bits */
    TCGv t0, t1;

    switch (opc) {
    
    case OPC_RISC_JALR: // CANNOT HAVE CHAINING WITH JALR
        t0 = tcg_temp_new();
        t1 = tcg_temp_new();
        gen_get_gpr(t0, rs1);
        tcg_gen_addi_tl(t0, t0, uimm);
        tcg_gen_andi_tl(t0, t0, 0xFFFFFFFFFFFFFFFEll);

        // store pc+4 to rd as necessary
        tcg_gen_movi_tl(t1, 4);
        tcg_gen_addi_tl(t1, t1, ctx->pc);
        gen_set_gpr(rd, t1);

        tcg_gen_mov_tl(cpu_PC, t0);
        tcg_gen_exit_tb(0); // NO CHAINING FOR JALR
        ctx->bstate = BS_BRANCH;
        break;
    default:
        // TODO: exception
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;

    }
    tcg_temp_free(t0);
    tcg_temp_free(t1);
}

static void gen_atomic(DisasContext *ctx, uint32_t opc, 
                      int rd, int rs1, int rs2)
{
    // TODO: handle aq, rl bits? - for now just get rid of them:
    opc = MASK_OP_ATOMIC_NO_AQ_RL(opc);

    TCGv source1, source2, dat;

    source1 = tcg_temp_new();
    source2 = tcg_temp_new();
    dat = tcg_temp_new();

    gen_get_gpr(source1, rs1);
    gen_get_gpr(source2, rs2);

    switch (opc) {
        // all currently implemented as non-atomics
    case OPC_RISC_LR_W:
        tcg_gen_qemu_ld32s(source1, source1, ctx->mem_idx);
        break;
    case OPC_RISC_SC_W:
        tcg_gen_qemu_st32(source2, source1, ctx->mem_idx);
        tcg_gen_movi_tl(source1, 0); // assume always success
        break;
    case OPC_RISC_AMOSWAP_W:
        tcg_gen_qemu_ld32s(dat, source1, ctx->mem_idx);
        tcg_gen_mov_tl(source2, source2); // replace with other op using dat
        tcg_gen_qemu_st32(source2, source1, ctx->mem_idx);
        tcg_gen_mov_tl(source1, dat);
        break;
    case OPC_RISC_AMOADD_W:   
        tcg_gen_qemu_ld32s(dat, source1, ctx->mem_idx);
        tcg_gen_add_tl(source2, dat, source2); // replace with other op using dat
        tcg_gen_qemu_st32(source2, source1, ctx->mem_idx);
        tcg_gen_mov_tl(source1, dat);
        break;
    case OPC_RISC_AMOXOR_W:
        tcg_gen_qemu_ld32s(dat, source1, ctx->mem_idx);
        tcg_gen_xor_tl(source2, dat, source2); // replace with other op using dat
        tcg_gen_qemu_st32(source2, source1, ctx->mem_idx);
        tcg_gen_mov_tl(source1, dat);
        break;
    case OPC_RISC_AMOAND_W:
        tcg_gen_qemu_ld32s(dat, source1, ctx->mem_idx);
        tcg_gen_and_tl(source2, dat, source2); // replace with other op using dat
        tcg_gen_qemu_st32(source2, source1, ctx->mem_idx);
        tcg_gen_mov_tl(source1, dat);
        break;
    case OPC_RISC_AMOOR_W:
        tcg_gen_qemu_ld32s(dat, source1, ctx->mem_idx);
        tcg_gen_or_tl(source2, dat, source2); // replace with other op using dat
        tcg_gen_qemu_st32(source2, source1, ctx->mem_idx);
        tcg_gen_mov_tl(source1, dat);
        break;
    case OPC_RISC_AMOMIN_W:
        {
            TCGv source1_l, source2_l, dat_l;
            source1_l = tcg_temp_local_new();
            source2_l = tcg_temp_local_new();
            dat_l = tcg_temp_local_new();
            int j = gen_new_label();
            int done = gen_new_label();
            tcg_gen_mov_tl(source1_l, source1);
            tcg_gen_mov_tl(source2_l, source2);
            tcg_gen_qemu_ld32s(dat_l, source1_l, ctx->mem_idx);
            tcg_gen_brcond_tl(TCG_COND_LT, dat_l, source2_l, j);
            tcg_gen_qemu_st32(source2_l, source1_l, ctx->mem_idx);
            tcg_gen_br(done);
            // here we store the thing on the left
            gen_set_label(j);
            tcg_gen_qemu_st32(dat_l, source1_l, ctx->mem_idx);
            //done
            gen_set_label(done);
            tcg_gen_mov_tl(source1, dat_l);
            tcg_temp_free(source1_l);
            tcg_temp_free(source2_l);
            tcg_temp_free(dat_l);
        }
        break;
    case OPC_RISC_AMOMAX_W:
        {
            TCGv source1_l, source2_l, dat_l;
            source1_l = tcg_temp_local_new();
            source2_l = tcg_temp_local_new();
            dat_l = tcg_temp_local_new();
            int j = gen_new_label();
            int done = gen_new_label();
            tcg_gen_mov_tl(source1_l, source1);
            tcg_gen_mov_tl(source2_l, source2);
            tcg_gen_qemu_ld32s(dat_l, source1_l, ctx->mem_idx);
            tcg_gen_brcond_tl(TCG_COND_GT, dat_l, source2_l, j);
            tcg_gen_qemu_st32(source2_l, source1_l, ctx->mem_idx);
            tcg_gen_br(done);
            // here we store the thing on the left
            gen_set_label(j);
            tcg_gen_qemu_st32(dat_l, source1_l, ctx->mem_idx);
            //done
            gen_set_label(done);
            tcg_gen_mov_tl(source1, dat_l);
            tcg_temp_free(source1_l);
            tcg_temp_free(source2_l);
            tcg_temp_free(dat_l);
        }
        break;
    case OPC_RISC_AMOMINU_W:
        {
            TCGv source1_l, source2_l, dat_l;
            source1_l = tcg_temp_local_new();
            source2_l = tcg_temp_local_new();
            dat_l = tcg_temp_local_new();
            int j = gen_new_label();
            int done = gen_new_label();
            tcg_gen_mov_tl(source1_l, source1);
            tcg_gen_mov_tl(source2_l, source2);
            tcg_gen_qemu_ld32s(dat_l, source1_l, ctx->mem_idx);
            tcg_gen_brcond_tl(TCG_COND_LTU, dat_l, source2_l, j);
            tcg_gen_qemu_st32(source2_l, source1_l, ctx->mem_idx);
            tcg_gen_br(done);
            // here we store the thing on the left
            gen_set_label(j);
            tcg_gen_qemu_st32(dat_l, source1_l, ctx->mem_idx);
            //done
            gen_set_label(done);
            tcg_gen_mov_tl(source1, dat_l);
            tcg_temp_free(source1_l);
            tcg_temp_free(source2_l);
            tcg_temp_free(dat_l);
        }
        break;
    case OPC_RISC_AMOMAXU_W:
        {
            TCGv source1_l, source2_l, dat_l;
            source1_l = tcg_temp_local_new();
            source2_l = tcg_temp_local_new();
            dat_l = tcg_temp_local_new();
            int j = gen_new_label();
            int done = gen_new_label();
            tcg_gen_mov_tl(source1_l, source1);
            tcg_gen_mov_tl(source2_l, source2);
            tcg_gen_qemu_ld32s(dat_l, source1_l, ctx->mem_idx);
            tcg_gen_brcond_tl(TCG_COND_GTU, dat_l, source2_l, j);
            tcg_gen_qemu_st32(source2_l, source1_l, ctx->mem_idx);
            tcg_gen_br(done);
            // here we store the thing on the left
            gen_set_label(j);
            tcg_gen_qemu_st32(dat_l, source1_l, ctx->mem_idx);
            //done
            gen_set_label(done);
            tcg_gen_mov_tl(source1, dat_l);
            tcg_temp_free(source1_l);
            tcg_temp_free(source2_l);
            tcg_temp_free(dat_l);
        }
        break;
    case OPC_RISC_LR_D:
        tcg_gen_qemu_ld64(source1, source1, ctx->mem_idx);
        break;
    case OPC_RISC_SC_D:       
        tcg_gen_qemu_st64(source2, source1, ctx->mem_idx);
        tcg_gen_movi_tl(source1, 0); // assume always success
        break;
    case OPC_RISC_AMOSWAP_D:
        tcg_gen_qemu_ld64(dat, source1, ctx->mem_idx);
        tcg_gen_mov_tl(source2, source2); // replace with other op using dat
        tcg_gen_qemu_st64(source2, source1, ctx->mem_idx);
        tcg_gen_mov_tl(source1, dat);
        break;
    case OPC_RISC_AMOADD_D:
        tcg_gen_qemu_ld64(dat, source1, ctx->mem_idx);
        tcg_gen_add_tl(source2, dat, source2); // replace with other op using dat
        tcg_gen_qemu_st64(source2, source1, ctx->mem_idx);
        tcg_gen_mov_tl(source1, dat);
        break;
    case OPC_RISC_AMOXOR_D: 
        tcg_gen_qemu_ld64(dat, source1, ctx->mem_idx);
        tcg_gen_xor_tl(source2, dat, source2); // replace with other op using dat
        tcg_gen_qemu_st64(source2, source1, ctx->mem_idx);
        tcg_gen_mov_tl(source1, dat);
        break;
    case OPC_RISC_AMOAND_D:
        tcg_gen_qemu_ld64(dat, source1, ctx->mem_idx);
        tcg_gen_and_tl(source2, dat, source2); // replace with other op using dat
        tcg_gen_qemu_st64(source2, source1, ctx->mem_idx);
        tcg_gen_mov_tl(source1, dat);
        break;
    case OPC_RISC_AMOOR_D:
        tcg_gen_qemu_ld64(dat, source1, ctx->mem_idx);
        tcg_gen_or_tl(source2, dat, source2); // replace with other op using dat
        tcg_gen_qemu_st64(source2, source1, ctx->mem_idx);
        tcg_gen_mov_tl(source1, dat);
        break;
    case OPC_RISC_AMOMIN_D:
        {
            TCGv source1_l, source2_l, dat_l;
            source1_l = tcg_temp_local_new();
            source2_l = tcg_temp_local_new();
            dat_l = tcg_temp_local_new();
            int j = gen_new_label();
            int done = gen_new_label();
            tcg_gen_mov_tl(source1_l, source1);
            tcg_gen_mov_tl(source2_l, source2);
            tcg_gen_qemu_ld64(dat_l, source1_l, ctx->mem_idx);
            tcg_gen_brcond_tl(TCG_COND_LT, dat_l, source2_l, j);
            tcg_gen_qemu_st64(source2_l, source1_l, ctx->mem_idx);
            tcg_gen_br(done);
            // here we store the thing on the left
            gen_set_label(j);
            tcg_gen_qemu_st64(dat_l, source1_l, ctx->mem_idx);
            //done
            gen_set_label(done);
            tcg_gen_mov_tl(source1, dat_l);
            tcg_temp_free(source1_l);
            tcg_temp_free(source2_l);
            tcg_temp_free(dat_l);
        }
        break;
    case OPC_RISC_AMOMAX_D:
        {
            TCGv source1_l, source2_l, dat_l;
            source1_l = tcg_temp_local_new();
            source2_l = tcg_temp_local_new();
            dat_l = tcg_temp_local_new();
            int j = gen_new_label();
            int done = gen_new_label();
            tcg_gen_mov_tl(source1_l, source1);
            tcg_gen_mov_tl(source2_l, source2);
            tcg_gen_qemu_ld64(dat_l, source1_l, ctx->mem_idx);
            tcg_gen_brcond_tl(TCG_COND_GT, dat_l, source2_l, j);
            tcg_gen_qemu_st64(source2_l, source1_l, ctx->mem_idx);
            tcg_gen_br(done);
            // here we store the thing on the left
            gen_set_label(j);
            tcg_gen_qemu_st64(dat_l, source1_l, ctx->mem_idx);
            //done
            gen_set_label(done);
            tcg_gen_mov_tl(source1, dat_l);
            tcg_temp_free(source1_l);
            tcg_temp_free(source2_l);
            tcg_temp_free(dat_l);
        }
        break;
    case OPC_RISC_AMOMINU_D:
        {
            TCGv source1_l, source2_l, dat_l;
            source1_l = tcg_temp_local_new();
            source2_l = tcg_temp_local_new();
            dat_l = tcg_temp_local_new();
            int j = gen_new_label();
            int done = gen_new_label();
            tcg_gen_mov_tl(source1_l, source1);
            tcg_gen_mov_tl(source2_l, source2);
            tcg_gen_qemu_ld64(dat_l, source1_l, ctx->mem_idx);
            tcg_gen_brcond_tl(TCG_COND_LTU, dat_l, source2_l, j);
            tcg_gen_qemu_st64(source2_l, source1_l, ctx->mem_idx);
            tcg_gen_br(done);
            // here we store the thing on the left
            gen_set_label(j);
            tcg_gen_qemu_st64(dat_l, source1_l, ctx->mem_idx);
            //done
            gen_set_label(done);
            tcg_gen_mov_tl(source1, dat_l);
            tcg_temp_free(source1_l);
            tcg_temp_free(source2_l);
            tcg_temp_free(dat_l);
        }
        break;
    case OPC_RISC_AMOMAXU_D:
        {
            TCGv source1_l, source2_l, dat_l;
            source1_l = tcg_temp_local_new();
            source2_l = tcg_temp_local_new();
            dat_l = tcg_temp_local_new();
            int j = gen_new_label();
            int done = gen_new_label();
            tcg_gen_mov_tl(source1_l, source1);
            tcg_gen_mov_tl(source2_l, source2);
            tcg_gen_qemu_ld64(dat_l, source1_l, ctx->mem_idx);
            tcg_gen_brcond_tl(TCG_COND_GTU, dat_l, source2_l, j);
            tcg_gen_qemu_st64(source2_l, source1_l, ctx->mem_idx);
            tcg_gen_br(done);
            // here we store the thing on the left
            gen_set_label(j);
            tcg_gen_qemu_st64(dat_l, source1_l, ctx->mem_idx);
            //done
            gen_set_label(done);
            tcg_gen_mov_tl(source1, dat_l);
            tcg_temp_free(source1_l);
            tcg_temp_free(source2_l);
            tcg_temp_free(dat_l);
        }
        break;
    default:
        // TODO EXCEPTION
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;

    }

    // set and free
    gen_set_gpr(rd, source1);
    tcg_temp_free(source1);
    tcg_temp_free(source2);
    tcg_temp_free(dat);
}



static void gen_system(DisasContext *ctx, uint32_t opc, 
                      int rd, int rs1, int csr)
{
    // get index into csr array
    int backup_csr = csr;
    if (csr == 0x506)  {
        printf("touched count: %x\n", ctx->opcode);
    } else if (csr == 0x507) {
        printf("touched compare: %x\n", ctx->opcode);
    }
    csr = csr_regno(csr);

    if (backup_csr == 0x50D || backup_csr == 0x505) {
        gen_helper_tlb_flush(cpu_env);
    }
//    gen_helper_tlb_flush(cpu_env);

    TCGv source1, csr_store, dest;
    source1 = tcg_temp_new();
    csr_store = tcg_temp_new();
    dest = tcg_temp_new();
    gen_get_gpr(source1, rs1);
    tcg_gen_movi_tl(csr_store, csr); // copy into temp reg to feed to helper

    switch (opc) {

    case OPC_RISC_SCALL:
        switch (backup_csr) {
            case 0x0: // SCALL
                generate_exception(ctx, RISCV_EXCP_SCALL);
                ctx->bstate = BS_STOP;
/*                tcg_gen_movi_tl(cpu_PC, ctx->pc); // excluding this before
                                                  // jumping into the helper
                                                  // was previously the issue
                gen_helper_scall(cpu_PC, cpu_env, cpu_PC);
                tcg_gen_exit_tb(0); // no chaining
                ctx->bstate = BS_BRANCH;
*/
                break;

            case 0x1: // SBREAK
                kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
                ctx->bstate = BS_STOP;
                break;

            case 0x800: // SRET
                gen_helper_sret(cpu_PC, cpu_env);
                tcg_gen_exit_tb(0); // no chaining
                ctx->bstate = BS_BRANCH;
                break;

            default:
                kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
                break;
        }
        break;
    case OPC_RISC_CSRRW:
        gen_helper_csrrw(dest, cpu_env, source1, csr_store);
        gen_set_gpr(rd, dest);
        break;
    case OPC_RISC_CSRRS:
        gen_helper_csrrs(dest, cpu_env, source1, csr_store);
        gen_set_gpr(rd, dest);
        break;
    case OPC_RISC_CSRRC:
        gen_helper_csrrc(dest, cpu_env, source1, csr_store);
        gen_set_gpr(rd, dest);
        break;
    case OPC_RISC_CSRRWI:
        {
            TCGv imm_rs1 = tcg_temp_new();
            tcg_gen_movi_tl(imm_rs1, rs1);
            gen_helper_csrrw(dest, cpu_env, imm_rs1, csr_store);
            gen_set_gpr(rd, dest);
            tcg_temp_free(imm_rs1);
        }
        break;
    case OPC_RISC_CSRRSI:
        {
            TCGv imm_rs1 = tcg_temp_new();
            tcg_gen_movi_tl(imm_rs1, rs1);
            gen_helper_csrrs(dest, cpu_env, imm_rs1, csr_store);
            gen_set_gpr(rd, dest);
            tcg_temp_free(imm_rs1);
        }
        break;
    case OPC_RISC_CSRRCI:
        {
            TCGv imm_rs1 = tcg_temp_new();
            tcg_gen_movi_tl(imm_rs1, rs1);
            gen_helper_csrrc(dest, cpu_env, imm_rs1, csr_store);
            gen_set_gpr(rd, dest);
            tcg_temp_free(imm_rs1);
        }
        break;
    default:
        // TODO: exception
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;

    }
    tcg_temp_free(source1);
    tcg_temp_free(dest);
    tcg_temp_free(csr_store);
}


static void gen_fp_load(DisasContext *ctx, uint32_t opc, 
                      int rd, int rs1, int16_t imm)
{

    target_ulong uimm = (target_long)imm; /* sign ext 16->64 bits */


    TCGv t0 = tcg_temp_new();
    gen_get_gpr(t0, rs1);
    tcg_gen_addi_tl(t0, t0, uimm); // 
    
    switch (opc) {

    case OPC_RISC_FLW:
        // TODO: sign extend or zero extend?
        tcg_gen_qemu_ld32u(t0, t0, ctx->mem_idx); // TODO: is ctx->mem_idx right?
        break;
    case OPC_RISC_FLD:
        tcg_gen_qemu_ld64(t0, t0, ctx->mem_idx); // TODO: is ctx->mem_idx right?
        break;
    default:
        // TODO Exception
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;

    }

    tcg_gen_mov_tl(cpu_fpr[rd], t0); // copy into fp reg
    tcg_temp_free(t0);
}

static void gen_fp_store(DisasContext *ctx, uint32_t opc, 
                      int rs1, int rs2, int16_t imm)
{
    target_ulong uimm = (target_long)imm; /* sign ext 16->64 bits */

    TCGv t0 = tcg_temp_new();
    TCGv dat = tcg_temp_new();
    gen_get_gpr(t0, rs1);
    tcg_gen_addi_tl(t0, t0, uimm); // 
    tcg_gen_mov_tl(dat, cpu_fpr[rs2]);

    switch (opc) {

    case OPC_RISC_FSW:
        tcg_gen_qemu_st32(dat, t0, ctx->mem_idx); // TODO: is ctx->mem_idx right?
        break;
    case OPC_RISC_FSD:
        tcg_gen_qemu_st64(dat, t0, ctx->mem_idx); // TODO: is ctx->mem_idx right?
        break;

    default:
        // TODO: exception
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;
    }

    tcg_temp_free(t0);
    tcg_temp_free(dat);
}

#define GET_B_IMM(inst)              ((int16_t)((((inst >> 25) & 0x3F) << 5) | ((((int32_t)inst) >> 31) << 12) | (((inst >> 8) & 0xF) << 1) | (((inst >> 7) & 0x1) << 11)))  /* THIS BUILDS 13 bit imm (implicit zero is tacked on here), also note that bit #12 is obtained in a special way to get sign extension */
#define GET_STORE_IMM(inst)           ((int16_t)(((((int32_t)inst) >> 25) << 5) | ((inst >> 7) & 0x1F)))
#define GET_JAL_IMM(inst)             ((int32_t)((inst & 0xFF000) | (((inst >> 20) & 0x1) << 11) | (((inst >> 21) & 0x3FF) << 1) | ((((int32_t)inst) >> 31) << 20)))

static void decode_opc (CPUMIPSState *env, DisasContext *ctx)
{
    int rs1;
    int rs2;
    int rd;
    uint32_t op;
    int16_t imm;
    target_ulong ubimm;

    /* make sure instructions are on a word boundary */
    if (ctx->pc & 0x3) { 
        // NOT tested for RISCV
        printf("misaligned instruction, not yet implemented for riscv\n");
        return;
    }

    // increment cycle:
//    tcg_gen_addi_tl(cpu_csr[CSR_CYCLE], cpu_csr[CSR_CYCLE], 1);

//    gen_helper_tlb_flush(cpu_env);

    op = MASK_OP_MAJOR(ctx->opcode);
    rs1 = (ctx->opcode >> 15) & 0x1f;
    rs2 = (ctx->opcode >> 20) & 0x1f;
    rd = (ctx->opcode >> 7) & 0x1f;
    imm = (int16_t)(((int32_t)ctx->opcode) >> 20); /* sign extends */
    switch (op) {

    case OPC_RISC_LUI:
        // TODO: mod to use reg setters/getters
        if (rd == 0) {
            break; // NOP
        }
        tcg_gen_movi_tl(cpu_gpr[rd], (ctx->opcode & 0xFFFFF000));
        tcg_gen_ext32s_tl(cpu_gpr[rd], cpu_gpr[rd]);
        break;

    case OPC_RISC_AUIPC:
        // TODO: mod to use reg setters/getters
        if (rd == 0) {
            break; // NOP
        }
        tcg_gen_movi_tl(cpu_gpr[rd], (ctx->opcode & 0xFFFFF000));
        tcg_gen_ext32s_tl(cpu_gpr[rd], cpu_gpr[rd]);
        tcg_gen_add_tl(cpu_gpr[rd], cpu_gpr[rd], tcg_const_tl(ctx->pc)); // TODO: CHECK THIS
        break;

    case OPC_RISC_JAL:
        ubimm = (target_long) (GET_JAL_IMM(ctx->opcode));
        // TODO: mod to use reg setters/getters
        if (rd != 0) {
            tcg_gen_movi_tl(cpu_gpr[rd], 4);
            tcg_gen_addi_tl(cpu_gpr[rd], cpu_gpr[rd], ctx->pc);
        }
        tcg_gen_movi_tl(cpu_PC, ctx->pc + ubimm);
#ifdef DISABLE_CHAINING_JAL
        tcg_gen_exit_tb(0);
#else
        tcg_gen_goto_tb(0);
        tcg_gen_exit_tb((uintptr_t)ctx->tb | 0x0);
#endif
        ctx->bstate = BS_BRANCH;
        break;

    case OPC_RISC_JALR:
        gen_jalr(ctx, MASK_OP_JALR(ctx->opcode), rd, rs1, imm);
        break;

    case OPC_RISC_BRANCH:
        gen_branch(ctx, MASK_OP_BRANCH(ctx->opcode), rs1, rs2, GET_B_IMM(ctx->opcode));
        break;

    case OPC_RISC_LOAD:
        gen_load(ctx, MASK_OP_LOAD(ctx->opcode), rd, rs1, imm);
        break;

    case OPC_RISC_STORE:
        gen_store(ctx, MASK_OP_STORE(ctx->opcode), rs1, rs2, GET_STORE_IMM(ctx->opcode));
        break;

    case OPC_RISC_ARITH_IMM:
        if (rd == 0) {
            break; // NOP
        }
        gen_arith_imm(ctx, MASK_OP_ARITH_IMM(ctx->opcode), rd, rs1, imm);
        break;

    case OPC_RISC_ARITH:
        if (rd == 0) {
            break; // NOP
        }
        gen_arith(ctx, MASK_OP_ARITH(ctx->opcode), rd, rs1, rs2);
        break;

    case OPC_RISC_ARITH_IMM_W:
        if (rd == 0) {
            break; // NOP
        }
        gen_arith_imm_w(ctx, MASK_OP_ARITH_IMM_W(ctx->opcode), rd, rs1, imm);
        break;

    case OPC_RISC_ARITH_W:
        if (rd == 0) {
            break; // NOP
        }
        gen_arith_w(ctx, MASK_OP_ARITH_W(ctx->opcode), rd, rs1, rs2);
        break;

    case OPC_RISC_FENCE:
        /* fences are nops for us? */
        break;

    case OPC_RISC_SYSTEM:
        gen_system(ctx, MASK_OP_SYSTEM(ctx->opcode), rd, rs1, (ctx->opcode & 0xFFF00000) >> 20);
        break;

    case OPC_RISC_ATOMIC:
        gen_atomic(ctx, MASK_OP_ATOMIC(ctx->opcode), rd, rs1, rs2);
        break;        

    case OPC_RISC_FP_LOAD:
        gen_fp_load(ctx, MASK_OP_FP_LOAD(ctx->opcode), rd, rs1, imm);
        break;

    case OPC_RISC_FP_STORE:
        gen_fp_store(ctx, MASK_OP_FP_STORE(ctx->opcode), rs1, rs2, GET_STORE_IMM(ctx->opcode));
        break;

    case OPC_RISC_FMADD:// not all fp is implemented currently
    case OPC_RISC_FMSUB:
    case OPC_RISC_FNMSUB:
    case OPC_RISC_FNMADD:
    case OPC_RISC_FP_ARITH:
        printf("GOT HERE GOT HERE");
        exit(0);
        kill_unknown(ctx, RISCV_EXCP_FP_DISABLED);
        break;

    default:
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        // TODO REMOVED FOR TESTING, REPLACE
        // generate_exception(ctx, EXCP_RI);
        break;
    }
}

static inline void
gen_intermediate_code_internal(MIPSCPU *cpu, TranslationBlock *tb,
                               bool search_pc)
{
    CPUState *cs = CPU(cpu);
    CPUMIPSState *env = &cpu->env;
    DisasContext ctx;
    target_ulong pc_start;
    uint16_t *gen_opc_end;
    CPUBreakpoint *bp;
    int j, lj = -1;
    int num_insns;
    int max_insns;
    int insn_bytes;

    if (search_pc)
        qemu_log("search pc %d\n", search_pc);

    pc_start = tb->pc;
    gen_opc_end = tcg_ctx.gen_opc_buf + OPC_MAX_SIZE;
    ctx.pc = pc_start;
    ctx.singlestep_enabled = cs->singlestep_enabled;
    ctx.tb = tb;
    ctx.bstate = BS_NONE;
    /* Restore delay slot state from the tb context.  */
    restore_cpu_state(env, &ctx);
#ifdef CONFIG_USER_ONLY
        ctx.mem_idx = MIPS_HFLAG_UM;
#else
        ctx.mem_idx = env->helper_csr[CSR_STATUS] & SR_S;

#endif
    num_insns = 0;
    max_insns = tb->cflags & CF_COUNT_MASK;
    if (max_insns == 0)
        max_insns = CF_COUNT_MASK;
    gen_tb_start();
    while (ctx.bstate == BS_NONE) {
        if (unlikely(!QTAILQ_EMPTY(&cs->breakpoints))) {
            QTAILQ_FOREACH(bp, &cs->breakpoints, entry) {
                if (bp->pc == ctx.pc) {
                    save_cpu_state(&ctx, 1);
                    ctx.bstate = BS_BRANCH;
                    gen_helper_0e0i(raise_exception, EXCP_DEBUG);
                    /* Include the breakpoint location or the tb won't
                     * be flushed when it must be.  */
                    ctx.pc += 4;
                    goto done_generating;
                }
            }
        }

        if (search_pc) {
            j = tcg_ctx.gen_opc_ptr - tcg_ctx.gen_opc_buf;
            if (lj < j) {
                lj++;
                while (lj < j)
                    tcg_ctx.gen_opc_instr_start[lj++] = 0;
            }
            tcg_ctx.gen_opc_pc[lj] = ctx.pc;
            tcg_ctx.gen_opc_instr_start[lj] = 1;
            tcg_ctx.gen_opc_icount[lj] = num_insns;
        }
        if (num_insns + 1 == max_insns && (tb->cflags & CF_LAST_IO)) {
            gen_io_start();
        }

        ctx.opcode = cpu_ldl_code(env, ctx.pc);
        insn_bytes = 4;
        decode_opc(env, &ctx);

        ctx.pc += insn_bytes;

        num_insns++;

        if ((ctx.pc & (TARGET_PAGE_SIZE - 1)) == 0) { 
            // handle tb at the end of a page
            break;
        }
        if (tcg_ctx.gen_opc_ptr >= gen_opc_end) {
            break;
        }

        if (num_insns >= max_insns) {
            break;
        }

        if (singlestep) {
            printf("singlestep is not currently supported for riscv\n");
            exit(1);
            break;
        }

    }
    if (tb->cflags & CF_LAST_IO) {
        gen_io_end();
    }
    switch (ctx.bstate) {
    case BS_STOP:
        gen_goto_tb(&ctx, 0, ctx.pc);







        break;
    case BS_NONE:
        /* handle end of page case, we can even chain these, I think */

        /* method 1: this may be unsafe, but is an optimization */
        /*tcg_gen_goto_tb(1); // try chaining
        tcg_gen_movi_tl(cpu_PC, ctx.pc); // NOT PC+4, that was already done
        tcg_gen_exit_tb((uintptr_t)ctx.tb | 0x1);*/
        /* end unsafe */

        /* method 2: this is safe */
        tcg_gen_movi_tl(cpu_PC, ctx.pc); // NOT PC+4, that was already done
        tcg_gen_exit_tb(0);
        /* end safe */

        break;
    case BS_EXCP: // TODO: not yet handled for riscv
        tcg_gen_exit_tb(0);
        break;
    case BS_BRANCH:
    default:
        break;
    }
done_generating:
    gen_tb_end(tb, num_insns);
    *tcg_ctx.gen_opc_ptr = INDEX_op_end;
    if (search_pc) {
        j = tcg_ctx.gen_opc_ptr - tcg_ctx.gen_opc_buf;
        lj++;
        while (lj <= j)
            tcg_ctx.gen_opc_instr_start[lj++] = 0;
    } else {
        tb->size = ctx.pc - pc_start;
        tb->icount = num_insns;
    }
#ifdef DEBUG_DISAS
    LOG_DISAS("\n");
    if (qemu_loglevel_mask(CPU_LOG_TB_IN_ASM)) {
        qemu_log("IN: %s\n", lookup_symbol(pc_start));
        log_target_disas(env, pc_start, ctx.pc - pc_start, 0);
        qemu_log("\n");
    }
#endif
}

void gen_intermediate_code (CPUMIPSState *env, struct TranslationBlock *tb)
{
    gen_intermediate_code_internal(riscv_env_get_cpu(env), tb, false);
}

void gen_intermediate_code_pc (CPUMIPSState *env, struct TranslationBlock *tb)
{
    gen_intermediate_code_internal(riscv_env_get_cpu(env), tb, true);
}

void riscv_cpu_dump_state(CPUState *cs, FILE *f, fprintf_function cpu_fprintf,
                         int flags)
{
    MIPSCPU *cpu = MIPS_CPU(cs);
    CPUMIPSState *env = &cpu->env;
    int i;

    cpu_fprintf(f, "pc=0x" TARGET_FMT_lx "\n", env->active_tc.PC);
    for (i = 0; i < 32; i++) {
        if ((i & 3) == 0) {
            cpu_fprintf(f, "GPR%02d:", i);
        }
        cpu_fprintf(f, " %s " TARGET_FMT_lx, regnames[i], env->active_tc.gpr[i]);
        if ((i & 3) == 3) {
            cpu_fprintf(f, "\n");
        }
    }
    for (i = 0; i < 32; i++) {
        if ((i & 3) == 0) {
            cpu_fprintf(f, "CSR%02d:", i);
        }
        cpu_fprintf(f, " %s " TARGET_FMT_lx, cs_regnames[i], env->helper_csr[i]);
        if ((i & 3) == 3) {
            cpu_fprintf(f, "\n");
        }
    }
}

void riscv_tcg_init(void)
{
    int i;
    static int inited;

    /* Initialize various static tables. */
    if (inited) {
        return;
    }

    cpu_env = tcg_global_reg_new_ptr(TCG_AREG0, "env");

    // WARNING: cpu_gpr[0] is not allocated ON PURPOSE. Do not use it.
    // Use the gen_set_gpr and gen_get_gpr helper functions when accessing
    // registers, unless you specifically block reads/writes to reg 0
    TCGV_UNUSED(cpu_gpr[0]);
    for (i = 1; i < 32; i++) {
        cpu_gpr[i] = tcg_global_mem_new(TCG_AREG0,
                                        offsetof(CPUMIPSState, active_tc.gpr[i]),
                                        regnames[i]);
    }

/*    for (i = 0; i < 32; i++) {
        cpu_csr[i] = tcg_global_mem_new(TCG_AREG0,
                                        offsetof(CPUMIPSState, active_tc.csr[i]),
                                        cs_regnames[i]);
    }
*/

    for (i = 0; i < 32; i++) {
        cpu_fpr[i] = tcg_global_mem_new(TCG_AREG0,
                                        offsetof(CPUMIPSState, active_tc.fpr[i]),
                                        fpr_regnames[i]);
    }

    cpu_PC = tcg_global_mem_new(TCG_AREG0,
                                offsetof(CPUMIPSState, active_tc.PC), "PC");
    inited = 1;
}

#include "translate_init.c"

MIPSCPU *cpu_riscv_init(const char *cpu_model)
{
    MIPSCPU *cpu;
    CPUMIPSState *env;
    const riscv_def_t *def;

    def = cpu_riscv_find_by_name(cpu_model);
    if (!def)
        return NULL;
    cpu = MIPS_CPU(object_new(TYPE_MIPS_CPU));
    env = &cpu->env;
    env->cpu_model = def;

    object_property_set_bool(OBJECT(cpu), true, "realized", NULL);

    return cpu;
}

void cpu_state_reset(CPUMIPSState *env)
{
    MIPSCPU *cpu = riscv_env_get_cpu(env);
    CPUState *cs = CPU(cpu);

    env->active_tc.PC = (int32_t)0x10000; // STARTING PC VALUE
    cs->exception_index = EXCP_NONE;
}

void restore_state_to_opc(CPUMIPSState *env, TranslationBlock *tb, int pc_pos)
{
    env->active_tc.PC = tcg_ctx.gen_opc_pc[pc_pos];
}
