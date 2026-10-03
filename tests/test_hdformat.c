/*
 * test_hdformat.c - Test del formattatore dei dischi fissi (hdformat.c).
 *
 * Se in %TEMP% ci sono le immagini fatte da HForm 2.23 (ref64.hdf,
 * ref128.hdf, ref256.hdf, ref512.hdf: vedi tools/mkhdf.py) si confronta
 * byte per byte, con lo stesso disc ID e nome. Sempre: coerenza della
 * mappa (ZoneCheck, CrossCheck), del blocco di boot e della radice.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hdformat.h"

static int failures = 0, checks = 0;
#define CHECK_EQ(a, b) do { uint32_t va_ = (uint32_t)(a), vb_ = (uint32_t)(b); checks++; \
    if (va_ != vb_) { failures++; fprintf(stderr, "FALLITO %s:%d: %s = %08X, atteso %08X\n", \
        __FILE__, __LINE__, #a, va_, vb_); } } while (0)

static uint8_t *load(const char *path, long *n)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    *n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)*n);
    if (d && fread(d, 1, (size_t)*n, fp) != (size_t)*n) { free(d); d = NULL; }
    fclose(fp);
    return d;
}

static void self_checks(const uint8_t *d, uint32_t mb)
{
    HdfParams p;
    hdf_params(mb, &p);
    const uint8_t *bb = d + 0xC00;
    unsigned c = 0;
    for (int i = 0x1FE; i >= 0; i--) c = (c & 0xFF) + (c >> 8) + bb[i];
    CHECK_EQ(bb[0x1FF], c & 0xFF);
    CHECK_EQ(bb[0x1C0 + 9], p.nzones);
    uint32_t zb = 4096 - (uint32_t)p.zone_spare;
    uint32_t map = ((uint32_t)(p.nzones / 2) * zb - 480) << p.log2bpmb;
    uint8_t cross = 0;
    for (int z = 0; z < p.nzones; z++) cross ^= d[map + (uint32_t)z * 512 + 3];
    CHECK_EQ(cross, 0xFF);
    CHECK_EQ(memcmp(d + map, d + map + (uint32_t)p.nzones * 512, (size_t)p.nzones * 512), 0);   /* due copie */
    const uint8_t *root = d + map + 2u * (uint32_t)p.nzones * 512;
    CHECK_EQ(memcmp(root + 1, "Hugo", 4), 0);
    CHECK_EQ(memcmp(root + 2048 - 5, "Hugo", 4), 0);
}

int main(void)
{
    const char *tmp = getenv("TEMP");
    char path[512], ref[512];
    snprintf(path, sizeof path, "%s/test_hdformat.hdf", tmp ? tmp : ".");

    static const uint32_t sizes[] = { 64, 128, 256, 512 };
    int compared = 0;
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        snprintf(ref, sizeof ref, "%s/ref%u.hdf", tmp ? tmp : ".", sizes[k]);
        long rn = 0;
        uint8_t *r = load(ref, &rn);
        if (!r) continue;
        HdfParams p;
        hdf_params(sizes[k], &p);
        uint32_t zb = 4096 - (uint32_t)p.zone_spare;
        uint32_t map = ((uint32_t)(p.nzones / 2) * zb - 480) << p.log2bpmb;
        uint16_t id = (uint16_t)(r[map + 4 + 20] | r[map + 4 + 21] << 8);
        CHECK_EQ(hdf_create(path, sizes[k], "IDEDisc4", id), 1);
        long n = 0;
        uint8_t *d = load(path, &n);
        CHECK_EQ(n, rn);
        long diff = 0;
        for (long i = 0; d && i < n && i < rn; i++) if (d[i] != r[i]) diff++;
        CHECK_EQ(diff, 0);
        if (d) self_checks(d, sizes[k]);
        free(d);
        free(r);
        compared++;
    }
    if (!compared) {
        /* senza i riferimenti: almeno la coerenza */
        CHECK_EQ(hdf_create(path, 64, "HardDisc4", 0x1234), 1);
        long n = 0;
        uint8_t *d = load(path, &n);
        CHECK_EQ(n, 64L << 20);
        if (d) self_checks(d, 64);
        free(d);
    }
    remove(path);
    printf("%d controlli, %d falliti (%d immagini di HForm confrontate)\n", checks, failures, compared);
    return failures ? 1 : 0;
}
