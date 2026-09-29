/*
 * test_fdc.c - Test del WD1772 e delle immagini ADFS (src/archie/fdc.c).
 *
 * Le immagini di prova (D da 800 KB, L da 640 KB) vengono create accanto
 * all'eseguibile con un contenuto noto. Il tempo e' simulato: il test
 * avanza 'now' da un evento del controller al successivo e, a ogni DRQ,
 * legge o scrive il registro dati dopo qualche microsecondo, come farebbe
 * il gestore FIQ di RISC OS.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fdc.h"

/* ------------------------------------------------------------------ */
/* mini framework                                                     */
/* ------------------------------------------------------------------ */

static int failures = 0, checks = 0;
static const char *current = "";

#define CHECK(cond_) do { checks++; if (!(cond_)) { failures++; \
    fprintf(stderr, "FALLITO [%s] %s:%d: %s\n", current, __FILE__, __LINE__, #cond_); } } while (0)
#define CHECK_EQ(a, b) do { unsigned long long va_ = (unsigned long long)(a), vb_ = (unsigned long long)(b); \
    checks++; if (va_ != vb_) { failures++; fprintf(stderr, "FALLITO [%s] %s:%d: %s = %llX, atteso %llX\n", \
        current, __FILE__, __LINE__, #a, va_, vb_); } } while (0)
#define TEST(name) static void name(void)
#define RUN_TEST(name) do { current = #name; name(); } while (0)

/* ------------------------------------------------------------------ */
/* ambiente                                                           */
/* ------------------------------------------------------------------ */

#define REV ARC_MS(200)
#define D_SIZE 819200
#define L_SIZE 655360

static Fdc fdc;
static ArcTime now;
static char path_d[600], path_l[600], path_bad[600];
static uint8_t buf[8192];
static ArcTime drq_at[8];

static uint8_t pat_d(int cyl, int head, int sec, int i)
{
    return (uint8_t)(i * 13 + cyl * 7 + head * 0x55 + sec * 0x31);
}

static uint8_t pat_l(int cyl, int head, int sec, int i)
{
    return (uint8_t)(i ^ (cyl * 3 + head * 0x80 + sec * 0x11));
}

static long d_offset(int cyl, int head, int sec) { return ((cyl * 2L + head) * 5 + sec) * 1024; }

static int write_file(const char *p, const uint8_t *data, size_t n)
{
    FILE *f = fopen(p, "wb");
    if (!f) return 0;
    size_t w = fwrite(data, 1, n, f);
    fclose(f);
    return w == n;
}

static int read_file_at(const char *p, long off, uint8_t *out, size_t n)
{
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fseek(f, off, SEEK_SET);
    size_t r = fread(out, 1, n, f);
    fclose(f);
    return r == n;
}

static void make_images(void)
{
    uint8_t *img = malloc(D_SIZE);
    for (int c = 0; c < 80; c++)
        for (int h = 0; h < 2; h++)
            for (int s = 0; s < 5; s++)
                for (int i = 0; i < 1024; i++)
                    img[d_offset(c, h, s) + i] = pat_d(c, h, s, i);
    CHECK(write_file(path_d, img, D_SIZE));
    for (int c = 0; c < 80; c++)
        for (int h = 0; h < 2; h++)
            for (int s = 0; s < 16; s++)
                for (int i = 0; i < 256; i++)
                    img[((c * 2 + h) * 16 + s) * 256 + i] = pat_l(c, h, s, i);
    CHECK(write_file(path_l, img, L_SIZE));
    CHECK(write_file(path_bad, img, 1000));
    free(img);
}

static uint16_t crc16(uint16_t crc, const uint8_t *p, int n)
{
    while (n-- > 0) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++)
            crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

static void wr(int reg, uint8_t v) { fdc_write(&fdc, reg, v, now); }
static uint8_t rd(int reg) { return fdc_read(&fdc, reg, now); }

/* latch A: unita' selezionata (attiva bassa), faccia (0 = faccia 1), motore (attivo basso) */
static void sel_unit(int drive, int side)
{
    uint8_t v = (uint8_t)(0x0F & ~(1 << drive));
    if (side == 0) v |= 0x10;
    v |= 0x20;                      /* motore del latch spento: comanda il WD1772 */
    fdc_latch_a(&fdc, v);
}

/* avanza senza servire i DRQ fino a INTRQ; 0 se non arriva entro 'max' */
static int wait_intrq(ArcTime max)
{
    ArcTime limit = now + max;
    while (!fdc_intrq(&fdc)) {
        ArcTime e = fdc_next_event(&fdc);
        if (e > limit) { now = limit; fdc_update(&fdc, now); return 0; }
        now = e;
        fdc_update(&fdc, now);
    }
    return 1;
}

static void advance(ArcTime dt)
{
    ArcTime limit = now + dt;
    for (;;) {
        ArcTime e = fdc_next_event(&fdc);
        if (e > limit) break;
        now = e;
        fdc_update(&fdc, now);
    }
    now = limit;
    fdc_update(&fdc, now);
}

/*
 * Gestore "FIQ": a ogni DRQ, dopo 'lat', legge il registro dati (src NULL)
 * oppure vi scrive il byte successivo di src. Si ferma a INTRQ o dopo
 * 'stop' byte; ritorna i byte trasferiti (-1 se INTRQ non arriva).
 */
static int transfer(uint8_t *dst, const uint8_t *src, int stop, ArcTime lat)
{
    ArcTime limit = now + ARC_MS(3000);
    int n = 0;
    while (!fdc_intrq(&fdc) && n < stop) {
        if (!fdc_drq(&fdc)) {
            ArcTime e = fdc_next_event(&fdc);
            if (e > limit) return -1;
            now = e;
            fdc_update(&fdc, now);
        }
        if (fdc_drq(&fdc)) {
            if (n < 8) drq_at[n] = now;
            now += lat;
            if (src) wr(3, src[n]);
            else {
                uint8_t v = rd(3);
                if (dst) dst[n] = v;
            }
            n++;
        }
    }
    return n;
}

/* esegue un comando di tipo I e ritorna la durata fino a INTRQ */
static ArcTime type1(uint8_t cmd)
{
    ArcTime t = now;
    wr(0, cmd);
    CHECK(wait_intrq(ARC_MS(3000)));
    return now - t;
}

static int check_pattern_d(const uint8_t *p, int cyl, int head, int sec)
{
    for (int i = 0; i < 1024; i++) if (p[i] != pat_d(cyl, head, sec, i)) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* test                                                               */
/* ------------------------------------------------------------------ */

TEST(insert_images)
{
    fdc_reset(&fdc);
    CHECK(!fdc_insert(&fdc, 0, path_bad));
    CHECK(!fdc_insert(&fdc, 0, "file_inesistente.adf"));
    CHECK(fdc_insert(&fdc, 0, path_d));
    CHECK_EQ(fdc.drive[0].sectors, 5);
    CHECK_EQ(fdc.drive[0].sector_size, 1024);
    CHECK_EQ(fdc.drive[0].sides, 2);
    CHECK(fdc_insert(&fdc, 1, path_l));
    CHECK_EQ(fdc.drive[1].sectors, 16);
    CHECK_EQ(fdc.drive[1].sector_size, 256);
    fdc_latch_b(&fdc, 0x08);        /* doppia densita', reset non attivo */
    sel_unit(0, 0);
    CHECK_EQ(fdc_disc_changed(&fdc), 0);
    CHECK_EQ(fdc_intrq(&fdc), 0);
    CHECK_EQ(fdc_drq(&fdc), 0);
}

TEST(restore_with_spinup)
{
    now = ARC_MS(50);
    wr(0, 0x00);                    /* restore, h = 0, motore spento: 6 giri */
    uint8_t s = rd(0);
    CHECK_EQ(s & 0x81, 0x81);       /* motore e busy */
    CHECK(wait_intrq(ARC_MS(2000)));
    CHECK_EQ(now, ARC_MS(1200));    /* sesto impulso di indice dopo 50 ms */
    s = rd(0);
    CHECK_EQ(s & 0xA5, 0xA4);       /* motore, spin-up, traccia 0, non busy */
    CHECK_EQ(fdc_intrq(&fdc), 0);   /* azzerato dalla lettura dello stato */
    CHECK_EQ(rd(1), 0);
}

TEST(seek_and_step_timing)
{
    now += ARC_MS(10);
    wr(3, 10);
    CHECK_EQ(type1(0x18), ARC_MS(60));      /* 10 passi da 6 ms, h = 1 */
    CHECK_EQ(rd(1), 10);
    CHECK_EQ(fdc.drive[0].track, 10);
    CHECK_EQ(fdc_disc_changed(&fdc), 1);    /* il passo azzera "disc changed" */
    CHECK_EQ(rd(0) & 0x04, 0);              /* non piu' in traccia 0 */

    CHECK_EQ(type1(0x51), ARC_MS(12));      /* step-in con aggiornamento, 12 ms */
    CHECK_EQ(rd(1), 11);
    CHECK_EQ(fdc.drive[0].track, 11);
    CHECK_EQ(type1(0x63), ARC_MS(3));       /* step-out senza aggiornamento, 3 ms */
    CHECK_EQ(rd(1), 11);
    CHECK_EQ(fdc.drive[0].track, 10);
    CHECK_EQ(type1(0x22), ARC_MS(2));       /* step: ripete la direzione, 2 ms */
    CHECK_EQ(fdc.drive[0].track, 9);

    /* restore con verifica: 9 passi, 15 ms di assestamento, poi un ID */
    ArcTime d = type1(0x04);
    CHECK(d >= ARC_MS(54 + 15) && d < ARC_MS(54 + 15 + 200));
    CHECK_EQ(rd(1), 0);
    CHECK_EQ(rd(0) & 0x18, 0);
}

TEST(seek_verify_error)
{
    wr(1, 7);                       /* registro traccia sbagliato: testina a 0 */
    wr(3, 7);
    ArcTime d = type1(0x14);        /* seek con verifica, nessun passo */
    CHECK_EQ(rd(0) & 0x10, 0x10);   /* seek error */
    CHECK(d > ARC_MS(15 + 800) && d <= ARC_MS(15 + 1000));
    CHECK_EQ(type1(0x00), 0);       /* restore gia' in traccia 0: immediato */
    CHECK_EQ(rd(0) & 0x10, 0);
    CHECK_EQ(rd(1), 0);
}

TEST(read_sector)
{
    wr(3, 5);
    type1(0x10);
    wr(2, 2);
    wr(0, 0x80);
    CHECK_EQ(rd(0) & 0x01, 1);
    memset(buf, 0, sizeof buf);
    int n = transfer(buf, NULL, 100000, ARC_US(5));
    CHECK_EQ(n, 1024);
    CHECK(check_pattern_d(buf, 5, 0, 2));
    CHECK_EQ(drq_at[1] - drq_at[0], ARC_US(32));
    CHECK_EQ(drq_at[7] - drq_at[6], ARC_US(32));
    CHECK_EQ(rd(0) & 0x7F, 0);      /* nessun errore, non busy */
    CHECK_EQ(rd(2), 2);

    /* faccia 1 (bit 4 del latch A a 0) */
    sel_unit(0, 1);
    wr(2, 4);
    wr(0, 0x80);
    n = transfer(buf, NULL, 100000, ARC_US(10));
    CHECK_EQ(n, 1024);
    CHECK(check_pattern_d(buf, 5, 1, 4));
    sel_unit(0, 0);
}

TEST(lost_data_and_rnf)
{
    wr(2, 1);
    wr(0, 0x80);
    CHECK(wait_intrq(ARC_MS(1000)));        /* nessuno legge il registro dati */
    uint8_t s = rd(0);
    CHECK_EQ(s & 0x05, 0x04);               /* lost data, non busy */
    CHECK_EQ(fdc_drq(&fdc), 0);

    wr(2, 9);                               /* settore inesistente */
    ArcTime t = now;
    wr(0, 0x80);
    CHECK(wait_intrq(ARC_MS(2000)));
    CHECK(now - t > 4 * REV && now - t <= 5 * REV);
    CHECK_EQ(rd(0) & 0x1F, 0x10);
}

TEST(read_address)
{
    wr(2, 0xEE);
    wr(0, 0xC0);
    int n = transfer(buf, NULL, 100, ARC_US(4));
    CHECK_EQ(n, 6);
    CHECK_EQ(buf[0], 5);
    CHECK_EQ(buf[1], 0);
    CHECK(buf[2] < 5);
    CHECK_EQ(buf[3], 3);
    uint8_t hdr[8] = { 0xA1, 0xA1, 0xA1, 0xFE, buf[0], buf[1], buf[2], buf[3] };
    uint16_t crc = crc16(0xFFFF, hdr, 8);
    CHECK_EQ(buf[4], crc >> 8);
    CHECK_EQ(buf[5], crc & 0xFF);
    CHECK_EQ(drq_at[5] - drq_at[4], ARC_US(32));
    CHECK_EQ(rd(2), 5);                     /* il registro settore riceve la traccia */
    CHECK_EQ(rd(0) & 0x1D, 0);
}

TEST(write_sector_and_save)
{
    uint8_t src[1024];
    for (int i = 0; i < 1024; i++) src[i] = (uint8_t)(i * 3 + 1);
    wr(2, 3);
    wr(0, 0xA0);
    int n = transfer(NULL, src, 100000, ARC_US(6));
    CHECK_EQ(n, 1024);
    CHECK_EQ(rd(0) & 0x7F, 0);
    CHECK(fdc.drive[0].dirty);

    wr(0, 0x80);
    n = transfer(buf, NULL, 100000, ARC_US(6));
    CHECK_EQ(n, 1024);
    CHECK(memcmp(buf, src, 1024) == 0);

    CHECK(fdc_flush(&fdc, 0));
    CHECK(!fdc.drive[0].dirty);
    memset(buf, 0, 1024);
    CHECK(read_file_at(path_d, d_offset(5, 0, 3), buf, 1024));
    CHECK(memcmp(buf, src, 1024) == 0);
    CHECK(read_file_at(path_d, d_offset(5, 0, 2), buf, 1024));
    CHECK(check_pattern_d(buf, 5, 0, 2));

    /* il primo byte non arriva in tempo: lost data e nessuna scrittura */
    wr(2, 4);
    wr(0, 0xA0);
    CHECK(wait_intrq(ARC_MS(1000)));
    CHECK_EQ(rd(0) & 0x05, 0x04);
    CHECK(!fdc.drive[0].dirty);
}

TEST(write_protect)
{
    fdc.drive[0].write_protect = 1;
    wr(3, 5);
    wr(2, 0);
    wr(0, 0xA0);
    int n = transfer(buf, (const uint8_t *)buf, 100000, ARC_US(5));
    CHECK_EQ(n, 0);
    CHECK_EQ(rd(0) & 0x41, 0x40);
    CHECK(!fdc.drive[0].dirty);
    CHECK_EQ(type1(0x18), 0);               /* seek sulla stessa traccia */
    CHECK_EQ(rd(0) & 0x40, 0x40);           /* anche nello stato di tipo I */
    fdc.drive[0].write_protect = 0;
}

TEST(force_interrupt)
{
    wr(2, 0);
    wr(0, 0x80);
    int n = transfer(buf, NULL, 100, ARC_US(5));
    CHECK_EQ(n, 100);
    wr(0, 0xD0);                            /* interrompe senza INTRQ */
    CHECK_EQ(rd(0) & 0x01, 0);
    CHECK_EQ(fdc_drq(&fdc), 0);
    CHECK_EQ(fdc_intrq(&fdc), 0);
    advance(ARC_MS(50));
    CHECK_EQ(fdc_drq(&fdc), 0);
    CHECK_EQ(fdc_intrq(&fdc), 0);

    wr(0, 0xD8);                            /* interrupt immediato */
    CHECK_EQ(fdc_intrq(&fdc), 1);
    rd(0);
    CHECK_EQ(fdc_intrq(&fdc), 1);           /* resta fino al comando successivo */
    wr(0, 0xD0);
    CHECK_EQ(fdc_intrq(&fdc), 0);
    CHECK_EQ(rd(1), 5);

    wr(0, 0xD4);                            /* interrupt a ogni impulso di indice */
    CHECK(wait_intrq(REV + 1));
    CHECK_EQ(now % REV, 0);
    CHECK_EQ(rd(0) & 0x02, 0x02);           /* bit di indice nello stato di tipo I */
    now += ARC_MS(10);
    CHECK_EQ(rd(0) & 0x02, 0);
    CHECK_EQ(fdc_intrq(&fdc), 0);
    CHECK(wait_intrq(REV + 1));
    CHECK_EQ(now % REV, 0);
    wr(0, 0xD0);
    advance(REV * 2);
    CHECK_EQ(fdc_intrq(&fdc), 0);
}

TEST(multi_sector)
{
    static uint8_t big[5 * 1024 + 16];
    wr(2, 0);
    wr(0, 0x90);                            /* lettura multipla fino a RNF */
    int n = transfer(big, NULL, 100000, ARC_US(5));
    CHECK_EQ(n, 5 * 1024);
    CHECK(check_pattern_d(big, 5, 0, 0));
    CHECK(check_pattern_d(big + 4096, 5, 0, 4));
    CHECK_EQ(big[3 * 1024 + 1], 4);         /* il settore 3 riscritto prima */
    CHECK_EQ(rd(0) & 0x1D, 0x10);
    CHECK_EQ(rd(2), 5);

    wr(3, 6);
    type1(0x10);
    for (int i = 0; i < 5 * 1024; i++) big[i] = (uint8_t)(i / 1024 + 0xA0);
    wr(2, 0);
    wr(0, 0xB0);                            /* scrittura multipla */
    n = transfer(NULL, big, 5 * 1024, ARC_US(5));
    CHECK_EQ(n, 5 * 1024);
    CHECK(wait_intrq(ARC_MS(1500)));
    CHECK_EQ(rd(0) & 0x1D, 0x10);
    wr(2, 2);
    wr(0, 0x80);
    n = transfer(buf, NULL, 100000, ARC_US(5));
    CHECK_EQ(n, 1024);
    CHECK_EQ(buf[0], 0xA2);
    CHECK_EQ(buf[1023], 0xA2);
}

/* costruisce il flusso di Write Track per una traccia */
static int format_stream(uint8_t *s, int cyl, int head, int nsec, int ncode, int gap3, int first_fill)
{
    int p = 0, size = 128 << ncode;
    for (int i = 0; i < 32; i++) s[p++] = 0x4E;
    for (int r = 0; r < nsec; r++) {
        for (int i = 0; i < 12; i++) s[p++] = 0x00;
        for (int i = 0; i < 3; i++) s[p++] = 0xF5;
        s[p++] = 0xFE;
        s[p++] = (uint8_t)cyl; s[p++] = (uint8_t)head; s[p++] = (uint8_t)r; s[p++] = (uint8_t)ncode;
        s[p++] = 0xF7;
        for (int i = 0; i < 22; i++) s[p++] = 0x4E;
        for (int i = 0; i < 12; i++) s[p++] = 0x00;
        for (int i = 0; i < 3; i++) s[p++] = 0xF5;
        s[p++] = 0xFB;
        for (int i = 0; i < size; i++) s[p++] = (uint8_t)(first_fill + r);
        s[p++] = 0xF7;
        for (int i = 0; i < gap3; i++) s[p++] = 0x4E;
    }
    while (p < 7000) s[p++] = 0x4E;
    return p;
}

TEST(write_track_format)
{
    static uint8_t stream[8000];
    wr(3, 7);
    type1(0x10);
    format_stream(stream, 7, 0, 5, 3, 90, 0x40);
    wr(0, 0xF0);
    int n = transfer(NULL, stream, 7000, ARC_US(5));
    CHECK(n > 6000 && n < 6260);
    CHECK_EQ(rd(0) & 0x5D, 0);
    wr(2, 3);
    wr(0, 0x80);
    n = transfer(buf, NULL, 100000, ARC_US(5));
    CHECK_EQ(n, 1024);
    CHECK_EQ(buf[0], 0x43);
    CHECK_EQ(buf[1023], 0x43);
    CHECK(fdc.drive[0].dirty);              /* geometria standard: finisce nel file */
    CHECK(fdc_flush(&fdc, 0));
    CHECK(read_file_at(path_d, d_offset(7, 0, 4), buf, 1024));
    CHECK_EQ(buf[512], 0x44);

    /* 16 settori da 256 sulla faccia 1 della traccia 8: traccia custom */
    sel_unit(0, 1);
    wr(3, 8);
    type1(0x10);
    format_stream(stream, 8, 1, 16, 1, 20, 0x10);
    wr(0, 0xF0);
    n = transfer(NULL, stream, 7000, ARC_US(5));
    CHECK(n > 6000);
    CHECK_EQ(rd(0) & 0x5D, 0);
    CHECK(!fdc.drive[0].dirty);
    wr(2, 15);
    wr(0, 0x80);
    n = transfer(buf, NULL, 100000, ARC_US(5));
    CHECK_EQ(n, 256);
    CHECK_EQ(buf[0], 0x1F);
    CHECK_EQ(rd(0) & 0x1D, 0);
    wr(0, 0xC0);
    n = transfer(buf, NULL, 100, ARC_US(5));
    CHECK_EQ(n, 6);
    CHECK_EQ(buf[0], 8);
    CHECK_EQ(buf[1], 1);
    CHECK_EQ(buf[3], 1);
    wr(2, 5);                               /* scrittura in una traccia custom */
    memset(stream, 0x77, 256);
    wr(0, 0xA0);
    CHECK_EQ(transfer(NULL, stream, 256, ARC_US(5)), 256);
    CHECK(wait_intrq(ARC_MS(500)));
    wr(0, 0x80);
    CHECK_EQ(transfer(buf, NULL, 100000, ARC_US(5)), 256);
    CHECK_EQ(buf[100], 0x77);
    CHECK(read_file_at(path_d, d_offset(8, 1, 0), buf, 1024));
    CHECK(check_pattern_d(buf, 8, 1, 0));   /* il file non cambia */
    sel_unit(0, 0);
}

TEST(read_track)
{
    static uint8_t trk[7000];
    wr(3, 5);
    type1(0x10);
    wr(0, 0xE0);
    int n = transfer(trk, NULL, 7000, ARC_US(5));
    CHECK_EQ(n, 6250);
    CHECK_EQ(trk[0], 0x4E);
    int ids = 0, dams = 0;
    for (int i = 0; i + 4 < n; i++) {
        if (trk[i] == 0xA1 && trk[i + 1] == 0xA1 && trk[i + 2] == 0xA1) {
            if (trk[i + 3] == 0xFE) { ids++; CHECK_EQ(trk[i + 4], 5); }
            if (trk[i + 3] == 0xFB) dams++;
        }
    }
    CHECK_EQ(ids, 5);
    CHECK_EQ(dams, 5);
    CHECK_EQ(rd(0) & 0x1D, 0);
}

TEST(density_and_reset)
{
    fdc_latch_b(&fdc, 0x0A);                /* singola densita': ID MFM illeggibili */
    wr(2, 0);
    wr(0, 0x80);
    CHECK(wait_intrq(ARC_MS(1500)));
    CHECK_EQ(rd(0) & 0x10, 0x10);
    fdc_latch_b(&fdc, 0x08);

    wr(0, 0x80);
    advance(ARC_MS(1));
    CHECK_EQ(rd(0) & 1, 1);
    fdc_latch_b(&fdc, 0x00);                /* reset attivo */
    CHECK_EQ(rd(0), 0);
    wr(0, 0x00);                            /* ignorato */
    fdc_latch_b(&fdc, 0x08);
    CHECK_EQ(rd(0) & 0x01, 0);
    CHECK_EQ(fdc_drq(&fdc), 0);
    CHECK_EQ(fdc_intrq(&fdc), 0);
    CHECK_EQ(rd(2), 1);
    advance(ARC_MS(1000));
    CHECK_EQ(fdc_drq(&fdc), 0);
    CHECK_EQ(fdc_intrq(&fdc), 0);
}

TEST(motor_off_and_autosave)
{
    uint8_t src[1024];
    memset(src, 0x5A, sizeof src);
    wr(3, 5);
    type1(0x18);
    wr(2, 1);
    wr(0, 0xA0);
    CHECK_EQ(transfer(NULL, src, 1024, ARC_US(5)), 1024);
    CHECK(wait_intrq(ARC_MS(500)));
    CHECK(fdc.drive[0].dirty);
    ArcTime end = now;
    advance(ARC_MS(1590));
    CHECK_EQ(rd(0) & 0x80, 0x80);           /* ancora acceso prima del nono giro */
    advance(end + ARC_MS(1801) - now);
    CHECK_EQ(rd(0) & 0x80, 0);
    CHECK(!fdc.drive[0].dirty);             /* salvato allo spegnimento */
    CHECK(read_file_at(path_d, d_offset(5, 0, 1), buf, 1024));
    CHECK_EQ(buf[0], 0x5A);

    /* con il motore spento e h = 1 niente spin-up */
    ArcTime t = now;
    wr(0, 0x80 | 0x08);
    CHECK_EQ(transfer(buf, NULL, 100000, ARC_US(5)), 1024);
    CHECK(now - t < REV + ARC_MS(50));
}

TEST(image_l_and_eject)
{
    sel_unit(1, 1);
    CHECK_EQ(type1(0x08), 0);               /* testina gia' in traccia 0 */
    wr(3, 3);
    type1(0x18);
    wr(2, 9);
    wr(0, 0x80);
    int n = transfer(buf, NULL, 100000, ARC_US(5));
    CHECK_EQ(n, 256);
    int ok = 1;
    for (int i = 0; i < 256; i++) if (buf[i] != pat_l(3, 1, 9, i)) ok = 0;
    CHECK(ok);

    /* espulsione con salvataggio */
    sel_unit(0, 0);
    uint8_t src[1024];
    memset(src, 0xC3, sizeof src);
    wr(1, 5);
    wr(2, 0);
    wr(0, 0xA0);
    CHECK_EQ(transfer(NULL, src, 1024, ARC_US(5)), 1024);
    CHECK(wait_intrq(ARC_MS(500)));
    fdc_eject(&fdc, 0);
    CHECK(fdc.drive[0].image == NULL);
    CHECK_EQ(fdc_disc_changed(&fdc), 0);
    CHECK(read_file_at(path_d, d_offset(5, 0, 0), buf, 1024));
    CHECK_EQ(buf[777], 0xC3);

    /* unita' vuota: niente impulsi di indice, il comando resta in attesa */
    wr(0, 0x80);
    advance(ARC_MS(3000));
    CHECK_EQ(rd(0) & 0x01, 0x01);
    CHECK_EQ(fdc_next_event(&fdc), ~(ArcTime)0);
    wr(0, 0xD0);
    CHECK_EQ(rd(0) & 0x01, 0);
    CHECK(fdc_insert(&fdc, 0, path_d));
    fdc_eject(&fdc, 0);
    fdc_eject(&fdc, 1);
}

int main(int argc, char **argv)
{
    /* le immagini di prova stanno accanto all'eseguibile */
    char dir[512] = ".";
    if (argc > 0 && argv[0]) {
        snprintf(dir, sizeof dir, "%s", argv[0]);
        char *a = strrchr(dir, '\\'), *b = strrchr(dir, '/');
        char *cut = a > b ? a : b;
        if (cut) *cut = 0;
        else strcpy(dir, ".");
    }
    snprintf(path_d, sizeof path_d, "%s/test_fdc_d.adf", dir);
    snprintf(path_l, sizeof path_l, "%s/test_fdc_l.adl", dir);
    snprintf(path_bad, sizeof path_bad, "%s/test_fdc_bad.adf", dir);
    make_images();

    RUN_TEST(insert_images);
    RUN_TEST(restore_with_spinup);
    RUN_TEST(seek_and_step_timing);
    RUN_TEST(seek_verify_error);
    RUN_TEST(read_sector);
    RUN_TEST(lost_data_and_rnf);
    RUN_TEST(read_address);
    RUN_TEST(write_sector_and_save);
    RUN_TEST(write_protect);
    RUN_TEST(force_interrupt);
    RUN_TEST(multi_sector);
    RUN_TEST(write_track_format);
    RUN_TEST(read_track);
    RUN_TEST(density_and_reset);
    RUN_TEST(motor_off_and_autosave);
    RUN_TEST(image_l_and_eject);

    remove(path_d);
    remove(path_l);
    remove(path_bad);
    printf("%d controlli, %d falliti\n", checks, failures);
    return failures ? 1 : 0;
}
