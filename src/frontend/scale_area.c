/* Ingrandimento ad area (vedi scale_area.h). Separabile: per ogni asse ogni
   punto d'uscita copre al massimo due pixel sorgente, con un peso 0..256 sul
   primo; le tabelle si ricalcolano solo quando cambiano le dimensioni. */
#include "scale_area.h"
#include <stdlib.h>

typedef struct { int i; int wt; } Tap;

static Tap      *tx, *ty;
static uint32_t *out, *row;
static int       cur_w, cur_h, cur_sw, cur_sh;

static void make_taps(Tap *t, int n, int sn)
{
    for (int x = 0; x < sn; x++) {
        double a = (double)x * n / sn, b = (double)(x + 1) * n / sn;
        int i = (int)a;
        if (i >= n) i = n - 1;
        if (b <= i + 1 || i + 1 >= n) {
            t[x].i = i;
            t[x].wt = 256;
        } else {
            t[x].i = i;
            t[x].wt = (int)(256.0 * (i + 1 - a) / (b - a) + 0.5);
        }
    }
}

/* a*wt + b*(256-wt) per canale, due canali alla volta */
static inline uint32_t blend(uint32_t a, uint32_t b, uint32_t wt)
{
    uint32_t iw = 256 - wt;
    uint32_t rb = ((a & 0xFF00FF) * wt + (b & 0xFF00FF) * iw) >> 8;
    uint32_t g  = ((a & 0x00FF00) * wt + (b & 0x00FF00) * iw) >> 8;
    return (rb & 0xFF00FF) | (g & 0x00FF00);
}

const uint32_t *scale_area(const uint32_t *src, int src_stride, int w, int h, int sw, int sh)
{
    if (w <= 0 || h <= 0 || sw <= 0 || sh <= 0) return NULL;
    if (w != cur_w || h != cur_h || sw != cur_sw || sh != cur_sh) {
        free(tx); free(ty); free(out); free(row);
        tx = malloc(sizeof *tx * (size_t)sw);
        ty = malloc(sizeof *ty * (size_t)sh);
        out = malloc(sizeof *out * (size_t)sw * (size_t)sh);
        row = malloc(sizeof *row * (size_t)w);
        if (!tx || !ty || !out || !row) {
            free(tx); free(ty); free(out); free(row);
            tx = ty = NULL; out = row = NULL;
            cur_w = cur_h = cur_sw = cur_sh = 0;
            return NULL;
        }
        make_taps(tx, w, sw);
        make_taps(ty, h, sh);
        cur_w = w; cur_h = h; cur_sw = sw; cur_sh = sh;
    }

    for (int y = 0; y < sh; y++) {
        const uint32_t *r0 = src + (size_t)ty[y].i * src_stride, *line = r0;
        if (ty[y].wt != 256) {
            const uint32_t *r1 = r0 + src_stride;
            uint32_t wt = (uint32_t)ty[y].wt;
            for (int x = 0; x < w; x++) row[x] = blend(r0[x], r1[x], wt);
            line = row;
        }
        uint32_t *d = out + (size_t)y * sw;
        for (int x = 0; x < sw; x++) {
            const Tap *t = &tx[x];
            d[x] = t->wt == 256 ? line[t->i] : blend(line[t->i], line[t->i + 1], (uint32_t)t->wt);
        }
    }
    return out;
}
