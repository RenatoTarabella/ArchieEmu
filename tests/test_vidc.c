/*
 * test_vidc.c - Test del VIDC1a: decodifica dei registri, geometria dei
 * modi standard di RISC OS e rendering dalla RAM fisica.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vidc.h"

static int checks, failures;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf("FALLITO %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); checks++; \
    if (a_ != b_) { failures++; printf("FALLITO %s:%d: %s = %lld, atteso %lld\n", \
    __FILE__, __LINE__, #a, a_, b_); } } while (0)

/* ------------------------------------------------------------------ */
/* programmazione di un modo                                          */
/* ------------------------------------------------------------------ */

/*
 * Scrive i registri come fa RISC OS 3 a partire dalla descrizione del
 * modo (sync, back porch, bordo, display, bordo, front porch, in pixel
 * e in righe): sono le formule del datasheet, usate dalla macro
 * VIDC_List del kernel. 'clk' e' il codice del clock pixel (0-3).
 */
typedef struct ModeDesc {
    int lbpp;
    int hsync, hbpch, hlbdr, hdisp, hrbdr, hfpch;
    int vsync, vbpch, vlbdr, vdisp, vrbdr, vfpch;
    int clk;
} ModeDesc;

static uint32_t reg(unsigned r, int v) { return (uint32_t)r << 24 | ((uint32_t)v & 0x3FF) << 14; }

static void program_mode(Vidc *v, const ModeDesc *m)
{
    static const int sub[4] = { 19, 11, 7, 5 };
    int s = sub[m->lbpp];
    int h1 = m->hsync, h2 = h1 + m->hbpch, h3 = h2 + m->hlbdr;
    int h4 = h3 + m->hdisp, h5 = h4 + m->hrbdr, h6 = h5 + m->hfpch;
    int v1 = m->vsync, v2 = v1 + m->vbpch, v3 = v2 + m->vlbdr;
    int v4 = v3 + m->vdisp, v5 = v4 + m->vrbdr, v6 = v5 + m->vfpch;

    vidc_write(v, reg(0x80, (h6 - 2) / 2));
    vidc_write(v, reg(0x84, (h1 - 2) / 2));
    vidc_write(v, reg(0x88, (h2 - 1) / 2));
    vidc_write(v, reg(0x8C, (h3 - s) / 2));
    vidc_write(v, reg(0x90, (h4 - s) / 2));
    vidc_write(v, reg(0x94, (h5 - 1) / 2));
    vidc_write(v, reg(0x9C, ((h6 - 2) / 2 + 1) / 2));
    vidc_write(v, reg(0xA0, v6 - 1));
    vidc_write(v, reg(0xA4, v1 - 1));
    vidc_write(v, reg(0xA8, v2 - 1));
    vidc_write(v, reg(0xAC, v3 - 1));
    vidc_write(v, reg(0xB0, v4 - 1));
    vidc_write(v, reg(0xB4, v5 - 1));
    vidc_write(v, 0xE0000000u | 0x30u | (uint32_t)m->lbpp << 2 | (uint32_t)m->clk);
}

/* tabelle del kernel, monitor tipo 0 (TV 50 Hz) e 3 (VGA, a 24 MHz sul VIDC1) */
static const ModeDesc MODE0  = { 0, 76, 88, 96, 640, 96, 28, 3, 19, 16, 256, 16, 2, 2 };
static const ModeDesc MODE12 = { 2, 76, 88, 96, 640, 96, 28, 3, 19, 16, 256, 16, 2, 2 };
static const ModeDesc MODE13 = { 3, 38, 44, 48, 320, 48, 14, 3, 19, 16, 256, 16, 2, 0 };
static const ModeDesc MODE15 = { 3, 76, 88, 96, 640, 96, 28, 3, 19, 16, 256, 16, 2, 2 };
static const ModeDesc MODE27 = { 2, 96, 46,  0, 640,  0, 18, 2, 32,  0, 480,  0, 11, 3 };
static const ModeDesc MODE28 = { 3, 96, 46,  0, 640,  0, 18, 2, 32,  0, 480,  0, 11, 3 };

/* ------------------------------------------------------------------ */
/* test                                                               */
/* ------------------------------------------------------------------ */

static void test_registers(void)
{
    Vidc v;
    vidc_reset(&v);

    vidc_write(&v, 0x00000123u);
    vidc_write(&v, 0x3C001FFFu);
    vidc_write(&v, 0x20000ABCu);                 /* colore 8 */
    CHECK_EQ(v.palette[0], 0x123);
    CHECK_EQ(v.palette[15], 0x1FFF);
    CHECK_EQ(v.palette[8], 0xABC);
    CHECK_EQ(v.palette[1], 0);

    vidc_write(&v, 0x40000F80u);
    vidc_write(&v, 0x44000111u);
    vidc_write(&v, 0x48000222u);
    vidc_write(&v, 0x4C000333u);
    CHECK_EQ(v.border, 0xF80);
    CHECK_EQ(v.cursor_palette[0], 0x111);
    CHECK_EQ(v.cursor_palette[1], 0x222);
    CHECK_EQ(v.cursor_palette[2], 0x333);
    CHECK_EQ(vidc_border_rgb(&v), 0x0088FF);

    vidc_write(&v, 0x60000003u);                 /* canale 7 */
    vidc_write(&v, 0x64000005u);                 /* canale 0 */
    vidc_write(&v, 0x7C000006u);                 /* canale 6 */
    CHECK_EQ(v.stereo[7], 3);
    CHECK_EQ(v.stereo[0], 5);
    CHECK_EQ(v.stereo[6], 6);

    vidc_write(&v, reg(0x80, 511));
    vidc_write(&v, reg(0xBC, 0x3FF));
    CHECK_EQ(v.hcr, 511);
    CHECK_EQ(v.vcer, 0x3FF);
    vidc_write(&v, 0x98000000u | 1234u << 13);   /* HCSR ha un bit in piu' */
    CHECK_EQ(v.hcsr, 1234);
    vidc_write(&v, 0xC00001FEu);
    CHECK_EQ(v.sound_freq, 0x1FE);
    vidc_write(&v, 0xE00000BEu);
    CHECK_EQ(v.control, 0xBE);

    /* le scritture non devono toccare altri registri */
    CHECK_EQ(v.hswr, 0);
    CHECK_EQ(v.palette[0], 0x123);
}

static void check_mode(const char *name, const ModeDesc *m, int w, int h, int bpp_log2,
                       int total, int dstart, ArcTime line, int hz_lo, int hz_hi)
{
    Vidc v;
    VidcTiming t;
    vidc_reset(&v);
    vidc_timing(&v, &t);
    CHECK(!t.valid);
    program_mode(&v, m);
    vidc_timing(&v, &t);
    printf("  %-8s %dx%d %d bpp, %d righe, display %d-%d, riga %llu, frame %llu (%.2f Hz)\n",
           name, t.width, t.height, 1 << t.log2bpp, t.total_lines, t.display_start,
           t.display_end, (unsigned long long)t.line_time, (unsigned long long)t.frame_time,
           t.frame_time ? (double)ARC_HZ / (double)t.frame_time : 0.0);
    CHECK(t.valid);
    CHECK_EQ(t.width, w);
    CHECK_EQ(t.height, h);
    CHECK_EQ(t.log2bpp, bpp_log2);
    CHECK_EQ(t.total_lines, total);
    CHECK_EQ(t.display_start, dstart);
    CHECK_EQ(t.display_end, dstart + h);
    CHECK_EQ(t.line_time, line);
    CHECK_EQ(t.frame_time, line * (ArcTime)total);
    ArcTime hz = t.frame_time ? ARC_HZ / t.frame_time : 0;
    CHECK(hz >= (ArcTime)hz_lo && hz <= (ArcTime)hz_hi);
}

static void test_modes(void)
{
    /* TV: 1024 pixel a 16 MHz = 64 us per riga, 312 righe, 50 Hz */
    check_mode("MODE 0",  &MODE0,  640, 256, 0, 312, 38, ARC_US(64), 50, 50);
    check_mode("MODE 12", &MODE12, 640, 256, 2, 312, 38, ARC_US(64), 50, 50);
    check_mode("MODE 13", &MODE13, 320, 256, 3, 312, 38, ARC_US(64), 50, 50);
    check_mode("MODE 15", &MODE15, 640, 256, 3, 312, 38, ARC_US(64), 50, 50);
    /* VGA: 800 pixel a 24 MHz = 33,3 us, 525 righe: 57 Hz sul VIDC1 */
    check_mode("MODE 27", &MODE27, 640, 480, 2, 525, 34, 800, 56, 60);
    check_mode("MODE 28", &MODE28, 640, 480, 3, 525, 34, 800, 56, 60);
}

/* modo minuscolo per i test di rendering: 'w' x 'h' pixel a 24 MHz */
static void tiny_mode(Vidc *v, int lbpp, int w, int h)
{
    ModeDesc m = { lbpp, 8, 13, 0, w, 0, 10, 2, 3, 0, h, 0, 5, 3 };
    vidc_reset(v);
    program_mode(v, &m);
}

#define RAM_SIZE (64 * 1024)
static uint8_t ram[RAM_SIZE];
static uint32_t out[1024 * 768];

static uint32_t c12(int i) { return (uint32_t)(i * 17) << 16 | (uint32_t)((15 - i) * 17) << 8 | (uint32_t)(i ^ 5) * 17; }
static void set_test_palette(Vidc *v)
{
    for (uint32_t i = 0; i < 16; i++)
        vidc_write(v, i << 26 | ((i ^ 5) << 8) | ((15 - i) << 4) | i);
}

static void test_render_depths(void)
{
    Vidc v;
    VidcTiming t;
    int w, h;

    for (int lbpp = 0; lbpp < 3; lbpp++) {
        int bits = 1 << lbpp, mask = (1 << bits) - 1;
        tiny_mode(&v, lbpp, 64, 3);
        set_test_palette(&v);
        vidc_timing(&v, &t);
        CHECK(t.valid);
        CHECK_EQ(t.width, 64);
        memset(ram, 0, sizeof ram);
        int nbytes = 64 * 3 * bits / 8;
        for (int i = 0; i < nbytes; i++)
            ram[0x1000 + i] = (uint8_t)(i * 37 + 11);
        memset(out, 0xEE, sizeof out);
        vidc_render(&v, ram, RAM_SIZE, 0x1000, 0x1000, 0x1FF0, 0, 0, out, 100, &w, &h);
        CHECK_EQ(w, 64);
        CHECK_EQ(h, 3);
        int bad = 0;
        for (int y = 0; y < 3; y++)
            for (int x = 0; x < 64; x++) {
                int bit = (y * 64 + x) * bits;
                int p = (ram[0x1000 + bit / 8] >> (bit % 8)) & mask;
                if (out[y * 100 + x] != c12(p)) bad++;
            }
        CHECK_EQ(bad, 0);
        CHECK_EQ(out[64], 0xEEEEEEEEu);              /* oltre la larghezza non scrive */
    }

    /* 1 bpp: il pixel piu' a sinistra e' il bit 0 */
    tiny_mode(&v, 0, 32, 1);
    vidc_write(&v, 0x00000000u);
    vidc_write(&v, 0x04000FFFu);
    memset(ram, 0, sizeof ram);
    ram[0] = 0x01;
    ram[3] = 0x80;
    vidc_render(&v, ram, RAM_SIZE, 0, 0, 0x100, 0, 0, out, 32, &w, &h);
    CHECK_EQ(out[0], 0xFFFFFF);
    CHECK_EQ(out[1], 0x000000);
    CHECK_EQ(out[30], 0x000000);
    CHECK_EQ(out[31], 0xFFFFFF);

    /* 4 bpp: nibble basso a sinistra */
    tiny_mode(&v, 2, 32, 1);
    set_test_palette(&v);
    ram[0] = 0x3A;
    vidc_render(&v, ram, RAM_SIZE, 0, 0, 0x100, 0, 0, out, 32, &w, &h);
    CHECK_EQ(out[0], c12(0xA));
    CHECK_EQ(out[1], c12(0x3));
}

static void test_render_wrap(void)
{
    Vidc v;
    int w, h;

    /* 1 bpp, 32x4 = 16 byte: 8 dall'ultima quadword prima di vend+16,
       poi il DMA riparte da vstart */
    tiny_mode(&v, 0, 32, 4);
    vidc_write(&v, 0x00000000u);
    vidc_write(&v, 0x04000FFFu);
    memset(ram, 0, sizeof ram);
    for (int i = 0; i < 8; i++) ram[0x2008 + i] = 0xFF;   /* righe 0-1 */
    ram[0x2010] = 0xFF;                                    /* oltre vend+16: non letto */
    ram[0x3000] = 0x0F;                                    /* riga 2, primo byte */
    ram[0x3007] = 0xF0;                                    /* riga 3, ultimo byte */
    vidc_render(&v, ram, RAM_SIZE, 0x2008, 0x3000, 0x2000, 0, 0, out, 32, &w, &h);
    CHECK_EQ(h, 4);
    CHECK_EQ(out[0], 0xFFFFFF);
    CHECK_EQ(out[63], 0xFFFFFF);
    CHECK_EQ(out[64 + 0], 0xFFFFFF);
    CHECK_EQ(out[64 + 3], 0xFFFFFF);
    CHECK_EQ(out[64 + 4], 0x000000);
    CHECK_EQ(out[96 + 27], 0x000000);
    CHECK_EQ(out[96 + 28], 0xFFFFFF);
    CHECK_EQ(out[96 + 31], 0xFFFFFF);
}

static void test_render_256(void)
{
    Vidc v;
    int w, h;

    tiny_mode(&v, 3, 16, 1);
    vidc_write(&v, 0x00000000u);                 /* colore 0 nero */
    vidc_write(&v, 0x14000FFFu);                 /* colore 5 bianco */
    memset(ram, 0, sizeof ram);
    const uint8_t px[8] = { 0x05, 0xF5, 0x10, 0x20, 0x40, 0x80, 0x00, 0x75 };
    memcpy(ram, px, sizeof px);
    vidc_render(&v, ram, RAM_SIZE, 0, 0, 0x100, 0, 0, out, 16, &w, &h);
    CHECK_EQ(w, 16);
    /* i bit R3, G3, G2, B3 della palette sono sostituiti dal pixel */
    CHECK_EQ(out[0], 0x773377);
    CHECK_EQ(out[1], 0xFFFFFF);
    CHECK_EQ(out[2], 0x880000);                  /* bit 4 -> R3 */
    CHECK_EQ(out[3], 0x004400);                  /* bit 5 -> G2 */
    CHECK_EQ(out[4], 0x008800);                  /* bit 6 -> G3 */
    CHECK_EQ(out[5], 0x000088);                  /* bit 7 -> B3 */
    CHECK_EQ(out[6], 0x000000);
    CHECK_EQ(out[7], 0xFFFF77);
}

static void test_cursor(void)
{
    Vidc v;
    int w, h;

    tiny_mode(&v, 2, 64, 6);
    vidc_write(&v, 0x00000000u);                 /* sfondo nero */
    vidc_write(&v, 0x4400000Fu);                 /* cursore 1 rosso */
    vidc_write(&v, 0x480000F0u);                 /* cursore 2 verde */
    vidc_write(&v, 0x4C000F00u);                 /* cursore 3 blu */
    memset(ram, 0, sizeof ram);

    /* cursore a x=40, y=1, alto 2 righe */
    int hstart = 2 * (int)v.hdsr + 7;
    vidc_write(&v, 0x98000000u | (uint32_t)(hstart + 40 - 6) << 13);
    vidc_write(&v, reg(0xB8, (int)v.vdsr + 1));
    vidc_write(&v, reg(0xBC, (int)v.vdsr + 3));
    uint8_t *cur = ram + 0x8000;
    cur[0] = 0xE4;                               /* pixel 0..3 = 0,1,2,3 */
    cur[8 + 7] = 0xC0;                           /* riga 1, pixel 31 = 3 (fuori schermo) */
    cur[8 + 2] = 0x03;                           /* riga 1, pixel 8 = 3 */

    vidc_render(&v, ram, RAM_SIZE, 0, 0, 0x1000, 0x8000, 0, out, 64, &w, &h);
    CHECK_EQ(out[64 + 41], 0x000000);            /* disabilitato: niente cursore */

    vidc_render(&v, ram, RAM_SIZE, 0, 0, 0x1000, 0x8000, 1, out, 64, &w, &h);
    CHECK_EQ(out[64 + 40], 0x000000);            /* 0 = trasparente */
    CHECK_EQ(out[64 + 41], 0xFF0000);
    CHECK_EQ(out[64 + 42], 0x00FF00);
    CHECK_EQ(out[64 + 43], 0x0000FF);
    CHECK_EQ(out[128 + 48], 0x0000FF);
    CHECK_EQ(out[128 + 63], 0x000000);           /* pixel 23 del cursore: vuoto */
    CHECK_EQ(out[0 + 41], 0x000000);             /* riga sopra */
    CHECK_EQ(out[192 + 41], 0x000000);           /* riga sotto (VCER esclusa) */

    /* cursore che sporge sopra il display: la riga 1 dei dati cade a y=0 */
    vidc_write(&v, reg(0xB8, (int)v.vdsr - 1));
    vidc_write(&v, reg(0xBC, (int)v.vdsr + 1));
    vidc_render(&v, ram, RAM_SIZE, 0, 0, 0x1000, 0x8000, 1, out, 64, &w, &h);
    CHECK_EQ(out[0 + 48], 0x0000FF);
    CHECK_EQ(out[64 + 48], 0x000000);
}

int main(void)
{
    test_registers();
    test_modes();
    test_render_depths();
    test_render_wrap();
    test_render_256();
    test_cursor();
    printf("%d controlli, %d falliti\n", checks, failures);
    return failures ? 1 : 0;
}
