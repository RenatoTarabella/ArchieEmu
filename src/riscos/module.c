/*
 * module.c - Caricamento dei moduli e decompressione "squeeze".
 *
 * Porting in C di Kernel/s/UnSqueeze (Acorn, 1987-1990, licenza Apache 2.0).
 * Il modulo compresso ha il bit 31 dell'offset di init alzato; il resto
 * dell'offset e' la lunghezza del file compresso, che termina con 5 parole:
 *   dimensione espansa, dimensione codificata, dimensione tabelle,
 *   numero di "short", numero di "long".
 * I dati si decodificano all'indietro, due parole per byte di nibble.
 */
#include "module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIN_SHORT 9     /* 2 + NibsLong */
#define MIN_LONG  2

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Decodifica una tabella delle parole frequenti. is_longs: valori a 3 byte. */
static const uint8_t *decode_table(const uint8_t *p, const uint8_t *end, uint32_t n,
                                   uint32_t *out, int is_longs)
{
    uint32_t prev = 0xFFFFFFFFu, i = 0;
    while (i < n) {
        if (p >= end) return NULL;
        uint32_t b = *p++;
        if (b == 0) {                              /* letterale */
            uint32_t v = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
            p += 3;
            if (!is_longs) v |= (uint32_t)*p++ << 24;
            prev += v;
            out[i++] = prev;
        } else if (b < 10) {                       /* b incrementi di 1 */
            while (b-- && i < n) out[i++] = ++prev;
        } else if (b < 92) {                       /* delta piccolo */
            prev += b - 10;
            out[i++] = prev;
        } else if (b < 174) {                      /* delta su un byte in piu' */
            prev += ((b - 92) << 8) | *p++;
            out[i++] = prev;
        } else {                                   /* delta su due byte in piu' */
            uint32_t v = ((b - 174) << 16) | p[0] | ((uint32_t)p[1] << 8);
            p += 2;
            prev += v;
            out[i++] = prev;
        }
    }
    return p;
}

uint8_t *module_unsqueeze(const uint8_t *in, size_t insize, size_t *outsize)
{
    if (insize < 28) return NULL;
    uint32_t init = rd32(in + MODULE_INIT);
    if (!(init & 0x80000000u)) return NULL;
    uint32_t total = init & 0x7FFFFFFFu;
    if (total > insize || total < 20) return NULL;

    const uint8_t *end = in + total;
    uint32_t decoded = rd32(end - 20), encoded = rd32(end - 16);
    uint32_t tables = rd32(end - 12), nshorts = rd32(end - 8), nlongs = rd32(end - 4);
    const uint8_t *tab_start = end - 20 - tables;
    const uint8_t *data_start = tab_start - encoded;
    if (tab_start < in || data_start < in || (decoded & 3) || nshorts > 0x10000 || nlongs > 0x10000)
        return NULL;

    uint32_t *shorts = malloc((nshorts + nlongs + 1) * sizeof(uint32_t));
    uint8_t *out = malloc(decoded);
    if (!shorts || !out) { free(shorts); free(out); return NULL; }
    uint32_t *longs = shorts + nshorts;

    const uint8_t *p = decode_table(tab_start, end - 20, nshorts, shorts, 0);
    if (p) p = decode_table(p, end - 20, nlongs, longs, 1);
    if (!p) { free(shorts); free(out); return NULL; }

    /* decodifica all'indietro: ogni byte di controllo produce due parole */
    const uint8_t *src = tab_start;
    uint8_t *dst = out + decoded;
    while (src > data_start) {
        uint32_t ctl = *--src;
        uint32_t w[2];
        for (int k = 0; k < 2; k++) {
            uint32_t nib = k ? ctl >> 4 : ctl & 15;
            if (nib >= MIN_SHORT) {
                uint32_t idx = ((nib - MIN_SHORT) << 8) | *--src;
                w[k] = idx < nshorts ? shorts[idx] : 0;
            } else if (nib >= MIN_LONG) {
                uint32_t idx = ((nib - MIN_LONG) << 8) | *--src;
                uint32_t hi = idx < nlongs ? longs[idx] : 0;
                w[k] = (hi << 8) | *--src;
            } else if (nib == 0) {
                w[k] = 0;
            } else {                                /* letterale, 4 byte */
                uint32_t v = *--src;
                v |= (uint32_t)*--src << 8;
                v |= (uint32_t)*--src << 16;
                v |= (uint32_t)*--src << 24;
                w[k] = v;
            }
        }
        if (dst - out < 8) break;
        /* STMDB R8!,{R4,R5}: R4 all'indirizzo piu' basso */
        dst -= 8;
        for (int b = 0; b < 4; b++) {
            dst[b]     = (uint8_t)(w[0] >> (8 * b));
            dst[4 + b] = (uint8_t)(w[1] >> (8 * b));
        }
    }
    free(shorts);
    if (dst != out) { free(out); return NULL; }
    *outsize = decoded;
    return out;
}

int module_load(const char *path, RiscosModule *mod, char *err, size_t errsize)
{
    memset(mod, 0, sizeof *mod);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errsize, "impossibile aprire %s", path); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0x20 || n > (16 << 20)) { fclose(f); snprintf(err, errsize, "%s: dimensione strana", path); return 0; }
    uint8_t *raw = malloc((size_t)n);
    if (!raw || fread(raw, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(raw);
        snprintf(err, errsize, "%s: errore di lettura", path);
        return 0;
    }
    fclose(f);

    if (rd32(raw + MODULE_INIT) & 0x80000000u) {
        size_t outsize;
        uint8_t *out = module_unsqueeze(raw, (size_t)n, &outsize);
        free(raw);
        if (!out) { snprintf(err, errsize, "%s: modulo compresso non valido", path); return 0; }
        mod->data = out;
        mod->size = outsize;
        mod->was_squeezed = 1;
    } else {
        mod->data = raw;
        mod->size = (size_t)n;
    }
    return 1;
}

void module_free(RiscosModule *mod)
{
    free(mod->data);
    memset(mod, 0, sizeof *mod);
}

uint32_t module_word(const RiscosModule *mod, uint32_t offset)
{
    return offset + 4 <= mod->size ? rd32(mod->data + offset) : 0;
}

const char *module_string(const RiscosModule *mod, uint32_t offset)
{
    if (offset >= mod->size) return "";
    for (size_t k = offset; k < mod->size; k++)
        if (mod->data[k] == 0) return (const char *)mod->data + offset;
    return "";
}
