/*
 * hdformat.c - Dischi fissi ADFS gia' formattati (vedi hdformat.h e
 * tools/mkhdf.py per la spiegazione del formato)
 */
#include "hdformat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SECSIZE  512
#define DIR_SIZE 2048

static uint32_t disc_bytes(uint32_t mb)
{
    uint32_t cyls = mb * 2048u / (HDF_HEADS * HDF_SPT);
    return cyls * HDF_HEADS * HDF_SPT * SECSIZE;
}

void hdf_params(uint32_t mb, HdfParams *p)
{
    /* quelli scelti da HForm 2.23 */
    static const struct { uint32_t mb; HdfParams p; } known[] = {
        { 64, { 14, 9, 33, 97 } }, { 100, { 14, 9, 51, 74 } }, { 128, { 15, 9, 65, 49 } },
        { 256, { 15, 10, 65, 49 } }, { 512, { 15, 11, 65, 49 } },
    };
    for (size_t k = 0; k < sizeof known / sizeof known[0]; k++)
        if (known[k].mb == mb) { *p = known[k].p; return; }
    /* le altre dimensioni: il bpmb e l'idlen piu' piccoli con cui gli id
       bastano, poi i bit divisi fra le zone (RISC OS le accetta: *CheckMap) */
    uint32_t size = disc_bytes(mb);
    for (int l2 = 9; l2 < 13; l2++) {
        uint32_t bits = size >> l2;
        for (int idlen = 13; idlen < 16; idlen++) {
            for (int spare = 32; spare < 200; spare++) {
                uint32_t zb = SECSIZE * 8 - (uint32_t)spare;
                uint32_t nz = (bits + 480 + zb - 1) / zb;
                if (zb / (uint32_t)(idlen + 1) * nz <= (1u << idlen) && nz <= 127) {
                    zb = (bits + 480 + nz - 1) / nz;
                    p->idlen = idlen;
                    p->log2bpmb = l2;
                    p->nzones = (int)nz;
                    p->zone_spare = SECSIZE * 8 - (int)zb;
                    return;
                }
            }
        }
    }
    p->idlen = 15; p->log2bpmb = 12; p->nzones = 127; p->zone_spare = 49;
}

static void set_bit(uint8_t *m, uint32_t b) { m[b >> 3] |= (uint8_t)(1u << (b & 7)); }

static void put_bits(uint8_t *m, uint32_t b, int n, uint32_t v)
{
    for (int k = 0; k < n; k++) if ((v >> k) & 1) set_bit(m, b + (uint32_t)k);
}

static uint8_t zone_check(const uint8_t *m)
{
    unsigned s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    for (int i = SECSIZE - 4; i > 0; i -= 4) {
        s0 += m[i] + (s3 >> 8); s3 &= 0xFF;
        s1 += m[i + 1] + (s0 >> 8); s0 &= 0xFF;
        s2 += m[i + 2] + (s1 >> 8); s1 &= 0xFF;
        s3 += m[i + 3] + (s2 >> 8); s2 &= 0xFF;
    }
    s0 += s3 >> 8;
    s1 += m[1] + (s0 >> 8);
    s2 += m[2] + (s1 >> 8);
    s3 += m[3] + (s2 >> 8);
    return (uint8_t)(s0 ^ s1 ^ s2 ^ s3);
}

static uint8_t boot_check(const uint8_t *b)
{
    unsigned c = 0;
    for (int i = 0x1FE; i >= 0; i--) c = (c & 0xFF) + (c >> 8) + b[i];
    return (uint8_t)c;
}

static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static uint32_t word(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* byte di controllo di una directory Hugo vuota (FileCore) */
static uint8_t dir_check(const uint8_t *d)
{
    uint32_t acc = 0;
    acc = word(d) ^ ror(acc, 13);
    acc = d[4] ^ ror(acc, 13);                 /* le voci finiscono dopo 5 byte */
    for (int p = DIR_SIZE - 40; p < DIR_SIZE - 4; p += 4) acc = word(d + p) ^ ror(acc, 13);
    acc ^= acc >> 16;
    acc ^= acc >> 8;
    return (uint8_t)acc;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void disc_record(uint8_t *r, const HdfParams *p, uint32_t size, uint32_t root, uint16_t id,
                        const char *name, int for_map)
{
    memset(r, 0, 60);
    r[0] = 9; r[1] = HDF_SPT; r[2] = HDF_HEADS; r[3] = 0;
    r[4] = (uint8_t)p->idlen; r[5] = (uint8_t)p->log2bpmb; r[6] = 0; r[7] = 0; r[8] = 1;
    r[9] = (uint8_t)p->nzones;
    r[10] = (uint8_t)p->zone_spare; r[11] = (uint8_t)(p->zone_spare >> 8);
    put32(r + 12, root);
    put32(r + 16, size);
    if (for_map) {
        r[20] = (uint8_t)id; r[21] = (uint8_t)(id >> 8);
        size_t n = strlen(name);
        if (n > 10) n = 10;
        memcpy(r + 22, name, n);
        if (n < 10) r[22 + n] = 0x0D;
        r[34] = 4;                             /* come HForm */
    }
}

static int write_at(FILE *fp, uint32_t off, const uint8_t *data, size_t n)
{
    return fseek(fp, (long)off, SEEK_SET) == 0 && fwrite(data, 1, n, fp) == n;
}

int hdf_create_params(const char *path, uint32_t mb, const char *name, uint16_t disc_id, const HdfParams *p)
{
    uint32_t cyls = mb * 2048u / (HDF_HEADS * HDF_SPT);
    uint32_t size = disc_bytes(mb);
    if (!cyls) return 0;
    uint32_t zone_bits = SECSIZE * 8 - (uint32_t)p->zone_spare;
    uint32_t bpmb = 1u << p->log2bpmb;
    uint32_t root = 0x200u | (uint32_t)(2 * p->nzones + 1);
    uint32_t total_bits = size >> p->log2bpmb;
    int mapzone = p->nzones / 2;
    uint32_t sys_len = (2u * (uint32_t)p->nzones * SECSIZE + DIR_SIZE + bpmb - 1) / bpmb;
    uint32_t boot_len = (0xE00 + bpmb - 1) / bpmb;
    if (sys_len < (uint32_t)p->idlen + 1) sys_len = (uint32_t)p->idlen + 1;
    if (boot_len < (uint32_t)p->idlen + 1) boot_len = (uint32_t)p->idlen + 1;
    if (!disc_id) { srand((unsigned)time(NULL)); disc_id = (uint16_t)(rand() ^ rand() << 8); }

    uint8_t *map = calloc((size_t)p->nzones, SECSIZE);
    if (!map) return 0;
    for (int z = 0; z < p->nzones; z++) {
        uint8_t *m = map + (size_t)z * SECSIZE;
        uint32_t first = 32 + (z == 0 ? 480 : 0), last = 32 + zone_bits;
        uint32_t end_g = total_bits + 480, zstart = (uint32_t)z * zone_bits;
        uint32_t zone_end = end_g - zstart < zone_bits ? end_g - zstart + 32 : last;
        uint32_t pos = first;
        if (z == 0) {                          /* oggetto 2: l'inizio del disco (boot) */
            put_bits(m, first, p->idlen, 2);
            set_bit(m, first + boot_len - 1);
            pos = first + boot_len;
        }
        if (z == mapzone) {                    /* oggetto 2: mappa e radice */
            put_bits(m, 32, p->idlen, 2);
            set_bit(m, 32 + sys_len - 1);
            if (pos == 32) pos = 32 + sys_len;
        }
        uint32_t freelink = 0x8000;
        if (pos < zone_end) {                  /* un frammento libero, l'ultimo della catena */
            set_bit(m, zone_end - 1);
            freelink |= pos - 8;
        }
        put_bits(m, zone_end, p->idlen, 1);    /* riserva e fine del disco: oggetto 1 */
        set_bit(m, SECSIZE * 8 - 1);
        m[1] = (uint8_t)freelink;
        m[2] = (uint8_t)(freelink >> 8);
        if (z == 0) disc_record(m + 4, p, size, root, disc_id, name, 1);
    }
    for (int z = 0; z < p->nzones; z++) {
        uint8_t *m = map + (size_t)z * SECSIZE;
        m[3] = z == 0 ? 0xFF : 0;              /* CrossCheck: lo XOR di tutte = &FF */
        m[0] = zone_check(m);
    }

    uint8_t dir[DIR_SIZE];
    memset(dir, 0, sizeof dir);
    memcpy(dir + 1, "Hugo", 4);
    int tail = DIR_SIZE - 41;
    dir[tail + 3] = (uint8_t)root;             /* genitore della radice: se stessa */
    dir[tail + 4] = (uint8_t)(root >> 8);
    dir[tail + 5] = (uint8_t)(root >> 16);
    dir[tail + 6] = '$';                       /* titolo */
    dir[tail + 25] = '$';                      /* nome */
    memcpy(dir + DIR_SIZE - 5, "Hugo", 4);
    dir[DIR_SIZE - 1] = dir_check(dir);

    uint8_t boot[SECSIZE];
    memset(boot, 0, sizeof boot);
    put32(boot, 0x20000000u);                  /* fine della lista dei difetti */
    memset(boot + 0x1AC, 0xFF, 4);             /* parametri dell'unita' IDE scritti da HForm */
    boot[0x1BB] = 1;
    put32(boot + 0x1BC, (cyls - 1) * HDF_HEADS * HDF_SPT * SECSIZE);   /* cilindro di parcheggio */
    disc_record(boot + 0x1C0, p, size, root, 0, name, 0);
    boot[0x1FF] = boot_check(boot);

    FILE *fp = fopen(path, "wb");
    if (!fp) { free(map); return 0; }
    static uint8_t zero[65536];
    int ok = 1;
    for (uint32_t k = 0; k < mb * 16 && ok; k++) ok = fwrite(zero, 1, sizeof zero, fp) == sizeof zero;
    uint32_t map_addr = ((uint32_t)mapzone * zone_bits - 480) * bpmb;
    ok = ok && write_at(fp, 0xC00, boot, sizeof boot);
    for (int copy = 0; copy < 2 && ok; copy++)
        ok = write_at(fp, map_addr + (uint32_t)(copy * p->nzones) * SECSIZE, map, (size_t)p->nzones * SECSIZE);
    ok = ok && write_at(fp, map_addr + 2u * (uint32_t)p->nzones * SECSIZE, dir, sizeof dir);
    free(map);
    return fclose(fp) == 0 && ok;
}

int hdf_create(const char *path, uint32_t mb, const char *name, uint16_t disc_id)
{
    HdfParams p;
    hdf_params(mb, &p);
    return hdf_create_params(path, mb, name, disc_id, &p);
}
