/*
 * vidc20.c - VIDC20 (vedi vidc20.h)
 */
#include "vidc20.h"
#include <string.h>

void vidc20_reset(Vidc20 *v)
{
    memset(v, 0, sizeof *v);
}

void vidc20_write(Vidc20 *v, uint32_t x)
{
    uint32_t top = x >> 28;
    switch (top) {
    case 0x0:
        v->palette[v->pal_index++] = x & 0x00FFFFFFu;
        break;
    case 0x1: v->pal_index = (uint8_t)x; break;
    case 0x4: v->border = x & 0x00FFFFFFu; break;
    case 0x5: case 0x6: case 0x7: v->cursor[top - 5] = x & 0x00FFFFFFu; break;
    case 0x8: v->horiz[(x >> 24) & 7] = x & 0x3FFFu; break;
    case 0x9: v->vert[(x >> 24) & 7] = x & 0x1FFFu; break;
    case 0xA: v->stereo[(x >> 24) & 7] = x & 7u; break;
    case 0xB:
        if ((x >> 24) & 1) v->sound_ctrl = x & 0xFFFFFFu;
        else               v->sound_freq = x & 0xFFFFFFu;
        break;
    case 0xC: v->ext = x & 0xFFFFFFu; break;
    case 0xD: v->fsyn = x & 0xFFFFFFu; break;
    case 0xE: v->control = x & 0xFFFFFFu; break;
    default:  v->datactl = x & 0xFFFFFFu; break;
    }
}

int vidc20_log2bpp(const Vidc20 *v)
{
    static const int map[8] = { 0, 1, 2, 3, 4, 4, 5, 5 };
    return map[(v->control >> 5) & 7];
}

void vidc20_size(const Vidc20 *v, int *w, int *h)
{
    int hds = (int)v->horiz[3], hde = (int)v->horiz[4];
    int vds = (int)v->vert[3], vde = (int)v->vert[4];
    *w = hde > hds ? hde - hds : 0;
    *h = vde > vds ? vde - vds : 0;
    if (*w > 2048) *w = 2048;
    if (*h > 2048) *h = 2048;
}

void vidc20_render(const Vidc20 *v, Vidc20Mem mem, void *ctx, uint32_t start,
                   uint32_t *out, int stride, int *pw, int *ph)
{
    int w, h;
    vidc20_size(v, &w, &h);
    *pw = w;
    *ph = h;
    if (!w || !h) return;
    int l2 = vidc20_log2bpp(v);
    int bpp = 1 << l2;
    uint32_t line_bytes = (uint32_t)w * (uint32_t)bpp / 8;
    for (int y = 0; y < h; y++) {
        const uint8_t *p = mem(ctx, start + (uint32_t)y * line_bytes, line_bytes);
        uint32_t *o = out + (size_t)y * (size_t)stride;
        for (int x = 0; x < w; x++) {
            uint32_t c;
            if (!p) {
                c = 0;
            } else if (bpp <= 8) {
                uint32_t bit = (uint32_t)x * (uint32_t)bpp;
                uint32_t idx = (p[bit >> 3] >> (bit & 7)) & ((1u << bpp) - 1);
                c = v->palette[idx];
            } else if (bpp == 16) {
                uint32_t px = p[2 * x] | p[2 * x + 1] << 8;
                uint32_t r = px & 31, g = (px >> 5) & 31, b = (px >> 10) & 31;
                c = (r << 3 | r >> 2) | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2) << 16;
            } else {
                c = p[4 * x] | p[4 * x + 1] << 8 | (uint32_t)p[4 * x + 2] << 16;
            }
            /* palette: R nei bit bassi; l'uscita vuole 0x00RRGGBB */
            o[x] = (c & 0xFF) << 16 | (c & 0xFF00) | ((c >> 16) & 0xFF);
        }
    }
}
