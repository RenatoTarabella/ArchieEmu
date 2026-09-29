/*
 * test_memc.c - Test del MEMC: CAM, protezioni, ROM all'avvio, registri.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "memc.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FALLITO %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { uint32_t va_ = (uint32_t)(a), vb_ = (uint32_t)(b); checks++; \
    if (va_ != vb_) { failures++; fprintf(stderr, "FALLITO %s:%d: %s = %08X, atteso %08X\n", __FILE__, __LINE__, #a, va_, vb_); } } while (0)

static uint32_t io_last_addr, io_last_value, vidc_last;
static uint32_t io_r(void *c, uint32_t a, int b) { (void)c; (void)b; io_last_addr = a; return 0x5A; }
static void io_w(void *c, uint32_t a, uint32_t v, int b) { (void)c; (void)b; io_last_addr = a; io_last_value = v; }
static void vidc_w(void *c, uint32_t v) { (void)c; vidc_last = v; }

static uint8_t ram[4 << 20];
static uint8_t rom[512 << 10];
static Arm2 cpu;
static Memc m;

/* indirizzo di scrittura della CAM per 32 KB: pagina logica l, fisica p, protezione ppl */
static uint32_t cam32k(uint32_t l, uint32_t p, uint32_t ppl)
{
    uint32_t a = 0x3800000u | (l & 0xFF) << 15 | ((l >> 8) & 3) << 10 | ppl << 8;
    a |= (p & 0xF) << 3 | ((p >> 4) & 1) | ((p >> 6) & 1) << 1 | ((p >> 5) & 1) << 2;
    return a;
}

static void set_mode(int mode) { cpu.r[15] = (cpu.r[15] & ~3u) | (uint32_t)mode; }

int main(void)
{
    for (uint32_t i = 0; i < sizeof rom; i += 4) { rom[i] = (uint8_t)(i >> 2); rom[i + 3] = 0xEA; }
    MemcIo io = { NULL, io_r, io_w, vidc_w, NULL };
    memc_init(&m, ram, sizeof ram, rom, sizeof rom, &cpu, &io);
    int ab = 0;
    set_mode(ARM_MODE_SVC);

    /* dopo il reset la ROM e' anche a 0; le scritture sono ignorate */
    CHECK_EQ(memc_read32(&m, 0x100, &ab), 0xEA000040u);
    CHECK_EQ(memc_read32(&m, 0, &ab), 0xEA000000u);
    CHECK(m.rom_latched);
    /* un accesso alla zona alta toglie la ROM da 0: ora 0 non e' mappato */
    CHECK_EQ(memc_read32(&m, 0x3800100, &ab), 0xEA000040u);
    CHECK(!m.rom_latched);
    ab = 0;
    memc_read32(&m, 0, &ab);
    CHECK(ab == 1);

    /* registro di controllo: pagine da 32 KB (bit 2-3 = 3), video DMA */
    ab = 0;
    memc_write32(&m, 0x36E0000u | 3u << 2 | 1u << 10, 0, &ab);
    CHECK_EQ(m.page_shift, 15);
    CHECK_EQ(m.video_dma, 1);

    /* CAM: pagina logica 1 (&8000) -> fisica 5, logica 0x3FF (ultima) -> fisica 100 */
    memc_write32(&m, cam32k(1, 5, 0), 0, &ab);
    memc_write32(&m, cam32k(0x3FF, 100, 0), 0, &ab);
    CHECK_EQ(memc_translate(&m, 0x8000), 5u * 0x8000);
    CHECK_EQ(memc_translate(&m, 0x8123), 5u * 0x8000 + 0x123);
    CHECK_EQ(memc_translate(&m, 0x1FF8000), 100u * 0x8000);
    memc_write32(&m, 0x8010, 0xCAFEBABE, &ab);
    CHECK_EQ(memc_read32(&m, 0x2000000u + 5 * 0x8000 + 0x10, &ab), 0xCAFEBABE);
    CHECK_EQ(memc_read8(&m, 0x8011, &ab), 0xBA);

    /* la stessa pagina fisica rimappata altrove sparisce dal vecchio posto */
    memc_write32(&m, cam32k(2, 5, 0), 0, &ab);
    CHECK_EQ(memc_translate(&m, 0x10000), 5u * 0x8000);
    CHECK_EQ(memc_translate(&m, 0x8000), 0xFFFFFFFFu);

    /* protezioni in modo utente */
    memc_write32(&m, cam32k(3, 6, 1), 0, &ab);          /* sola lettura */
    memc_write32(&m, cam32k(4, 7, 2), 0, &ab);          /* nessun accesso */
    set_mode(ARM_MODE_USR);
    ab = 0; memc_read32(&m, 0x18000, &ab); CHECK(ab == 0);
    ab = 0; memc_write32(&m, 0x18000, 1, &ab); CHECK(ab == 1);
    ab = 0; memc_read32(&m, 0x20000, &ab); CHECK(ab == 1);
    ab = 0; memc_read32(&m, 0x2000000, &ab); CHECK(ab == 1);   /* RAM fisica: solo supervisore */
    ab = 0; memc_read32(&m, 0x3200000, &ab); CHECK(ab == 1);   /* I/O: solo supervisore */
    ab = 0; memc_read32(&m, 0x3800100, &ab); CHECK(ab == 0);   /* ROM: leggibile anche in utente */
    ab = 0; memc_write32(&m, 0x3400000, 0, &ab); CHECK(ab == 1); /* VIDC: scrittura solo supervisore */
    ab = 0; memc_write32(&m, 0x10000, 7, &ab); CHECK(ab == 0);  /* PPL 0: tutto permesso */
    set_mode(ARM_MODE_SVC);
    ab = 0; memc_write32(&m, 0x20000, 9, &ab); CHECK(ab == 0);  /* il supervisore puo' tutto */
    cpu.trans_user = 1;                                          /* STRT/LDRT dal supervisore */
    ab = 0; memc_write32(&m, 0x18000, 1, &ab); CHECK(ab == 1);
    ab = 0; memc_read32(&m, 0x18000, &ab); CHECK(ab == 0);
    cpu.trans_user = 0;

    /* RAM fisica ripetuta oltre la dimensione (RISC OS la misura cosi') */
    memc_write32(&m, 0x2000040, 0x12345678, &ab);
    CHECK_EQ(memc_read32(&m, 0x2400040, &ab), 0x12345678);

    /* registri DMA: valore = bit 2-16 dell'indirizzo * 16 */
    memc_write32(&m, 0x3600000u | 0u << 17 | (0x1000 / 16) << 2, 0, &ab);
    memc_write32(&m, 0x3600000u | 1u << 17 | (0x2000 / 16) << 2, 0, &ab);
    memc_write32(&m, 0x3600000u | 2u << 17 | (0x7FF0 / 16) << 2, 0, &ab);
    CHECK_EQ(m.vinit, 0x1000);
    CHECK_EQ(m.vstart, 0x2000);
    CHECK_EQ(m.vend, 0x7FF0);

    /* VIDC: STRB replica il byte; I/O: indirizzo e dato passano */
    memc_write8(&m, 0x3400000, 0x42, &ab);
    CHECK_EQ(vidc_last, 0x42424242u);
    memc_write32(&m, 0x3200040, 0x00AB0000, &ab);
    CHECK_EQ(io_last_addr, 0x3200040);
    CHECK_EQ(memc_read8(&m, 0x3200004, &ab), 0x5A);

    /* pagine da 4 KB: formato diverso della CAM */
    memc_write32(&m, 0x36E0000u, 0, &ab);                  /* page size 0 */
    CHECK_EQ(m.page_shift, 12);
    uint32_t a4 = 0x3800000u | (0x123u & 0x7FF) << 12 | 42u;
    memc_write32(&m, a4, 0, &ab);
    CHECK_EQ(memc_translate(&m, 0x123000), 42u * 4096);

    printf("%d controlli, %d falliti\n", checks, failures);
    return failures ? 1 : 0;
}
