/*
 * arm6.c - Modulo CPU: ARMv3 (ARM610/ARM710) con CP15 e MMU
 *
 * Stessa struttura dell'ARM2 (arm2.c): durante l'esecuzione r[15] punta gia'
 * all'istruzione successiva (indirizzo + 4), la pipeline a tre stadi tiene
 * le due istruzioni gia' lette, i tempi si contano in cicli S/N/I.
 *
 * Differenze dall'ARMv2a:
 *  - PC a 32 bit e CPSR separato; nei modi a 26 bit (bit 4 del modo a 0)
 *    R15 si legge ancora come PC + PSR e le scritture "con S" di R15
 *    cambiano i flag come sull'ARM2;
 *  - modi a 32 bit con SPSR (MRS/MSR, MOVS PC e LDM^ copiano l'SPSR);
 *  - le eccezioni entrano nei modi a 32 bit se il bit P del registro di
 *    controllo e' 1, altrimenti nei modi a 26 bit come sull'ARM2;
 *  - l'address exception esiste solo coi dati a 26 bit (bit D a 0);
 *  - MMU: sezioni da 1 MB, pagine da 64 KB e 4 KB con quattro sottopagine,
 *    16 domini, FSR e FAR per gli abort dei dati.
 */
#include "arm6.h"
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
            z, !z, c, !c, n, !n, v, !v,
            c && !z, !c || z,
            n == v, n != v,
            !z && n == v, z || n != v,
            1, 0
        };
        for (int k = 0; k < 16; k++) cond_table[k][f] = (uint8_t)pass[k];
    }
    built = 1;
}

#define CYC(s, n, i, f) ((uint32_t)(s) | (uint32_t)(n) << 8 | (uint32_t)(i) << 16 | (uint32_t)(f) << 24)
#define CYC_EXCEPTION   CYC(2, 1, 0, 3)

#define PC26_MASK 0x03FFFFFCu

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

static inline int is26(const Arm6 *c)     { return !(c->cpsr & 0x10u); }
static inline int is_user(const Arm6 *c)  { return (c->cpsr & 0x0Fu) == 0; }
static inline uint32_t pc_mask(const Arm6 *c) { return is26(c) ? PC26_MASK : 0xFFFFFFFCu; }

/* i bit del PSR come stanno in R15 nei modi a 26 bit */
static inline uint32_t psr26(const Arm6 *c)
{
    return (c->cpsr & 0xF0000000u) | ((c->cpsr & 0xC0u) << 20) | (c->cpsr & 3u);
}

/* R15 letto come operando "Rm" (con il PSR nei modi a 26 bit), PC + 4 + extra */
static inline uint32_t r15_full(const Arm6 *c, uint32_t extra)
{
    uint32_t pc = c->r[15] + extra;
    return is26(c) ? (pc & PC26_MASK) | psr26(c) : pc;
}

/* R15 letto come indirizzo ("Rn"): solo il PC */
static inline uint32_t r15_addr(const Arm6 *c, uint32_t extra)
{
    return (c->r[15] + extra) & pc_mask(c);
}

uint32_t arm6_r15_26(const Arm6 *c)
{
    return (c->r[15] & PC26_MASK) | psr26(c);
}

/* ------------------------------------------------------------------ */
/* banchi di registri                                                 */
/* ------------------------------------------------------------------ */

/* banco di un modo: 0 USR, 1 FIQ, 2 IRQ, 3 SVC, 4 ABT, 5 UND */
static int bank_of(uint32_t mode)
{
    switch (mode & 0x1Fu) {
    case 0x01: case 0x11: return 1;
    case 0x02: case 0x12: return 2;
    case 0x03: case 0x13: return 3;
    case 0x17:            return 4;
    case 0x1B:            return 5;
    default:              return 0;
    }
}

static int mode_valid(uint32_t m)
{
    m &= 0x1Fu;
    return m <= 3 || m == 0x10 || m == 0x11 || m == 0x12 || m == 0x13 || m == 0x17 || m == 0x1B;
}

void arm6_set_cpsr(Arm6 *c, uint32_t psr)
{
    psr &= ARM6_PSR_BITS;
    int ob = bank_of(c->cpsr), nb = bank_of(psr);
    if (ob != nb) {
        memcpy(ob == 1 ? c->fiq_r8_12 : c->usr_r8_12, &c->r[8], 5 * sizeof(uint32_t));
        c->r13_14[ob][0] = c->r[13];
        c->r13_14[ob][1] = c->r[14];
        memcpy(&c->r[8], nb == 1 ? c->fiq_r8_12 : c->usr_r8_12, 5 * sizeof(uint32_t));
        c->r[13] = c->r13_14[nb][0];
        c->r[14] = c->r13_14[nb][1];
    }
    c->cpsr = psr;
}

void arm6_set_pc(Arm6 *c, uint32_t pc)
{
    c->r[15] = pc & pc_mask(c);
    c->pipe_valid = 0;
}

/* Scrittura di R15 "con S" in un modo a 26 bit (MOVS PC, LDM^): flag
   sempre, I/F/modo solo se privilegiato, come sull'ARM2 */
static void write_r15_psr26(Arm6 *c, uint32_t value)
{
    uint32_t psr;
    if (is_user(c)) psr = (value & 0xF0000000u) | (c->cpsr & 0x0FFFFFFFu);
    else            psr = (value & 0xF0000000u) | ((value >> 20) & 0xC0u) | (value & 3u);
    arm6_set_cpsr(c, psr);
    arm6_set_pc(c, value);
}

/* TEQP & co. in un modo a 26 bit: solo i bit del PSR, PC invariato */
static void write_psr26_only(Arm6 *c, uint32_t value)
{
    uint32_t psr = (value & 0xF0000000u) | (c->cpsr & 0x0FFFFFFFu);
    if (!is_user(c)) psr = (psr & ~0xDFu) | ((value >> 20) & 0xC0u) | (value & 3u);
    arm6_set_cpsr(c, psr);
}

/* Modi a 32 bit: MOVS PC / LDM^ con PC copiano l'SPSR nel CPSR */
static void restore_spsr(Arm6 *c)
{
    int b = bank_of(c->cpsr);
    if (b) arm6_set_cpsr(c, c->spsr[b]);
}

static int user_reg_is_banked(const Arm6 *c, int n)
{
    int b = bank_of(c->cpsr);
    if (b == 0 || n < 8 || n == 15) return 0;
    return b == 1 || n >= 13;
}

uint32_t arm6_get_user_reg(const Arm6 *c, int n)
{
    if (!user_reg_is_banked(c, n)) return c->r[n];
    return n < 13 ? c->usr_r8_12[n - 8] : c->r13_14[0][n - 13];
}

void arm6_set_user_reg(Arm6 *c, int n, uint32_t v)
{
    if (!user_reg_is_banked(c, n)) c->r[n] = v;
    else if (n < 13) c->usr_r8_12[n - 8] = v;
    else c->r13_14[0][n - 13] = v;
}

const char *arm6_mode_name(int mode)
{
    switch (mode & 0x1F) {
    case 0x00: return "USR26"; case 0x01: return "FIQ26";
    case 0x02: return "IRQ26"; case 0x03: return "SVC26";
    case 0x10: return "USR32"; case 0x11: return "FIQ32";
    case 0x12: return "IRQ32"; case 0x13: return "SVC32";
    case 0x17: return "ABT32"; case 0x1B: return "UND32";
    default:   return "???";
    }
}

/* ------------------------------------------------------------------ */
/* eccezioni                                                          */
/* ------------------------------------------------------------------ */

/* link: indirizzo di ritorno senza PSR. Coi programmi a 26 bit (P = 0)
   si entra nel modo a 26 bit corrispondente (abort e istruzioni non
   definite in SVC26, come sull'ARM2) e R14 riceve anche il PSR. */
static void take_exception(Arm6 *c, uint32_t vector, uint32_t mode32, uint32_t link, int mask_fiq)
{
    uint32_t old = c->cpsr;
    uint32_t mode, lr;
    if (c->ctrl & ARM6_CTRL_P) {
        mode = mode32;
        lr = link;
    } else {
        mode = mode32 & 3u;            /* ABT32 e UND32 -> SVC26 */
        lr = (link & PC26_MASK) | psr26(c);
    }
    if (c->exception_hook) c->exception_hook(c, vector, lr, c->exception_user);
    uint32_t psr = (old & (0xF0000000u | ARM6_F)) | ARM6_I | mode;
    if (mask_fiq) psr |= ARM6_F;
    arm6_set_cpsr(c, psr);
    c->spsr[bank_of(mode)] = old;
    c->r[14] = lr;
    arm6_set_pc(c, vector);
}

static void data_abort(Arm6 *c)
{
    take_exception(c, ARM6_VEC_DABORT, ARM6_ABT32, c->r[15] + 4, 0);
}

static void address_exception(Arm6 *c)
{
    take_exception(c, ARM6_VEC_ADDRESS, ARM6_SVC32, c->r[15] + 4, 0);
}

/* coi dati a 26 bit un indirizzo oltre i 64 MB da' l'address exception */
static inline int bad_address(const Arm6 *c, uint32_t addr)
{
    return !(c->ctrl & ARM6_CTRL_D) && (addr & 0xFC000000u);
}

void arm6_init(Arm6 *c, const ArmBus *bus, uint32_t cpu_id)
{
    build_cond_table();
    memset(c, 0, sizeof *c);
    c->bus = *bus;
    c->cp15_id = cpu_id;
    c->s_ticks = 1;
    c->n_ticks = 1;
    arm6_reset(c);
}

void arm6_reset(Arm6 *c)
{
    arm6_set_cpsr(c, ARM6_I | ARM6_F | ARM6_SVC26);
    c->ctrl = 0;
    arm6_tlb_flush(c);
    arm6_set_pc(c, 0);
    c->halted = 0;
}

/* ------------------------------------------------------------------ */
/* MMU                                                                */
/* ------------------------------------------------------------------ */

void arm6_tlb_flush(Arm6 *c)
{
    memset(c->tlb, 0, sizeof c->tlb);
}

/* stato del fault per un accesso, dati dominio e permessi AP (0 = ok) */
static uint8_t check_access(const Arm6 *c, int dom, int ap, int user, int write, int page)
{
    int d = (c->dacr >> (2 * dom)) & 3;
    if (d == 0 || d == 2) return (uint8_t)(dom << 4 | (page ? 0xB : 0x9));
    if (d == 3) return 0;                               /* manager */
    int ok;
    switch (ap) {
    case 0: {
        int s = (c->ctrl & ARM6_CTRL_S) != 0, r = (c->ctrl & ARM6_CTRL_R) != 0;
        if (s && !r)      ok = !user && !write;
        else if (!s && r) ok = !write;
        else              ok = 0;
        break;
    }
    case 1:  ok = !user; break;
    case 2:  ok = !user || !write; break;
    default: ok = 1; break;
    }
    return ok ? 0 : (uint8_t)(dom << 4 | (page ? 0xF : 0xD));
}

/* Percorre le tabelle e riempie la voce del TLB per il blocco da 1 KB.
   Ritorna 0, o lo stato del fault di traduzione (che non va nel TLB). */
static int tlb_fill(Arm6 *c, uint32_t va, Arm6TlbEntry *e)
{
    int abort = 0;
    uint32_t l1 = c->bus.read32(c->bus.ctx, (c->ttb & 0xFFFFC000u) | ((va >> 18) & 0x3FFCu), &abort);
    if (abort) return 0x0C;                             /* abort esterno, primo livello */
    int dom = (int)((l1 >> 5) & 15), ap, page;
    uint32_t pa;
    switch (l1 & 3) {
    case 2:                                             /* sezione da 1 MB */
        pa = (l1 & 0xFFF00000u) | (va & 0x000FFC00u);
        ap = (int)((l1 >> 10) & 3);
        page = 0;
        break;
    case 1: {                                           /* tabella delle pagine */
        uint32_t l2 = c->bus.read32(c->bus.ctx, (l1 & 0xFFFFFC00u) | ((va >> 10) & 0x3FCu), &abort);
        if (abort) return dom << 4 | 0x0E;
        switch (l2 & 3) {
        case 1:                                         /* pagina grande, 64 KB */
            pa = (l2 & 0xFFFF0000u) | (va & 0x0000FC00u);
            ap = (int)((l2 >> (4 + 2 * ((va >> 14) & 3))) & 3);
            break;
        case 2:                                         /* pagina piccola, 4 KB */
            pa = (l2 & 0xFFFFF000u) | (va & 0x00000C00u);
            ap = (int)((l2 >> (4 + 2 * ((va >> 10) & 3))) & 3);
            break;
        default:
            return dom << 4 | 0x07;                     /* traduzione, pagina */
        }
        page = 1;
        break;
    }
    default:
        return 0x05;                                    /* traduzione, sezione */
    }
    e->tag = (va & 0xFFFFFC00u) | 1;
    e->pa = pa;
    for (int k = 0; k < 4; k++)
        e->fsr[k] = check_access(c, dom, ap, k >> 1, k & 1, page);
    return 0;
}

/* 0 e *pa, oppure lo stato del fault (dominio << 4 | codice) */
static inline int mmu(Arm6 *c, uint32_t va, int write, int user, uint32_t *pa)
{
    if (!(c->ctrl & ARM6_CTRL_M)) { *pa = va; return 0; }
    Arm6TlbEntry *e = &c->tlb[(va >> 10) & (ARM6_TLB_SIZE - 1)];
    if (e->tag != ((va & 0xFFFFFC00u) | 1)) {
        int f = tlb_fill(c, va, e);
        if (f) return f;
    }
    int f = e->fsr[user * 2 + write];
    if (f) return f;
    *pa = e->pa | (va & 0x3FFu);
    return 0;
}

int arm6_translate(Arm6 *c, uint32_t va, int write, int user, uint32_t *pa)
{
    return mmu(c, va, write, user, pa);
}

static int fault(Arm6 *c, uint32_t va, int status)
{
    c->fsr = (uint32_t)status;
    c->far = va;
    return 1;
}

/* Accessi ai dati: ritornano 1 se c'e' un abort (FSR e FAR gia' scritti). */
static int load32(Arm6 *c, uint32_t va, int user, uint32_t *v)
{
    uint32_t pa;
    int f, abort = 0;
    if ((c->ctrl & ARM6_CTRL_A) && (va & 3)) return fault(c, va, 0x01);
    if ((f = mmu(c, va, 0, user, &pa)) != 0) return fault(c, va, f);
    uint32_t w = c->bus.read32(c->bus.ctx, pa & ~3u, &abort);
    if (abort) return fault(c, va, 0x08);
    *v = ror32(w, (va & 3) * 8);         /* LDR non allineata: parola ruotata */
    return 0;
}

static int load8(Arm6 *c, uint32_t va, int user, uint32_t *v)
{
    uint32_t pa;
    int f, abort = 0;
    if ((f = mmu(c, va, 0, user, &pa)) != 0) return fault(c, va, f);
    *v = c->bus.read8(c->bus.ctx, pa, &abort);
    if (abort) return fault(c, va, 0x08);
    return 0;
}

static int store32(Arm6 *c, uint32_t va, int user, uint32_t v)
{
    uint32_t pa;
    int f, abort = 0;
    if ((c->ctrl & ARM6_CTRL_A) && (va & 3)) return fault(c, va, 0x01);
    if ((f = mmu(c, va, 1, user, &pa)) != 0) return fault(c, va, f);
    c->bus.write32(c->bus.ctx, pa, v, &abort);
    if (abort) return fault(c, va, 0x08);
    return 0;
}

static int store8(Arm6 *c, uint32_t va, int user, uint32_t v)
{
    uint32_t pa;
    int f, abort = 0;
    if ((f = mmu(c, va, 1, user, &pa)) != 0) return fault(c, va, f);
    c->bus.write8(c->bus.ctx, pa, (uint8_t)v, &abort);
    if (abort) return fault(c, va, 0x08);
    return 0;
}

/* lettura di un'istruzione: i fault non toccano FSR e FAR */
static uint32_t fetch(Arm6 *c, uint32_t va, uint8_t *abort)
{
    uint32_t pa;
    int a = 0;
    if (mmu(c, va, 0, is_user(c), &pa)) { *abort = 1; return 0; }
    uint32_t v = c->bus.read32(c->bus.ctx, pa, &a);
    *abort = (uint8_t)a;
    return v;
}

/* ------------------------------------------------------------------ */
/* barrel shifter                                                     */
/* ------------------------------------------------------------------ */

static inline uint32_t shift_imm(const Arm6 *c, uint32_t v, int type, int amount, int *carry)
{
    switch (type) {
    case 0:
        if (amount == 0) return v;
        *carry = (v >> (32 - amount)) & 1;
        return v << amount;
    case 1:
        if (amount == 0) { *carry = v >> 31; return 0; }
        *carry = (v >> (amount - 1)) & 1;
        return v >> amount;
    case 2:
        if (amount == 0) { *carry = v >> 31; return (uint32_t)((int32_t)v >> 31); }
        *carry = (v >> (amount - 1)) & 1;
        return (uint32_t)((int32_t)v >> amount);
    default:
        if (amount == 0) {
            uint32_t r = (v >> 1) | ((c->cpsr & ARM6_C) ? 0x80000000u : 0);
            *carry = v & 1;
            return r;
        }
        *carry = (v >> (amount - 1)) & 1;
        return ror32(v, (unsigned)amount);
    }
}

static inline uint32_t shift_reg(uint32_t v, int type, uint32_t amount, int *carry)
{
    if (amount == 0) return v;
    switch (type) {
    case 0:
        if (amount < 32)  { *carry = (v >> (32 - amount)) & 1; return v << amount; }
        *carry = amount == 32 ? (int)(v & 1) : 0;
        return 0;
    case 1:
        if (amount < 32)  { *carry = (v >> (amount - 1)) & 1; return v >> amount; }
        *carry = amount == 32 ? (int)(v >> 31) : 0;
        return 0;
    case 2:
        if (amount < 32)  { *carry = (v >> (amount - 1)) & 1; return (uint32_t)((int32_t)v >> amount); }
        *carry = (int)(v >> 31);
        return (uint32_t)((int32_t)v >> 31);
    default:
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

static uint32_t exec_data_processing(Arm6 *c, uint32_t i)
{
    int op = (i >> 21) & 15;
    int s  = (i >> 20) & 1;
    int rn = (i >> 16) & 15;
    int rd = (i >> 12) & 15;
    uint32_t cyc = CYC(1, 0, 0, 1);
    int carry = (c->cpsr & ARM6_C) ? 1 : 0;
    uint32_t op2, a;
    uint32_t pc_extra = 4;

    if (i & (1u << 25)) {
        int rot = ((i >> 8) & 15) * 2;
        op2 = ror32(i & 0xFF, (unsigned)rot);
        if (rot) carry = (int)(op2 >> 31);
    } else {
        int rm = i & 15, type = (i >> 5) & 3;
        if (i & 0x10) {
            pc_extra = 8;
            cyc += CYC(0, 0, 1, 0);
            int rs = (i >> 8) & 15;
            uint32_t amount = (rs == 15 ? r15_full(c, pc_extra) : c->r[rs]) & 0xFF;
            uint32_t vm = rm == 15 ? r15_full(c, pc_extra) : c->r[rm];
            op2 = shift_reg(vm, type, amount, &carry);
        } else {
            uint32_t vm = rm == 15 ? r15_full(c, pc_extra) : c->r[rm];
            op2 = shift_imm(c, vm, type, (i >> 7) & 31, &carry);
        }
    }

    a = rn == 15 ? r15_addr(c, pc_extra) : c->r[rn];

    uint32_t res;
    int v = (c->cpsr & ARM6_V) ? 1 : 0;
    int cin = (c->cpsr & ARM6_C) ? 1 : 0;

    switch (op) {
    case 0x0: case 0x8: res = a & op2; break;
    case 0x1: case 0x9: res = a ^ op2; break;
    case 0x2: case 0xA: res = alu_add(a, ~op2, 1, &carry, &v); break;
    case 0x3: res = alu_add(op2, ~a, 1, &carry, &v); break;
    case 0x4: case 0xB: res = alu_add(a, op2, 0, &carry, &v); break;
    case 0x5: res = alu_add(a, op2, (uint32_t)cin, &carry, &v); break;
    case 0x6: res = alu_add(a, ~op2, (uint32_t)cin, &carry, &v); break;
    case 0x7: res = alu_add(op2, ~a, (uint32_t)cin, &carry, &v); break;
    case 0xC: res = a | op2; break;
    case 0xD: res = op2; break;
    case 0xE: res = a & ~op2; break;
    default:  res = ~op2; break;
    }

    if (op >= 0x8 && op <= 0xB) {
        /* (senza S sono MRS/MSR, gia' smistate prima) */
        if (rd == 15) {
            /* TEQP & co.: a 26 bit scrivono il PSR, a 32 bit ripristinano l'SPSR */
            if (is26(c)) write_psr26_only(c, res);
            else         restore_spsr(c);
            return cyc;
        }
    } else if (rd == 15) {
        if (s && is26(c)) {
            write_r15_psr26(c, res);
        } else {
            if (s) restore_spsr(c);
            arm6_set_pc(c, res);
        }
        return cyc + CYC(1, 1, 0, 2);
    } else {
        c->r[rd] = res;
        if (!s) return cyc;
    }

    uint32_t f = (res & 0x80000000u) | (res == 0 ? ARM6_Z : 0)
               | (carry ? ARM6_C : 0) | (v ? ARM6_V : 0);
    c->cpsr = (c->cpsr & 0x0FFFFFFFu) | f;
    return cyc;
}

/* MRS e MSR (nello spazio di TST/TEQ/CMP/CMN senza S) */
static uint32_t exec_psr_transfer(Arm6 *c, uint32_t i)
{
    int spsr = (i >> 22) & 1;
    int b = bank_of(c->cpsr);

    if ((i & 0x0FBF0FFFu) == 0x010F0000u) {                   /* MRS */
        int rd = (i >> 12) & 15;
        uint32_t v = spsr ? (b ? c->spsr[b] : c->cpsr) : c->cpsr;
        if (rd != 15) c->r[rd] = v;
        return CYC(1, 0, 0, 1);
    }
    int imm = (i & 0x0FB0F000u) == 0x0320F000u;
    if (!imm && (i & 0x0FB0FFF0u) != 0x0120F000u) return CYC(1, 0, 0, 1);  /* non definita: NOP */

    uint32_t v;
    if (imm) v = ror32(i & 0xFF, ((i >> 8) & 15) * 2);
    else     v = (i & 15) == 15 ? c->r[15] + 4 : c->r[i & 15];

    uint32_t mask = 0;
    if (i & (1u << 19)) mask |= 0xF0000000u;                  /* campo f */
    if (i & (1u << 16)) mask |= 0x000000DFu;                  /* campo c */
    if (spsr) {
        if (b) c->spsr[b] = ((c->spsr[b] & ~mask) | (v & mask)) & ARM6_PSR_BITS;
        return CYC(1, 0, 0, 1);
    }
    if (is_user(c)) mask &= 0xF0000000u;
    uint32_t psr = (c->cpsr & ~mask) | (v & mask);
    /* modi inesistenti, o a 32 bit con la configurazione a 26: il modo resta */
    uint32_t m = psr & ARM6_MODE_MASK;
    if (!mode_valid(m) || ((m & 0x10u) && !(c->ctrl & ARM6_CTRL_P)))
        psr = (psr & ~ARM6_MODE_MASK) | (c->cpsr & ARM6_MODE_MASK);
    arm6_set_cpsr(c, psr);
    if (is26(c) && (c->r[15] & ~PC26_MASK)) c->r[15] &= PC26_MASK;
    return CYC(1, 0, 0, 1);
}

/* ------------------------------------------------------------------ */
/* moltiplicazione e swap                                             */
/* ------------------------------------------------------------------ */

static uint32_t exec_multiply(Arm6 *c, uint32_t i)
{
    int rd = (i >> 16) & 15, rn = (i >> 12) & 15, rs = (i >> 8) & 15, rm = i & 15;
    uint32_t vs = c->r[rs];
    uint32_t res = c->r[rm] * vs;
    if (i & (1u << 21)) res += c->r[rn];

    if (rd != 15) c->r[rd] = res;
    if (i & (1u << 20))
        c->cpsr = (c->cpsr & 0x3FFFFFFFu) | (res & ARM6_N) | (res == 0 ? ARM6_Z : 0);
    int m = 0;
    do { m++; vs >>= 2; } while (vs && m < 16);
    return CYC(1, 0, m, 1);
}

static uint32_t exec_swap(Arm6 *c, uint32_t i)
{
    int rn = (i >> 16) & 15, rd = (i >> 12) & 15, rm = i & 15;
    uint32_t addr = c->r[rn];
    if (bad_address(c, addr)) {
        address_exception(c);
        return CYC_EXCEPTION;
    }
    int user = is_user(c);
    uint32_t src = c->r[rm], old;
    int ab;
    if (i & (1u << 22)) {
        ab = load8(c, addr, user, &old);
        if (!ab) ab = store8(c, addr, user, src);
    } else {
        ab = load32(c, addr, user, &old);
        if (!ab) ab = store32(c, addr, user, src);
    }
    if (ab) {
        data_abort(c);
        return CYC_EXCEPTION;
    }
    if (rd != 15) c->r[rd] = old;
    return CYC(1, 2, 1, 1);
}

/* ------------------------------------------------------------------ */
/* LDR / STR                                                          */
/* ------------------------------------------------------------------ */

static uint32_t exec_single_transfer(Arm6 *c, uint32_t i)
{
    int p = (i >> 24) & 1, u = (i >> 23) & 1, b = (i >> 22) & 1;
    int w = (i >> 21) & 1, l = (i >> 20) & 1;
    int rn = (i >> 16) & 15, rd = (i >> 12) & 15;
    uint32_t offset;

    if (i & (1u << 25)) {
        int rm = i & 15, dummy = 0;
        uint32_t vm = rm == 15 ? r15_full(c, 4) : c->r[rm];
        offset = shift_imm(c, vm, (i >> 5) & 3, (i >> 7) & 31, &dummy);
    } else {
        offset = i & 0xFFF;
    }

    uint32_t base = rn == 15 ? r15_addr(c, 4) : c->r[rn];
    uint32_t moved = u ? base + offset : base - offset;
    uint32_t addr = p ? moved : base;
    int writeback = (!p || w) && rn != 15;

    if (bad_address(c, addr)) {
        address_exception(c);
        return CYC_EXCEPTION;
    }

    /* LDRT/STRT: permessi dell'utente anche in un modo privilegiato */
    int user = is_user(c) || (!p && w);
    if (l) {
        uint32_t v;
        if (b ? load8(c, addr, user, &v) : load32(c, addr, user, &v)) {
            /* abort "tardivi" (ARM710 con L = 1): la base si aggiorna lo stesso */
            if (writeback && (c->ctrl & ARM6_CTRL_L)) c->r[rn] = moved;
            data_abort(c);
            return CYC_EXCEPTION;
        }
        if (writeback) c->r[rn] = moved;
        if (rd == 15) { arm6_set_pc(c, v); return CYC(2, 2, 1, 3); }
        c->r[rd] = v;
        return CYC(1, 1, 1, 1);
    }

    uint32_t v = rd == 15 ? r15_full(c, 8) : c->r[rd];      /* STR PC: PC+12 */
    if (b ? store8(c, addr, user, v) : store32(c, addr, user, v)) {
        if (writeback && (c->ctrl & ARM6_CTRL_L)) c->r[rn] = moved;
        data_abort(c);
        return CYC_EXCEPTION;
    }
    if (writeback) c->r[rn] = moved;
    return CYC(0, 2, 0, 1);
}

/* ------------------------------------------------------------------ */
/* LDM / STM                                                          */
/* ------------------------------------------------------------------ */

static uint32_t exec_block_transfer(Arm6 *c, uint32_t i)
{
    int p = (i >> 24) & 1, u = (i >> 23) & 1, s = (i >> 22) & 1;
    int w = (i >> 21) & 1, l = (i >> 20) & 1;
    int rn = (i >> 16) & 15;
    uint32_t list = i & 0xFFFF;
    int n = popcount16(list);
    if (n == 0) return CYC(1, 0, 0, 1);

    uint32_t base = rn == 15 ? r15_addr(c, 4) : c->r[rn];
    uint32_t wb = u ? base + 4u * (uint32_t)n : base - 4u * (uint32_t)n;
    uint32_t addr;
    if (u) addr = p ? base + 4 : base;
    else   addr = p ? base - 4u * (uint32_t)n : base - 4u * (uint32_t)n + 4;
    if (rn == 15) w = 0;

    if (bad_address(c, addr)) {
        address_exception(c);
        return CYC_EXCEPTION;
    }
    /* coi dati a 26 bit gli indirizzi successivi si avvolgono */
    uint32_t amask = (c->ctrl & ARM6_CTRL_D) ? 0xFFFFFFFFu : 0x03FFFFFFu;

    int user_bank = s && (!l || !(list & 0x8000));
    int user = is_user(c);
    int aborted = 0;

    if (!l) {
        int first = 1;
        for (int r = 0; r < 16; r++) {
            if (!(list & (1u << r))) continue;
            uint32_t v;
            if (r == 15)        v = r15_full(c, 8);
            else if (user_bank) v = arm6_get_user_reg(c, r);
            else                v = c->r[r];
            /* dopo il primo abort le scritture non arrivano alla memoria;
               FSR e FAR restano quelli del primo */
            if (!aborted) aborted = store32(c, addr & amask & ~3u, user, v);
            addr += 4;
            if (first && w) c->r[rn] = wb;
            first = 0;
        }
        if (aborted) { data_abort(c); return CYC_EXCEPTION; }
        return CYC(n - 1, 2, 0, 1);
    }

    uint32_t vals[16];
    for (int r = 0; r < 16; r++) {
        if (!(list & (1u << r))) continue;
        if (!aborted) aborted = load32(c, addr & amask & ~3u, user, &vals[r]);
        addr += 4;
    }
    if (w) c->r[rn] = wb;
    if (aborted) {
        data_abort(c);
        return CYC_EXCEPTION;
    }
    for (int r = 0; r < 15; r++) {
        if (!(list & (1u << r))) continue;
        if (user_bank) arm6_set_user_reg(c, r, vals[r]);
        else           c->r[r] = vals[r];
    }
    if (list & 0x8000) {
        if (s && is26(c)) {
            write_r15_psr26(c, vals[15]);
        } else {
            if (s) restore_spsr(c);
            arm6_set_pc(c, vals[15]);
        }
        return CYC(n + 1, 2, 1, 3);
    }
    return CYC(n, 1, 1, 1);
}

/* ------------------------------------------------------------------ */
/* coprocessore 15                                                    */
/* ------------------------------------------------------------------ */

/* MRC/MCR p15. Ritorna 0 se l'istruzione va trattata come non definita. */
static int exec_cp15(Arm6 *c, uint32_t i)
{
    if (is_user(c)) return 0;
    int crn = (i >> 16) & 15, rd = (i >> 12) & 15;

    if (i & (1u << 20)) {                                      /* MRC */
        uint32_t v;
        switch (crn) {
        case 0:  v = c->cp15_id; break;
        case 1:  v = c->ctrl; break;
        case 2:  v = c->ttb; break;
        case 3:  v = c->dacr; break;
        case 5:  v = c->fsr; break;
        case 6:  v = c->far; break;
        default: v = 0; break;
        }
        if (rd == 15) c->cpsr = (c->cpsr & 0x0FFFFFFFu) | (v & 0xF0000000u);
        else          c->r[rd] = v;
        return 1;
    }

    uint32_t v = rd == 15 ? r15_full(c, 8) : c->r[rd];      /* MCR */
    switch (crn) {
    case 1:  c->ctrl = v & 0x3FFu; arm6_tlb_flush(c); break;
    case 2:  c->ttb = v & 0xFFFFC000u; arm6_tlb_flush(c); break;
    case 3:  c->dacr = v; arm6_tlb_flush(c); break;
    case 5:                                                    /* svuota il TLB */
    case 6:  arm6_tlb_flush(c); break;                         /* toglie una voce */
    default: break;                                            /* c7: svuota la cache */
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* ciclo principale                                                   */
/* ------------------------------------------------------------------ */

static int account(Arm6 *c, uint32_t cyc)
{
    uint32_t s = cyc & 0xFF, n = (cyc >> 8) & 0xFF, i = (cyc >> 16) & 0xFF;
    uint32_t ticks = s * c->s_ticks + n * c->n_ticks + i + c->extra_cycles;
    c->extra_cycles = 0;
    c->cycles += ticks;
    return (int)ticks;
}

int arm6_step(Arm6 *c)
{
    if (c->fiq_line && !(c->cpsr & ARM6_F)) {
        take_exception(c, ARM6_VEC_FIQ, ARM6_FIQ32, c->r[15] + 4, 1);
        return account(c, CYC_EXCEPTION);
    }
    if (c->irq_line && !(c->cpsr & ARM6_I)) {
        take_exception(c, ARM6_VEC_IRQ, ARM6_IRQ32, c->r[15] + 4, 0);
        return account(c, CYC_EXCEPTION);
    }

    uint32_t pm = pc_mask(c);
    uint32_t pc = c->r[15] & pm;
    if (!c->pipe_valid || c->pipe_addr != pc) {
        c->pipe[0] = fetch(c, pc, &c->pipe_abort[0]);
        c->pipe[1] = fetch(c, (pc + 4) & pm, &c->pipe_abort[1]);
    }
    uint32_t i = c->pipe[0];
    int abort = c->pipe_abort[0];
    c->pipe[0] = c->pipe[1];
    c->pipe_abort[0] = c->pipe_abort[1];
    c->pipe[1] = fetch(c, (pc + 8) & pm, &c->pipe_abort[1]);
    c->pipe_addr = (pc + 4) & pm;
    c->pipe_valid = 1;
    c->r[15] = (pc + 4) & pm;

    if (abort) {
        take_exception(c, ARM6_VEC_PABORT, ARM6_ABT32, c->r[15], 0);
        return account(c, CYC_EXCEPTION);
    }

    c->instructions++;
    if (c->trace_hook) c->trace_hook(c, pc, i);
    uint32_t cyc;

    if (!cond_table[i >> 28][c->cpsr >> 28]) {
        cyc = CYC(1, 0, 0, 1);
    } else {
        switch ((i >> 25) & 7) {
        case 0:
            if ((i & 0x0FC000F0u) == 0x00000090u)      cyc = exec_multiply(c, i);
            else if ((i & 0x0FB00FF0u) == 0x01000090u) cyc = exec_swap(c, i);
            else if ((i & 0x90u) == 0x90u)             goto undefined;
            else if ((i & 0x01900000u) == 0x01000000u) cyc = exec_psr_transfer(c, i);
            else                                       cyc = exec_data_processing(c, i);
            break;
        case 1:
            if ((i & 0x01900000u) == 0x01000000u)      cyc = exec_psr_transfer(c, i);
            else                                       cyc = exec_data_processing(c, i);
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
        case 5: {
            uint32_t off = (uint32_t)((int32_t)(i << 8) >> 6);
            if (i & (1u << 24)) c->r[14] = r15_full(c, 0);
            arm6_set_pc(c, c->r[15] + 4 + off);
            cyc = CYC(2, 1, 0, 3);
            break;
        }
        case 7:
            if (i & (1u << 24)) {
                if (c->swi_hook && c->swi_hook(c, i & 0x00FFFFFFu, c->swi_user)) {
                    cyc = CYC_EXCEPTION;
                    break;
                }
                take_exception(c, ARM6_VEC_SWI, ARM6_SVC32, c->r[15], 0);
                cyc = CYC_EXCEPTION;
                break;
            }
            if ((i & 0x10) && ((i >> 8) & 15) == 15 && exec_cp15(c, i)) {
                cyc = CYC(1, 0, 2, 1);
                break;
            }
            /* fallthrough: CDP e coprocessori assenti */
        default:
        undefined:
            take_exception(c, ARM6_VEC_UNDEF, ARM6_UND32, c->r[15], 0);
            cyc = CYC_EXCEPTION;
            break;
        }
    }
    return account(c, cyc);
}

uint64_t arm6_run(Arm6 *c, uint64_t max_cycles)
{
    uint64_t start = c->cycles;
    while (!c->halted && c->cycles - start < max_cycles)
        arm6_step(c);
    return c->cycles - start;
}
