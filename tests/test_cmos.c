/*
 * test_cmos.c - Test del PCF8583: protocollo I2C pilotato come fa il
 * kernel (bit-bang su SCL/SDA), orologio, configurazione predefinita e
 * somma di controllo di RISC OS.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "cmos.h"

static int failures = 0, checks = 0;

#define CHECK(cond_) do { checks++; if (!(cond_)) { failures++; \
    printf("  FALLITO %s:%d: %s\n", __FILE__, __LINE__, #cond_); } } while (0)
#define CHECK_EQ(a, b) do { int va_ = (int)(a), vb_ = (int)(b); checks++; \
    if (va_ != vb_) { failures++; printf("  FALLITO %s:%d: %s = %d (&%02X), atteso %d (&%02X)\n", \
    __FILE__, __LINE__, #a, va_, va_, vb_, vb_); } } while (0)

/* ------------------------------------------------------------------ */
/* master I2C: ogni chiamata e' una scrittura del registro dell'IOC   */
/* ------------------------------------------------------------------ */

static int scl = 1, sda = 1;

static int set(Cmos *c, int new_scl, int new_sda)
{
    scl = new_scl;
    sda = new_sda;
    return cmos_i2c(c, scl, sda);
}

static void start(Cmos *c)
{
    set(c, scl, 1);
    set(c, 1, 1);
    set(c, 1, 0);
    set(c, 0, 0);
}

static void stop(Cmos *c)
{
    set(c, 0, 0);
    set(c, 1, 0);
    set(c, 1, 1);
}

/* manda un byte, ritorna 1 se il chip ha risposto ACK */
static int write_byte(Cmos *c, int b)
{
    for (int i = 7; i >= 0; i--) {
        set(c, 0, (b >> i) & 1);
        set(c, 1, (b >> i) & 1);
        set(c, 0, (b >> i) & 1);
    }
    set(c, 0, 1);
    int ack = set(c, 1, 1) == 0;
    set(c, 0, 1);
    return ack;
}

static int read_byte(Cmos *c, int ack)
{
    int b = 0;
    set(c, 0, 1);
    for (int i = 0; i < 8; i++) {
        b = b << 1 | set(c, 1, 1);
        set(c, 0, 1);
    }
    set(c, 0, !ack);
    set(c, 1, !ack);
    set(c, 0, !ack);
    return b;
}

static int read_regs(Cmos *c, int addr, uint8_t *out, int n)
{
    start(c);
    if (!write_byte(c, 0xA0) || !write_byte(c, addr)) { stop(c); return 0; }
    start(c);                                  /* START ripetuto */
    if (!write_byte(c, 0xA1)) { stop(c); return 0; }
    for (int i = 0; i < n; i++) out[i] = (uint8_t)read_byte(c, i < n - 1);
    stop(c);
    return 1;
}

static int write_regs(Cmos *c, int addr, const uint8_t *in, int n)
{
    start(c);
    int ok = write_byte(c, 0xA0) && write_byte(c, addr);
    for (int i = 0; ok && i < n; i++) ok = write_byte(c, in[i]);
    stop(c);
    return ok;
}

/* Somma di controllo scritta come nel kernel, per blocchi di indirizzi
   logici: [00..C0) -> fisico +&40, [C0..EF) -> fisico -&B0. */
static int riscos_checksum(const uint8_t *ram)
{
    int sum = 1;                              /* CMOSxseed */
    for (int l = 0x00; l < 0xC0; l++) sum += ram[l + 0x40];
    for (int l = 0xC0; l < 0xEF; l++) sum += ram[l - 0xB0];
    return sum & 0xFF;
}

static int logical(const Cmos *c, int l) { return c->ram[l < 0xC0 ? l + 0x40 : l - 0xB0]; }

static int from_bcd(int v) { return (v >> 4) * 10 + (v & 15); }

/* ------------------------------------------------------------------ */

static void test_defaults(void)
{
    Cmos c;
    cmos_init(&c, NULL);
    CHECK_EQ(c.ram[0x3F], riscos_checksum(c.ram));      /* CheckSumCMOS &EF */
    CHECK_EQ(logical(&c, 0xEF), c.ram[0x3F]);
    CHECK_EQ(logical(&c, 0x05), 8);                      /* ADFS */
    CHECK_EQ(logical(&c, 0x87) & 7, 2);                  /* 2 floppy */
    CHECK_EQ((logical(&c, 0x85) >> 2) & 31, 1);          /* MonitorType 1 */
    CHECK_EQ((logical(&c, 0x0A) & 15) | ((logical(&c, 0x85) & 2) << 3), 27);  /* Mode 27 */
    CHECK_EQ(logical(&c, 0xC4), 27);                     /* WimpMode 27 */
    CHECK_EQ(logical(&c, 0xB9), 10);                     /* Language: Desktop */
    CHECK_EQ(logical(&c, 0x0C), 32);                     /* KeyDelay */
    CHECK_EQ(logical(&c, 0x00), 0);                      /* stazione Econet */
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    CHECK_EQ(logical(&c, 0x81) * 100 + logical(&c, 0x80), t->tm_year + 1900);
    CHECK(!c.dirty);

    /* dopo una modifica dall'host la somma va rifatta */
    c.ram[0x45] ^= 0xFF;
    CHECK(c.ram[0x3F] != riscos_checksum(c.ram));
    cmos_fix_checksum(&c);
    CHECK_EQ(c.ram[0x3F], riscos_checksum(c.ram));
    /* i registri dell'orologio non entrano nella somma */
    c.ram[0x05] ^= 0xFF;
    CHECK_EQ(c.ram[0x3F], riscos_checksum(c.ram));
}

static void test_i2c_ram(void)
{
    Cmos c;
    cmos_init(&c, NULL);
    scl = sda = 1;
    cmos_i2c(&c, 1, 1);

    uint8_t w[4] = { 0x12, 0x34, 0x56, 0x78 }, r[4];
    CHECK(write_regs(&c, 0x80, w, 4));
    CHECK(c.dirty);
    CHECK_EQ(c.ram[0x80], 0x12);
    CHECK_EQ(c.ram[0x83], 0x78);
    CHECK(read_regs(&c, 0x80, r, 4));
    CHECK(memcmp(r, w, 4) == 0);

    /* lettura senza indirizzo: riparte dal byte dopo l'ultimo letto */
    start(&c);
    CHECK(write_byte(&c, 0xA1));
    CHECK_EQ(read_byte(&c, 0), c.ram[0x84]);
    stop(&c);

    /* auto-incremento con giro da &FF a &00 */
    uint8_t w2[2] = { 0xAA, 0x55 };
    CHECK(write_regs(&c, 0xFF, w2, 2));
    CHECK_EQ(c.ram[0xFF], 0xAA);
    CHECK_EQ(c.ram[0x00], 0x55);

    /* un altro indirizzo di periferica non riceve ACK */
    start(&c);
    CHECK(!write_byte(&c, 0xA2));
    stop(&c);
    CHECK_EQ(set(&c, 1, 1), 1);               /* bus rilasciato */

    /* il computer che tiene SDA basso vince sempre */
    CHECK_EQ(cmos_i2c(&c, 1, 0), 0);
    cmos_i2c(&c, 1, 1);
}

static void test_clock(void)
{
    Cmos c;
    cmos_init(&c, NULL);
    scl = sda = 1;
    cmos_i2c(&c, 1, 1);

    uint8_t r[7];
    time_t before = time(NULL);
    CHECK(read_regs(&c, 0, r, 7));
    struct tm t = *localtime(&before);
    CHECK_EQ(r[0] & 0x80, 0);                          /* orologio in marcia */
    CHECK((r[2] & 15) <= 9 && r[2] <= 0x59);
    CHECK((r[3] & 15) <= 9 && r[3] <= 0x59);
    CHECK_EQ(r[4] & 0x80, 0);                          /* formato 24 ore */
    CHECK_EQ(from_bcd(r[5] & 0x3F), t.tm_mday);
    CHECK_EQ(from_bcd(r[6] & 0x1F), t.tm_mon + 1);
    CHECK_EQ(r[5] >> 6, (t.tm_year + 1900) & 3);
    CHECK_EQ(r[6] >> 5, t.tm_wday);
    int host = t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
    int rtc = from_bcd(r[4] & 0x3F) * 3600 + from_bcd(r[3]) * 60 + from_bcd(r[2]);
    CHECK(rtc - host >= 0 && rtc - host <= 2);
}

static void test_save_load(void)
{
    char path[64];
    snprintf(path, sizeof path, "test_cmos_%ld.bin", (long)time(NULL));
    Cmos c, d;
    cmos_init(&c, NULL);
    c.ram[0x40 + 0x05] = 0x0B;                        /* FileLang: un altro FS */
    cmos_fix_checksum(&c);
    CHECK(cmos_save(&c, path));
    cmos_init(&d, path);
    CHECK_EQ(d.ram[0x45], 0x0B);
    CHECK_EQ(d.ram[0x3F], riscos_checksum(d.ram));

    /* file con la somma sbagliata: valori di fabbrica */
    c.ram[0x3F] ^= 1;
    CHECK(cmos_save(&c, path));
    cmos_init(&d, path);
    CHECK_EQ(d.ram[0x45], 8);
    CHECK_EQ(d.ram[0x3F], riscos_checksum(d.ram));
    remove(path);

    /* file mancante: valori di fabbrica */
    cmos_init(&d, "file_che_non_esiste.bin");
    CHECK_EQ(d.ram[0x45], 8);
}

int main(void)
{
    test_defaults();
    test_i2c_ram();
    test_clock();
    test_save_load();
    printf("%d controlli, %d falliti\n", checks, failures);
    return failures ? 1 : 0;
}
