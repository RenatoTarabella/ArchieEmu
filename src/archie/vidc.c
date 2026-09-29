/*
 * vidc.c - VIDC1a (vedi vidc.h)
 *
 * Il chip e' solo scritto dalla CPU: qui si conservano i valori grezzi e
 * la geometria si ricava al bisogno con le formule del datasheet
 * (le stesse che RISC OS usa, al contrario, per programmare i modi).
 */
#include "vidc.h"
#include <string.h>

/* numero di registro = bit 31-26 del dato */
enum {
    REG_PALETTE  = 0,    /* &00-&3C: colori logici 0-15 */
    REG_BORDER   = 16,   /* &40 */
    REG_CURSOR   = 17,   /* &44-&4C: colori 1-3 del cursore */
    REG_STEREO   = 24,   /* &60-&7C: immagine stereo */
    REG_HCR      = 32,   /* &80-&BC: temporizzazioni */
    REG_SFR      = 48,   /* &C0: frequenza del suono */
    REG_CONTROL  = 56    /* &E0 */
};

/*
 * HDSR e HDER sono sfasati di un ritardo di pipeline che dipende dalla
 * profondita': inizio reale in pixel = 2 * valore + X (datasheet VIDC).
 */
static const int hdisp_delay[4] = { 19, 11, 7, 5 };

void vidc_reset(Vidc *v)
{
    memset(v, 0, sizeof *v);
}

void vidc_write(Vidc *v, uint32_t data)
{
    unsigned reg = data >> 26;
    uint32_t r10 = (data >> 14) & 0x3FF;     /* campo a 10 bit dei registri di tempo */

    if (reg < 16) {
        v->palette[reg] = (uint16_t)(data & 0x1FFF);
        return;
    }
    if (reg == REG_BORDER) {
        v->border = (uint16_t)(data & 0x1FFF);
        return;
    }
    if (reg >= REG_CURSOR && reg < REG_CURSOR + 3) {
        v->cursor_palette[reg - REG_CURSOR] = (uint16_t)(data & 0x1FFF);
        return;
    }
    if (reg >= REG_STEREO && reg < REG_STEREO + 8) {
        /* &60 e' il canale 7, &64 il canale 0 e cosi' via */
        v->stereo[(reg - REG_STEREO + 7) & 7] = (uint8_t)(data & 7);
        return;
    }
    switch (reg) {
    case REG_HCR + 0:  v->hcr  = r10; break;
    case REG_HCR + 1:  v->hswr = r10; break;
    case REG_HCR + 2:  v->hbsr = r10; break;
    case REG_HCR + 3:  v->hdsr = r10; break;
    case REG_HCR + 4:  v->hder = r10; break;
    case REG_HCR + 5:  v->hber = r10; break;
    case REG_HCR + 6:  v->hcsr = (data >> 13) & 0x7FF; break;  /* risoluzione di un pixel */
    case REG_HCR + 7:  v->hir  = r10; break;
    case REG_HCR + 8:  v->vcr  = r10; break;
    case REG_HCR + 9:  v->vswr = r10; break;
    case REG_HCR + 10: v->vbsr = r10; break;
    case REG_HCR + 11: v->vdsr = r10; break;
    case REG_HCR + 12: v->vder = r10; break;
    case REG_HCR + 13: v->vber = r10; break;
    case REG_HCR + 14: v->vcsr = r10; break;
    case REG_HCR + 15: v->vcer = r10; break;
    case REG_SFR:      v->sound_freq = data & 0x1FF; break;    /* bit 8: test */
    case REG_CONTROL:  v->control = data & 0xFFFF; break;
    default: break;                                            /* indirizzi non usati */
    }
}

/* durata di un pixel in unita' da 1/24 MHz, moltiplicata per 2 per restare intera */
static const unsigned half_units_per_pixel[4] = { 6, 4, 3, 2 };   /* 8, 12, 16, 24 MHz */

void vidc_timing(const Vidc *v, VidcTiming *t)
{
    int lbpp = (int)((v->control >> 2) & 3);
    int hstart = 2 * (int)v->hdsr + hdisp_delay[lbpp];
    int hend = 2 * (int)v->hder + hdisp_delay[lbpp];
    uint32_t hpixels = 2 * v->hcr + 2;       /* periodo di riga in pixel */

    memset(t, 0, sizeof *t);
    t->log2bpp = lbpp;
    t->width = hend - hstart;
    /* VDSR/VDER contano le righe dall'inizio del sync verticale, meno uno */
    t->display_start = (int)v->vdsr + 1;
    t->display_end = (int)v->vder + 1;
    t->height = t->display_end - t->display_start;
    t->total_lines = (int)v->vcr + 1;
    t->line_time = (ArcTime)hpixels * half_units_per_pixel[v->control & 3] / 2;
    t->frame_time = t->line_time * (ArcTime)t->total_lines;

    /* dopo il reset i registri sono a zero: finche' il sistema non
       programma un modo coerente non c'e' niente da mostrare */
    t->valid = v->hcr > 0 && v->vcr > 0
            && t->width > 0 && t->height > 0
            && t->display_end <= t->total_lines
            && hend <= (int)hpixels;
}

/* colore a 12 bit (B11-8, G7-4, R3-0) -> &00RRGGBB */
static uint32_t rgb12(uint32_t c)
{
    uint32_t r = c & 15, g = (c >> 4) & 15, b = (c >> 8) & 15;
    return (r * 17) << 16 | (g * 17) << 8 | b * 17;
}

uint32_t vidc_border_rgb(const Vidc *v)
{
    return rgb12(v->border);
}

#define VIDC_MAX_W 1024
#define VIDC_MAX_H 768

void vidc_render(const Vidc *v, const uint8_t *ram, uint32_t ram_size,
                 uint32_t vinit, uint32_t vstart, uint32_t vend,
                 uint32_t cinit, int cursor_enabled,
                 uint32_t *out, int stride, int *w, int *h)
{
    VidcTiming t;
    uint32_t lut[256];

    vidc_timing(v, &t);
    *w = *h = 0;
    if (!t.valid || ram_size == 0)
        return;

    int width = t.width < VIDC_MAX_W ? t.width : VIDC_MAX_W;
    int height = t.height < VIDC_MAX_H ? t.height : VIDC_MAX_H;
    int lbpp = t.log2bpp;
    int bits = 1 << lbpp;
    int per_byte = 8 >> lbpp;
    uint32_t mask = (1u << bits) - 1;

    /* tabella dei colori: a 8 bpp i 4 bit alti del pixel scavalcano la
       palette e forniscono R3 (bit 4), G2 (bit 5), G3 (bit 6), B3 (bit 7) */
    if (lbpp == 3) {
        for (uint32_t p = 0; p < 256; p++) {
            uint32_t c = v->palette[p & 15] & 0x0737;
            c |= ((p >> 4) & 1) << 3 | ((p >> 5) & 3) << 6 | ((p >> 7) & 1) << 11;
            lut[p] = rgb12(c);
        }
    } else {
        for (uint32_t p = 0; p <= mask; p++)
            lut[p] = rgb12(v->palette[p]);
    }

    /* il DMA avanza a parole di quattro, ma confrontare byte per byte
       con la fine allineata da' lo stesso risultato */
    uint32_t addr = vinit;
    uint32_t wrap = (vend & ~15u) + 16;
    for (int y = 0; y < height; y++) {
        uint32_t *row = out + (size_t)y * (size_t)stride;
        uint32_t byte = 0;
        for (int x = 0; x < width; x++) {
            int sub = x & (per_byte - 1);
            if (sub == 0) {
                byte = ram[addr % ram_size];
                if (++addr == wrap)
                    addr = vstart;
            }
            row[x] = lut[(byte >> (sub * bits)) & mask];
        }
        /* se la larghezza e' stata tagliata, il DMA continua comunque */
        for (int x = width; x < t.width; x += per_byte)
            if (++addr == wrap)
                addr = vstart;
    }

    if (cursor_enabled) {
        /* posizione relativa all'inizio del display: HCSR conta in pixel
           dal sync con un ritardo fisso di 6, VCSR/VCER come VDSR */
        int cx = (int)v->hcsr + 6 - (2 * (int)v->hdsr + hdisp_delay[lbpp]);
        int cy = (int)v->vcsr - (int)v->vdsr;
        int ch = (int)v->vcer - (int)v->vcsr;
        uint32_t cur[3];
        for (int i = 0; i < 3; i++)
            cur[i] = rgb12(v->cursor_palette[i]);
        for (int line = 0; line < ch; line++) {
            int y = cy + line;
            if (y < 0 || y >= height)
                continue;
            for (int i = 0; i < 32; i++) {
                int x = cx + i;
                uint32_t b = ram[(cinit + (uint32_t)line * 8 + (uint32_t)(i >> 2)) % ram_size];
                uint32_t p = (b >> ((i & 3) * 2)) & 3;
                if (p != 0 && x >= 0 && x < width)
                    out[(size_t)y * (size_t)stride + (size_t)x] = cur[p - 1];
            }
        }
    }

    *w = width;
    *h = height;
}
