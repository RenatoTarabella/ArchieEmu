/*
 * vdu.c - Driver VDU (vedi vdu.h)
 */
#include "vdu.h"
#include "font.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Costi in tick (8 MHz) tarati confrontando gli stessi programmi BASIC con
   RISC OS 3.11 emulato fedelmente nella macchina Archimedes (modo 12) */
#define COST_CHAR         680u   /* un carattere disegnato */
#define COST_PARAM         60u   /* un byte di parametri in coda */
#define COST_CONTROL      300u   /* un codice di controllo */
#define COST_COLOUR      4420u   /* COLOUR, GCOL, palette */
#define COST_PLOT         660u   /* una PLOT, esclusi i pixel */

/* riempire o spostare 'bytes' di memoria video: ~2,2 tick per byte */
static inline void cost_bytes(Vdu *v, uint32_t bytes) { v->cost += bytes * 2 + bytes / 5; }

/* ------------------------------------------------------------------ */
/* modi                                                               */
/* ------------------------------------------------------------------ */

typedef struct ModeInfo { int mode, w, h, log2bpp; } ModeInfo;

static const ModeInfo mode_table[] = {
    {  0, 640, 256, 0 }, {  1, 320, 256, 1 }, {  2, 160, 256, 2 },
    {  4, 320, 256, 0 }, {  5, 160, 256, 1 },
    {  8, 640, 256, 1 }, {  9, 320, 256, 2 }, { 10, 160, 256, 3 },
    { 11, 640, 250, 1 }, { 12, 640, 256, 2 }, { 13, 320, 256, 3 },
    { 14, 640, 250, 2 }, { 15, 640, 256, 3 },
    { 18, 640, 512, 0 }, { 19, 640, 512, 1 }, { 20, 640, 512, 2 }, { 21, 640, 512, 3 },
    { 25, 640, 480, 0 }, { 26, 640, 480, 1 }, { 27, 640, 480, 2 }, { 28, 640, 480, 3 },
    { 29, 800, 600, 0 }, { 30, 800, 600, 1 }, { 31, 800, 600, 2 }, { 32, 800, 600, 3 },
    { 33, 768, 288, 0 }, { 34, 768, 288, 1 }, { 35, 768, 288, 2 }, { 36, 768, 288, 3 },
    { 37, 896, 352, 0 }, { 38, 896, 352, 1 }, { 39, 896, 352, 2 }, { 40, 896, 352, 3 },
    { 41, 640, 352, 0 }, { 42, 640, 352, 1 }, { 43, 640, 352, 2 },
    { 44, 640, 200, 0 }, { 45, 640, 200, 1 }, { 46, 640, 200, 2 },
    { 47, 360, 480, 3 }, { 48, 640, 480, 4 }, { 49, 640, 480, 5 },
    { 50, 800, 600, 4 }, { 51, 800, 600, 5 }, { 52, 1024, 768, 3 }, { 53, 1024, 768, 5 },
};

static const ModeInfo *find_mode(int mode)
{
    for (size_t k = 0; k < sizeof mode_table / sizeof mode_table[0]; k++)
        if (mode_table[k].mode == mode) return &mode_table[k];
    return NULL;
}

int vdu_mode_valid(int mode) { return find_mode(mode & 0x7F) != NULL; }

/* ------------------------------------------------------------------ */
/* colori                                                             */
/* ------------------------------------------------------------------ */

static const uint32_t bbc_colours[8] = {
    0x000000, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF
};

/* pixel a 8 bpp nel formato del VIDC: bit 0-1 tinta, 2 R2, 3 B2, 4 R3, 5 G2, 6 G3, 7 B3 */
static uint32_t vidc256_rgb(uint32_t b)
{
    uint32_t t = b & 3;
    uint32_t r = (((b >> 4) & 1) << 3) | (((b >> 2) & 1) << 2) | t;
    uint32_t g = (((b >> 6) & 1) << 3) | (((b >> 5) & 1) << 2) | t;
    uint32_t bl = (((b >> 7) & 1) << 3) | (((b >> 3) & 1) << 2) | t;
    return (r * 17) << 16 | (g * 17) << 8 | bl * 17;
}

/* colore BASIC 0-63 (bit 0-1 rosso, 2-3 verde, 4-5 blu) + tinta 0-3 -> pixel */
static uint32_t vidc256_pixel(uint32_t c, int tint)
{
    uint32_t r = c & 3, g = (c >> 2) & 3, b = (c >> 4) & 3;
    return (uint32_t)(tint & 3) | (r & 1) << 2 | (b & 1) << 3 | (r >> 1) << 4
         | (g & 1) << 5 | (g >> 1) << 6 | (b >> 1) << 7;
}

static uint32_t rgb_to_pixel(const Vdu *v, uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    if (v->log2bpp == 5) return b << 16 | g << 8 | r;              /* &00BBGGRR */
    if (v->log2bpp == 4) return (b >> 3) << 10 | (g >> 3) << 5 | (r >> 3);
    /* modi con palette: il colore piu' vicino */
    uint32_t best = 0, bestd = 0xFFFFFFFFu;
    for (uint32_t i = 0; i <= v->ncolour; i++) {
        uint32_t p = v->palette[i].first;
        int dr = (int)((p >> 16) & 255) - (int)r, dg = (int)((p >> 8) & 255) - (int)g, db = (int)(p & 255) - (int)b;
        uint32_t d = (uint32_t)(dr * dr * 3 + dg * dg * 4 + db * db * 2);
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

static uint32_t pixel_to_rgb(const Vdu *v, uint32_t p, int phase)
{
    if (v->log2bpp == 5) return (p & 255) << 16 | (p & 0xFF00) | ((p >> 16) & 255);
    if (v->log2bpp == 4) {
        uint32_t r = p & 31, g = (p >> 5) & 31, b = (p >> 10) & 31;
        return (r * 255 / 31) << 16 | (g * 255 / 31) << 8 | b * 255 / 31;
    }
    const VduPalette *e = &v->palette[p & 255];
    return phase ? e->second : e->first;
}

static void default_palette(Vdu *v)
{
    for (int i = 0; i < 256; i++) v->palette[i].first = v->palette[i].second = 0;
    switch (v->log2bpp) {
    case 0:
        v->palette[1].first = v->palette[1].second = 0xFFFFFF;
        break;
    case 1: {
        static const uint32_t c4[4] = { 0x000000, 0xFF0000, 0xFFFF00, 0xFFFFFF };
        for (int i = 0; i < 4; i++) v->palette[i].first = v->palette[i].second = c4[i];
        break;
    }
    case 2:
        for (int i = 0; i < 8; i++) {
            v->palette[i].first = v->palette[i].second = bbc_colours[i];
            v->palette[i + 8].first = bbc_colours[i];          /* 8-15 lampeggiano */
            v->palette[i + 8].second = bbc_colours[7 - i];
        }
        break;
    case 3:
        for (int i = 0; i < 256; i++) v->palette[i].first = v->palette[i].second = vidc256_rgb((uint32_t)i);
        break;
    default:
        break;
    }
    v->border = 0;
}

/* COLOUR/GCOL n: colore logico -> valore del pixel */
static uint32_t logical_colour(const Vdu *v, uint32_t n, int tint)
{
    switch (v->log2bpp) {
    case 3:  return vidc256_pixel(n & 63, tint >> 6);
    case 4:
    case 5:  return rgb_to_pixel(v, bbc_colours[n & 7]);
    default: return n & v->ncolour;
    }
}

static void default_colours(Vdu *v)
{
    v->tint_tf = v->tint_gf = 0xC0;
    v->tint_tb = v->tint_gb = 0;
    uint32_t white = v->log2bpp >= 4 ? rgb_to_pixel(v, 0xFFFFFF)
                   : v->log2bpp == 3 ? 0xFF
                   : v->log2bpp == 2 ? 7 : v->ncolour;
    v->tfg = v->gfg = white;
    v->tbg = v->gbg = 0;
    v->gfg_action = v->gbg_action = 0;
}

/* ------------------------------------------------------------------ */
/* pixel                                                              */
/* ------------------------------------------------------------------ */

static uint32_t get_pixel(const Vdu *v, int x, int y)
{
    const uint8_t *row = v->screen + (size_t)y * (size_t)v->line_bytes;
    switch (v->log2bpp) {
    case 5: { const uint8_t *p = row + 4 * x; return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
    case 4: { const uint8_t *p = row + 2 * x; return p[0] | (uint32_t)p[1] << 8; }
    case 3: return row[x];
    default: {
        int bpp = 1 << v->log2bpp, per = 8 / bpp;
        int shift = (x % per) * bpp;                   /* pixel a sinistra nei bit bassi */
        return (row[x / per] >> shift) & ((1u << bpp) - 1);
    }
    }
}

static void put_pixel(Vdu *v, int x, int y, uint32_t c)
{
    uint8_t *row = v->screen + (size_t)y * (size_t)v->line_bytes;
    switch (v->log2bpp) {
    case 5: { uint8_t *p = row + 4 * x; p[0] = (uint8_t)c; p[1] = (uint8_t)(c >> 8); p[2] = (uint8_t)(c >> 16); p[3] = 0; break; }
    case 4: { uint8_t *p = row + 2 * x; p[0] = (uint8_t)c; p[1] = (uint8_t)(c >> 8); break; }
    case 3: row[x] = (uint8_t)c; break;
    default: {
        int bpp = 1 << v->log2bpp, per = 8 / bpp;
        int shift = (x % per) * bpp;
        uint8_t mask = (uint8_t)(((1u << bpp) - 1) << shift);
        uint8_t *b = &row[x / per];
        *b = (uint8_t)((*b & ~mask) | ((c << shift) & mask));
    }
    }
}

static uint32_t colour_mask(const Vdu *v)
{
    return v->log2bpp == 5 ? 0xFFFFFFu : v->log2bpp == 4 ? 0x7FFFu : v->ncolour;
}

/* pixel grafico con l'azione di GCOL e il ritaglio alla finestra */
static void plot_pixel(Vdu *v, int x, int y, uint32_t c, int action)
{
    if (x < v->gwl || x > v->gwr || y < v->gwt || y > v->gwb) return;
    uint32_t old = get_pixel(v, x, y), m = colour_mask(v);
    switch (action & 7) {
    case 0: break;
    case 1: c |= old; break;
    case 2: c &= old; break;
    case 3: c ^= old; break;
    case 4: c = ~old; break;
    case 5: return;
    case 6: c = old & ~c; break;
    default: c = old | ~c; break;
    }
    put_pixel(v, x, y, c & m);
}

static void hline(Vdu *v, int x0, int x1, int y, uint32_t c, int action)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y < v->gwt || y > v->gwb) return;
    if (x0 < v->gwl) x0 = v->gwl;
    if (x1 > v->gwr) x1 = v->gwr;
    if (x1 >= x0) cost_bytes(v, (((uint32_t)(x1 - x0 + 1)) << v->log2bpp) / 8 + 1);
    for (int x = x0; x <= x1; x++) plot_pixel(v, x, y, c, action);
}

/* ------------------------------------------------------------------ */
/* modo                                                               */
/* ------------------------------------------------------------------ */

static void reset_windows(Vdu *v)
{
    v->twl = 0; v->twt = 0; v->twr = v->cols - 1; v->twb = v->rows - 1;
    v->gwl = 0; v->gwt = 0; v->gwr = v->width - 1; v->gwb = v->height - 1;
    v->orgx = v->orgy = 0;
}

static int apply_mode(Vdu *v, int w, int h, int log2bpp)
{
    size_t bytes = ((size_t)w << log2bpp) / 8 * (size_t)h;
    if (w < 8 || h < 8 || bytes > VDU_MAX_BYTES) return 0;
    v->width = w;
    v->height = h;
    v->log2bpp = log2bpp;
    v->line_bytes = (w << log2bpp) / 8;
    v->xeig = w <= 200 ? 3 : w <= 400 ? 2 : 1;           /* 1280 unita' OS di larghezza */
    v->yeig = h <= 300 ? 2 : 1;
    v->ncolour = log2bpp == 5 ? 0xFFFFFFFFu : log2bpp == 4 ? 65535u : (1u << (1 << log2bpp)) - 1;
    if (log2bpp == 3) v->ncolour = 255;
    v->cols = w / 8;
    v->rows = h / 8;
    default_palette(v);
    default_colours(v);
    reset_windows(v);
    memset(v->screen, 0, bytes);
    v->tx = v->ty = 0;
    v->gx = v->gy = v->ox = v->oy = v->oox = v->ooy = 0;
    v->cursor_on = 1;
    v->vdu5 = 0;
    v->pending_wrap = 0;
    return 1;
}

int vdu_set_mode(Vdu *v, int mode)
{
    const ModeInfo *m = find_mode(mode & 0x7F);
    if (!m || !apply_mode(v, m->w, m->h, m->log2bpp)) return 0;
    v->mode = m->mode;
    return 1;
}

int vdu_set_mode_spec(Vdu *v, int w, int h, int log2bpp)
{
    if (log2bpp < 0 || log2bpp > 5 || w > 2048 || h > 2048) return 0;
    for (size_t k = 0; k < sizeof mode_table / sizeof mode_table[0]; k++) {
        if (mode_table[k].w == w && mode_table[k].h == h && mode_table[k].log2bpp == log2bpp)
            return vdu_set_mode(v, mode_table[k].mode);
    }
    if (!apply_mode(v, w, h, log2bpp)) return 0;
    v->mode = -1;
    return 1;
}

void vdu_init(Vdu *v, uint8_t *screen, uint32_t screen_addr)
{
    memset(v, 0, sizeof *v);
    v->screen = screen;
    v->screen_addr = screen_addr;
    memset(v->font, 0, sizeof v->font);
    memcpy(v->font[32], riscos_font, sizeof riscos_font);
    vdu_set_mode(v, 27);
}

/* ------------------------------------------------------------------ */
/* testo                                                              */
/* ------------------------------------------------------------------ */

static void fill_rect_px(Vdu *v, int x0, int y0, int x1, int y1, uint32_t c)
{
    if (x1 >= x0 && y1 >= y0)
        cost_bytes(v, (((uint32_t)(x1 - x0 + 1) * (uint32_t)(y1 - y0 + 1)) << v->log2bpp) / 8);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) put_pixel(v, x, y, c);
}

static void clear_text_window(Vdu *v)
{
    fill_rect_px(v, v->twl * 8, v->twt * 8, v->twr * 8 + 7, v->twb * 8 + 7, v->tbg);
    v->tx = v->ty = 0;
}

static void scroll_text(Vdu *v, int up)
{
    int x0 = v->twl * 8, x1 = v->twr * 8 + 7;
    int bx0 = (x0 << v->log2bpp) / 8, bx1 = ((x1 + 1) << v->log2bpp) / 8;
    size_t span = (size_t)(bx1 - bx0);
    int top = v->twt * 8, bottom = v->twb * 8 + 7;
    cost_bytes(v, (uint32_t)(span * (size_t)(bottom - top + 1)));
    if (up) {
        for (int y = top; y + 8 <= bottom; y++)
            memmove(v->screen + (size_t)y * v->line_bytes + bx0,
                    v->screen + (size_t)(y + 8) * v->line_bytes + bx0, span);
        fill_rect_px(v, x0, bottom - 7, x1, bottom, v->tbg);
    } else {
        for (int y = bottom; y - 8 >= top; y--)
            memmove(v->screen + (size_t)y * v->line_bytes + bx0,
                    v->screen + (size_t)(y - 8) * v->line_bytes + bx0, span);
        fill_rect_px(v, x0, top, x1, top + 7, v->tbg);
    }
}

static int win_cols(const Vdu *v) { return v->twr - v->twl + 1; }
static int win_rows(const Vdu *v) { return v->twb - v->twt + 1; }

static void cursor_down(Vdu *v)
{
    if (++v->ty >= win_rows(v)) { v->ty = win_rows(v) - 1; scroll_text(v, 1); }
}

static void cursor_up(Vdu *v)
{
    if (--v->ty < 0) { v->ty = 0; scroll_text(v, 0); }
}

static void cursor_right(Vdu *v)
{
    if (v->vdu5) { v->gx += 8 << v->xeig; return; }
    if (++v->tx >= win_cols(v)) { v->tx = 0; cursor_down(v); }
}

static void cursor_left(Vdu *v)
{
    if (v->vdu5) { v->gx -= 8 << v->xeig; return; }
    if (--v->tx < 0) { v->tx = win_cols(v) - 1; cursor_up(v); }
}

/* coordinate OS -> pixel (y verso il basso) */
static int os_to_px_x(const Vdu *v, int x) { return x >> v->xeig; }
static int os_to_px_y(const Vdu *v, int y) { return v->height - 1 - (y >> v->yeig); }

static void draw_char(Vdu *v, int ch)
{
    v->cost += COST_CHAR;
    const uint8_t *g = v->font[ch & 255];
    if (v->vdu5) {
        int px = os_to_px_x(v, v->gx), py = os_to_px_y(v, v->gy);
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++)
                if (g[r] & (0x80 >> c)) plot_pixel(v, px + c, py + r, v->gfg, v->gfg_action);
        cursor_right(v);
        return;
    }
    int px = (v->twl + v->tx) * 8, py = (v->twt + v->ty) * 8;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            put_pixel(v, px + c, py + r, (g[r] & (0x80 >> c)) ? v->tfg : v->tbg);
    cursor_right(v);
}

/* ------------------------------------------------------------------ */
/* grafica                                                            */
/* ------------------------------------------------------------------ */

static void line_px(Vdu *v, int x0, int y0, int x1, int y1, uint32_t c, int action, int skip_last)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        int last = x0 == x1 && y0 == y1;
        if (!(last && skip_last)) plot_pixel(v, x0, y0, c, action);
        if (last) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void triangle_px(Vdu *v, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c, int action)
{
    /* ordina per y e riempi con scanline */
    if (y1 < y0) { int t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    if (y2 < y0) { int t = x0; x0 = x2; x2 = t; t = y0; y0 = y2; y2 = t; }
    if (y2 < y1) { int t = x1; x1 = x2; x2 = t; t = y1; y1 = y2; y2 = t; }
    for (int y = y0; y <= y2; y++) {
        double xa = y2 == y0 ? x0 : x0 + (double)(x2 - x0) * (y - y0) / (y2 - y0);
        double xb;
        if (y < y1) xb = y1 == y0 ? x1 : x0 + (double)(x1 - x0) * (y - y0) / (y1 - y0);
        else        xb = y2 == y1 ? x2 : x1 + (double)(x2 - x1) * (y - y1) / (y2 - y1);
        int a = (int)(xa + 0.5), b = (int)(xb + 0.5);
        hline(v, a, b, y, c, action);
    }
}

static void ellipse_px(Vdu *v, int cx, int cy, int a, int b, uint32_t c, int action, int fill)
{
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    if (a == 0 && b == 0) { plot_pixel(v, cx, cy, c, action); return; }
    int prev = -1;
    for (int dy = b; dy >= 0; dy--) {
        /* larghezza della riga dy */
        double t = b ? 1.0 - (double)dy * dy / ((double)b * b) : 1.0;
        int w = (int)(a * (t > 0 ? sqrt(t) : 0) + 0.5);
        if (fill) {
            hline(v, cx - w, cx + w, cy - dy, c, action);
            if (dy) hline(v, cx - w, cx + w, cy + dy, c, action);
        } else {
            int from = prev < 0 ? w : prev + 1;
            if (from > w) from = w;
            for (int x = from; x <= w; x++) {
                plot_pixel(v, cx - x, cy - dy, c, action);
                plot_pixel(v, cx + x, cy - dy, c, action);
                if (dy) {
                    plot_pixel(v, cx - x, cy + dy, c, action);
                    plot_pixel(v, cx + x, cy + dy, c, action);
                }
            }
        }
        prev = w;
    }
}

static void flood_fill(Vdu *v, int x, int y, uint32_t c, int action, int until_fg)
{
    /* riempie finche' trova il colore di primo piano (until_fg) oppure
       finche' il pixel e' uguale al colore di sfondo */
    if (x < v->gwl || x > v->gwr || y < v->gwt || y > v->gwb) return;
    uint32_t bgc = v->gbg, fgc = v->gfg;
#define FILLABLE(px, py) (until_fg ? get_pixel(v, px, py) != fgc : get_pixel(v, px, py) == bgc)
    if (!FILLABLE(x, y) || (!until_fg && bgc == c && action == 0)) return;
    size_t cap = 4096, n = 0;
    int *stack = malloc(cap * 2 * sizeof(int));
    if (!stack) return;
    stack[n * 2] = x; stack[n * 2 + 1] = y; n++;
    uint8_t *seen = calloc((size_t)v->width * v->height, 1);
    if (!seen) { free(stack); return; }
    while (n) {
        n--;
        int sx = stack[n * 2], sy = stack[n * 2 + 1];
        if (seen[(size_t)sy * v->width + sx] || !FILLABLE(sx, sy)) continue;
        int l = sx, r = sx;
        while (l > v->gwl && !seen[(size_t)sy * v->width + l - 1] && FILLABLE(l - 1, sy)) l--;
        while (r < v->gwr && !seen[(size_t)sy * v->width + r + 1] && FILLABLE(r + 1, sy)) r++;
        for (int k = l; k <= r; k++) seen[(size_t)sy * v->width + k] = 1;
        for (int k = l; k <= r; k++) plot_pixel(v, k, sy, c, action);
        for (int d = -1; d <= 1; d += 2) {
            int ny = sy + d;
            if (ny < v->gwt || ny > v->gwb) continue;
            for (int k = l; k <= r; k++) {
                if (n + 1 >= cap) {
                    cap *= 2;
                    int *ns = realloc(stack, cap * 2 * sizeof(int));
                    if (!ns) { free(stack); free(seen); return; }
                    stack = ns;
                }
                stack[n * 2] = k; stack[n * 2 + 1] = ny; n++;
            }
        }
    }
#undef FILLABLE
    free(stack);
    free(seen);
}

void vdu_plot(Vdu *v, int k, int x, int y)
{
    v->cost += COST_PLOT;
    int tx, ty;
    if (k & 4) { tx = x + v->orgx; ty = y + v->orgy; }
    else       { tx = v->gx + x;   ty = v->gy + y; }

    uint32_t c;
    int action;
    switch (k & 3) {
    case 1:  c = v->gfg; action = v->gfg_action; break;
    case 2:  c = 0; action = 4; break;                 /* inverso logico */
    case 3:  c = v->gbg; action = v->gbg_action; break;
    default: c = 0; action = 5; break;                 /* solo movimento */
    }
    int base = k & ~7;
    int px0 = os_to_px_x(v, v->gx), py0 = os_to_px_y(v, v->gy);
    int px1 = os_to_px_x(v, tx),    py1 = os_to_px_y(v, ty);
    int pxo = os_to_px_x(v, v->ox), pyo = os_to_px_y(v, v->oy);

    if ((k & 3) != 0) {
        if (base < 64) {
            line_px(v, px0, py0, px1, py1, c, action, (base & 8) != 0);
        } else if (base == 64) {
            plot_pixel(v, px1, py1, c, action);
        } else if (base == 80) {
            triangle_px(v, pxo, pyo, px0, py0, px1, py1, c, action);
        } else if (base == 96) {
            int xa = px0 < px1 ? px0 : px1, xb = px0 < px1 ? px1 : px0;
            int ya = py0 < py1 ? py0 : py1, yb = py0 < py1 ? py1 : py0;
            for (int yy = ya; yy <= yb; yy++) hline(v, xa, xb, yy, c, action);
        } else if (base == 112) {
            /* parallelogramma: quarto vertice = ultimo + (vecchio - penultimo) */
            int x3 = px1 + (pxo - px0), y3 = py1 + (pyo - py0);
            triangle_px(v, pxo, pyo, px0, py0, px1, py1, c, action);
            triangle_px(v, pxo, pyo, px1, py1, x3, y3, c, action);
        } else if (base == 128 || base == 136) {
            flood_fill(v, px1, py1, c, action, base == 136);
        } else if (base == 144 || base == 152) {
            /* centro = punto precedente, raggio = distanza */
            int dx = (tx - v->gx) >> v->xeig, dy = (ty - v->gy) >> v->yeig;
            double r = sqrt((double)dx * dx + (double)dy * dy);
            int ry = (int)(r * (1 << v->xeig) / (1 << v->yeig) + 0.5);
            ellipse_px(v, px0, py0, (int)(r + 0.5), ry, c, action, base == 152);
        } else if (base == 192 || base == 200) {
            /* ellisse: centro = vecchio, semiasse x dal penultimo, y dall'ultimo */
            int a = abs(px0 - pxo), b = abs(py1 - pyo);
            ellipse_px(v, pxo, pyo, a, b, c, action, base == 200);
        }
    }
    v->oox = v->ox; v->ooy = v->oy;
    v->ox = v->gx;  v->oy = v->gy;
    v->gx = tx;     v->gy = ty;
}

int vdu_point(Vdu *v, int x, int y, uint32_t *value)
{
    int px = os_to_px_x(v, x + v->orgx), py = os_to_px_y(v, y + v->orgy);
    if (px < v->gwl || px > v->gwr || py < v->gwt || py > v->gwb) return 0;
    *value = get_pixel(v, px, py);
    return 1;
}

/* ------------------------------------------------------------------ */
/* colori da BASIC                                                    */
/* ------------------------------------------------------------------ */

static void set_colour(Vdu *v, uint8_t n)
{
    int bg = n >= 128;
    if (bg) v->tbg = logical_colour(v, n & 127, v->tint_tb);
    else    v->tfg = logical_colour(v, n, v->tint_tf);
}

static void set_gcol(Vdu *v, uint8_t action, uint8_t n)
{
    if (n >= 128) { v->gbg = logical_colour(v, n & 127, v->tint_gb); v->gbg_action = action; }
    else          { v->gfg = logical_colour(v, n, v->tint_gf);       v->gfg_action = action; }
}

void vdu_set_text_rgb(Vdu *v, uint32_t rgb, int background)
{
    uint32_t p = rgb_to_pixel(v, rgb);
    if (background) v->tbg = p; else v->tfg = p;
}

void vdu_set_gcol_rgb(Vdu *v, uint32_t rgb, int background, int action)
{
    uint32_t p = rgb_to_pixel(v, rgb);
    if (background) { v->gbg = p; v->gbg_action = action; }
    else            { v->gfg = p; v->gfg_action = action; }
}

static void set_palette(Vdu *v, const uint8_t *q)
{
    int l = q[0], p = q[1];
    uint32_t rgb = (uint32_t)q[2] << 16 | (uint32_t)q[3] << 8 | q[4];
    if (p == 24) { v->border = rgb; return; }
    if (v->log2bpp > 3 || (uint32_t)l > v->ncolour) return;
    if (p == 16) {
        v->palette[l].first = v->palette[l].second = rgb;
    } else if (p < 16) {
        v->palette[l].first = bbc_colours[p & 7];
        v->palette[l].second = p >= 8 ? bbc_colours[7 - (p & 7)] : bbc_colours[p & 7];
    }
}

/* ------------------------------------------------------------------ */
/* VDU 23                                                             */
/* ------------------------------------------------------------------ */

static void vdu23(Vdu *v, const uint8_t *q)
{
    int n = q[0];
    if (n >= 32) { memcpy(v->font[n], q + 1, 8); return; }
    switch (n) {
    case 0:                                    /* registri del 6845 */
        if (q[1] == 10) v->cursor_on = (q[2] & 0x60) != 0x20;
        break;
    case 1:
        v->cursor_on = q[1] != 0;
        break;
    case 17:                                   /* TINT */
        switch (q[1]) {
        case 0: v->tint_tf = q[2]; break;
        case 1: v->tint_tb = q[2]; break;
        case 2: v->tint_gf = q[2]; break;
        case 3: v->tint_gb = q[2]; break;
        default: break;
        }
        if (v->log2bpp == 3 && q[1] <= 3) {
            int t = (q[2] >> 6) & 3;
            uint32_t *c = q[1] == 0 ? &v->tfg : q[1] == 1 ? &v->tbg : q[1] == 2 ? &v->gfg : &v->gbg;
            *c = (*c & ~3u) | (uint32_t)t;
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* flusso VDU                                                         */
/* ------------------------------------------------------------------ */

static const int8_t param_count[32] = {
    0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 2, 5, 0, 0, 1, 9, 8, 5, 0, 0, 4, 4, 0, 2
};

static int16_t s16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

static void clamp_graphics_window(Vdu *v)
{
    if (v->gwl < 0) v->gwl = 0;
    if (v->gwt < 0) v->gwt = 0;
    if (v->gwr >= v->width) v->gwr = v->width - 1;
    if (v->gwb >= v->height) v->gwb = v->height - 1;
}

static void execute(Vdu *v, int code, const uint8_t *q)
{
    switch (code) {
    case 4:  v->vdu5 = 0; break;
    case 5:  v->vdu5 = 1; break;
    case 7:  v->bell++; break;
    case 8:  cursor_left(v); break;
    case 9:  cursor_right(v); break;
    case 10: if (v->vdu5) v->gy -= 8 << v->yeig; else cursor_down(v); break;
    case 11: if (v->vdu5) v->gy += 8 << v->yeig; else cursor_up(v); break;
    case 12:
        if (v->vdu5) { execute(v, 16, q); break; }
        clear_text_window(v);
        break;
    case 13: if (v->vdu5) v->gx = v->gwl << v->xeig; else v->tx = 0; break;
    case 16:
        for (int y = v->gwt; y <= v->gwb; y++) hline(v, v->gwl, v->gwr, y, v->gbg, v->gbg_action);
        break;
    case 17: set_colour(v, q[0]); break;
    case 18: set_gcol(v, q[0], q[1]); break;
    case 19: set_palette(v, q); break;
    case 20: default_palette(v); default_colours(v); break;
    case 22: vdu_set_mode(v, q[0]); break;
    case 23: vdu23(v, q); break;
    case 24: {
        int l = os_to_px_x(v, s16(q) + v->orgx), b = os_to_px_y(v, s16(q + 2) + v->orgy);
        int r = os_to_px_x(v, s16(q + 4) + v->orgx), t = os_to_px_y(v, s16(q + 6) + v->orgy);
        if (l <= r && t <= b && l >= 0 && t >= 0 && r < v->width && b < v->height) {
            v->gwl = l; v->gwb = b; v->gwr = r; v->gwt = t;
            clamp_graphics_window(v);
        }
        break;
    }
    case 25: vdu_plot(v, q[0], s16(q + 1), s16(q + 3)); break;
    case 26: reset_windows(v); v->tx = v->ty = 0; break;
    case 28: {
        int l = q[0], b = q[1], r = q[2], t = q[3];
        if (l <= r && t <= b && r < v->cols && b < v->rows) {
            v->twl = l; v->twb = b; v->twr = r; v->twt = t;
            v->tx = v->ty = 0;
        }
        break;
    }
    case 29: v->orgx = s16(q); v->orgy = s16(q + 2); break;
    case 30:
        if (v->vdu5) { v->gx = v->gwl << v->xeig; v->gy = (v->height - 1 - v->gwt) << v->yeig; }
        else v->tx = v->ty = 0;
        break;
    case 31:
        if (q[0] < win_cols(v) && q[1] < win_rows(v)) { v->tx = q[0]; v->ty = q[1]; }
        break;
    case 127:
        cursor_left(v);
        if (v->vdu5) {
            int px = os_to_px_x(v, v->gx), py = os_to_px_y(v, v->gy);
            for (int r = 0; r < 8; r++) hline(v, px, px + 7, py + r, v->gbg, 0);
        } else {
            int px = (v->twl + v->tx) * 8, py = (v->twt + v->ty) * 8;
            fill_rect_px(v, px, py, px + 7, py + 7, v->tbg);
        }
        break;
    default:
        break;
    }
}

void vdu_write(Vdu *v, uint8_t ch)
{
    if (v->queue_need) {
        v->cost += COST_PARAM;
        v->queue[v->queue_have++] = ch;
        if (v->queue_have < v->queue_need) return;
        v->queue_need = 0;
        int code = v->queue[15];
        if (code >= 17 && code <= 20) v->cost += COST_COLOUR;
        if (!v->disabled) execute(v, code, v->queue);
        return;
    }
    if (ch < 32 || ch == 127) v->cost += COST_CONTROL;
    if (v->echo) v->echo(v->echo_ctx, ch);
    if (ch < 32) {
        if (ch == 6) { v->disabled = 0; return; }
        if (param_count[ch]) {
            v->queue_need = param_count[ch];
            v->queue_have = 0;
            v->queue[15] = ch;
            return;
        }
        if (ch == 21) { v->disabled = 1; return; }
        if (!v->disabled) execute(v, ch, v->queue);
        return;
    }
    if (v->disabled) return;
    if (ch == 127) { execute(v, 127, v->queue); return; }
    draw_char(v, ch);
}

/* ------------------------------------------------------------------ */
/* letture                                                            */
/* ------------------------------------------------------------------ */

int vdu_char_at(const Vdu *v, int col, int row)
{
    if (col < 0 || row < 0 || col >= v->cols || row >= v->rows) return 0;
    int px = col * 8, py = row * 8;
    uint32_t pix[64], seen[64];
    int nseen = 0;
    for (int i = 0; i < 64; i++) {
        pix[i] = get_pixel(v, px + (i & 7), py + (i >> 3));
        int k = 0;
        while (k < nseen && seen[k] != pix[i]) k++;
        if (k == nseen) seen[nseen++] = pix[i];
    }
    if (nseen == 1) return 32;
    /* prova ogni colore presente come inchiostro: vale per qualunque
       combinazione di primo piano e sfondo */
    for (int k = 0; k < nseen; k++) {
        uint8_t cell[8] = { 0 };
        for (int i = 0; i < 64; i++)
            if (pix[i] == seen[k]) cell[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
        for (int ch = 33; ch < 256; ch++)
            if (!memcmp(cell, v->font[ch], 8)) return ch;
    }
    return 0;
}

int vdu_char_at_cursor(Vdu *v)
{
    return vdu_char_at(v, v->twl + v->tx, v->twt + v->ty);
}

int32_t vdu_read_variable(Vdu *v, int var)
{
    switch (var) {
    case 0:   return 0;                                  /* ModeFlags */
    case 1:   return v->cols - 1;                        /* ScrRCol */
    case 2:   return v->rows - 1;                        /* ScrBRow */
    case 3:   return (int32_t)(v->log2bpp == 3 ? 63 : v->ncolour);  /* NColour */
    case 4:   return v->xeig;
    case 5:   return v->yeig;
    case 6:   return v->line_bytes;
    case 7:   return v->line_bytes * v->height;           /* ScreenSize */
    case 8:   return 0;                                  /* YShftFactor */
    case 9:   return v->log2bpp;
    case 10:  return v->log2bpp;                         /* Log2BPC */
    case 11:  return v->width - 1;                       /* XWindLimit */
    case 12:  return v->height - 1;                      /* YWindLimit */
    case 0x80: return v->gwl;
    case 0x81: return v->height - 1 - v->gwb;
    case 0x82: return v->gwr;
    case 0x83: return v->height - 1 - v->gwt;
    case 0x84: return v->twl;
    case 0x85: return v->twb;
    case 0x86: return v->twr;
    case 0x87: return v->twt;
    case 0x88: return v->orgx;
    case 0x89: return v->orgy;
    case 0x8A: return v->gx - v->orgx;
    case 0x8B: return v->gy - v->orgy;
    case 0x8C: return v->oox - v->orgx;
    case 0x8D: return v->ooy - v->orgy;
    case 0x8E: return v->ox - v->orgx;
    case 0x8F: return v->oy - v->orgy;
    case 0x90: return v->gx >> v->xeig;
    case 0x91: return v->gy >> v->yeig;
    case 0x94: case 0x95: return (int32_t)v->screen_addr;
    case 0x96: return (int32_t)VDU_MAX_BYTES;
    case 0x97: return v->gfg_action;
    case 0x98: return v->gbg_action;
    case 0x99: return (int32_t)v->gfg;
    case 0x9A: return (int32_t)v->gbg;
    case 0x9B: return (int32_t)v->tfg;
    case 0x9C: return (int32_t)v->tbg;
    case 0x9D: return v->tint_gf;
    case 0x9E: return v->tint_gb;
    case 0x9F: return v->tint_tf;
    case 0xA0: return v->tint_tb;
    case 0xA1: return 53;                                /* MaxMode */
    case 0xA2: case 0xA3: case 0xA4: case 0xA5: return 8;
    case 0xA7: case 0xA8: case 0xA9: case 0xAA: return 8;
    case 0xC0: return 1;                                 /* WindowWidth... */
    default:   return 0;
    }
}

/* ------------------------------------------------------------------ */
/* conversione per il frontend                                        */
/* ------------------------------------------------------------------ */

void vdu_render(const Vdu *v, uint32_t *out, int flash_phase, int cursor_visible)
{
    for (int y = 0; y < v->height; y++)
        for (int x = 0; x < v->width; x++)
            out[(size_t)y * v->width + x] = pixel_to_rgb(v, get_pixel(v, x, y), flash_phase);

    if (cursor_visible && v->cursor_on && !v->vdu5) {
        int px = (v->twl + v->tx) * 8, py = (v->twt + v->ty) * 8;
        uint32_t rgb = pixel_to_rgb(v, v->tfg, 0);
        int first = v->yeig == 2 ? 7 : 6;              /* stesso spessore a schermo */
        for (int r = first; r < 8; r++)
            for (int c = 0; c < 8; c++)
                if (px + c < v->width && py + r < v->height)
                    out[(size_t)(py + r) * v->width + px + c] = rgb;
    }
}
