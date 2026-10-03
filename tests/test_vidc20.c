/*
 * test_vidc20.c - Test del VIDC20 del Risc PC: tempi dai registri (i valori
 * che RISC OS 3.5 scrive per 640x480 a 60 Hz e per il modo 12), pixel a 4,
 * 16 e 32 bpp con la palette usata come tabella, cursore hardware.
 */
#include <stdio.h>
#include <string.h>
#include "vidc20.h"

static int failures = 0, checks = 0;
#define CHECK_EQ(a, b) do { uint32_t va_ = (uint32_t)(a), vb_ = (uint32_t)(b); checks++; \
    if (va_ != vb_) { failures++; fprintf(stderr, "FALLITO %s:%d: %s = %08X, atteso %08X\n", \
        __FILE__, __LINE__, #a, va_, vb_); } } while (0)

static Vidc20 v;
static uint8_t screen[640 * 4 * 2];

static const uint8_t *mem(void *ctx, uint32_t addr, uint32_t len)
{
    (void)ctx;
    return addr + len <= sizeof screen ? screen + addr : NULL;
}

/* i registri di un modo, come li scrive RISC OS */
static void program(uint32_t control, uint32_t fsyn, const uint32_t *h, const uint32_t *vt)
{
    vidc20_reset(&v);
    for (uint32_t k = 0; k < 8; k++) {
        vidc20_write(&v, 0x80000000u | k << 24 | h[k]);
        vidc20_write(&v, 0x90000000u | k << 24 | vt[k]);
    }
    vidc20_write(&v, 0xD0000000u | fsyn);
    vidc20_write(&v, 0xE0000000u | control);
}

int main(void)
{
    /* 640x480 a 60 Hz: 25,2 MHz, 800 x 525 */
    static const uint32_t h[8] = { 792, 88, 130, 124, 764, 770, 424, 400 };
    static const uint32_t vt[8] = { 523, 0, 32, 32, 512, 512, 271, 293 };
    program(0x44C, 0x1404, h, vt);
    Vidc20Timing t;
    CHECK_EQ(vidc20_timing(&v, &t), 1);
    CHECK_EQ((uint32_t)(t.pixel_hz / 1000), 25200);
    CHECK_EQ(t.line_time, 762);                     /* 31,75 us */
    CHECK_EQ(t.frame_time / 24, 16668);              /* 16,7 ms: 60 Hz */
    CHECK_EQ(t.flyback_at, 762 * 512);
    int w, hh;
    vidc20_size(&v, &w, &hh);
    CHECK_EQ(w, 640);
    CHECK_EQ(hh, 480);
    CHECK_EQ(vidc20_log2bpp(&v), 2);

    /* modo 12: 16 MHz, 1024 x 312, 50 Hz */
    static const uint32_t h12[8] = { 1016, 64, 122, 204, 844, 938, 504, 512 };
    static const uint32_t v12[8] = { 310, 1, 17, 34, 290, 307, 169, 180 };
    program(0x454, 0x0701, h12, v12);
    vidc20_timing(&v, &t);
    CHECK_EQ((uint32_t)(t.pixel_hz / 1000), 16000);
    CHECK_EQ(t.frame_time / 24, 19968);             /* 64 us x 312 */

    /* 4 bpp: palette, nibble basso = pixel a sinistra */
    program(0x44C, 0x1404, h, vt);
    vidc20_write(&v, 0x10000000u);                  /* indirizzo della palette 0 */
    for (uint32_t k = 0; k < 16; k++) vidc20_write(&v, k * 0x111111u);
    memset(screen, 0, sizeof screen);
    screen[0] = 0x2F;
    static uint32_t out[480 * 640];
    vidc20_render(&v, mem, NULL, 0, out, 640, &w, &hh);
    CHECK_EQ(out[0], 0xFFFFFF);
    CHECK_EQ(out[1], 0x222222);

    /* 16 bpp: la palette come la riempie RISC OS (5 bit espansi per canale) */
    program(0x48C, 0x1404, h, vt);
    vidc20_write(&v, 0x10000000u);
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t r = i & 31, g = (i >> 1) & 31, b = (i >> 2) & 31;
        vidc20_write(&v, (r << 3 | r >> 2) | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2) << 16);
    }
    uint16_t px = 31 | 0 << 5 | 16 << 10;           /* rosso pieno, blu a meta' */
    screen[0] = (uint8_t)px;
    screen[1] = (uint8_t)(px >> 8);
    vidc20_render(&v, mem, NULL, 0, out, 640, &w, &hh);
    CHECK_EQ(out[0], 0xFF0084);

    /* 32 bpp: un byte per canale attraverso la palette (qui una rampa) */
    program(0x7CC, 0x1404, h, vt);
    vidc20_write(&v, 0x10000000u);
    for (uint32_t i = 0; i < 256; i++) vidc20_write(&v, i * 0x010101u);
    screen[0] = 0x12; screen[1] = 0x34; screen[2] = 0x56; screen[3] = 0;
    vidc20_render(&v, mem, NULL, 0, out, 640, &w, &hh);
    CHECK_EQ(out[0], 0x123456);

    /* cursore: HCSR = x + HDSR - 20 (4 bpp); il colore 1 e' cursor[0] */
    program(0x44C, 0x1404, h, vt);
    vidc20_write(&v, 0x50000000u | 0x0000FF);       /* colore 1: rosso */
    uint8_t cur[8 * 2] = { 0x01 };                  /* primo pixel colore 1 */
    v.horiz[6] = 100 + 124 - 20;
    v.vert[6] = 32 + 10;
    v.vert[7] = 32 + 12;
    memset(out, 0, sizeof out);
    vidc20_draw_cursor(&v, cur, out, 640, 640, 480);
    CHECK_EQ(out[10 * 640 + 100], 0xFF0000);
    CHECK_EQ(out[10 * 640 + 101], 0);

    printf("%d controlli, %d falliti\n", checks, failures);
    return failures ? 1 : 0;
}
