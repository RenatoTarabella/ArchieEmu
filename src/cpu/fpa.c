/*
 * fpa.c - Modulo FPU: FPA10 / FPA11 (vedi fpa.h).
 *
 * Riferimenti: il data sheet GEC Plessey della FPA10 (istruzioni eseguite in
 * hardware, FPSR/FPCR, tempi) e i sorgenti ARM del codice di supporto e del
 * FPEmulator (FPASC, licenza BSD, pubblicati da RISC OS Open): formato
 * interno dei registri, conversioni di LDF/LFM, flag dei confronti, come il
 * FPEmulator 4 riconosce il chip (RFC che non rimbalza, RFS = 0x81 nel byte
 * alto) e come gestisce i rimbalzi (SB: emula l'istruzione da capo; DA: chip
 * disattivato, riattiva il contesto e la ripete).
 *
 * In hardware (data sheet): LDF/STF S D E, LFM/SFM, ADF SUF RSF MUF DVF RDF
 * FML FDV FRD MVF MNF ABS URD NRM, FLT FIX, WFS RFS WFC RFC, CMF CNF CMFE CNFE.
 * Al codice di supporto: il formato decimale (P), SQT RND RMF POW RPW POL e
 * le trascendenti; e ogni istruzione con un operando speciale (infinito,
 * NaN, denormale, non normalizzato) o con un'eccezione (operazione non
 * valida, divisione per zero, overflow, underflow, o inesatto con la
 * trappola abilitata). Qui rimbalzano anche URD e NRM: il codice di
 * supporto le calcola giuste, solo piu' lentamente. FML, FDV e FRD come il
 * codice di supporto: FML moltiplica solo i 32 bit alti delle mantisse,
 * FDV e FRD sono DVF e RDF (il GCC usa FML per le moltiplicazioni in
 * singola precisione).
 */
#include "fpa.h"
#include <string.h>

/* FPSR */
#define FPSR_IOC 0x01u
#define FPSR_DZC 0x02u
#define FPSR_OFC 0x04u
#define FPSR_UFC 0x08u
#define FPSR_IXC 0x10u
#define FPSR_IXE 0x00100000u
#define FPSR_WRITABLE 0x001F1F1Fu     /* trappole, controllo, flag */

/* FPCR */
#define FPCR_DA 0x00000100u
#define FPCR_RE 0x00000200u
#define FPCR_AB 0x00000400u
#define FPCR_SB 0x00000800u

/* formato interno */
#define I_SIGN     0x80000000u
#define I_UNCOMMON 0x40000000u
#define I_EXP      0x00007FFFu
#define I_UNITS    0x80000000u
#define EBIAS      16383

/* risultati dei confronti in N Z C V */
#define COMP_GT 0x20000000u
#define COMP_EQ 0x60000000u
#define COMP_LT 0x80000000u

/* tempi del data sheet (cicli, il "throughput"): li aspetta la CPU */
#define T_LDF(p)  (2 + (p))           /* S 2, D 3, E 4 */
#define T_LFM(n)  (1 + 3 * (n))       /* 4 7 10 13 */
#define T_MOVE    1
#define T_ADD     2
#define T_MUL     8
#define T_FML     5
#define T_DIV(p)  ((p) == 0 ? 30 : (p) == 1 ? 58 : 70)
#define T_FLT     6
#define T_FIX     8
#define T_CMP     5
#define T_RW      3

/* ------------------------------------------------------------------ */
/* interi a 128 bit (MSVC non ha __int128)                            */
/* ------------------------------------------------------------------ */

typedef struct { uint64_t hi, lo; } U128;

static U128 u128_shl(U128 a, int n)
{
    U128 r;
    if (n == 0) return a;
    if (n >= 128) { r.hi = r.lo = 0; return r; }
    if (n >= 64) { r.hi = a.lo << (n - 64); r.lo = 0; return r; }
    r.hi = a.hi << n | a.lo >> (64 - n);
    r.lo = a.lo << n;
    return r;
}

/* a destra, con i bit persi raccolti in *sticky */
static U128 u128_shr_sticky(U128 a, int n, int *sticky)
{
    U128 r;
    if (n == 0) return a;
    if (n >= 128) { *sticky |= (a.hi | a.lo) != 0; r.hi = r.lo = 0; return r; }
    if (n >= 64) {
        *sticky |= a.lo != 0 || (n > 64 && (a.hi << (128 - n)) != 0);
        r.lo = n == 64 ? a.hi : a.hi >> (n - 64);
        r.hi = 0;
        return r;
    }
    *sticky |= (a.lo << (64 - n)) != 0;
    r.lo = a.lo >> n | a.hi << (64 - n);
    r.hi = a.hi >> n;
    return r;
}

static U128 u128_add(U128 a, U128 b)
{
    U128 r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + (r.lo < a.lo);
    return r;
}

static U128 u128_sub(U128 a, U128 b)
{
    U128 r;
    r.lo = a.lo - b.lo;
    r.hi = a.hi - b.hi - (a.lo < b.lo);
    return r;
}

static int u128_cmp(U128 a, U128 b)
{
    if (a.hi != b.hi) return a.hi < b.hi ? -1 : 1;
    if (a.lo != b.lo) return a.lo < b.lo ? -1 : 1;
    return 0;
}

static U128 mul64(uint64_t a, uint64_t b)
{
    uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    U128 r;
    r.lo = (uint32_t)p00 | mid << 32;
    r.hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    return r;
}

static int clz64(uint64_t v)
{
    int n = 0;
    if (!v) return 64;
    while (!(v & 0x8000000000000000ull)) { v <<= 1; n++; }
    return n;
}

/* ------------------------------------------------------------------ */
/* valori spacchettati                                                */
/* ------------------------------------------------------------------ */

/* un numero "comune": zero o normale (unita' a 1), uncommon a 0 */
typedef struct { int sign; int exp; uint64_t mant; } Num;

static Num reg_num(const FpaReg *r)
{
    Num n;
    n.sign = (r->w[0] & I_SIGN) != 0;
    n.exp = (int)(r->w[0] & I_EXP);
    n.mant = (uint64_t)r->w[1] << 32 | r->w[2];
    return n;
}

/* zero o normale: il chip li calcola, il resto rimbalza */
static int is_plain(const FpaReg *r)
{
    uint32_t e = r->w[0] & I_EXP;
    if (r->w[0] & I_UNCOMMON) return 0;
    if (r->w[1] & I_UNITS) return e != I_EXP;
    return e == 0 && r->w[1] == 0 && r->w[2] == 0;        /* zero */
}

static void num_reg(FpaReg *r, Num n)
{
    if (n.mant == 0) n.exp = 0;
    r->w[0] = (n.sign ? I_SIGN : 0) | (uint32_t)n.exp;
    r->w[1] = (uint32_t)(n.mant >> 32);
    r->w[2] = (uint32_t)n.mant;
}

/* costanti dei CPDO e confronti (i = 1) */
static Num constant(int k)
{
    /* 0, 1, 2, 3, 4, 5, 0.5, 10 */
    static const struct { int e; uint64_t m; } c[8] = {
        { 0, 0 }, { EBIAS, 0x8000000000000000ull }, { EBIAS + 1, 0x8000000000000000ull },
        { EBIAS + 1, 0xC000000000000000ull }, { EBIAS + 2, 0x8000000000000000ull },
        { EBIAS + 2, 0xA000000000000000ull }, { EBIAS - 1, 0x8000000000000000ull },
        { EBIAS + 3, 0xA000000000000000ull }
    };
    Num n;
    n.sign = 0; n.exp = c[k].e; n.mant = c[k].m;
    return n;
}

/* ------------------------------------------------------------------ */
/* arrotondamento                                                     */
/* ------------------------------------------------------------------ */

static const int prec_bits[3] = { 24, 53, 64 };
/* esponenti (con l'eccesso 16383) dei numeri normali in ogni precisione */
static const int prec_emin[3] = { EBIAS - 126, EBIAS - 1022, 1 };
static const int prec_emax[3] = { EBIAS + 127, EBIAS + 1023, 0x7FFE };

enum { RM_NEAREST, RM_PLUS, RM_MINUS, RM_ZERO };

/* Il valore V * 2^scale (V != 0, segno a parte) arrotondato a 'prec' con il
   modo 'rm'. Ritorna 0 se va bene (in *out, *inexact), 1 se c'e' overflow o
   underflow: allora il chip rimbalza. */
static int round_to(int sign, U128 v, int scale, int sticky, int prec, int rm, Num *out, int *inexact)
{
    int lead = v.hi ? 127 - clz64(v.hi) : 63 - clz64(v.lo);
    int exp = lead + scale + EBIAS;
    int p = prec_bits[prec], up = 0;
    uint64_t keep, mant;
    v = u128_shl(v, 127 - lead);                /* l'unita' nel bit 127 */
    if (exp < prec_emin[prec]) return 1;        /* (anche prima di arrotondare) */
    if (p == 64) {
        int half = (v.lo >> 63) != 0, rest = (v.lo << 1) != 0 || sticky;
        keep = v.hi;
        *inexact = half || rest;
        if (rm == RM_NEAREST) up = half && (rest || (keep & 1));
        else if (rm == RM_PLUS) up = *inexact && !sign;
        else if (rm == RM_MINUS) up = *inexact && sign;
        if (up && ++keep == 0) { keep = 0x8000000000000000ull; exp++; }
        mant = keep;
    } else {
        int sh = 64 - p, half, rest;
        uint64_t low = v.hi & ((1ull << sh) - 1), hb = 1ull << (sh - 1);
        keep = v.hi >> sh;
        half = (low & hb) != 0;
        rest = (low & (hb - 1)) != 0 || v.lo != 0 || sticky;
        *inexact = half || rest;
        if (rm == RM_NEAREST) up = half && (rest || (keep & 1));
        else if (rm == RM_PLUS) up = *inexact && !sign;
        else if (rm == RM_MINUS) up = *inexact && sign;
        if (up && ++keep == 1ull << p) { keep >>= 1; exp++; }
        mant = keep << sh;
    }
    if (exp > prec_emax[prec]) return 1;
    out->sign = sign;
    out->exp = exp;
    out->mant = mant;
    return 0;
}

/* ------------------------------------------------------------------ */
/* operazioni (operandi comuni)                                       */
/* ------------------------------------------------------------------ */

/* a + b; ritorna 1 = rimbalza */
static int op_add(Num a, Num b, int prec, int rm, Num *r, int *inexact)
{
    U128 va, vb, v;
    int sticky = 0, d;
    *inexact = 0;
    if (a.mant == 0 && b.mant == 0) {
        r->mant = 0; r->exp = 0;
        r->sign = a.sign == b.sign ? a.sign : rm == RM_MINUS;
        return 0;
    }
    if (a.mant == 0) return round_to(b.sign, (U128){ 0, b.mant }, b.exp - EBIAS - 63, 0, prec, rm, r, inexact);
    if (b.mant == 0) return round_to(a.sign, (U128){ 0, a.mant }, a.exp - EBIAS - 63, 0, prec, rm, r, inexact);
    if (a.exp < b.exp || (a.exp == b.exp && a.mant < b.mant)) { Num t = a; a = b; b = t; }
    /* il valore e' V * 2^(a.exp - EBIAS - 126): un bit libero per il riporto */
    va.hi = a.mant >> 1; va.lo = a.mant << 63;
    vb.hi = b.mant >> 1; vb.lo = b.mant << 63;
    d = a.exp - b.exp;
    vb = u128_shr_sticky(vb, d > 130 ? 130 : d, &sticky);
    if (a.sign == b.sign) v = u128_add(va, vb);
    else {
        v = u128_sub(va, vb);
        if (sticky) v = u128_sub(v, (U128){ 0, 1 });    /* il resto andava tolto */
    }
    if (v.hi == 0 && v.lo == 0 && !sticky) {
        r->mant = 0; r->exp = 0; r->sign = rm == RM_MINUS;
        return 0;
    }
    return round_to(a.sign, v, a.exp - EBIAS - 126, sticky, prec, rm, r, inexact);
}

static int op_mul(Num a, Num b, int prec, int rm, Num *r, int *inexact)
{
    *inexact = 0;
    if (a.mant == 0 || b.mant == 0) { r->sign = a.sign ^ b.sign; r->exp = 0; r->mant = 0; return 0; }
    return round_to(a.sign ^ b.sign, mul64(a.mant, b.mant), a.exp + b.exp - 2 * EBIAS - 126, 0, prec, rm, r, inexact);
}

/* a / b; b = 0 rimbalza (divisione per zero, o 0/0) */
static int op_div(Num a, Num b, int prec, int rm, Num *r, int *inexact)
{
    U128 q = { 0, 0 }, rem, d;
    *inexact = 0;
    if (b.mant == 0) return 1;
    if (a.mant == 0) { r->sign = a.sign ^ b.sign; r->exp = 0; r->mant = 0; return 0; }
    /* 67 bit del quoziente, uno alla volta */
    rem.hi = 0; rem.lo = a.mant;
    d.hi = 0; d.lo = b.mant;
    for (int i = 0; i < 67; i++) {
        q = u128_shl(q, 1);
        if (u128_cmp(rem, d) >= 0) { rem = u128_sub(rem, d); q.lo |= 1; }
        rem = u128_shl(rem, 1);
    }
    return round_to(a.sign ^ b.sign, q, a.exp - b.exp - 66, rem.hi || rem.lo, prec, rm, r, inexact);
}

/* ------------------------------------------------------------------ */
/* il chip                                                            */
/* ------------------------------------------------------------------ */

void fpa_reset(Fpa *f)
{
    /* il FPSR non si azzera al reset (lo fa il codice di supporto con WFS);
       il FPCR si', tranne DA, AB e SB che vanno a 1 */
    f->fpcr = FPCR_DA | FPCR_AB | FPCR_SB;
}

void fpa_init(Fpa *f, int model)
{
    memset(f, 0, sizeof *f);
    f->model = model;
    fpa_reset(f);
}

/* rimbalzo sincrono: il codice di supporto emula l'istruzione */
static int bounce(Fpa *f)
{
    f->fpcr |= FPCR_SB;
    f->bounces++;
    return FPA_BOUNCE;
}

/* inesatto: il flag, o rimbalza se la trappola e' abilitata */
static int inexact_ok(Fpa *f, int inexact)
{
    if (!inexact) return 1;
    if (f->fpsr & FPSR_IXE) return 0;
    f->fpsr |= FPSR_IXC;
    return 1;
}

/* ---- LDF / STF / LFM / SFM ---- */

static void load_single(FpaReg *r, uint32_t m)
{
    uint32_t s = m & I_SIGN, e = (m >> 23) & 0xFF;
    if ((m << 1) == 0) { r->w[0] = s; r->w[1] = r->w[2] = 0; return; }
    e += EBIAS - 127;
    r->w[0] = s | e;
    r->w[1] = I_UNITS | (m & 0x7FFFFF) << 8;
    r->w[2] = 0;
    if (e == EBIAS - 127 || e == EBIAS + 128) r->w[0] |= I_UNCOMMON;  /* denormale, infinito, NaN */
}

static void load_double(FpaReg *r, uint32_t m0, uint32_t m1)
{
    uint32_t s = m0 & I_SIGN, e = (m0 >> 20) & 0x7FF;
    if ((m0 << 1) == 0 && m1 == 0) { r->w[0] = s; r->w[1] = r->w[2] = 0; return; }
    e += EBIAS - 1023;
    r->w[0] = s | e;
    r->w[1] = I_UNITS | (m0 & 0xFFFFF) << 11 | m1 >> 21;
    r->w[2] = m1 << 11;
    if (e == EBIAS - 1023 || e == EBIAS + 1024) r->w[0] |= I_UNCOMMON;
}

static void load_extended(FpaReg *r, uint32_t m0, uint32_t m1, uint32_t m2)
{
    uint32_t w0 = m0 & (I_SIGN | I_EXP);                  /* via i bit riservati */
    if (!(m1 & I_UNITS) || (w0 & I_EXP) == I_EXP) w0 |= I_UNCOMMON;
    if ((w0 & I_EXP) == 0 && m1 == 0 && m2 == 0) w0 &= ~I_UNCOMMON;   /* zero */
    r->w[0] = w0; r->w[1] = m1; r->w[2] = m2;
}

/* STF di un registro comune in precisione 'prec'; 1 = rimbalza */
static int store_value(Fpa *f, const FpaReg *r, int prec, uint32_t *m)
{
    Num n, o;
    int inexact = 0;
    if (!is_plain(r)) return 1;
    n = reg_num(r);
    if (prec == 2) {
        m[0] = r->w[0] & (I_SIGN | I_EXP); m[1] = r->w[1]; m[2] = r->w[2];
        return 0;
    }
    if (n.mant == 0) {
        m[0] = n.sign ? I_SIGN : 0; m[1] = 0;
        return 0;
    }
    /* STF arrotonda sempre al piu' vicino */
    if (round_to(n.sign, (U128){ 0, n.mant }, n.exp - EBIAS - 63, 0, prec, RM_NEAREST, &o, &inexact)) return 1;
    if (!inexact_ok(f, inexact)) return 1;
    if (prec == 0) {
        m[0] = (o.sign ? I_SIGN : 0) | (uint32_t)(o.exp - EBIAS + 127) << 23 | (uint32_t)(o.mant >> 40) & 0x7FFFFF;
    } else {
        uint64_t fr = o.mant >> 11 & 0xFFFFFFFFFFFFFull;
        m[0] = (o.sign ? I_SIGN : 0) | (uint32_t)(o.exp - EBIAS + 1023) << 20 | (uint32_t)(fr >> 32);
        m[1] = (uint32_t)fr;
    }
    return 0;
}

static int exec_cpdt(Fpa *f, uint32_t i, const FpaHost *h, int *cycles)
{
    int cp = (i >> 8) & 15, p = (i >> 24) & 1, u = (i >> 23) & 1, w = (i >> 21) & 1, l = (i >> 20) & 1;
    int rn = (i >> 16) & 15, fd = (i >> 12) & 7;
    int bit22 = (i >> 22) & 1, bit15 = (i >> 15) & 1;
    uint32_t base = h->get_reg(h->ctx, rn), off = (i & 0xFF) * 4;
    uint32_t moved = u ? base + off : base - off, addr = p ? moved : base;
    int words, n, rc;
    uint32_t m[12];

    if (cp == 1) {
        int prec = bit22 << 1 | bit15;               /* S D E P */
        if (prec == 3) return bounce(f);             /* decimale: al supporto */
        words = prec + 1;
        if (l) {
            for (int k = 0; k < words; k++)
                if ((rc = h->read32(h->ctx, addr + 4 * k, &m[k])) != FPA_DONE) return rc;
            if (prec == 0) load_single(&f->f[fd], m[0]);
            else if (prec == 1) load_double(&f->f[fd], m[0], m[1]);
            else load_extended(&f->f[fd], m[0], m[1], m[2]);
        } else {
            if (store_value(f, &f->f[fd], prec, m)) return bounce(f);
            for (int k = 0; k < words; k++)
                if ((rc = h->write32(h->ctx, addr + 4 * k, m[k])) != FPA_DONE) return rc;
        }
        *cycles = T_LDF(prec);
    } else {                                         /* coprocessore 2: LFM / SFM */
        n = bit22 << 1 | bit15;
        if (n == 0) n = 4;
        words = 3 * n;
        if (l) {
            for (int k = 0; k < words; k++)
                if ((rc = h->read32(h->ctx, addr + 4 * k, &m[k])) != FPA_DONE) return rc;
            for (int k = 0; k < n; k++) {
                FpaReg *r = &f->f[(fd + k) & 7];
                uint32_t *q = m + 3 * k;
                if (q[0] & I_UNCOMMON) { r->w[0] = q[0]; r->w[1] = q[1]; r->w[2] = q[2]; }
                else load_extended(r, q[0], q[1], q[2]);
            }
        } else {
            for (int k = 0; k < n; k++) {
                const FpaReg *r = &f->f[(fd + k) & 7];
                m[3 * k] = r->w[0]; m[3 * k + 1] = r->w[1]; m[3 * k + 2] = r->w[2];
            }
            for (int k = 0; k < words; k++)
                if ((rc = h->write32(h->ctx, addr + 4 * k, m[k])) != FPA_DONE) return rc;
        }
        *cycles = T_LFM(n);
    }
    if (w && rn != 15) h->set_reg(h->ctx, rn, moved);
    return FPA_DONE;
}

/* ---- CPDO: le operazioni aritmetiche ---- */

static int exec_cpdo(Fpa *f, uint32_t i, int *cycles)
{
    int op = (i >> 20) & 15, monadic = (i >> 15) & 1;
    int prec = ((i >> 18) & 2) | ((i >> 7) & 1), rm = (i >> 5) & 3;
    int fn = (i >> 16) & 7, fd = (i >> 12) & 7, inexact = 0, bad;
    Num a, b, r;

    if (prec == 3) return bounce(f);                 /* non definita */
    if (i & 8) b = constant(i & 7);
    else {
        if (!is_plain(&f->f[i & 7])) return bounce(f);
        b = reg_num(&f->f[i & 7]);
    }
    if (monadic) {
        switch (op) {
        case 0: case 1: case 2:                      /* MVF MNF ABS */
            if (op == 1) b.sign ^= 1;
            if (op == 2) b.sign = 0;
            if (b.mant == 0) r = b;
            else if (round_to(b.sign, (U128){ 0, b.mant }, b.exp - EBIAS - 63, 0, prec, rm, &r, &inexact))
                return bounce(f);
            *cycles = T_MOVE;
            break;
        default:                                     /* RND SQT trascendenti URD NRM */
            return bounce(f);
        }
    } else {
        if (op > 11 || (op >= 6 && op <= 8)) return bounce(f);  /* POW RPW RMF POL, non definite */
        if (!is_plain(&f->f[fn])) return bounce(f);
        a = reg_num(&f->f[fn]);
        switch (op) {
        case 0: bad = op_add(a, b, prec, rm, &r, &inexact); *cycles = T_ADD; break;        /* ADF */
        case 1: bad = op_mul(a, b, prec, rm, &r, &inexact); *cycles = T_MUL; break;        /* MUF */
        case 2: b.sign ^= 1; bad = op_add(a, b, prec, rm, &r, &inexact); *cycles = T_ADD; break;  /* SUF */
        case 3: a.sign ^= 1; bad = op_add(b, a, prec, rm, &r, &inexact); *cycles = T_ADD; break;  /* RSF */
        case 4: bad = op_div(a, b, prec, rm, &r, &inexact); *cycles = T_DIV(prec); break;  /* DVF */
        case 5: bad = op_div(b, a, prec, rm, &r, &inexact); *cycles = T_DIV(prec); break;  /* RDF */
        case 9:                                      /* FML: i 32 bit alti delle mantisse */
            a.mant &= 0xFFFFFFFF00000000ull;
            b.mant &= 0xFFFFFFFF00000000ull;
            bad = op_mul(a, b, prec, rm, &r, &inexact);
            *cycles = T_FML;
            break;
        case 10: bad = op_div(a, b, prec, rm, &r, &inexact); *cycles = T_DIV(prec); break; /* FDV */
        default: bad = op_div(b, a, prec, rm, &r, &inexact); *cycles = T_DIV(prec); break; /* FRD */
        }
        if (bad) return bounce(f);
    }
    if (!inexact_ok(f, inexact)) return bounce(f);
    num_reg(&f->f[fd], r);
    return FPA_DONE;
}

/* ---- CPRT: FLT FIX, registri di stato e controllo, confronti ---- */

static int exec_cprt(Fpa *f, uint32_t i, const FpaHost *h, int *cycles)
{
    int op = (i >> 20) & 15, rd = (i >> 12) & 15, fn = (i >> 16) & 7;
    int prec = ((i >> 18) & 2) | ((i >> 7) & 1), rm = (i >> 5) & 3, inexact = 0;
    Num a, b, r;

    switch (op) {
    case 0: {                                        /* FLT Fn, Rd */
        uint32_t v = h->get_reg(h->ctx, rd);
        if (prec == 3) return bounce(f);
        if (v == 0) { r.sign = 0; r.exp = 0; r.mant = 0; }
        else {
            int s = (int32_t)v < 0;
            uint32_t mag = s ? 0u - v : v;
            if (round_to(s, (U128){ 0, mag }, 0, 0, prec, rm, &r, &inexact)) return bounce(f);
            if (!inexact_ok(f, inexact)) return bounce(f);
        }
        num_reg(&f->f[fn], r);
        *cycles = T_FLT;
        return FPA_DONE;
    }
    case 1: {                                        /* FIX Rd, Fm */
        int64_t iv;
        int sh;
        uint64_t q, low;
        if (i & 8 || !is_plain(&f->f[i & 7])) return bounce(f);
        a = reg_num(&f->f[i & 7]);
        if (a.mant == 0) iv = 0;
        else {
            sh = 63 - (a.exp - EBIAS);               /* bit della frazione */
            if (sh <= 31) return bounce(f);          /* |x| >= 2^32: non valida */
            if (sh >= 64) { q = 0; low = a.mant; inexact = 1; }
            else { q = a.mant >> sh; low = a.mant << (64 - sh); inexact = low != 0; }
            if (inexact) {
                int up = 0, half = sh >= 64 ? sh == 64 && (low >> 63) : (low >> 63) != 0;
                int rest = sh > 64 ? 1 : (low << 1) != 0;
                if (sh > 64) half = 0;
                if (rm == RM_NEAREST) up = half && (rest || (q & 1));
                else if (rm == RM_PLUS) up = !a.sign;
                else if (rm == RM_MINUS) up = a.sign;
                q += up;
            }
            iv = a.sign ? -(int64_t)q : (int64_t)q;
            if (iv < -2147483647 - 1 || iv > 2147483647) return bounce(f);
        }
        if (!inexact_ok(f, inexact)) return bounce(f);
        h->set_reg(h->ctx, rd, (uint32_t)iv);
        *cycles = T_FIX;
        return FPA_DONE;
    }
    case 2:                                          /* WFS */
        f->fpsr = (f->fpsr & ~FPSR_WRITABLE) | (h->get_reg(h->ctx, rd) & FPSR_WRITABLE);
        *cycles = T_RW;
        return FPA_DONE;
    case 3:                                          /* RFS */
        h->set_reg(h->ctx, rd, FPA_SYSID << 24 | (f->fpsr & FPSR_WRITABLE));
        *cycles = T_RW;
        return FPA_DONE;
    case 4:                                          /* WFC: solo SB, AB, DA */
        if (!h->privileged(h->ctx)) return bounce(f);
        f->fpcr = (f->fpcr & ~(FPCR_SB | FPCR_AB | FPCR_DA)) |
                  (h->get_reg(h->ctx, rd) & (FPCR_SB | FPCR_AB | FPCR_DA));
        *cycles = T_RW;
        return FPA_DONE;
    case 5:                                          /* RFC: li azzera */
        if (!h->privileged(h->ctx)) return bounce(f);
        h->set_reg(h->ctx, rd, f->fpcr);
        f->fpcr &= ~(FPCR_SB | FPCR_AB | FPCR_DA);
        *cycles = T_RW;
        return FPA_DONE;
    case 9: case 11: case 13: case 15: {             /* CMF CNF CMFE CNFE */
        uint32_t res;
        if (rd != 15 || !is_plain(&f->f[fn])) return bounce(f);
        if (i & 8) b = constant(i & 7);
        else {
            if (!is_plain(&f->f[i & 7])) return bounce(f);
            b = reg_num(&f->f[i & 7]);
        }
        a = reg_num(&f->f[fn]);
        if (op == 11 || op == 15) b.sign ^= 1;      /* CNF: con -Fm */
        if (a.mant == 0 && b.mant == 0) res = COMP_EQ;
        else if (a.sign != b.sign) res = a.sign ? COMP_LT : COMP_GT;
        else {
            int c = a.exp != b.exp ? (a.exp < b.exp ? -1 : 1) : a.mant != b.mant ? (a.mant < b.mant ? -1 : 1) : 0;
            if (a.mant == 0) c = -1;                 /* zero contro non zero */
            if (b.mant == 0) c = a.mant ? 1 : 0;
            if (a.sign) c = -c;
            res = c < 0 ? COMP_LT : c > 0 ? COMP_GT : COMP_EQ;
        }
        h->set_flags(h->ctx, res);
        *cycles = T_CMP;
        return FPA_DONE;
    }
    default:                                         /* non definite */
        return bounce(f);
    }
}

int fpa_exec(Fpa *f, uint32_t i, const FpaHost *h, int *cycles)
{
    int cp = (i >> 8) & 15, rc;
    *cycles = 1;
    if (cp != 1 && cp != 2) return FPA_NOT_FPA;
    /* disattivato: rimbalza tutto tranne RFC e WFC */
    if (f->fpcr & FPCR_DA) {
        int rt = ((i >> 24) & 15) == 14 && (i & 0x10) && cp == 1;
        int op = (i >> 20) & 15;
        if (!(rt && (op == 4 || op == 5))) { f->bounces++; return FPA_BOUNCE; }
    }
    if (((i >> 25) & 7) == 6) rc = exec_cpdt(f, i, h, cycles);           /* LDC/STC */
    else if (cp == 2) rc = bounce(f);                                    /* CPDO/CPRT del 2 */
    else if (i & 0x10) rc = exec_cprt(f, i, h, cycles);
    else rc = exec_cpdo(f, i, cycles);
    if (rc == FPA_DONE) f->executed++;
    return rc;
}
