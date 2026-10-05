/*
 * test_fpa.c - Prove della FPA10 (src/cpu/fpa.c) con una CPU finta: le
 * operazioni in singola e doppia precisione confrontate bit per bit con
 * l'aritmetica IEEE del PC (arrotondamento al piu' vicino), FLT/FIX, i
 * confronti, l'estesa, le conversioni di LDF e i rimbalzi al codice di
 * supporto.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "cpu/fpa.h"

static uint32_t mem[1024], reg[16], flags;
static int priv = 1, fails;

static uint32_t h_get(void *c, int n) { (void)c; return reg[n]; }
static void h_set(void *c, int n, uint32_t v) { (void)c; reg[n] = v; }
static void h_flags(void *c, uint32_t v) { (void)c; flags = v; }
static int h_rd(void *c, uint32_t a, uint32_t *v) { (void)c; *v = mem[(a >> 2) & 1023]; return FPA_DONE; }
static int h_wr(void *c, uint32_t a, uint32_t v) { (void)c; mem[(a >> 2) & 1023] = v; return FPA_DONE; }
static int h_priv(void *c) { (void)c; return priv; }
static const FpaHost host = { 0, h_get, h_set, h_flags, h_rd, h_wr, h_priv };
static Fpa fpa;

static int run(uint32_t i)
{
    int cyc;
    return fpa_exec(&fpa, i, &host, &cyc);
}

/* codifiche (condizione AL) */
static uint32_t ldf(int l, int prec, int fd, int rn, int off)
{
    return 0xED800100u | (uint32_t)l << 20 | (uint32_t)(prec & 2) << 21 | (uint32_t)(prec & 1) << 15 |
           (uint32_t)rn << 16 | (uint32_t)fd << 12 | (uint32_t)off;
}
static uint32_t cpdo(int op, int monadic, int prec, int rm, int fd, int fn, int fm)
{
    return 0xEE000100u | (uint32_t)op << 20 | (uint32_t)(prec & 2) << 18 | (uint32_t)fn << 16 |
           (uint32_t)monadic << 15 | (uint32_t)fd << 12 | (uint32_t)(prec & 1) << 7 | (uint32_t)rm << 5 | (uint32_t)fm;
}
static uint32_t cprt(int op, int prec, int rm, int fn, int rd, int fm)
{
    return 0xEE000110u | (uint32_t)op << 20 | (uint32_t)(prec & 2) << 18 | (uint32_t)fn << 16 |
           (uint32_t)rd << 12 | (uint32_t)(prec & 1) << 7 | (uint32_t)rm << 5 | (uint32_t)fm;
}

static void check(int ok, const char *what)
{
    if (!ok) { printf("  ERRORE: %s\n", what); fails++; }
}

/* numeri casuali "normali" su un intervallo di esponenti */
static uint64_t rnd64(void)
{
    uint64_t v = 0;
    for (int i = 0; i < 4; i++) v = v << 16 | (uint64_t)(rand() & 0xFFFF);
    return v;
}
static double rnd_double(int erange)
{
    uint64_t b = rnd64() & 0x800FFFFFFFFFFFFFull;
    int e = 1023 + (rand() % (2 * erange + 1)) - erange;
    b |= (uint64_t)e << 52;
    double d;
    memcpy(&d, &b, 8);
    return d;
}

static void put_double(int at, double d)
{
    uint64_t b;
    memcpy(&b, &d, 8);
    mem[at] = (uint32_t)(b >> 32); mem[at + 1] = (uint32_t)b;
}
static double get_double(int at)
{
    uint64_t b = (uint64_t)mem[at] << 32 | mem[at + 1];
    double d;
    memcpy(&d, &b, 8);
    return d;
}
static void put_float(int at, float f) { memcpy(&mem[at], &f, 4); }
static float get_float(int at) { float f; memcpy(&f, &mem[at], 4); return f; }

static int is_normal_or_zero_d(double d) { return d == 0 || (isfinite(d) && fabs(d) >= 2.2250738585072014e-308); }
static int is_normal_or_zero_f(float f) { return f == 0 || (isfinite(f) && fabsf(f) >= 1.17549435e-38f); }

/* op: 0 ADF, 1 MUF, 2 SUF, 4 DVF */
static void test_arith_double(int op, const char *name)
{
    int bad = 0, bounced = 0;
    for (int k = 0; k < 20000; k++) {
        double a = rnd_double(k < 10000 ? 8 : 300), b = rnd_double(k < 10000 ? 8 : 300), want;
        if (k % 7 == 0) b = -b;
        if (k % 97 == 0) b = 0;
        want = op == 0 ? a + b : op == 1 ? a * b : op == 2 ? a - b : a / b;
        put_double(0, a); put_double(2, b);
        reg[0] = 0;
        run(ldf(1, 1, 1, 0, 0)); run(ldf(1, 1, 2, 0, 2));
        fpa.fpsr = 0;
        if (run(cpdo(op, 0, 1, 0, 3, 1, 2)) != FPA_DONE) {
            /* deve rimbalzare solo se il risultato non e' normale (o b = 0 in DVF) */
            if (is_normal_or_zero_d(want) && !(op == 4 && b == 0)) { if (bad++ < 5) printf("    %s %.17g %.17g: rimbalzo inatteso\n", name, a, b); }
            bounced++;
            continue;
        }
        run(ldf(0, 1, 3, 0, 4));
        if (memcmp(&want, &(double){ get_double(4) }, 8) != 0) {
            if (bad++ < 5) printf("    %s %.17g %.17g = %.17g, atteso %.17g\n", name, a, b, get_double(4), want);
        }
    }
    printf("  %s doppia: %d errori, %d rimbalzi su 20000\n", name, bad, bounced);
    if (bad) fails++;
}

static void test_arith_single(int op, const char *name)
{
    int bad = 0;
    for (int k = 0; k < 20000; k++) {
        float a = (float)rnd_double(20), b = (float)rnd_double(20), want;
        if (k % 5 == 0) b = -b;
        want = op == 0 ? a + b : op == 1 ? a * b : op == 2 ? a - b : a / b;
        if (!is_normal_or_zero_f(want)) continue;
        put_float(0, a); put_float(1, b);
        reg[0] = 0;
        run(ldf(1, 0, 1, 0, 0)); run(ldf(1, 0, 2, 0, 1));
        if (run(cpdo(op, 0, 0, 0, 3, 1, 2)) != FPA_DONE) { bad++; continue; }
        run(ldf(0, 0, 3, 0, 4));
        if (memcmp(&want, &(float){ get_float(4) }, 4) != 0) {
            if (bad++ < 5) printf("    %s %.9g %.9g = %.9g, atteso %.9g\n", name, a, b, get_float(4), want);
        }
    }
    printf("  %s singola: %d errori\n", name, bad);
    if (bad) fails++;
}

int main(void)
{
    srand(12345);
    fpa_init(&fpa, FPA_FPA10);

    /* al reset e' disattivata: tutto rimbalza tranne RFC e WFC */
    check(run(ldf(1, 1, 0, 0, 0)) == FPA_BOUNCE, "da disattivata LDF deve rimbalzare");
    check(run(cprt(5, 0, 0, 0, 4, 0)) == FPA_DONE, "RFC da disattivata");
    check((reg[4] & 0xD00) == 0xD00, "RFC: DA AB SB al reset");
    check(run(cprt(3, 0, 0, 0, 2, 0)) == FPA_DONE && reg[2] >> 24 == 0x81, "RFS: identificativo 0x81");
    priv = 0;
    check(run(cprt(5, 0, 0, 0, 4, 0)) == FPA_BOUNCE, "RFC in modo utente rimbalza");
    priv = 1;
    run(cprt(5, 0, 0, 0, 4, 0));                    /* azzera SB */
    reg[3] = 0;
    run(cprt(2, 0, 0, 0, 3, 0));                    /* WFS 0 */

    test_arith_double(0, "ADF");
    test_arith_double(2, "SUF");
    test_arith_double(1, "MUF");
    test_arith_double(4, "DVF");
    test_arith_single(0, "ADF");
    test_arith_single(1, "MUF");
    test_arith_single(4, "DVF");

    /* FLT e FIX, nei quattro modi di arrotondamento */
    {
        static const double vals[] = { 0.5, 1.5, 2.5, -0.5, -1.5, -2.5, 3.7, -3.7, 123456.49, -0.0001, 2147483647.0, -2147483648.0 };
        static const int32_t want[4][12] = {
            { 0, 2, 2, 0, -2, -2, 4, -4, 123456, 0, 2147483647, -2147483647 - 1 },     /* piu' vicino */
            { 1, 2, 3, 0, -1, -2, 4, -3, 123457, 0, 2147483647, -2147483647 - 1 },     /* +inf */
            { 0, 1, 2, -1, -2, -3, 3, -4, 123456, -1, 2147483647, -2147483647 - 1 },   /* -inf */
            { 0, 1, 2, 0, -1, -2, 3, -3, 123456, 0, 2147483647, -2147483647 - 1 }      /* zero */
        };
        int bad = 0;
        for (int m = 0; m < 4; m++)
            for (int k = 0; k < 12; k++) {
                put_double(0, vals[k]);
                run(ldf(1, 1, 1, 0, 0));
                if (run(cprt(1, 0, m, 0, 5, 1)) != FPA_DONE || (int32_t)reg[5] != want[m][k]) {
                    if (bad++ < 5) printf("    FIX modo %d di %g = %d, atteso %d\n", m, vals[k], (int32_t)reg[5], want[m][k]);
                }
            }
        put_double(0, 3e9);
        run(ldf(1, 1, 1, 0, 0));
        check(run(cprt(1, 0, 0, 0, 5, 1)) == FPA_BOUNCE, "FIX di 3e9 rimbalza (non valida)");
        for (int k = 0; k < 2000; k++) {
            int32_t v = (int32_t)(rnd64() >> (rand() % 40));
            reg[6] = (uint32_t)v;
            run(cprt(0, 1, 0, 2, 6, 0));            /* FLTD F2, R6 */
            run(ldf(0, 1, 2, 0, 4));
            if (get_double(4) != (double)v) { if (bad++ < 5) printf("    FLT %d\n", v); }
            run(cprt(0, 0, 0, 2, 6, 0));            /* FLTS: arrotondato */
            run(ldf(0, 0, 2, 0, 4));
            if (get_float(4) != (float)v) { if (bad++ < 5) printf("    FLTS %d\n", v); }
        }
        printf("  FLT/FIX: %d errori\n", bad);
        if (bad) fails++;
    }

    /* confronti: N Z C V */
    {
        static const double p[][2] = { { 1, 2 }, { 2, 1 }, { 3, 3 }, { -1, 1 }, { 0, -0.0 }, { -5, -7 }, { 0, 1e-300 } };
        static const uint32_t want[] = { 0x80000000u, 0x20000000u, 0x60000000u, 0x80000000u, 0x60000000u, 0x20000000u, 0x80000000u };
        for (int k = 0; k < 7; k++) {
            put_double(0, p[k][0]); put_double(2, p[k][1]);
            run(ldf(1, 1, 1, 0, 0)); run(ldf(1, 1, 2, 0, 2));
            flags = 0;
            run(cprt(9, 0, 0, 1, 15, 2));           /* CMF F1, F2 */
            if (flags != want[k]) { printf("  ERRORE CMF %g %g: %08X\n", p[k][0], p[k][1], flags); fails++; }
        }
        put_double(0, 3);
        run(ldf(1, 1, 1, 0, 0));
        run(cprt(11, 0, 0, 1, 15, 8 | 3));          /* CNF F1, #3 -> 3 contro -3 */
        check(flags == 0x20000000u, "CNF con costante");
    }

    /* estesa: 1 + 2^-60 si rappresenta, in doppia no */
    {
        put_double(0, 1.0); put_double(2, ldexp(1.0, -60));
        run(ldf(1, 1, 1, 0, 0)); run(ldf(1, 1, 2, 0, 2));
        run(cpdo(0, 0, 2, 0, 3, 1, 2));             /* ADFE */
        run(ldf(0, 2, 3, 0, 4));                    /* STFE */
        check(mem[4] == 0x3FFF && mem[5] == 0x80000000u && mem[6] == 0x8, "ADFE 1 + 2^-60");
        run(cpdo(0, 0, 1, 0, 3, 1, 2));             /* ADFD: arrotonda a 1 */
        run(ldf(0, 1, 3, 0, 4));
        check(get_double(4) == 1.0, "ADFD 1 + 2^-60 = 1");
    }

    /* LDF dei valori speciali: il bit uncommon, e STF che rimbalza */
    {
        mem[0] = 0x00000001u;                       /* denormale singolo */
        run(ldf(1, 0, 1, 0, 0));
        check((fpa.f[1].w[0] & 0x40007FFFu) == (0x40000000u | 0x3F80), "LDFS denormale: uncommon, esponente &3F80");
        check(run(ldf(0, 0, 1, 0, 4)) == FPA_BOUNCE, "STF di un denormale rimbalza");
        mem[0] = 0x7F800000u;                       /* infinito */
        run(ldf(1, 0, 1, 0, 0));
        check((fpa.f[1].w[0] & 0x40007FFFu) == (0x40000000u | 0x407F), "LDFS infinito: &407F");
        check(run(cpdo(0, 0, 1, 0, 3, 1, 1)) == FPA_BOUNCE, "ADF con un infinito rimbalza");
        check(fpa.fpcr & 0x800, "rimbalzo sincrono: SB");
        run(cprt(5, 0, 0, 0, 4, 0));
        check(!(fpa.fpcr & 0xD00), "RFC azzera SB AB DA");
    }

    /* istruzioni lasciate al codice di supporto */
    check(run(cpdo(4, 1, 1, 0, 1, 0, 2)) == FPA_BOUNCE, "SQT rimbalza");
    check(run(cpdo(8, 1, 1, 0, 1, 0, 2)) == FPA_BOUNCE, "SIN rimbalza");
    check(run(ldf(1, 3, 1, 0, 0)) == FPA_BOUNCE, "LDFP rimbalza");

    /* LFM / SFM: il formato interno passa intatto */
    {
        FpaReg save[4];
        memcpy(save, &fpa.f[4], sizeof save);
        reg[0] = 0;
        run(0xED800200u | 4u << 12 | 64);           /* SFM F4, 4, [R0, #256] */
        memset(&fpa.f[4], 0, sizeof save);
        run(0xED900200u | 4u << 12 | 64);           /* LFM F4, 4, [R0, #256] */
        check(!memcmp(save, &fpa.f[4], sizeof save), "SFM/LFM");
    }

    /* trappola dell'inesatto abilitata: rimbalza invece di mettere il flag */
    {
        reg[3] = 0x00100000u;                       /* IXE */
        run(cprt(2, 0, 0, 0, 3, 0));
        put_double(0, 1.0); put_double(2, 3.0);
        run(ldf(1, 1, 1, 0, 0)); run(ldf(1, 1, 2, 0, 2));
        check(run(cpdo(4, 0, 1, 0, 3, 1, 2)) == FPA_BOUNCE, "1/3 con IXE rimbalza");
        check(run(cpdo(0, 0, 1, 0, 3, 1, 2)) == FPA_DONE, "1+3 con IXE e' esatto");
    }

    printf(fails ? "%d ERRORI\n" : "tutto giusto\n", fails);
    return fails != 0;
}
