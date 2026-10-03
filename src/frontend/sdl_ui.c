/*
 * sdl_ui.c - Interfaccia minima per i front end SDL2 (vedi sdl_ui.h)
 */
#include "sdl_ui.h"
#include "riscos/font.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static SDL_Window   *win;
static SDL_Renderer *ren;
static SDL_Texture  *tex;
static UiCanvas      canvas;
static void        (*backdrop)(void *);
static void         *backdrop_ctx;

void ui_init(SDL_Window *w, SDL_Renderer *r)
{
    win = w;
    ren = r;
}

void ui_quit(void)
{
    if (tex) SDL_DestroyTexture(tex);
    tex = NULL;
    free(canvas.px);
    canvas.px = NULL;
    canvas.w = canvas.h = 0;
}

void ui_set_backdrop(void (*draw)(void *ctx), void *ctx)
{
    backdrop = draw;
    backdrop_ctx = ctx;
}

UiCanvas *ui_begin(void)
{
    int w, h;
    SDL_GetRendererOutputSize(ren, &w, &h);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w != canvas.w || h != canvas.h || !tex) {
        if (tex) SDL_DestroyTexture(tex);
        free(canvas.px);
        canvas.px = calloc((size_t)w * (size_t)h, 4);
        canvas.w = w;
        canvas.h = h;
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    }
    if (canvas.px) memset(canvas.px, 0, (size_t)canvas.w * (size_t)canvas.h * 4);
    return &canvas;
}

void ui_end(void)
{
    if (!tex || !canvas.px) return;
    SDL_UpdateTexture(tex, NULL, canvas.px, canvas.w * 4);
    SDL_RenderCopy(ren, tex, NULL, NULL);
}

/* ------------------------------------------------------------------ */
/* disegno                                                            */
/* ------------------------------------------------------------------ */

int ui_scale(void) { return 2; }
int ui_char_w(void) { return 8 * ui_scale(); }
int ui_line_h(void) { return 8 * ui_scale() + 10; }
int ui_text_w(const char *s) { return (int)strlen(s) * ui_char_w(); }

void ui_fill(UiCanvas *c, int x, int y, int w, int h, uint32_t argb)
{
    if (!c->px) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;
    for (int j = y0; j < y1; j++) {
        uint32_t *p = c->px + (size_t)j * c->w;
        for (int i = x0; i < x1; i++) p[i] = argb;
    }
}

void ui_frame(UiCanvas *c, int x, int y, int w, int h, uint32_t argb)
{
    ui_fill(c, x, y, w, 1, argb);
    ui_fill(c, x, y + h - 1, w, 1, argb);
    ui_fill(c, x, y, 1, h, argb);
    ui_fill(c, x + w - 1, y, 1, h, argb);
}

void ui_text(UiCanvas *c, int x, int y, const char *s, uint32_t argb)
{
    int k = ui_scale();
    for (; *s; s++, x += 8 * k) {
        unsigned ch = (unsigned char)*s;
        if (ch < 32) continue;
        const unsigned char *g = riscos_font[ch - 32];
        for (int r = 0; r < 8; r++)
            for (int b = 0; b < 8; b++)
                if (g[r] & (0x80 >> b)) ui_fill(c, x + b * k, y + r * k, k, k, argb);
    }
}

/* ------------------------------------------------------------------ */
/* finestre modali                                                    */
/* ------------------------------------------------------------------ */

typedef void (*PaintFn)(UiCanvas *c, void *ctx);

static void present(PaintFn paint, void *ctx)
{
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    if (backdrop) backdrop(backdrop_ctx);
    UiCanvas *c = ui_begin();
    paint(c, ctx);
    ui_end();
    SDL_RenderPresent(ren);
}

static int inside(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

static void bevel_button(UiCanvas *c, int x, int y, int w, int h, const char *label, int def)
{
    ui_fill(c, x, y, w, h, UI_WHITE);
    ui_frame(c, x, y, w, h, def ? UI_HILITE : UI_GREY);
    if (def) ui_frame(c, x + 1, y + 1, w - 2, h - 2, UI_HILITE);
    ui_text(c, x + (w - ui_text_w(label)) / 2, y + (h - 8 * ui_scale()) / 2, label, UI_TEXT);
}

/* --- menu a comparsa ---------------------------------------------- */

typedef struct Menu {
    const UiItem *items;
    int n, x, y, w, sel;
} Menu;

static int item_h(const UiItem *it) { return (it->flags & UI_SEPARATOR) ? 7 : ui_line_h(); }

static void menu_paint(UiCanvas *c, void *ctx)
{
    Menu *m = ctx;
    int h = 4;
    for (int i = 0; i < m->n; i++) h += item_h(&m->items[i]);
    ui_fill(c, m->x + 4, m->y + 4, m->w, h, 0x50000000);        /* ombra */
    ui_fill(c, m->x, m->y, m->w, h, UI_WHITE);
    ui_frame(c, m->x, m->y, m->w, h, UI_TEXT);
    int y = m->y + 2;
    for (int i = 0; i < m->n; i++) {
        const UiItem *it = &m->items[i];
        int ih = item_h(it);
        if (it->flags & UI_SEPARATOR) {
            ui_fill(c, m->x + 6, y + 3, m->w - 12, 1, UI_GREY);
        } else {
            int hi = i == m->sel && !(it->flags & UI_DISABLED);
            if (hi) ui_fill(c, m->x + 2, y, m->w - 4, ih, UI_HILITE);
            uint32_t col = (it->flags & UI_DISABLED) ? UI_GREY : hi ? UI_WHITE : UI_TEXT;
            int ty = y + (ih - 8 * ui_scale()) / 2;
            if (it->flags & UI_CHECKED) {
                int s = ui_scale();
                ui_fill(c, m->x + 8, ty + 2 * s, 4 * s, 4 * s, col);
            }
            ui_text(c, m->x + 8 + ui_char_w() + 4, ty, it->label, col);
        }
        y += ih;
    }
}

static int menu_hit(const Menu *m, int mx, int my)
{
    if (mx < m->x || mx >= m->x + m->w) return -1;
    int y = m->y + 2;
    for (int i = 0; i < m->n; i++) {
        int ih = item_h(&m->items[i]);
        if (my >= y && my < y + ih)
            return (m->items[i].flags & (UI_SEPARATOR | UI_DISABLED)) ? -1 : i;
        y += ih;
    }
    return -1;
}

int ui_menu(const UiItem *items, int n, int x, int y)
{
    Menu m = { items, n, x, y, 0, -1 };
    int h = 4;
    for (int i = 0; i < n; i++) {
        int w = ui_text_w(items[i].label) + ui_char_w() + 28;
        if (w > m.w) m.w = w;
        h += item_h(&items[i]);
    }
    int ww, wh;
    SDL_GetRendererOutputSize(ren, &ww, &wh);
    if (m.x + m.w > ww) m.x = ww - m.w;
    if (m.y + h > wh) m.y = wh - h;
    if (m.x < 0) m.x = 0;
    if (m.y < 0) m.y = 0;

    int moved = 0;
    for (;;) {
        present(menu_paint, &m);
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 50)) continue;
        switch (e.type) {
        case SDL_QUIT:
            SDL_PushEvent(&e);
            return -1;
        case SDL_MOUSEMOTION:
            m.sel = menu_hit(&m, e.motion.x, e.motion.y);
            if (m.sel >= 0) moved = 1;
            break;
        case SDL_MOUSEBUTTONDOWN: {
            int hit = menu_hit(&m, e.button.x, e.button.y);
            if (hit >= 0) return hit;
            if (!inside(e.button.x, e.button.y, m.x, m.y, m.w, h)) return -1;
            break;
        }
        case SDL_MOUSEBUTTONUP: {
            /* premuto sulla barra e rilasciato su una voce */
            int hit = menu_hit(&m, e.button.x, e.button.y);
            if (hit >= 0 && moved) return hit;
            break;
        }
        case SDL_KEYDOWN:
            switch (e.key.keysym.sym) {
            case SDLK_ESCAPE: return -1;
            case SDLK_RETURN: case SDLK_KP_ENTER: if (m.sel >= 0) return m.sel; break;
            case SDLK_DOWN: case SDLK_UP: {
                int d = e.key.keysym.sym == SDLK_DOWN ? 1 : -1;
                for (int k = 0; k < n; k++) {
                    m.sel = m.sel < 0 ? (d > 0 ? 0 : n - 1) : (m.sel + d + n) % n;
                    if (!(items[m.sel].flags & (UI_SEPARATOR | UI_DISABLED))) break;
                }
                break;
            }
            default: break;
            }
            break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) return -1;
            break;
        default: break;
        }
    }
}

/* --- messaggi ----------------------------------------------------- */

#define MSG_LINES 24

typedef struct Msg {
    const char *title;
    char  lines[MSG_LINES][100];
    int   nlines, cancel;
    const char *ok;
    int   x, y, w, h, bw, bh, ok_x, cancel_x, by;
} Msg;

static void wrap(Msg *m, const char *text, int cols)
{
    m->nlines = 0;
    const char *p = text;
    while (*p && m->nlines < MSG_LINES) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        if (len > cols) {
            int cut = cols;
            while (cut > 0 && p[cut] != ' ') cut--;
            if (cut == 0) cut = cols;
            len = cut;
        }
        snprintf(m->lines[m->nlines++], sizeof m->lines[0], "%.*s", len, p);
        p += len;
        while (*p == ' ') p++;
        if (*p == '\n') p++;
    }
}

static void msg_paint(UiCanvas *c, void *ctx)
{
    Msg *m = ctx;
    ui_fill(c, 0, 0, c->w, c->h, UI_DIM);
    ui_fill(c, m->x, m->y, m->w, m->h, UI_BG);
    ui_frame(c, m->x, m->y, m->w, m->h, UI_TEXT);
    ui_fill(c, m->x + 1, m->y + 1, m->w - 2, ui_line_h(), UI_TITLE);
    ui_text(c, m->x + (m->w - ui_text_w(m->title)) / 2, m->y + 6, m->title, UI_TEXT);
    int y = m->y + ui_line_h() + 14;
    for (int i = 0; i < m->nlines; i++, y += 8 * ui_scale() + 4)
        ui_text(c, m->x + 16, y, m->lines[i], UI_TEXT);
    bevel_button(c, m->ok_x, m->by, m->bw, m->bh, m->ok, 1);
    if (m->cancel) bevel_button(c, m->cancel_x, m->by, m->bw, m->bh, "Cancel", 0);
}

static int message_box(const char *title, const char *text, const char *ok, int cancel)
{
    Msg m;
    memset(&m, 0, sizeof m);
    m.title = title;
    m.ok = ok;
    m.cancel = cancel;
    int ww, wh;
    SDL_GetRendererOutputSize(ren, &ww, &wh);
    int cols = (ww - 80) / ui_char_w();
    if (cols > 56) cols = 56;
    if (cols < 20) cols = 20;
    wrap(&m, text, cols);
    int maxw = ui_text_w(title);
    for (int i = 0; i < m.nlines; i++) if (ui_text_w(m.lines[i]) > maxw) maxw = ui_text_w(m.lines[i]);
    m.bw = ui_text_w("Cancel") + 32;
    if (ui_text_w(ok) + 32 > m.bw) m.bw = ui_text_w(ok) + 32;
    m.bh = ui_line_h() + 4;
    m.w = maxw + 32;
    if (m.w < m.bw * 2 + 48) m.w = m.bw * 2 + 48;
    m.h = ui_line_h() + 14 + m.nlines * (8 * ui_scale() + 4) + 16 + m.bh + 16;
    m.x = (ww - m.w) / 2;
    m.y = (wh - m.h) / 2;
    m.by = m.y + m.h - m.bh - 14;
    m.ok_x = m.x + m.w - m.bw - 16;
    m.cancel_x = m.ok_x - m.bw - 12;
    for (;;) {
        present(msg_paint, &m);
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 50)) continue;
        if (e.type == SDL_QUIT) { SDL_PushEvent(&e); return 0; }
        if (e.type == SDL_KEYDOWN) {
            SDL_Keycode k = e.key.keysym.sym;
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER) return 1;
            if (k == SDLK_ESCAPE) return !cancel;
        }
        if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
            if (inside(e.button.x, e.button.y, m.ok_x, m.by, m.bw, m.bh)) return 1;
            if (cancel && inside(e.button.x, e.button.y, m.cancel_x, m.by, m.bw, m.bh)) return 0;
        }
    }
}

void ui_message(const char *title, const char *text) { message_box(title, text, "OK", 0); }

int ui_confirm(const char *title, const char *text, const char *ok_label)
{
    return message_box(title, text, ok_label, 1);
}

/* ------------------------------------------------------------------ */
/* scelta dei file                                                    */
/* ------------------------------------------------------------------ */

static int in_path(const char *prog)
{
    const char *path = getenv("PATH");
    if (!path) return 0;
    char buf[1024];
    while (*path) {
        const char *colon = strchr(path, ':');
        size_t len = colon ? (size_t)(colon - path) : strlen(path);
        snprintf(buf, sizeof buf, "%.*s/%s", (int)len, path, prog);
        if (access(buf, X_OK) == 0) return 1;
        path += len;
        if (*path == ':') path++;
    }
    return 0;
}

/* argomento per la shell tra apici singoli */
static void quote(char *out, size_t size, const char *s)
{
    size_t o = 0;
    if (o + 1 < size) out[o++] = '\'';
    for (; *s && o + 5 < size; s++) {
        if (*s == '\'') { memcpy(out + o, "'\\''", 4); o += 4; }
        else out[o++] = *s;
    }
    if (o + 1 < size) out[o++] = '\'';
    out[o] = 0;
}

/* zenity o kdialog: 1 scelto, 0 annullato, -1 nessuno dei due */
static int external_dialog(const char *title, const char *dir, int save, const char *name, char *out, size_t size)
{
    char start[1200], qstart[2500], qtitle[400], cmd[3200];
    snprintf(start, sizeof start, "%s/%s", dir && dir[0] ? dir : ".", save && name ? name : "");
    quote(qstart, sizeof qstart, start);
    quote(qtitle, sizeof qtitle, title);
    if (in_path("zenity"))
        snprintf(cmd, sizeof cmd, "zenity --file-selection %s--title=%s --filename=%s 2>/dev/null",
                 save ? "--save --confirm-overwrite " : "", qtitle, qstart);
    else if (in_path("kdialog"))
        snprintf(cmd, sizeof cmd, "kdialog --title %s %s %s 2>/dev/null",
                 qtitle, save ? "--getsavefilename" : "--getopenfilename", qstart);
    else
        return -1;
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    char line[1024] = "";
    if (!fgets(line, sizeof line, p)) line[0] = 0;
    int status = pclose(p);
    line[strcspn(line, "\r\n")] = 0;
    if (status != 0 || !line[0]) return 0;
    snprintf(out, size, "%s", line);
    return 1;
}

/* elenco dentro la finestra */
typedef struct Entry { char name[256]; int dir; } Entry;

typedef struct Browser {
    const char *title;
    char  dir[1024];
    Entry *e;
    int   n, cap, sel, top, save;
    char  name[256];
    int   x, y, w, h, rows, list_y;
} Browser;

static int cmp_entry(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcasecmp(x->name, y->name);
}

static void browser_load(Browser *b)
{
    b->n = 0;
    b->sel = 0;
    b->top = 0;
    DIR *d = opendir(b->dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".")) continue;
        if (de->d_name[0] == '.' && strcmp(de->d_name, "..")) continue;     /* nascosti */
        if (!strcmp(de->d_name, "..") && !strcmp(b->dir, "/")) continue;
        if (b->n == b->cap) {
            int cap = b->cap ? b->cap * 2 : 256;
            Entry *ne = realloc(b->e, (size_t)cap * sizeof *ne);
            if (!ne) break;
            b->e = ne;
            b->cap = cap;
        }
        Entry *en = &b->e[b->n++];
        snprintf(en->name, sizeof en->name, "%s", de->d_name);
        char full[1300];
        snprintf(full, sizeof full, "%s/%s", b->dir, de->d_name);
        struct stat st;
        en->dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
    }
    closedir(d);
    qsort(b->e, (size_t)b->n, sizeof *b->e, cmp_entry);
}

static void browser_paint(UiCanvas *c, void *ctx)
{
    Browser *b = ctx;
    int lh = ui_line_h() - 4;
    ui_fill(c, 0, 0, c->w, c->h, UI_DIM);
    ui_fill(c, b->x, b->y, b->w, b->h, UI_BG);
    ui_frame(c, b->x, b->y, b->w, b->h, UI_TEXT);
    ui_fill(c, b->x + 1, b->y + 1, b->w - 2, ui_line_h(), UI_TITLE);
    ui_text(c, b->x + 12, b->y + 6, b->title, UI_TEXT);
    char shown[200];
    int maxc = (b->w - 24) / ui_char_w();
    size_t len = strlen(b->dir);
    if ((int)len > maxc) snprintf(shown, sizeof shown, "...%s", b->dir + len - (size_t)(maxc - 3));
    else snprintf(shown, sizeof shown, "%s", b->dir);
    ui_text(c, b->x + 12, b->y + ui_line_h() + 8, shown, UI_GREY);
    ui_fill(c, b->x + 10, b->list_y - 2, b->w - 20, b->rows * lh + 4, UI_WHITE);
    for (int r = 0; r < b->rows && b->top + r < b->n; r++) {
        const Entry *en = &b->e[b->top + r];
        int y = b->list_y + r * lh, hi = b->top + r == b->sel;
        if (hi) ui_fill(c, b->x + 12, y, b->w - 24, lh, UI_HILITE);
        char label[300];
        snprintf(label, sizeof label, "%.*s%s", maxc - 2, en->name, en->dir ? "/" : "");
        ui_text(c, b->x + 16, y + (lh - 8 * ui_scale()) / 2, label, hi ? UI_WHITE : UI_TEXT);
    }
    int fy = b->y + b->h - ui_line_h() - 14;
    if (b->save) {
        ui_text(c, b->x + 12, fy + 4, "Name:", UI_TEXT);
        int fx = b->x + 12 + ui_text_w("Name: ");
        ui_fill(c, fx, fy, b->w - (fx - b->x) - 12, ui_line_h(), UI_WHITE);
        ui_frame(c, fx, fy, b->w - (fx - b->x) - 12, ui_line_h(), UI_GREY);
        char nm[300];
        snprintf(nm, sizeof nm, "%s_", b->name);
        ui_text(c, fx + 6, fy + 5, nm, UI_TEXT);
    } else {
        ui_text(c, b->x + 12, fy + 4, "Enter: open   Esc: cancel", UI_GREY);
    }
}

static void browser_move(Browser *b, int sel)
{
    if (sel < 0) sel = 0;
    if (sel >= b->n) sel = b->n - 1;
    b->sel = sel;
    if (b->sel < b->top) b->top = b->sel;
    if (b->sel >= b->top + b->rows) b->top = b->sel - b->rows + 1;
    if (b->top < 0) b->top = 0;
}

/* Invio o doppio clic: 1 = file scelto in out */
static int browser_open(Browser *b, char *out, size_t size)
{
    if (b->save && (b->sel < 0 || b->sel >= b->n || !b->e[b->sel].dir)) {
        if (!b->name[0]) return 0;
        snprintf(out, size, "%s/%s", strcmp(b->dir, "/") ? b->dir : "", b->name);
        return 1;
    }
    if (b->sel < 0 || b->sel >= b->n) return 0;
    const Entry *en = &b->e[b->sel];
    if (en->dir) {
        if (!strcmp(en->name, "..")) {
            char *slash = strrchr(b->dir, '/');
            if (slash && slash != b->dir) *slash = 0;
            else strcpy(b->dir, "/");
        } else {
            size_t len = strlen(b->dir);
            snprintf(b->dir + len, sizeof b->dir - len, "%s%s", len && b->dir[len - 1] == '/' ? "" : "/", en->name);
        }
        browser_load(b);
        return 0;
    }
    snprintf(out, size, "%s/%s", strcmp(b->dir, "/") ? b->dir : "", en->name);
    return 1;
}

static int internal_dialog(const char *title, const char *dir, int save, const char *name, char *out, size_t size)
{
    Browser b;
    memset(&b, 0, sizeof b);
    b.title = title;
    b.save = save;
    snprintf(b.dir, sizeof b.dir, "%s", dir && dir[0] ? dir : "/");
    if (save && name) snprintf(b.name, sizeof b.name, "%s", name);
    browser_load(&b);
    int ww, wh, lh = ui_line_h() - 4;
    SDL_GetRendererOutputSize(ren, &ww, &wh);
    b.w = ww * 8 / 10;
    if (b.w > 900) b.w = 900;
    b.h = wh * 8 / 10;
    b.x = (ww - b.w) / 2;
    b.y = (wh - b.h) / 2;
    b.list_y = b.y + 2 * ui_line_h() + 12;
    b.rows = (b.y + b.h - ui_line_h() - 24 - b.list_y) / lh;
    if (b.rows < 1) b.rows = 1;
    uint32_t last_click = 0;
    int last_sel = -1, result = 0;
    SDL_StartTextInput();
    for (;;) {
        present(browser_paint, &b);
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 50)) continue;
        if (e.type == SDL_QUIT) { SDL_PushEvent(&e); break; }
        if (e.type == SDL_KEYDOWN) {
            SDL_Keycode k = e.key.keysym.sym;
            if (k == SDLK_ESCAPE) break;
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { if ((result = browser_open(&b, out, size))) break; }
            else if (k == SDLK_DOWN) browser_move(&b, b.sel + 1);
            else if (k == SDLK_UP) browser_move(&b, b.sel - 1);
            else if (k == SDLK_PAGEDOWN) browser_move(&b, b.sel + b.rows);
            else if (k == SDLK_PAGEUP) browser_move(&b, b.sel - b.rows);
            else if (k == SDLK_HOME) browser_move(&b, 0);
            else if (k == SDLK_END) browser_move(&b, b.n - 1);
            else if (k == SDLK_BACKSPACE && save && b.name[0]) b.name[strlen(b.name) - 1] = 0;
        } else if (e.type == SDL_TEXTINPUT && save) {
            size_t len = strlen(b.name);
            for (const char *s = e.text.text; *s && len + 1 < sizeof b.name; s++)
                if ((unsigned char)*s >= 32 && *s != '/') b.name[len++] = *s;
            b.name[len] = 0;
            b.sel = -1;
        } else if (e.type == SDL_MOUSEWHEEL) {
            b.top -= e.wheel.y * 3;
            if (b.top > b.n - b.rows) b.top = b.n - b.rows;
            if (b.top < 0) b.top = 0;
        } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
            int mx = e.button.x, my = e.button.y;
            if (!inside(mx, my, b.x, b.y, b.w, b.h)) break;
            if (inside(mx, my, b.x + 10, b.list_y, b.w - 20, b.rows * lh)) {
                int i = b.top + (my - b.list_y) / lh;
                if (i < b.n) {
                    uint32_t t = SDL_GetTicks();
                    b.sel = i;
                    if (i == last_sel && t - last_click < 400) {
                        if ((result = browser_open(&b, out, size))) break;
                        last_sel = -1;
                    } else {
                        last_sel = i;
                        last_click = t;
                        if (save && !b.e[i].dir) snprintf(b.name, sizeof b.name, "%s", b.e[i].name);
                    }
                }
            }
        }
    }
    free(b.e);
    return result;
}

int ui_file(const char *title, const char *dir, int save, const char *name, char *out, size_t size)
{
    int r = external_dialog(title, dir, save, name, out, size);
    if (r >= 0) return r;
    return internal_dialog(title, dir, save, name, out, size);
}
