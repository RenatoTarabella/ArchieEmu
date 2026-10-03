/*
 * test_iomd.c - Test dell'IOMD del Risc PC: identita', interrupt, timer,
 * DMA del suono e tastiera PS/2 (il comportamento che il POST e il kernel
 * di RISC OS 3.5 si aspettano).
 */
#include <stdio.h>
#include "iomd.h"
#include "ps2kbd.h"

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

/* tastiera collegata all'IOMD: comandi, risposte e parita' */
static Ps2Kbd kbd;
static void to_kbd(void *ctx, uint8_t b) { (void)ctx; ps2kbd_rx(&kbd, b); }
static int from_kbd(void *ctx, uint8_t *b) { (void)ctx; return ps2kbd_tx(&kbd, b); }

static uint8_t kbd_read(ArcTime *t)
{
    for (int k = 0; k < 100 && !(rd(0x08, *t) & 0x20); k++) *t += ARC_US(500);
    return (uint8_t)rd(0x04, *t);
}

static void test_keyboard(void)
{
    IomdHooks h = { NULL, NULL, NULL, to_kbd, from_kbd };
    iomd_init(&m, &h);
    ps2kbd_reset(&kbd);
    ArcTime t = 0;
    CHECK_EQ(rd(0x20, t) & 0xC0, 0);                /* spenta: niente interrupt */
    wr(0x08, 0x08, t);
    CHECK_EQ(rd(0x20, t) & 0xC0, 0x40);             /* trasmettitore vuoto */
    wr(0x04, 0xFF, t);                              /* reset */
    CHECK_EQ(rd(0x08, t) & 0xC0, 0x40);             /* occupato */
    CHECK_EQ(kbd_read(&t), 0xFA);
    uint8_t aa = kbd_read(&t);
    CHECK_EQ(aa, 0xAA);
    CHECK_EQ(rd(0x08, t) & 0x04, 0x04);             /* &AA: quattro 1, parita' dispari = 1 */
    wr(0x04, 0xED, t);                              /* LED */
    CHECK_EQ(kbd_read(&t), 0xFA);
    wr(0x04, 0x02, t);
    CHECK_EQ(kbd_read(&t), 0xFA);
    CHECK_EQ(kbd.leds, 2);
    ps2kbd_key(&kbd, ps2_code_from_vk('A', 0), 1);
    ps2kbd_key(&kbd, ps2_code_from_vk('A', 0), 0);
    ps2kbd_key(&kbd, ps2_code_from_vk(0x26, 1), 1);  /* freccia su */
    CHECK_EQ(rd(0x20, t) & 0x80, 0);
    CHECK_EQ(kbd_read(&t), 0x1C);
    CHECK_EQ(kbd_read(&t), 0xF0);
    CHECK_EQ(kbd_read(&t), 0x1C);
    CHECK_EQ(kbd_read(&t), 0xE0);
    CHECK_EQ(kbd_read(&t), 0x75);
    CHECK_EQ(rd(0x08, t) & 0x04, 0);                /* &75: cinque 1 */
}

int main(void)
{
    test_id_and_irq();
    test_timer();
    test_sound_dma();
    test_keyboard();
    printf("%d controlli, %d falliti\n", checks, failures);
    return failures ? 1 : 0;
}
