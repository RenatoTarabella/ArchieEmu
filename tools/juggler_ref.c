/*
 * juggler_ref.c - Riferimento sul PC per il Juggler di Eric Graham (1987).
 *
 * Il motore e' rt1.c di Eric Graham tradotto fedelmente (stesse funzioni,
 * stesso ordine delle operazioni); la scena si legge dai file .dat di ssg
 * (robot.dat, ele.dat, dragon.dat) con la grammatica ricavata dal binario.
 * Ray tracer originale: Copyright 1987 Eric Graham, rilasciato da lui nel
 * 2026 a chiunque, a patto di citarlo.
 *
 * L'uscita e' il dump a 8 bit di ssg (opzione D=): per ogni componente
 * (int)(128*brite+4) limitato a 0..255; i 4 bit dell'Amiga sono byte/8
 * (= 16*brite+0.5 di ham() in rt2.c). Si confronta con robot.rgb, catturato
 * dal binario originale.
 *
 * Compilato due volte: juggler_ref (double, come rt1.c) e juggler_ref_f
 * (float, come ssg che usava mathffp).
 *
 *   juggler_ref robot.dat [--skip n] [--reflect archived|correct]
 *               [--rgb out.rgb] [--png out.png] [--cmp robot.rgb]
 *               [--diff diff.png]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include "png.h"

#ifdef JUGGLER_FLOAT
typedef float real;
#define SQRT(x) ((real)sqrt((double)(x)))
#define VARIANT "float"
#else
typedef double real;
#define SQRT(x) sqrt(x)
#define VARIANT "double"
#endif

#define BIG 1.0e10
#define SMALL 1.0e-3
#define DULL    0
#define BRIGHT  1
#define MIRROR  2

struct lamp { real pos[3], color[3], radius; };
struct sphere { real pos[3], color[3], radius; int type; };
struct patch { real pos[3], normal[3], color[3]; };
struct world {
    int numsp; struct sphere *sp;
    int numlmp; struct lamp *lmp;
    struct patch horizon[2];
    real illum[3], skyhor[3], skyzen[3];
};
struct observer {
    real obspos[3], viewdir[3], uhat[3], vhat[3];
    real fl, px, py;
    int nx, ny;
};

static int reflect_archived;    /* 1 = reflect() come nel listato di rt1.c */

static int raytrace(real brite[3], real *line, struct world *w);

/* ---------------- rt1.c ---------------- */

static void vecsub(real *a, real *b, real *c)
{
    for (int k = 0; k < 3; ++k) a[k] = b[k] - c[k];
}

static real dot(real *a, real *b)
{
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

static void genline(real *l, real *a, real *b)
{
    for (int k = 0; k < 3; ++k) { *l++ = a[k]; *l++ = b[k] - a[k]; }
}

static void point(real *pos, real t, real *line)
{
    real a;
    for (int k = 0; k < 3; ++k) { a = *line++; pos[k] = a + (*line++)*t; }
}

static void vecprod(real *a, real *b, real *c)
{
    a[0] = b[1]*c[2] - b[2]*c[1];
    a[1] = b[2]*c[0] - b[0]*c[2];
    a[2] = b[0]*c[1] - b[1]*c[0];
}

static int veczero(real *v)
{
    return v[0] == 0.0 && v[1] == 0.0 && v[2] == 0.0;
}

static void reflect(real *y, real *n, real *x)
{
    real u[3], v[3], vv, xn, xv;
    vecprod(u, x, n);
    if (veczero(u)) { y[0] = -x[0]; y[1] = -x[1]; y[2] = -x[2]; return; }
    if (reflect_archived) {
        /* il listato: y = xv*v/(xn*n), matematicamente sbagliato */
        vecprod(v, u, n);
        vv = dot(v, v); xv = dot(x, v)/vv; xn = dot(x, n);
        for (int k = 0; k < 3; ++k) y[k] = xv*v[k]/(xn*n[k]);
    } else {
        /* corretto (e' quello del binario ssg): y = x - 2(x.n)n */
        xn = dot(x, n);
        for (int k = 0; k < 3; ++k) y[k] = x[k] - 2.0*xn*n[k];
    }
}

static long n_intsplin;         /* test raggio-sfera, per stimare il tempo sull'ARM */

static int intsplin(real *t, real *line, real *pos, real radius)
{
    real a, b, c, d, p, q, tt;
    n_intsplin++;
    a = b = 0.0; c = radius; c = -c*c;
    for (int k = 0; k < 3; ++k) {
        p = (*line++) - pos[k]; q = *line++;
        a = q*q + a; tt = q*p; b = tt + tt + b; c = p*p + c;
    }
    d = b*b - 4.0*a*c;
    if (d <= 0) return 0;
    d = SQRT(d); *t = -(b + d)/(a + a);
    if (*t < SMALL) *t = (d - b)/(a + a);
    return *t > SMALL;
}

static int inthor(real *t, real *line)
{
    if (line[5] == 0.0) return 0;
    *t = -line[4]/line[5]; return *t > SMALL;
}

static void setnorm(struct patch *p, struct sphere *s)
{
    real a;
    vecsub(p->normal, p->pos, s->pos); a = 1.0/s->radius;
    for (int k = 0; k < 3; ++k) p->normal[k] = p->normal[k]*a;
}

static int gingham(real *pos)
{
    real x, y; int kx, ky;
    kx = ky = 0; x = pos[0]; y = pos[1];
    if (x < 0) { x = -x; ++kx; }
    if (y < 0.0) { y = -y; ++ky; }
    return ((((int)x) + kx)/3 + (((int)y) + ky)/3) % 2;
}

static void skybrite(real brite[3], real *line, struct world *w)
{
    real sin2, cos2;
    sin2 = line[5]*line[5];
    sin2 /= (line[1]*line[1] + line[3]*line[3] + sin2);
    cos2 = 1.0 - sin2;
    for (int k = 0; k < 3; ++k) brite[k] = cos2*w->skyhor[k] + sin2*w->skyzen[k];
}

/* la lampada l e' nascosta da una sfera (diversa da spc)? */
static int shadowed(real *line, struct world *w, struct sphere *spc)
{
    real t;
    for (int k = 0; k < w->numsp; ++k) {
        if (w->sp + k == spc) continue;
        if (intsplin(&t, line, w->sp[k].pos, w->sp[k].radius)) return 1;
    }
    return 0;
}

static void pixbrite(real brite[3], struct patch *p, struct world *w, struct sphere *spc)
{
    real line[6], r, lp[3], *pp, *ll, cosi, diffuse;
    static real zenith[3] = {0.0, 0.0, 1.0}, f1 = 1.5, f2 = 0.4;
    diffuse = (dot(zenith, p->normal) + f1)*f2;
    for (int k = 0; k < 3; ++k) brite[k] = diffuse*w->illum[k]*p->color[k];
    for (int l = 0; l < w->numlmp; ++l) {
        ll = w->lmp[l].pos; pp = p->pos; vecsub(lp, ll, pp);
        cosi = dot(lp, p->normal); if (cosi <= 0.0) continue;
        genline(line, pp, ll);
        if (shadowed(line, w, spc)) continue;
        r = SQRT(dot(lp, lp)); cosi = cosi/(r*r*r);
        for (int k = 0; k < 3; ++k)
            brite[k] = brite[k] + cosi*p->color[k]*w->lmp[l].color[k];
    }
}

static int glint(real brite[3], struct patch *p, struct world *w,
                 struct sphere *spc, real *incident)
{
    int firstlite = 1; static real minglint = 0.95;
    real line[6], t, lp[3], *pp, *ll, cosi, incvec[3], refvec[3], ref2 = 0;
    for (int l = 0; l < w->numlmp; ++l) {
        ll = w->lmp[l].pos; pp = p->pos;
        vecsub(lp, ll, pp); cosi = dot(lp, p->normal);
        if (cosi <= 0.0) continue;
        genline(line, pp, ll);
        if (shadowed(line, w, spc)) continue;
        if (firstlite) {
            incvec[0] = incident[1]; incvec[1] = incident[3]; incvec[2] = incident[5];
            reflect(refvec, p->normal, incvec);
            ref2 = dot(refvec, refvec); firstlite = 0;
        }
        t = dot(lp, refvec);
        t *= t/(dot(lp, lp)*ref2);
        if (t > minglint) {
            for (int k = 0; k < 3; ++k) brite[k] = 1.0;
            return 1;
        }
    }
    return 0;
}

static int depth_mirror;        /* profondita' massima raggiunta dagli specchi */

static void mirror(real brite[3], struct patch *p, struct world *w, real *incident)
{
    static int depth;
    real line[6], incvec[3], refvec[3], t;
    incvec[0] = incident[1]; incvec[1] = incident[3]; incvec[2] = incident[5];
    t = dot(p->normal, incvec);
    if (t >= 0) { for (int k = 0; k < 3; ++k) brite[k] = 0.0; return; }
    reflect(refvec, p->normal, incvec);
    line[0] = p->pos[0]; line[2] = p->pos[1]; line[4] = p->pos[2];
    line[1] = refvec[0]; line[3] = refvec[1]; line[5] = refvec[2];
    if (++depth > depth_mirror) depth_mirror = depth;
    if (depth > 1000) { brite[0] = brite[1] = brite[2] = 0; }   /* solo sicurezza */
    else raytrace(brite, line, w);
    --depth;
    for (int k = 0; k < 3; ++k) brite[k] = brite[k]*p->color[k];
}

static int raytrace(real brite[3], real *line, struct world *w)
{
    real t, tmin, pos[3]; int k;
    struct patch ptch; struct sphere *spnear; struct lamp *lmpnear;

    tmin = BIG; spnear = 0;
    for (k = 0; k < w->numsp; ++k)
        if (intsplin(&t, line, w->sp[k].pos, w->sp[k].radius))
            if (t < tmin) { tmin = t; spnear = w->sp + k; }
    lmpnear = 0;
    for (k = 0; k < w->numlmp; ++k)
        if (intsplin(&t, line, w->lmp[k].pos, w->lmp[k].radius))
            if (t < tmin) { tmin = t; lmpnear = w->lmp + k; }
    if (lmpnear) {
        for (k = 0; k < 3; ++k)
            brite[k] = lmpnear->color[k]/(lmpnear->radius*lmpnear->radius);
        return 0;
    }
    if (inthor(&t, line))
        if (t < tmin) {
            point(pos, t, line); k = gingham(pos);
            memcpy(w->horizon[k].pos, pos, sizeof pos);
            pixbrite(brite, &w->horizon[k], w, 0);
            return 0;
        }
    if (spnear) {
        point(ptch.pos, tmin, line); setnorm(&ptch, spnear);
        memcpy(ptch.color, spnear->color, sizeof ptch.color);
        switch (spnear->type) {
        case BRIGHT:
            if (glint(brite, &ptch, w, spnear, line)) return 0;
            /* fallthrough */
        case DULL:
            pixbrite(brite, &ptch, w, spnear); return 0;
        case MIRROR:
            mirror(brite, &ptch, w, line); return 0;
        }
        return 0;
    }
    skybrite(brite, line, w);
    return 0;
}

static void pixline(real *line, struct observer *o, int i, int j)
{
    real x, y, tp[3];
    y = (0.5*o->ny - j)*o->py;
    x = (i - 0.5*o->nx)*o->px;
    for (int k = 0; k < 3; ++k)
        tp[k] = o->viewdir[k]*o->fl + y*o->vhat[k] + x*o->uhat[k] + o->obspos[k];
    genline(line, o->obspos, tp);
}

/* ---------------- setup (rt2.c) e lettura del .dat (ssg) ---------------- */

static const char *src;         /* testo del .dat */

static void fail(const char *what)
{
    fprintf(stderr, "errore nel .dat: atteso %s prima di: %.40s\n", what, src);
    exit(1);
}

static void skipws(void) { while (*src && isspace((unsigned char)*src)) src++; }

static void expect(char c)
{
    char what[4] = { '\'', c, '\'', 0 };
    skipws();
    if (*src != c) fail(what);
    src++;
}

static int peek(void) { skipws(); return *src; }

static real number(void)
{
    char *end;
    skipws();
    double v = strtod(src, &end);
    if (end == src) fail("un numero");
    src = end;
    return (real)v;
}

static void triple(char open, char close, real v[3])
{
    expect(open); v[0] = number();
    expect(','); v[1] = number();
    expect(','); v[2] = number();
    expect(close);
}

static void sphpoint(real pos[3], real *r)
{
    triple('(', ')', pos); expect(':'); *r = number();
}

static void addsphere(struct world *w, int *cap, struct sphere *s)
{
    if (w->numsp == *cap) {
        *cap = *cap ? *cap*2 : 64;
        w->sp = realloc(w->sp, *cap*sizeof *w->sp);
    }
    w->sp[w->numsp++] = *s;
}

static void readdat(const char *path, struct observer *o, struct world *w)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "non trovo %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    fread(buf, 1, n, f); buf[n] = 0; fclose(f);
    src = buf;

    real alt, az, fl;
    triple('(', ')', o->obspos);
    expect('['); alt = number(); expect(','); az = number(); expect(']');
    fl = number();

    /* osservatore come in rt2.c (degtorad, px, py), focale 0.028*fl */
    real degtorad = 0.0174533;
    o->nx = 320; o->ny = 200;
    o->px = 1.0/o->nx; o->py = 0.75/o->ny;
    o->fl = 0.028*fl;
    alt *= degtorad; az *= degtorad;
    o->viewdir[0] = cos(az)*cos(alt);
    o->viewdir[1] = sin(az)*cos(alt);
    o->viewdir[2] = sin(alt);
    o->uhat[0] = sin(az); o->uhat[1] = -cos(az); o->uhat[2] = 0.0;
    o->vhat[0] = -cos(az)*sin(alt);
    o->vhat[1] = -sin(az)*sin(alt);
    o->vhat[2] = cos(alt);

    /* oggetti: <r,g,b> tipo (x,y,z):r [n (x,y,z):r]... ;   lista chiusa da ; */
    memset(w, 0, sizeof *w);
    int cap = 0;
    while (peek() != ';') {
        struct sphere s; real lp[3], lr, np[3], nr;
        triple('<', '>', s.color);
        s.type = (int)number();
        sphpoint(lp, &lr);
        while (peek() != ';') {
            int count = (int)number();
            sphpoint(np, &nr);
            /* come il binario: count+1 sfere da lp (incluso) a np (escluso) */
            for (int a = 0; a <= count; ++a) {
                real t = (real)a/(real)(count + 1);
                for (int k = 0; k < 3; ++k) s.pos[k] = lp[k] + (np[k] - lp[k])*t;
                s.radius = lr + (nr - lr)*t;
                addsphere(w, &cap, &s);
            }
            memcpy(lp, np, sizeof lp); lr = nr;
        }
        memcpy(s.pos, lp, sizeof lp); s.radius = lr;
        addsphere(w, &cap, &s);
        expect(';');
    }
    expect(';');

    w->numlmp = (int)number();
    w->lmp = calloc(w->numlmp ? w->numlmp : 1, sizeof *w->lmp);
    for (int i = 0; i < w->numlmp; i++) {
        sphpoint(w->lmp[i].pos, &w->lmp[i].radius);
        triple('<', '>', w->lmp[i].color);
    }
    triple('<', '>', w->horizon[0].color);
    triple('<', '>', w->horizon[1].color);
    triple('<', '>', w->illum);
    triple('<', '>', w->skyzen);
    triple('<', '>', w->skyhor);
    for (int i = 0; i < 2; i++) {
        w->horizon[i].normal[0] = w->horizon[i].normal[1] = 0.0;
        w->horizon[i].normal[2] = 1.0;
    }
    free(buf);

    /* lampfac di rt2.c: l'esposizione delle lampade */
    real lampfac = BIG, t, r, tp[3];
    for (int i = 0; i < w->numsp; ++i)
        for (int j = 0; j < w->numlmp; ++j) {
            vecsub(tp, w->sp[i].pos, w->lmp[j].pos);
            r = SQRT(dot(tp, tp));
            r -= w->sp[i].radius;
            for (int k = 0; k < 3; ++k) {
                t = w->sp[i].color[k]*w->lmp[j].color[k]/(r*r);
                if (t == 0.0) continue;
                t = (1.0 - w->sp[i].color[k]*w->illum[k])/t;
                if (t < lampfac) lampfac = t;
            }
        }
    for (int j = 0; j < w->numlmp; ++j)
        for (int k = 0; k < 3; ++k) w->lmp[j].color[k] *= lampfac;
    printf("sfere %d, lampade %d, lampfac %.6f\n", w->numsp, w->numlmp, (double)lampfac);
}

/* ---------------- main ---------------- */

static uint32_t to_rgb32(const uint8_t *p)
{
    /* il byte vale 128*brite+4: lo schermo ha brite 1 = bianco */
    uint32_t c = 0;
    for (int k = 0; k < 3; k++) {
        int v = p[k]*2;
        c = c << 8 | (v > 255 ? 255 : v);
    }
    return c;
}

int main(int argc, char **argv)
{
    const char *dat = 0, *rgbout = 0, *pngout = 0, *cmp = 0, *diffout = 0;
    int skip = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--skip") && i + 1 < argc) skip = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--reflect") && i + 1 < argc)
            reflect_archived = !strcmp(argv[++i], "archived");
        else if (!strcmp(argv[i], "--rgb") && i + 1 < argc) rgbout = argv[++i];
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) pngout = argv[++i];
        else if (!strcmp(argv[i], "--cmp") && i + 1 < argc) cmp = argv[++i];
        else if (!strcmp(argv[i], "--diff") && i + 1 < argc) diffout = argv[++i];
        else if (argv[i][0] != '-') dat = argv[i];
        else { fprintf(stderr, "opzione sconosciuta %s\n", argv[i]); return 1; }
    }
    if (!dat || skip < 1) {
        fprintf(stderr, "uso: juggler_ref scena.dat [--skip n] [--reflect archived|correct]\n"
                        "       [--rgb out.rgb] [--png out.png] [--cmp ref.rgb] [--diff diff.png]\n");
        return 1;
    }

    struct observer o; struct world w;
    readdat(dat, &o, &w);
    printf("motore %s, reflect %s\n", VARIANT, reflect_archived ? "come archiviato" : "corretto");

    int ow = 1 + (o.nx - 1)/skip, oh = 1 + (o.ny - 1)/skip;
    uint8_t *img = malloc((size_t)ow*oh*3);
    real line[6], brite[3];
    for (int j = 0, jj = 0; j < o.ny; j += skip, jj++)
        for (int i = 0, ii = 0; i < o.nx; i += skip, ii++) {
            pixline(line, &o, i, j);
            raytrace(brite, line, &w);
            for (int k = 0; k < 3; k++) {
                int v = (int)(128.0*brite[k] + 4.0);
                img[((size_t)jj*ow + ii)*3 + k] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
        }
    printf("profondita' massima degli specchi %d, %ld test raggio-sfera\n",
           depth_mirror, n_intsplin);

    if (rgbout) {
        FILE *f = fopen(rgbout, "wb");
        if (!f) { fprintf(stderr, "non posso scrivere %s\n", rgbout); return 1; }
        fwrite(img, 1, (size_t)ow*oh*3, f); fclose(f);
    }
    uint32_t *pix = malloc((size_t)ow*oh*4);
    if (pngout) {
        for (int p = 0; p < ow*oh; p++) pix[p] = to_rgb32(img + p*3);
        png_write(pngout, pix, ow, oh, 1);
    }
    if (cmp) {
        size_t n = (size_t)ow*oh*3;
        uint8_t *ref = malloc(n);
        FILE *f = fopen(cmp, "rb");
        if (!f || fread(ref, 1, n, f) != n) { fprintf(stderr, "%s: dimensione diversa\n", cmp); return 1; }
        fclose(f);
        long bytes = 0, bytes4 = 0, pixels = 0, maxd = 0; double sum = 0;
        for (int p = 0; p < ow*oh; p++) {
            int dp = 0, m = 0;
            for (int k = 0; k < 3; k++) {
                int a = img[p*3 + k], b = ref[p*3 + k], d = abs(a - b);
                if (d) bytes++, dp = 1;
                if (a/8 != b/8) bytes4++;
                if (d > m) m = d;
                if (d > maxd) maxd = d;
                sum += d;
            }
            pixels += dp;
            /* immagine delle differenze: il riferimento in grigio scuro */
            uint32_t g = to_rgb32(ref + p*3);
            int y = (((g >> 16) & 255) + ((g >> 8) & 255) + (g & 255))/12;
            /* giallo fino a 3, arancio fino a 15, rosso oltre */
            pix[p] = !m ? (uint32_t)y*0x010101u
                   : m > 15 ? 0xFF0000u : m > 3 ? 0xFF8000u : 0xFFFF00u;
        }
        printf("confronto con %s: %ld byte diversi su %zu (%ld nei 4 bit), %ld pixel, "
               "max %ld, media %.5f\n", cmp, bytes, n, bytes4, pixels, maxd, sum/n);
        if (diffout) png_write(diffout, pix, ow, oh, 1);
    }
    return 0;
}
