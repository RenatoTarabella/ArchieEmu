/*
 * arm2_disasm.c - Disassemblatore ARMv2a
 */
#include "arm2_disasm.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static const char *cond_names[16] = {
    "EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC",
    "HI", "LS", "GE", "LT", "GT", "LE", "", "NV"
};
static const char *dp_names[16] = {
    "AND", "EOR", "SUB", "RSB", "ADD", "ADC", "SBC", "RSC",
    "TST", "TEQ", "CMP", "CMN", "ORR", "MOV", "BIC", "MVN"
};
static const char *shift_names[4] = { "LSL", "LSR", "ASR", "ROR" };

typedef struct { char *p; size_t left; int len; } Out;

static void emit(Out *o, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->p, o->left, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    o->len += n;
    if ((size_t)n >= o->left) n = o->left ? (int)o->left - 1 : 0;
    o->p += n;
    o->left -= (size_t)n;
}

static const char *reg(int r)
{
    static const char *names[16] = {
        "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
        "R8", "R9", "R10", "R11", "R12", "R13", "R14", "PC"
    };
    return names[r & 15];
}

static void emit_hex(Out *o, uint32_t v)
{
    if (v < 10) emit(o, "%u", v);
    else        emit(o, "&%X", v);
}

/* Rm con shift immediato, come nell'operando 2 e negli offset di LDR/STR */
static void emit_shifted_reg(Out *o, uint32_t i)
{
    int rm = i & 15, type = (i >> 5) & 3, amount = (i >> 7) & 31;
    emit(o, "%s", reg(rm));
    if (i & 0x10) {
        emit(o, ",%s %s", shift_names[type], reg((i >> 8) & 15));
        return;
    }
    if (amount == 0) {
        if (type == 0) return;
        if (type == 3) { emit(o, ",RRX"); return; }
        amount = 32;
    }
    emit(o, ",%s #%d", shift_names[type], amount);
}

static void emit_reglist(Out *o, uint32_t list)
{
    int first = 1;
    emit(o, "{");
    for (int r = 0; r < 16; r++) {
        if (!(list & (1u << r))) continue;
        int end = r;
        while (end < 15 && (list & (1u << (end + 1)))) end++;
        emit(o, "%s%s", first ? "" : ",", reg(r));
        if (end >= r + 2)      { emit(o, "-%s", reg(end)); r = end; }
        first = 0;
    }
    emit(o, "}");
}

/* PSR come operando di MRS/MSR: CPSR o SPSR, con i campi scritti */
static void emit_psr(Out *o, uint32_t i, int fields)
{
    emit(o, "%s", (i & (1u << 22)) ? "SPSR" : "CPSR");
    if (!fields) return;
    int m = (int)(i >> 16) & 15;
    if (m == 9) { emit(o, "_all"); return; }
    if (m == 8) { emit(o, "_flg"); return; }
    if (m == 1) { emit(o, "_ctl"); return; }
    emit(o, "_");
    if (m & 8) emit(o, "f");
    if (m & 4) emit(o, "s");
    if (m & 2) emit(o, "x");
    if (m & 1) emit(o, "c");
}

/* v3: istruzioni dell'ARMv3 e indirizzi a 32 bit */
static int disasm(uint32_t i, uint32_t addr, int v3, char *out, size_t size)
{
    Out o = { out, size, 0 };
    const char *cc = cond_names[i >> 28];
    uint32_t amask = v3 ? 0xFFFFFFFFu : 0x03FFFFFFu;
    if (size) out[0] = 0;

    if (v3 && (i & 0x0D900000u) == 0x01000000u && ((i >> 25) & 1 || (i & 0x90u) != 0x90u)) {
        if ((i & 0x0FBF0FFFu) == 0x010F0000u) {
            emit(&o, "MRS%s %s,", cc, reg((i >> 12) & 15));
            emit_psr(&o, i, 0);
            return o.len;
        }
        if ((i & 0x0FB0F000u) == 0x0320F000u || (i & 0x0FB0FFF0u) == 0x0120F000u) {
            emit(&o, "MSR%s ", cc);
            emit_psr(&o, i, 1);
            if (i & (1u << 25)) {
                int rot = ((i >> 8) & 15) * 2;
                uint32_t imm = i & 0xFF;
                imm = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
                emit(&o, ",#");
                emit_hex(&o, imm);
            } else {
                emit(&o, ",%s", reg(i & 15));
            }
            return o.len;
        }
    }
    if (v3 && (i & 0x0F000010u) == 0x0E000010u) {
        emit(&o, "%s%s P%d,%d,%s,C%d,C%d", (i & (1u << 20)) ? "MRC" : "MCR", cc,
             (int)(i >> 8) & 15, (int)(i >> 21) & 7, reg((i >> 12) & 15),
             (int)(i >> 16) & 15, (int)i & 15);
        if ((i >> 5) & 7) emit(&o, ",%d", (int)(i >> 5) & 7);
        return o.len;
    }

    switch ((i >> 25) & 7) {
    case 0:
    case 1:
        if ((i & 0x0FC000F0u) == 0x00000090u) {
            int a = (i >> 21) & 1;
            emit(&o, "%s%s%s %s,%s,%s", a ? "MLA" : "MUL", cc, (i & (1u << 20)) ? "S" : "",
                 reg((i >> 16) & 15), reg(i & 15), reg((i >> 8) & 15));
            if (a) emit(&o, ",%s", reg((i >> 12) & 15));
            return o.len;
        }
        if ((i & 0x0FB00FF0u) == 0x01000090u) {
            emit(&o, "SWP%s%s %s,%s,[%s]", cc, (i & (1u << 22)) ? "B" : "",
                 reg((i >> 12) & 15), reg(i & 15), reg((i >> 16) & 15));
            return o.len;
        }
        if (!(i & (1u << 25)) && (i & 0x90u) == 0x90u) break;
        {
            int op = (i >> 21) & 15, s = (i >> 20) & 1;
            int rn = (i >> 16) & 15, rd = (i >> 12) & 15;
            int test = op >= 8 && op <= 11;
            if (test && !s) { emit(&o, "NOP%s ; %08X", cc, i); return o.len; }
            emit(&o, "%s%s", dp_names[op], cc);
            if (test)            { if (rd == 15) emit(&o, "P"); }
            else if (s)          emit(&o, "S");
            emit(&o, " ");
            if (!test)                   emit(&o, "%s,", reg(rd));
            if (op != 0xD && op != 0xF)  emit(&o, "%s,", reg(rn));
            if (i & (1u << 25)) {
                int rot = ((i >> 8) & 15) * 2;
                uint32_t imm = i & 0xFF;
                imm = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
                emit(&o, "#");
                emit_hex(&o, imm);
            } else {
                emit_shifted_reg(&o, i);
            }
            return o.len;
        }
    case 3:
        if (i & 0x10) break;
        /* fallthrough */
    case 2: {
        int p = (i >> 24) & 1, u = (i >> 23) & 1, b = (i >> 22) & 1;
        int w = (i >> 21) & 1, l = (i >> 20) & 1;
        int rn = (i >> 16) & 15, rd = (i >> 12) & 15;
        emit(&o, "%s%s%s%s %s,", l ? "LDR" : "STR", cc, b ? "B" : "", (!p && w) ? "T" : "", reg(rd));
        if (!(i & (1u << 25)) && rn == 15 && p && !w) {
            /* indirizzo relativo al PC: mostriamo il bersaglio */
            uint32_t off = i & 0xFFF;
            emit(&o, "&%X", (addr + 8 + (u ? off : (uint32_t)-(int32_t)off)) & amask);
            return o.len;
        }
        emit(&o, "[%s", reg(rn));
        if (!p) emit(&o, "]");
        if (i & (1u << 25)) {
            emit(&o, ",%s", u ? "" : "-");
            emit_shifted_reg(&o, i);
        } else if (i & 0xFFF) {
            emit(&o, ",#%s", u ? "" : "-");
            emit_hex(&o, i & 0xFFF);
        }
        if (p) emit(&o, "]%s", w ? "!" : "");
        return o.len;
    }
    case 4: {
        int p = (i >> 24) & 1, u = (i >> 23) & 1, s = (i >> 22) & 1;
        int w = (i >> 21) & 1, l = (i >> 20) & 1, rn = (i >> 16) & 15;
        static const char *modes[4] = { "DA", "IA", "DB", "IB" };
        static const char *stack_ld[4] = { "FA", "FD", "EA", "ED" };
        static const char *stack_st[4] = { "ED", "EA", "FD", "FA" };
        int pu = (p << 1) | u;
        const char *m = rn == 13 ? (l ? stack_ld[pu] : stack_st[pu]) : modes[pu];
        emit(&o, "%s%s%s %s%s,", l ? "LDM" : "STM", cc, m, reg(rn), w ? "!" : "");
        emit_reglist(&o, i & 0xFFFF);
        if (s) emit(&o, "^");
        return o.len;
    }
    case 5: {
        uint32_t off = (uint32_t)((int32_t)(i << 8) >> 6);
        emit(&o, "%s%s &%X", (i & (1u << 24)) ? "BL" : "B", cc, (addr + 8 + off) & amask & ~3u);
        return o.len;
    }
    case 7:
        if (i & (1u << 24)) {
            emit(&o, "SWI%s &%X", cc, i & 0x00FFFFFFu);
            return o.len;
        }
        break;
    default:
        break;
    }
    emit(&o, "DCD &%08X", i);
    return o.len;
}

int arm2_disasm(uint32_t instr, uint32_t addr, char *out, size_t size)
{
    return disasm(instr, addr, 0, out, size);
}

int arm6_disasm(uint32_t instr, uint32_t addr, char *out, size_t size)
{
    return disasm(instr, addr, 1, out, size);
}
