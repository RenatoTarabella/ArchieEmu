/*
 * test_fdc82077.c - Test del controller floppy 82077 del Risc PC: reset e
 * SENSE INTERRUPT, RECALIBRATE/SEEK, stato dell'unita', lettura con il DMA
 * (DACK) e il terminal count, fine del cilindro senza TC, scrittura,
 * formattazione, densita' sbagliata, protezione dalla scrittura.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fdc82077.h"

static int failures = 0, checks = 0;
#define CHECK_EQ(a, b) do { uint32_t va_ = (uint32_t)(a), vb_ = (uint32_t)(b); checks++; \
    if (va_ != vb_) { failures++; fprintf(stderr, "FALLITO %s:%d: %s = %08X, atteso %08X\n", \
        __FILE__, __LINE__, #a, va_, vb_); } } while (0)

static Fdc82077 f;
static ArcTime t;

static void step(ArcTime dt) { t += dt; fdc82077_update(&f, t); }

/* aspetta che il controller sia pronto per un byte (RQM), al massimo 2 s */
static int wait_rqm(void)
{
    for (int k = 0; k < 200000; k++) {
        if (fdc82077_read(&f, 4, t) & 0x80) return 1;
        step(ARC_US(10));
    }
    return 0;
}

static void cmd(const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) { wait_rqm(); fdc82077_write(&f, 5, b[i], t); }
}

static int results(uint8_t *r)
{
    int n = 0;
    while (wait_rqm() && (fdc82077_read(&f, 4, t) & 0x40) && n < 10) r[n++] = fdc82077_read(&f, 5, t);
    return n;
}

/* aspetta un byte del DMA (DRQ) */
static int wait_drq(void)
{
    for (int k = 0; k < 400000; k++) {
        if (fdc82077_drq(&f)) return 1;
        step(ARC_US(4));
    }
    return 0;
}

static void sense_int(uint8_t *st0, uint8_t *pcn)
{
    static const uint8_t c[1] = { 0x08 };
    uint8_t r[10];
    cmd(c, 1);
    int n = results(r);
    *st0 = r[0];
    *pcn = n > 1 ? r[1] : 0xFF;
}

static const char *make_image(const char *name, uint32_t size)
{
    static char path[2][512];
    static int k;
    char *p = path[k++ & 1];
    const char *tmp = getenv("TEMP");
    snprintf(p, 512, "%s/%s", tmp ? tmp : ".", name);
    FILE *fp = fopen(p, "wb");
    for (uint32_t i = 0; i < size; i++) fputc((int)((i / 1024) & 0xFF), fp);   /* ogni settore: il suo numero */
    fclose(fp);
    return p;
}

static void setup(const char *image, int rate)
{
    fdc82077_init(&f);
    t = 0;
    if (image) fdc_insert(&f.media, 0, image);
    fdc82077_write(&f, 2, 0x00, t);                   /* reset */
    fdc82077_write(&f, 2, 0x1C, t);                   /* unita' 0, motore, DMA */
    fdc82077_write(&f, 7, (uint8_t)rate, t);
    uint8_t st0, pcn;
    for (int d = 0; d < 4; d++) sense_int(&st0, &pcn);
    static const uint8_t spec[3] = { 0x03, 0xDF, 0x02 };         /* SPECIFY, con DMA */
    cmd(spec, 3);
}

static void test_reset_and_seek(void)
{
    fdc82077_init(&f);
    t = 0;
    fdc82077_write(&f, 2, 0x00, t);
    fdc82077_write(&f, 2, 0x0C, t);
    CHECK_EQ(fdc82077_irq(&f), 1);                    /* interrupt dopo il reset */
    uint8_t st0, pcn;
    for (int d = 0; d < 4; d++) { sense_int(&st0, &pcn); CHECK_EQ(st0, 0xC0 | d); }
    CHECK_EQ(fdc82077_irq(&f), 0);
    sense_int(&st0, &pcn);
    CHECK_EQ(st0, 0x80);                              /* niente da riportare */

    static const uint8_t seek[3] = { 0x0F, 0x00, 40 };
    cmd(seek, 3);
    CHECK_EQ(fdc82077_read(&f, 4, t) & 0x01, 0x01);   /* unita' 0 in movimento */
    step(ARC_MS(800));
    CHECK_EQ(fdc82077_irq(&f), 1);
    sense_int(&st0, &pcn);
    CHECK_EQ(st0, 0x20);
    CHECK_EQ(pcn, 40);

    static const uint8_t recal3[2] = { 0x07, 0x03 };   /* unita' 3 assente */
    cmd(recal3, 2);
    step(ARC_MS(100));
    sense_int(&st0, &pcn);
    CHECK_EQ(st0, 0x73);                              /* equipment check */
}

static void test_read_dma_and_tc(void)
{
    setup(make_image("t82077_e.adf", 819200), 2);     /* ADFS E, 250 kbit/s */
    CHECK_EQ(fdc82077_read(&f, 7, t) & 0x80, 0x80);   /* disco appena inserito */
    static const uint8_t seek[3] = { 0x0F, 0x00, 2 };
    cmd(seek, 3);
    step(ARC_MS(200));
    uint8_t st0, pcn;
    sense_int(&st0, &pcn);
    CHECK_EQ(pcn, 2);
    CHECK_EQ(fdc82077_read(&f, 7, t) & 0x80, 0);      /* il passo ha azzerato "cambiato" */

    static const uint8_t sds[2] = { 0x04, 0x00 };
    uint8_t r[10];
    cmd(sds, 2);
    results(r);
    CHECK_EQ(r[0], 0x28);                             /* pronta, due facce, non traccia 0 */

    /* cilindro 2, faccia 1, settori 3-4: il secondo con il terminal count */
    static const uint8_t rd[9] = { 0x46, 0x04, 2, 1, 3, 3, 4, 0x1B, 0xFF };
    cmd(rd, 9);
    int got = 0, ok = 1;
    for (int i = 0; i < 2048; i++) {
        if (!wait_drq()) { ok = 0; break; }
        uint8_t v = fdc82077_dack_read(&f, i == 2047, t);
        uint8_t expect = (uint8_t)(((2 * 2 + 1) * 5 + 3 + i / 1024) & 0xFF);
        if (v != expect) ok = 0;
        got++;
    }
    CHECK_EQ(ok, 1);
    CHECK_EQ(got, 2048);
    step(ARC_MS(1));
    CHECK_EQ(fdc82077_irq(&f), 1);
    int n = results(r);
    CHECK_EQ(n, 7);
    CHECK_EQ(r[0], 0x04);                             /* normale, testa 1 */
    CHECK_EQ(r[5], 1);                                /* TC su EOT: R = 1... */
    CHECK_EQ(r[3], 3);                                /* ...e cilindro + 1 */

    /* senza TC alla fine del cilindro: terminazione "anormale" con EN */
    static const uint8_t rd2[9] = { 0x46, 0x00, 2, 0, 4, 3, 4, 0x1B, 0xFF };
    cmd(rd2, 9);
    for (int i = 0; i < 1024; i++) { wait_drq(); fdc82077_dack_read(&f, 0, t); }
    step(ARC_MS(1));
    results(r);
    CHECK_EQ(r[0], 0x40);
    CHECK_EQ(r[1], 0x80);
}

static void test_write_and_protect(void)
{
    const char *img = make_image("t82077_w.adf", 819200);
    setup(img, 2);
    static const uint8_t wr[9] = { 0x45, 0x00, 0, 0, 1, 3, 1, 0x1B, 0xFF };
    cmd(wr, 9);
    for (int i = 0; i < 1024; i++) { wait_drq(); fdc82077_dack_write(&f, (uint8_t)(0x40 + (i & 15)), i == 1023, t); }
    step(ARC_MS(1));
    uint8_t r[10];
    results(r);
    CHECK_EQ(r[0], 0x00);
    fdc_eject(&f.media, 0);                           /* salva nel file */
    FILE *fp = fopen(img, "rb");
    uint8_t b[16];
    fseek(fp, 1024, SEEK_SET);
    if (fread(b, 1, 16, fp) != 16) b[0] = 0;
    fclose(fp);
    CHECK_EQ(b[0], 0x40);
    CHECK_EQ(b[15], 0x4F);

    f.media.drive[0].image = NULL;
    fdc_insert(&f.media, 0, img);
    f.media.drive[0].write_protect = 1;
    cmd(wr, 9);
    results(r);
    CHECK_EQ(r[0], 0x40);
    CHECK_EQ(r[1], 0x02);                             /* not writable */
}

static void test_format_hd_and_density(void)
{
    setup(make_image("t82077_f.adf", 1638400), 0);    /* ADFS F, 500 kbit/s */
    static const uint8_t fmt[6] = { 0x4D, 0x00, 3, 10, 0x5A, 0xE5 };
    cmd(fmt, 6);
    for (int s = 0; s < 10; s++) {
        uint8_t id[4] = { 0, 0, (uint8_t)s, 3 };
        for (int k = 0; k < 4; k++) { wait_drq(); fdc82077_dack_write(&f, id[k], 0, t); }
    }
    step(ARC_MS(1));
    uint8_t r[10];
    results(r);
    CHECK_EQ(r[0], 0x00);
    CHECK_EQ(f.media.drive[0].image[5 * 1024], 0xE5);  /* settori riempiti */
    CHECK_EQ(f.media.drive[0].dirty, 1);

    static const uint8_t rid[2] = { 0x4A, 0x00 };
    cmd(rid, 2);
    results(r);
    CHECK_EQ(r[0], 0x00);
    CHECK_EQ(r[6], 3);

    fdc82077_write(&f, 7, 2, t);                      /* 250 kbit/s su un disco HD */
    cmd(rid, 2);
    step(ARC_MS(500));
    results(r);
    CHECK_EQ(r[0], 0x40);
    CHECK_EQ(r[1], 0x01);                             /* missing address mark */
}

int main(void)
{
    test_reset_and_seek();
    test_read_dma_and_tc();
    test_write_and_protect();
    test_format_hd_and_density();
    printf("%d controlli, %d falliti\n", checks, failures);
    return failures ? 1 : 0;
}
