/*
 * test_iomd.c - Test dell'IOMD del Risc PC: identita', interrupt, timer e
 * DMA del suono (il comportamento che il POST e il kernel di RISC OS 3.5
 * si aspettano).
 */
#include <stdio.h>
#include "iomd.h"

static int failures = 0, checks = 0;

#define CHECK_EQ(a, b) do { uint32_t va_ = (uint32_t)(a), vb_ = (uint32_t)(b); checks++; \
    if (va_ != vb_) { failures++; fprintf(stderr, "FALLITO %s:%d: %s = %08X, atteso %08X\n", \
        __FILE__, __LINE__, #a, va_, vb_); } } while (0)

static Iomd m;
static int known;

static uint32_t rd(uint32_t off, ArcTime t) { return iomd_read(&m, off, t, &known); }
static void wr(uint32_t off, uint32_t v, ArcTime t) { iomd_write(&m, off, v, t, &known); }

static void test_id_and_irq(void)
{
    IomdHooks h = { 0 };
    iomd_init(&m, &h);
    CHECK_EQ(rd(0x94, 0) | rd(0x98, 0) << 8, IOMD_ID);
    CHECK_EQ(rd(0x10, 0), 0x90);                    /* forzato + accensione */
    wr(0x14, 0x10, 0);                              /* azzera l'accensione */
    CHECK_EQ(rd(0x10, 0), 0x80);
    CHECK_EQ(iomd_irq(&m), 0);
    iomd_set_flyback(&m, 1);
    wr(0x18, 0x08, 0);
    CHECK_EQ(iomd_irq(&m), 1);
    CHECK_EQ(rd(0x00, 0) & 0x80, 0x80);             /* IOCR: flyback */
}

static void test_timer(void)
{
    IomdHooks h = { 0 };
    iomd_init(&m, &h);
    wr(0x40, 20000 & 0xFF, 0);                      /* 100 Hz a 2 MHz */
    wr(0x44, 20000 >> 8, 0);
    wr(0x48, 0, 0);
    wr(0x4C, 0, ARC_MS(4));
    CHECK_EQ(rd(0x40, ARC_MS(4)) | rd(0x44, ARC_MS(4)) << 8, 12000);
    CHECK_EQ(rd(0x10, ARC_MS(9)) & 0x20, 0);
    CHECK_EQ(rd(0x10, ARC_MS(11)) & 0x20, 0x20);
}

static void test_sound_dma(void)
{
    IomdHooks h = { 0 };
    iomd_init(&m, &h);
    uint32_t addr;
    wr(0x190, 0xB0, 0);                             /* azzera e abilita, 16 byte */
    CHECK_EQ(rd(0x194, 0), 6);                      /* due buffer vuoti */
    wr(0x180, 0x02000000, 0);
    wr(0x184, 0x1F0, 0);                            /* A: 512 byte */
    CHECK_EQ(rd(0x194, 0), 2);                      /* A pronto, B da riempire */
    wr(0x188, 0x02000200, 0);
    wr(0x18C, 0x800003F0u, 0);                      /* B: 512 byte, poi stop */
    CHECK_EQ(rd(0x194, 0), 0);
    int blocks = 0;
    uint32_t first = 0, last = 0;
    while (iomd_sound_next(&m, &addr)) {
        if (!blocks) first = addr;
        last = addr;
        blocks++;
        if (blocks == 32) CHECK_EQ(rd(0x194, 0), 3);   /* finito A: si lavora su B, riempire A */
    }
    CHECK_EQ(blocks, 64);
    CHECK_EQ(first, 0x02000000);
    CHECK_EQ(last, 0x020003F0);
    CHECK_EQ(rd(0x194, 0) & 6, 6);                  /* tutti e due vuoti */
    wr(0x1F8, 0x10, 0);
    CHECK_EQ(iomd_irq(&m), 0);                      /* fermo (stop): niente richiesta */
}

int main(void)
{
    test_id_and_irq();
    test_timer();
    test_sound_dma();
    printf("%d controlli, %d falliti\n", checks, failures);
    return failures ? 1 : 0;
}
