/*
 * arm2.c - Modulo CPU: ARMv2a (ARM2 + SWP dell'ARM3)
 *
 * Convenzione interna: durante l'esecuzione r[15] punta gia' all'istruzione
 * successiva (indirizzo + 4). Chi legge R15 come operando vede +8 (o +12
 * con shift da registro e nelle STR/STM), come nella pipeline vera.
 */
#include "arm2.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* utilita'                                                           */
/* ------------------------------------------------------------------ */

static uint8_t cond_table[16][16];   /* [cond][NZCV] -> passa? */

static void build_cond_table(void)
{
    static int built = 0;
    if (built) return;
    for (int f = 0; f < 16; f++) {
        int n = (f >> 3) & 1, z = (f >> 2) & 1, c = (f >> 1) & 1, v = f & 1;
        int pass[16] = {
            z, !z, c, !c, n, !n, v, !v,             /* EQ NE CS CC MI PL VS VC */
            c && !z, !c || z,                       /* HI LS                   */
            n == v, n != v,                         /* GE LT                   */
            !z && n == v, z || n != v,              /* GT LE                   */
            1, 0                                    /* AL NV                   */
        };
        for (int k = 0; k < 16; k++) cond_table[k][f] = (uint8_t)pass[k];
    }
    built = 1;
}

/* Tempi dell'ARM2 (datasheet): ogni istruzione dichiara i suoi cicli
   S (sequenziali), N (non sequenziali), I (interni) e quanti di questi
   sono fetch di istruzioni. arm2_step li converte in tick con i costi
   impostati dalla macchina (vedi Arm2.s_ticks & co. in arm2.h). */
#define CYC(s, n, i, f) ((uint32_t)(s) | (uint32_t)(n) << 8 | (uint32_t)(i) << 16 | (uint32_t)(f) << 24)
#define CYC_EXCEPTION   CYC(2, 1, 0, 3)

static inline uint32_t ror32(uint32_t v, unsigned n)
{
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

static inline int popcount16(uint32_t v)
{
    int n = 0;
    for (v &= 0xFFFF; v; v &= v - 1) n++;
    return n;
}

/* PC avanzato di 'delta' senza sporcare i bit del PSR */
static inline uint32_t r15_plus(const Arm2 *c, uint32_t delta)
{
    return ((c->r[15] + delta) & ARM_PC_MASK) | (c->r[15] & ARM_PSR_MASK);
}

/* ------------------------------------------------------------------ */
/* banchi di registri                                                 */
/* ------------------------------------------------------------------ */

static void bank_save(Arm2 *c, int mode)
{
    if (mode == ARM_MODE_FIQ) {
        memcpy(c->fiq_r8_14, &c->r[8], 7 * sizeof(uint32_t));
        return;
    }
    memcpy(c->usr_r8_14, &c->r[8], 5 * sizeof(uint32_t));
    uint32_t *hi = mode == ARM_MODE_IRQ ? c->irq_r13_14
                 : mode == ARM_MODE_SVC ? c->svc_r13_14
                 : &c->usr_r8_14[5];
    hi[0] = c->r[13];
    hi[1] = c->r[14];
}

static void bank_load(Arm2 *c, int mode)
{
    if (mode == ARM_MODE_FIQ) {
        memcpy(&c->r[8], c->fiq_r8_14, 7 * sizeof(uint32_t));
        return;
    }
    memcpy(&c->r[8], c->usr_r8_14, 5 * sizeof(uint32_t));
    const uint32_t *hi = mode == ARM_MODE_IRQ ? c->irq_r13_14
                       : mode == ARM_MODE_SVC ? c->svc_r13_14
                       : &c->usr_r8_14[5];
    c->r[13] = hi[0];
    c->r[14] = hi[1];
}

void arm2_set_r15(Arm2 *c, uint32_t value)
{
    int oldm = (int)(c->r[15] & 3), newm = (int)(value & 3);
    if ((value ^ c->r[15]) & ARM_PC_MASK) c->pipe_valid = 0;   /* cambia il PC: si svuota */
    if (oldm != newm) {
        bank_save(c, oldm);
        bank_load(c, newm);
    }
    c->r[15] = value;
}

void arm2_set_pc(Arm2 *c, uint32_t pc)
{
    c->r[15] = (c->r[15] & ARM_PSR_MASK) | (pc & ARM_PC_MASK);
    c->pipe_valid = 0;
}

/* Scrittura di R15 "con S": in modo utente I, F e il modo sono protetti */
static void write_r15_psr(Arm2 *c, uint32_t value)
{
    if ((c->r[15] & 3) == ARM_MODE_USR)
        value = (value & (ARM_PC_MASK | 0xF0000000u)) | (c->r[15] & (ARM_I | ARM_F | 3));
    arm2_set_r15(c, value);
    c->pipe_valid = 0;
}

/* Aggiorna solo i bit di PSR (TEQP & co.), PC invariato */
static void write_psr_only(Arm2 *c, uint32_t value)
{
    uint32_t mask = (c->r[15] & 3) == ARM_MODE_USR ? 0xF0000000u : ARM_PSR_MASK;
    arm2_set_r15(c, (c->r[15] & ~mask) | (value & mask));
}

static int user_reg_is_banked(const Arm2 *c, int n)
{
    int m = (int)(c->r[15] & 3);
    if (m == ARM_MODE_USR || n < 8 || n == 15) return 0;
    if (m == ARM_MODE_FIQ) return 1;
    return n >= 13;
}

uint32_t arm2_get_user_reg(const Arm2 *c, int n)
{
    return user_reg_is_banked(c, n) ? c->usr_r8_14[n - 8] : c->r[n];
}

void arm2_set_user_reg(Arm2 *c, int n, uint32_t v)
{
    if (user_reg_is_banked(c, n)) c->usr_r8_14[n - 8] = v;
    else c->r[n] = v;
}

const char *arm2_mode_name(int mode)
{
    static const char *names[4] = { "USR", "FIQ", "IRQ", "SVC" };
    return names[mode & 3];
}

/* ------------------------------------------------------------------ */
/* eccezioni                                                          */
/* ------------------------------------------------------------------ */

static void take_exception(Arm2 *c, uint32_t vector, int mode, uint32_t link, int mask_fiq)
{
    if (c->exception_hook) c->exception_hook(c, vector, link, c->exception_user);
    uint32_t psr = (c->r[15] & 0xF0000000u) | ARM_I | (c->r[15] & ARM_F);
    if (mask_fiq) psr |= ARM_F;
    arm2_set_r15(c, psr | (uint32_t)mode | (c->r[15] & ARM_PC_MASK));
    c->r[14] = link;
    arm2_set_pc(c, vector);
}

void arm2_init(Arm2 *c, const ArmBus *bus)
{
    build_cond_table();
    memset(c, 0, sizeof *c);
    c->bus = *bus;
    /* predefinito: ogni ciclo vale un tick, memoria tutta uguale */
    c->s_ticks = 1;
    c->n_ticks = 1;
    c->slow_base = 0xFFFFFFFFu;
    arm2_reset(c);
}

void arm2_reset(Arm2 *c)
{
    /* Al reset: modo SVC, IRQ e FIQ disabilitati, PC = 0 */
    uint32_t link = c->r[15];
    arm2_set_r15(c, ARM_I | ARM_F | ARM_MODE_SVC);
    c->r[14] = link;
    c->halted = 0;
}

/* ------------------------------------------------------------------ */
/* accessi alla memoria                                               */
/* ------------------------------------------------------------------ */

static inline uint32_t load_word(Arm2 *c, uint32_t addr, int *abort)
{
    uint32_t v = c->bus.read32(c->bus.ctx, addr & ~3u, abort);
    return ror32(v, (addr & 3) * 8);   /* LDR non allineata: parola ruotata */
}

/* ------------------------------------------------------------------ */
/* barrel shifter                                                     */
/* ------------------------------------------------------------------ */

/* Shift con quantita' immediata (campo di 5 bit, con i casi speciali #0) */
static inline uint32_t shift_imm(const Arm2 *c, uint32_t v, int type, int amount, int *carry)
{
    switch (type) {
    case 0: /* LSL */
        if (amount == 0) return v;
        *carry = (v >> (32 - amount)) & 1;
        return v << amount;
    case 1: /* LSR, #0 significa #32 */
        if (amount == 0) { *carry = v >> 31; return 0; }
        *carry = (v >> (amount - 1)) & 1;
        return v >> amount;
    case 2: /* ASR, #0 significa #32 */
        if (amount == 0) { *carry = v >> 31; return (uint32_t)((int32_t)v >> 31); }
        *carry = (v >> (amount - 1)) & 1;
        return (uint32_t)((int32_t)v >> amount);
    default: /* ROR, #0 significa RRX */
        if (amount == 0) {
            uint32_t r = (v >> 1) | ((c->r[15] & ARM_C) ? 0x80000000u : 0);
            *carry = v & 1;
            return r;
        }
        *carry = (v >> (amount - 1)) & 1;
        return ror32(v, (unsigned)amount);
    }
}

/* Shift con quantita' da registro (byte basso di Rs, 0..255) */
static inline uint32_t shift_reg(uint32_t v, int type, uint32_t amount, int *carry)
{
    if (amount == 0) return v;
    switch (type) {
    case 0: /* LSL */
        if (amount < 32)  { *carry = (v >> (32 - amount)) & 1; return v << amount; }
        *carry = amount == 32 ? (int)(v & 1) : 0;
        return 0;
    case 1: /* LSR */
        if (amount < 32)  { *carry = (v >> (amount - 1)) & 1; return v >> amount; }
        *carry = amount == 32 ? (int)(v >> 31) : 0;
        return 0;
    case 2: /* ASR */
        if (amount < 32)  { *carry = (v >> (amount - 1)) & 1; return (uint32_t)((int32_t)v >> amount); }
        *carry = (int)(v >> 31);
        return (uint32_t)((int32_t)v >> 31);
    default: /* ROR */
        amount &= 31;
        if (amount == 0) { *carry = (int)(v >> 31); return v; }
        *carry = (v >> (amount - 1)) & 1;
        return ror32(v, amount);
    }
}

/* ------------------------------------------------------------------ */
/* elaborazione dati                                                  */
/* ------------------------------------------------------------------ */

static inline uint32_t alu_add(uint32_t a, uint32_t b, uint32_t cin, int *carry, int *overflow)
{
    uint64_t r64 = (uint64_t)a + b + cin;
    uint32_t r = (uint32_t)r64;
    *carry = (int)(r64 >> 32);
    *overflow = (int)(((~(a ^ b)) & (a ^ r)) >> 31);
    return r;
}

static uint32_t exec_data_processing(Arm2 *c, uint32_t i)
{
    int op = (i >> 21) & 15;
    int s  = (i >> 20) & 1;
    int rn = (i >> 16) & 15;
    int rd = (i >> 12) & 15;
    uint32_t cyc = CYC(1, 0, 0, 1);
    int carry = (c->r[15] & ARM_C) ? 1 : 0;
    uint32_t op2, a;
    uint32_t pc_extra = 4;           /* R15 letto come PC+8 */

    if (i & (1u << 25)) {
        int rot = ((i >> 8) & 15) * 2;
        op2 = ror32(i & 0xFF, (unsigned)rot);
        if (rot) carry = (int)(op2 >> 31);
    } else {
        int rm = i & 15, type = (i >> 5) & 3;
        if (i & 0x10) {
            /* shift da registro: un ciclo I in piu', R15 si legge come PC+12 */
            pc_extra = 8;
            cyc += CYC(0, 0, 1, 0);
            int rs = (i >> 8) & 15;
            uint32_t amount = (rs == 15 ? r15_plus(c, pc_extra) : c->r[rs]) & 0xFF;
            uint32_t vm = rm == 15 ? r15_plus(c, pc_extra) : c->r[rm];
            op2 = shift_reg(vm, type, amount, &carry);
        } else {
            uint32_t vm = rm == 15 ? r15_plus(c, pc_extra) : c->r[rm];
            op2 = shift_imm(c, vm, type, (i >> 7) & 31, &carry);
        }
    }

    /* R15 come primo operando: solo i bit del PC */
    a = rn == 15 ? ((c->r[15] + pc_extra) & ARM_PC_MASK) : c->r[rn];

    uint32_t res;
    int v = (c->r[15] & ARM_V) ? 1 : 0;
    int cin = (c->r[15] & ARM_C) ? 1 : 0;
    int arith = 1;

    switch (op) {
    case 0x0: case 0x8: res = a & op2;  arith = 0; break;          /* AND TST */
    case 0x1: case 0x9: res = a ^ op2;  arith = 0; break;          /* EOR TEQ */
    case 0x2: case 0xA: res = alu_add(a, ~op2, 1, &carry, &v); break;  /* SUB CMP */
    case 0x3: res = alu_add(op2, ~a, 1, &carry, &v); break;            /* RSB */
    case 0x4: case 0xB: res = alu_add(a, op2, 0, &carry, &v); break;   /* ADD CMN */
    case 0x5: res = alu_add(a, op2, (uint32_t)cin, &carry, &v); break; /* ADC */
    case 0x6: res = alu_add(a, ~op2, (uint32_t)cin, &carry, &v); break;/* SBC */
    case 0x7: res = alu_add(op2, ~a, (uint32_t)cin, &carry, &v); break;/* RSC */
    case 0xC: res = a | op2;  arith = 0; break;                    /* ORR */
    case 0xD: res = op2;      arith = 0; break;                    /* MOV */
    case 0xE: res = a & ~op2; arith = 0; break;                    /* BIC */
    default:  res = ~op2;     arith = 0; break;                    /* MVN */
    }
    (void)arith;

    if (op >= 0x8 && op <= 0xB) {
        /* TST TEQ CMP CMN: senza S sono NOP sull'ARM2 */
        if (!s) return cyc;
        if (rd == 15) {                     /* forma "P": TEQP ecc. */
            write_psr_only(c, res);
            return cyc;
        }
    } else if (rd == 15) {
        if (s) write_r15_psr(c, res);
        else   arm2_set_pc(c, res);
        return cyc + CYC(1, 1, 0, 2);      /* svuotamento della pipeline: 2S+1N */
    } else {
        c->r[rd] = res;
        if (!s) return cyc;
    }

    uint32_t f = (res & 0x80000000u) | (res == 0 ? ARM_Z : 0)
               | (carry ? ARM_C : 0) | (v ? ARM_V : 0);
    c->r[15] = (c->r[15] & 0x0FFFFFFFu) | f;
    return cyc;
}

/* ------------------------------------------------------------------ */
/* moltiplicazione e swap                                             */
/* ------------------------------------------------------------------ */

static uint32_t exec_multiply(Arm2 *c, uint32_t i)
{
    int rd = (i >> 16) & 15, rn = (i >> 12) & 15, rs = (i >> 8) & 15, rm = i & 15;
    uint32_t vs = c->r[rs];
    uint32_t res = c->r[rm] * vs;
    if (i & (1u << 21)) res += c->r[rn];               /* MLA */

    if (rd != 15) c->r[rd] = res;
    if (i & (1u << 20)) {
        /* N e Z validi; C "senza significato" sull'ARM2: lo lasciamo; V intatto */
        c->r[15] = (c->r[15] & 0x3FFFFFFFu) | (res & ARM_N) | (res == 0 ? ARM_Z : 0);
    }
    /* Booth: 2 bit di Rs per ciclo, finche' restano bit significativi */
    int m = 0;
    do { m++; vs >>= 2; } while (vs && m < 16);
    return CYC(1, 0, m, 1);
}

static uint32_t exec_swap(Arm2 *c, uint32_t i)
{
    int rn = (i >> 16) & 15, rd = (i >> 12) & 15, rm = i & 15;
    uint32_t addr = c->r[rn];
    int abort = 0;
    if (addr & ~0x03FFFFFFu) {
        take_exception(c, ARM_VEC_ADDRESS, ARM_MODE_SVC, r15_plus(c, 4), 0);
        return CYC_EXCEPTION;
    }
    uint32_t src = c->r[rm];
    uint32_t old;
    if (i & (1u << 22)) {
        old = c->bus.read8(c->bus.ctx, addr, &abort);
        if (!abort) c->bus.write8(c->bus.ctx, addr, (uint8_t)src, &abort);
    } else {
        old = load_word(c, addr, &abort);
        if (!abort) c->bus.write32(c->bus.ctx, addr, src, &abort);
    }
    if (abort) {
        take_exception(c, ARM_VEC_DABORT, ARM_MODE_SVC, r15_plus(c, 4), 0);
        return CYC_EXCEPTION;
    }
    if (rd != 15) c->r[rd] = old;
    return CYC(1, 2, 1, 1);
}

/* ------------------------------------------------------------------ */
/* LDR / STR                                                          */
/* ------------------------------------------------------------------ */

static uint32_t exec_single_transfer(Arm2 *c, uint32_t i)
{
    int p = (i >> 24) & 1, u = (i >> 23) & 1, b = (i >> 22) & 1;
    int w = (i >> 21) & 1, l = (i >> 20) & 1;
    int rn = (i >> 16) & 15, rd = (i >> 12) & 15;
    uint32_t offset;

    if (i & (1u << 25)) {
        int rm = i & 15, dummy = 0;
        uint32_t vm = rm == 15 ? r15_plus(c, 4) : c->r[rm];
        offset = shift_imm(c, vm, (i >> 5) & 3, (i >> 7) & 31, &dummy);
    } else {
        offset = i & 0xFFF;
    }

    uint32_t base = rn == 15 ? ((c->r[15] + 4) & ARM_PC_MASK) : c->r[rn];
    uint32_t moved = u ? base + offset : base - offset;
    uint32_t addr = p ? moved : base;
    int writeback = (!p || w) && rn != 15;

    if (addr & ~0x03FFFFFFu) {
        take_exception(c, ARM_VEC_ADDRESS, ARM_MODE_SVC, r15_plus(c, 4), 0);
        return CYC_EXCEPTION;
    }

    int abort = 0;
    /* LDRT/STRT (post-indicizzati con W): accesso con i permessi dell'utente */
    c->trans_user = !p && w;
    if (l) {
        uint32_t v = b ? c->bus.read8(c->bus.ctx, addr, &abort) : load_word(c, addr, &abort);
        c->trans_user = 0;
        if (abort) {
            take_exception(c, ARM_VEC_DABORT, ARM_MODE_SVC, r15_plus(c, 4), 0);
            return CYC_EXCEPTION;
        }
        if (writeback) c->r[rn] = moved;
        if (rd == 15) { arm2_set_pc(c, v); return CYC(2, 2, 1, 3); }
        c->r[rd] = v;
        return CYC(1, 1, 1, 1);
    }

    uint32_t v = rd == 15 ? r15_plus(c, 8) : c->r[rd];   /* STR PC: PC+12 */
    if (b) c->bus.write8(c->bus.ctx, addr, (uint8_t)v, &abort);
    /* l'ARM2 mette sul bus l'indirizzo intero anche per le parole: la
       memoria ignora i bit 1-0, ma il MEMC li usa per la CAM */
    else   c->bus.write32(c->bus.ctx, addr, v, &abort);
    c->trans_user = 0;
    if (abort) {
        take_exception(c, ARM_VEC_DABORT, ARM_MODE_SVC, r15_plus(c, 4), 0);
        return CYC_EXCEPTION;
    }
    if (writeback) c->r[rn] = moved;
    return CYC(0, 2, 0, 1);
}

/* ------------------------------------------------------------------ */
/* LDM / STM                                                          */
/* ------------------------------------------------------------------ */

static uint32_t exec_block_transfer(Arm2 *c, uint32_t i)
{
    int p = (i >> 24) & 1, u = (i >> 23) & 1, s = (i >> 22) & 1;
    int w = (i >> 21) & 1, l = (i >> 20) & 1;
    int rn = (i >> 16) & 15;
    uint32_t list = i & 0xFFFF;
    int n = popcount16(list);
    if (n == 0) return CYC(1, 0, 0, 1);

    uint32_t base = rn == 15 ? ((c->r[15] + 4) & ARM_PC_MASK) : c->r[rn];
    uint32_t wb = u ? base + 4u * (uint32_t)n : base - 4u * (uint32_t)n;
    uint32_t addr;
    if (u) addr = p ? base + 4 : base;
    else   addr = p ? base - 4u * (uint32_t)n : base - 4u * (uint32_t)n + 4;
    if (rn == 15) w = 0;

    if (addr & ~0x03FFFFFFu) {
        take_exception(c, ARM_VEC_ADDRESS, ARM_MODE_SVC, r15_plus(c, 4), 0);
        return CYC_EXCEPTION;
    }

    /* S senza PC nella lista (o in una STM): registri del banco utente */
    int user_bank = s && (!l || !(list & 0x8000));
    int abort = 0;

    if (!l) {
        int first = 1;
        for (int r = 0; r < 16; r++) {
            if (!(list & (1u << r))) continue;
            uint32_t v;
            if (r == 15)        v = r15_plus(c, 8);
            else if (user_bank) v = arm2_get_user_reg(c, r);
            else                v = c->r[r];
            c->bus.write32(c->bus.ctx, addr & 0x03FFFFFFu, v, &abort);
            addr += 4;
            /* ARM2: il write-back avviene dopo il primo trasferimento, quindi
               la base nella lista viene salvata vecchia solo se e' la prima */
            if (first && w) c->r[rn] = wb;
            first = 0;
        }
        if (abort) take_exception(c, ARM_VEC_DABORT, ARM_MODE_SVC, r15_plus(c, 4), 0);
        return CYC(n - 1, 2, 0, 1);
    }

    uint32_t vals[16];
    for (int r = 0; r < 16; r++) {
        if (!(list & (1u << r))) continue;
        vals[r] = c->bus.read32(c->bus.ctx, addr & 0x03FFFFFCu, &abort);
        addr += 4;
    }
    if (abort) {
        if (w) c->r[rn] = wb;
        take_exception(c, ARM_VEC_DABORT, ARM_MODE_SVC, r15_plus(c, 4), 0);
        return CYC_EXCEPTION;
    }
    if (w) c->r[rn] = wb;
    for (int r = 0; r < 15; r++) {
        if (!(list & (1u << r))) continue;
        if (user_bank) arm2_set_user_reg(c, r, vals[r]);
        else           c->r[r] = vals[r];
    }
    if (list & 0x8000) {
        if (s) write_r15_psr(c, vals[15]);
        else   arm2_set_pc(c, vals[15]);
        return CYC(n + 1, 2, 1, 3);
    }
    return CYC(n, 1, 1, 1);
}

/* ------------------------------------------------------------------ */
/* ciclo principale                                                   */
/* ------------------------------------------------------------------ */

/* cicli S/N/I -> tick, con i fetch dalla memoria lenta (ROM) piu' cari */
static int account(Arm2 *c, uint32_t cyc, uint32_t pc)
{
    uint32_t s = cyc & 0xFF, n = (cyc >> 8) & 0xFF, i = (cyc >> 16) & 0xFF, f = cyc >> 24;
    uint32_t ticks = s * c->s_ticks + n * c->n_ticks + i;
    if (pc >= c->slow_base) ticks += f * c->slow_fetch_extra;
    ticks += c->extra_cycles;
    c->extra_cycles = 0;
    c->cycles += ticks;
    return (int)ticks;
}

int arm2_step(Arm2 *c)
{
    /* Interrupt: campionati tra un'istruzione e l'altra. R14 = PC + 4 */
    if (c->fiq_line && !(c->r[15] & ARM_F)) {
        take_exception(c, ARM_VEC_FIQ, ARM_MODE_FIQ, r15_plus(c, 4), 1);
        return account(c, CYC_EXCEPTION, 0);
    }
    if (c->irq_line && !(c->r[15] & ARM_I)) {
        take_exception(c, ARM_VEC_IRQ, ARM_MODE_IRQ, r15_plus(c, 4), 0);
        return account(c, CYC_EXCEPTION, 0);
    }

    uint32_t pc = c->r[15] & ARM_PC_MASK;
    int abort = 0;
    if (!c->pipe_valid || c->pipe_addr != pc) {
        /* pipeline vuota: si leggono l'istruzione e la successiva */
        int a0 = 0, a1 = 0;
        c->pipe[0] = c->bus.read32(c->bus.ctx, pc, &a0);
        c->pipe[1] = c->bus.read32(c->bus.ctx, (pc + 4) & ARM_PC_MASK, &a1);
        c->pipe_abort[0] = (uint8_t)a0;
        c->pipe_abort[1] = (uint8_t)a1;
    }
    uint32_t i = c->pipe[0];
    abort = c->pipe_abort[0];
    /* all'inizio dell'esecuzione si legge gia' l'istruzione ad A+8 */
    int a2 = 0;
    uint32_t next = c->bus.read32(c->bus.ctx, (pc + 8) & ARM_PC_MASK, &a2);
    c->pipe[0] = c->pipe[1];
    c->pipe_abort[0] = c->pipe_abort[1];
    c->pipe[1] = next;
    c->pipe_abort[1] = (uint8_t)a2;
    c->pipe_addr = (pc + 4) & ARM_PC_MASK;
    c->pipe_valid = 1;
    c->r[15] = r15_plus(c, 4);

    if (abort) {
        take_exception(c, ARM_VEC_PABORT, ARM_MODE_SVC, c->r[15], 0);
        return account(c, CYC_EXCEPTION, pc);
    }

    c->instructions++;
    if (c->trace_hook) c->trace_hook(c, pc, i);
    uint32_t cyc;

    if (!cond_table[i >> 28][c->r[15] >> 28]) {
        cyc = CYC(1, 0, 0, 1);
    } else {
        switch ((i >> 25) & 7) {
        case 0:
            if ((i & 0x0FC000F0u) == 0x00000090u)      cyc = exec_multiply(c, i);
            else if ((i & 0x0FB00FF0u) == 0x01000090u) cyc = exec_swap(c, i);
            else if ((i & 0x90u) == 0x90u)             goto undefined;
            else                                       cyc = exec_data_processing(c, i);
            break;
        case 1:
            cyc = exec_data_processing(c, i);
            break;
        case 3:
            if (i & 0x10) goto undefined;
            /* fallthrough */
        case 2:
            cyc = exec_single_transfer(c, i);
            break;
        case 4:
            cyc = exec_block_transfer(c, i);
            break;
        case 5: {                                      /* B, BL */
            uint32_t off = (uint32_t)((int32_t)(i << 8) >> 6);
            if (i & (1u << 24)) c->r[14] = c->r[15];   /* PC+4 con PSR */
            arm2_set_pc(c, (c->r[15] & ARM_PC_MASK) + 4 + off);
            cyc = CYC(2, 1, 0, 3);
            break;
        }
        case 7:
            if (i & (1u << 24)) {                      /* SWI */
                if (c->swi_hook && c->swi_hook(c, i & 0x00FFFFFFu, c->swi_user)) {
                    cyc = CYC_EXCEPTION;
                    break;
                }
                take_exception(c, ARM_VEC_SWI, ARM_MODE_SVC, c->r[15], 0);
                cyc = CYC_EXCEPTION;
                break;
            }
            /* fallthrough: CDP/MRC/MCR senza coprocessore */
        default:                                       /* LDC/STC, ecc. */
        undefined:
            take_exception(c, ARM_VEC_UNDEF, ARM_MODE_SVC, c->r[15], 0);
            cyc = CYC_EXCEPTION;
            break;
        }
    }
    return account(c, cyc, pc);
}

uint64_t arm2_run(Arm2 *c, uint64_t max_cycles)
{
    uint64_t start = c->cycles;
    while (!c->halted && c->cycles - start < max_cycles)
        arm2_step(c);
    return c->cycles - start;
}
