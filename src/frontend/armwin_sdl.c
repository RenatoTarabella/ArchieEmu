/*
 * armwin_sdl.c - Finestra della macchina BBC BASIC per Linux (SDL2).
 *
 *   armwin [--rom modulo] [--mode n] [--ram MB] [--mhz N] [--turbo] [--disc dir]
 *
 * Come win32_main.c e armwin_mac.m: clock limitato a 8 MHz (F12 per il
 * turbo), selezione del testo col mouse (doppio clic: una parola), Ctrl+C
 * copia la selezione, Ctrl+V incolla, il tasto destro copia o incolla.
 * La ROM e il disco si cercano accanto all'eseguibile (third_party/riscos/
 * BASIC e disc) o piu' su, nell'albero dei sorgenti.
 *
 * Per le prove senza schermo (SDL_VIDEODRIVER=offscreen):
 * ARCHIE_SDL_SHOT=file.png salva la finestra dopo ARCHIE_SDL_SHOT_MS ms ed esce.
 */
#include <SDL.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "machine/machine.h"
#include "png.h"
#include "sdl_keys.h"

#define FRAME_HZ     50
#define FLASH_CS     25            /* periodo del lampeggio (come *FX 9/10) */

typedef struct App {
    Machine   m;
    uint32_t *pixels;
    int       pix_w, pix_h;
    double    mhz;
    int       turbo, quit;
    char     *paste;               /* testo da incollare ancora da inviare */
    size_t    paste_pos, paste_len;
    uint64_t  stat_c0;
    double    stat_t0, stat_mhz;
    char      notice[64];          /* messaggio temporaneo nel titolo */
    double    notice_until;
    int       last_w, last_h;
    SDL_Rect  img;                 /* posizione dell'immagine nella finestra */
    int       sel_valid, sel_dragging;
    int       sel_c0, sel_r0, sel_c1, sel_r1;
    uint32_t  last_click;
} App;

static App app;
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *tex;
static int tex_w, tex_h;

static double now_s(void) { return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }

static void update_title(void)
{
    char t[128];
    if (app.notice[0] && now_s() < app.notice_until)
        snprintf(t, sizeof t, "BBC BASIC V   [%s]", app.notice);
    else if (app.turbo)
        snprintf(t, sizeof t, "BBC BASIC V   [TURBO: %.0f MHz, F12 for %.0f MHz]", app.stat_mhz, app.mhz);
    else
        snprintf(t, sizeof t, "BBC BASIC V   [ARM2 %.0f MHz, F12 for turbo]", app.mhz);
    if (strcmp(t, SDL_GetWindowTitle(window))) SDL_SetWindowTitle(window, t);
}

static void set_notice(const char *msg)
{
    snprintf(app.notice, sizeof app.notice, "%s", msg);
    app.notice_until = now_s() + 2.0;
    update_title();
}

/* dimensione a schermo del modo corrente: pixel alti raddoppiati */
static void display_size(int *w, int *h)
{
    const Vdu *v = &app.m.vdu;
    *w = v->width << (v->xeig - 1);
    *h = v->height << (v->yeig - 1);
    if (*w < 640) { *h = *h * 640 / *w; *w = 640; }
}

static void fit_window(void)
{
    int w, h;
    display_size(&w, &h);
    SDL_Rect work;
    if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(window), &work) != 0) work = (SDL_Rect){ 0, 0, 1920, 1080 };
    int scale = 2;
    while (scale > 1 && (w * scale > work.w * 9 / 10 || h * scale > work.h * 9 / 10)) scale--;
    SDL_SetWindowSize(window, w * scale, h * scale);
}

/* ------------------------------------------------------------------ */
/* selezione e appunti                                                */
/* ------------------------------------------------------------------ */

static void sel_range(int *c0, int *r0, int *c1, int *r1)
{
    int a = app.sel_r0 * 4096 + app.sel_c0, b = app.sel_r1 * 4096 + app.sel_c1;
    if (a > b) { int t = a; a = b; b = t; }
    *r0 = a / 4096; *c0 = a % 4096; *r1 = b / 4096; *c1 = b % 4096;
}

static int in_selection(int col, int row)
{
    int c0, r0, c1, r1;
    sel_range(&c0, &r0, &c1, &r1);
    int k = row * 4096 + col;
    return k >= r0 * 4096 + c0 && k <= r1 * 4096 + c1;
}

static void highlight_selection(void)
{
    const Vdu *v = &app.m.vdu;
    for (int row = 0; row < v->rows; row++)
        for (int col = 0; col < v->cols; col++) {
            if (!in_selection(col, row)) continue;
            for (int y = 0; y < 8; y++) {
                uint32_t *p = app.pixels + (size_t)(row * 8 + y) * v->width + col * 8;
                for (int x = 0; x < 8; x++) p[x] ^= 0xFFFFFF;
            }
        }
}

static void mouse_to_cell(int x, int y, int *col, int *row)
{
    const Vdu *v = &app.m.vdu;
    int px = app.img.w > 0 ? (x - app.img.x) * v->width / app.img.w : 0;
    int py = app.img.h > 0 ? (y - app.img.y) * v->height / app.img.h : 0;
    *col = px < 0 ? 0 : px / 8 >= v->cols ? v->cols - 1 : px / 8;
    *row = py < 0 ? 0 : py / 8 >= v->rows ? v->rows - 1 : py / 8;
}

static void copy_selection(void)
{
    const Vdu *v = &app.m.vdu;
    int c0, r0, c1, r1;
    sel_range(&c0, &r0, &c1, &r1);
    size_t cap = ((size_t)(r1 - r0 + 1) * (size_t)(v->cols + 1) + 1) * 2, n = 0, chars = 0;
    char *text = malloc(cap);
    if (!text) return;
    for (int row = r0; row <= r1; row++) {
        int from = row == r0 ? c0 : 0, to = row == r1 ? c1 : v->cols - 1;
        size_t line_start = n;
        for (int col = from; col <= to; col++) {
            int ch = vdu_char_at(v, col, row);
            if (!ch) ch = ' ';
            if (ch < 128) text[n++] = (char)ch;
            else { text[n++] = (char)(0xC0 | (ch >> 6)); text[n++] = (char)(0x80 | (ch & 0x3F)); }   /* Latin-1 -> UTF-8 */
        }
        while (n > line_start && text[n - 1] == ' ') n--;       /* spazi finali */
        chars += n - line_start;
        if (row < r1) text[n++] = '\n';
    }
    text[n] = 0;
    SDL_SetClipboardText(text);
    free(text);
    char msg[64];
    snprintf(msg, sizeof msg, "copied %u characters", (unsigned)chars);
    set_notice(msg);
}

static void select_word(int col, int row)
{
    const Vdu *v = &app.m.vdu;
    int ch = vdu_char_at(v, col, row);
    if (!ch || ch == ' ') return;
    int a = col, b = col;
    while (a > 0) { int c = vdu_char_at(v, a - 1, row); if (!c || c == ' ') break; a--; }
    while (b < v->cols - 1) { int c = vdu_char_at(v, b + 1, row); if (!c || c == ' ') break; b++; }
    app.sel_c0 = a; app.sel_r0 = row; app.sel_c1 = b; app.sel_r1 = row;
    app.sel_valid = 1;
}

static void start_paste(void)
{
    char *str = SDL_GetClipboardText();
    if (!str) return;
    char *s = malloc(strlen(str) * 3 + 1);              /* "..." puo' triplicare */
    if (!s) { SDL_free(str); return; }
    size_t o = 0;
    const char *p = str;
    uint32_t c;
    while ((c = sdl_utf8_next(&p))) {
        if (c == '\r') { s[o++] = 13; if (*p == '\n') p++; }
        else if (c == '\n') s[o++] = 13;
        else if (c == '\t' || c == 0xA0) s[o++] = ' ';
        else if (c < 256) s[o++] = (char)c;
        /* tipografia di chat e pagine web -> ASCII */
        else if (c == 0x201C || c == 0x201D || c == 0x201E || c == 0x2033) s[o++] = '"';
        else if (c == 0x2018 || c == 0x2019 || c == 0x201A || c == 0x2032) s[o++] = '\'';
        else if (c == 0x2013 || c == 0x2014 || c == 0x2212) s[o++] = '-';
        else if (c == 0x2026) { s[o++] = '.'; s[o++] = '.'; s[o++] = '.'; }
        else if (c >= 0x2000 && c <= 0x200B) s[o++] = ' ';
        else if (c == 0xFEFF) continue;                              /* BOM */
        else s[o++] = '?';
    }
    SDL_free(str);
    free(app.paste);
    app.paste = s;
    app.paste_len = o;
    app.paste_pos = 0;
}

static void feed_paste(void)
{
    while (app.paste && app.paste_pos < app.paste_len && kernel_keys_pending(&app.m.kernel) < 200)
        kernel_key(&app.m.kernel, (uint8_t)app.paste[app.paste_pos++]);
    if (app.paste && app.paste_pos >= app.paste_len) { free(app.paste); app.paste = NULL; }
}

/* ------------------------------------------------------------------ */
/* tastiera e mouse                                                   */
/* ------------------------------------------------------------------ */

static void key_down(const SDL_KeyboardEvent *k)
{
    RiscosKernel *kn = &app.m.kernel;
    SDL_Keycode sym = k->keysym.sym;
    int ctrl = (k->keysym.mod & KMOD_CTRL) != 0, shift = (k->keysym.mod & KMOD_SHIFT) != 0;
    switch (sym) {
    case SDLK_ESCAPE:
        if (app.sel_valid) { app.sel_valid = 0; return; }        /* prima annulla la selezione */
        free(app.paste); app.paste = NULL;
        kernel_key(kn, 27);
        return;
    case SDLK_F12: if (!k->repeat) { app.turbo = !app.turbo; update_title(); } return;
    case SDLK_LEFT:  kernel_key(kn, 0x8C); return;
    case SDLK_RIGHT: kernel_key(kn, 0x8D); return;
    case SDLK_DOWN:  kernel_key(kn, 0x8E); return;
    case SDLK_UP:    kernel_key(kn, 0x8F); return;
    case SDLK_DELETE: kernel_key(kn, 127); return;
    case SDLK_HOME: kernel_key(kn, 30); return;
    case SDLK_BACKSPACE: app.sel_valid = 0; kernel_key(kn, 8); return;
    case SDLK_RETURN: case SDLK_KP_ENTER: app.sel_valid = 0; kernel_key(kn, 13); return;
    case SDLK_TAB: kernel_key(kn, 9); return;
    case SDLK_INSERT:
        if (ctrl && app.sel_valid) { copy_selection(); app.sel_valid = 0; }
        else if (shift) start_paste();
        return;
    default: break;
    }
    if (ctrl && sym >= SDLK_a && sym <= SDLK_z) {
        if (sym == SDLK_v) { start_paste(); return; }
        if (sym == SDLK_c && app.sel_valid) { copy_selection(); app.sel_valid = 0; return; }
        app.sel_valid = 0;
        kernel_key(kn, (uint8_t)(sym - SDLK_a + 1));             /* caratteri di controllo */
    }
}

static void text_input(const char *s)
{
    if (SDL_GetModState() & KMOD_CTRL) return;                   /* gia' gestiti in key_down */
    uint32_t c;
    while ((c = sdl_utf8_next(&s))) {
        app.sel_valid = 0;
        if (c < 256) kernel_key(&app.m.kernel, (uint8_t)c);
    }
}

static void handle_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_QUIT: app.quit = 1; break;
    case SDL_KEYDOWN: key_down(&e->key); break;
    case SDL_TEXTINPUT: text_input(e->text.text); break;
    case SDL_MOUSEBUTTONDOWN:
        if (e->button.button == SDL_BUTTON_LEFT) {
            int col, row;
            mouse_to_cell(e->button.x, e->button.y, &col, &row);
            if (e->button.clicks == 2) { select_word(col, row); break; }
            app.sel_c0 = app.sel_c1 = col;
            app.sel_r0 = app.sel_r1 = row;
            app.sel_dragging = 1;
            app.sel_valid = 0;
        }
        break;
    case SDL_MOUSEMOTION:
        if (app.sel_dragging) {
            int col, row;
            mouse_to_cell(e->motion.x, e->motion.y, &col, &row);
            if (col != app.sel_c0 || row != app.sel_r0) app.sel_valid = 1;
            app.sel_c1 = col;
            app.sel_r1 = row;
        }
        break;
    case SDL_MOUSEBUTTONUP:
        if (e->button.button == SDL_BUTTON_LEFT) app.sel_dragging = 0;
        else if (e->button.button == SDL_BUTTON_RIGHT) {
            if (app.sel_valid) { copy_selection(); app.sel_valid = 0; }
            else start_paste();
        }
        break;
    case SDL_DROPTEXT: SDL_free(e->drop.file); break;
    default: break;
    }
}

/* ------------------------------------------------------------------ */
/* schermo                                                            */
/* ------------------------------------------------------------------ */

static void draw(void)
{
    Vdu *v = &app.m.vdu;
    if (v->width != app.pix_w || v->height != app.pix_h) {
        free(app.pixels);
        app.pixels = malloc((size_t)v->width * (size_t)v->height * 4);
        app.pix_w = v->width;
        app.pix_h = v->height;
        if (!app.pixels) return;
    }
    uint64_t cs = kernel_time_cs(&app.m.kernel);
    int phase = (int)((cs / FLASH_CS) & 1);
    vdu_render(v, app.pixels, phase, !phase && !app.sel_valid);
    if (app.sel_valid) highlight_selection();
    if (!tex || tex_w != v->width || tex_h != v->height) {
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, v->width, v->height);
        tex_w = v->width;
        tex_h = v->height;
    }
    if (tex) SDL_UpdateTexture(tex, NULL, app.pixels, v->width * 4);

    int ww, wh, dw, dh;
    SDL_GetRendererOutputSize(renderer, &ww, &wh);
    display_size(&dw, &dh);
    /* scala mantenendo le proporzioni, bordo nel colore del border */
    int sw = ww, sh = (int)((double)ww * dh / dw);
    if (sh > wh) { sh = wh; sw = (int)((double)wh * dw / dh); }
    app.img = (SDL_Rect){ (ww - sw) / 2, (wh - sh) / 2, sw, sh };
    SDL_SetRenderDrawColor(renderer, (v->border >> 16) & 255, (v->border >> 8) & 255, v->border & 255, 255);
    SDL_RenderClear(renderer);
    if (tex) SDL_RenderCopy(renderer, tex, NULL, &app.img);
}

/* ------------------------------------------------------------------ */
/* file                                                               */
/* ------------------------------------------------------------------ */

static int exists(const char *p) { return access(p, R_OK) == 0; }

static const char *find_rom(void)
{
    static char path[PATH_MAX];
    static const char *rel[] = { "third_party/riscos/BASIC", "../third_party/riscos/BASIC",
                                 "../../third_party/riscos/BASIC", "../../../third_party/riscos/BASIC" };
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) {
            slash[1] = 0;
            for (size_t i = 0; i < sizeof rel / sizeof rel[0]; i++) {
                snprintf(path, sizeof path, "%s%s", exe, rel[i]);
                if (exists(path)) return path;
            }
        }
    }
    for (size_t i = 0; i < sizeof rel / sizeof rel[0]; i++)
        if (exists(rel[i])) return rel[i];
    return rel[0];
}

int main(int argc, char **argv)
{
    MachineConfig cfg = { NULL, 4, -1, 0, NULL, 0 };
    static char disc[PATH_MAX];
    app.mhz = 8;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) cfg.rom_path = argv[++i];
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) cfg.mode = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ram") && i + 1 < argc) cfg.ram_mb = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mhz") && i + 1 < argc) app.mhz = atof(argv[++i]);
        else if (!strcmp(argv[i], "--turbo")) app.turbo = 1;
        else if (!strcmp(argv[i], "--disc") && i + 1 < argc) cfg.disc_dir = argv[++i];
    }
    if (app.mhz <= 0) app.mhz = 8;
    if (!cfg.rom_path) cfg.rom_path = find_rom();
    if (!cfg.disc_dir) {
        machine_default_disc(cfg.rom_path, disc, sizeof disc);
        cfg.disc_dir = disc;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    char err[256];
    if (!machine_create(&app.m, &cfg, err, sizeof err)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "BBC BASIC", err, NULL);
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    window = SDL_CreateWindow("BBC BASIC V", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 960,
                              SDL_WINDOW_RESIZABLE);
    if (!window) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    app.last_w = app.m.vdu.width;
    app.last_h = app.m.vdu.height;
    fit_window();
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    update_title();
    SDL_StartTextInput();

    const char *shot = getenv("ARCHIE_SDL_SHOT");
    double shot_at = shot && getenv("ARCHIE_SDL_SHOT_MS") ? atof(getenv("ARCHIE_SDL_SHOT_MS")) / 1000.0 : -1;
    double frame = 1.0 / FRAME_HZ, next = now_s(), emulated = 0;
    app.stat_t0 = next;
    app.stat_c0 = app.m.cpu.cycles;
    while (!app.quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) handle_event(&e);
        double t = now_s();
        if (t < next) { SDL_Delay(1); continue; }
        feed_paste();
        if (app.turbo) {
            /* esegue finche' resta tempo nel frame, poi disegna */
            double until = t + frame * 0.8;
            while (!app.m.cpu.halted && now_s() < until) {
                machine_run(&app.m, 200000);
                if (app.m.kernel.waiting) break;
            }
        } else {
            machine_run(&app.m, (uint64_t)(app.mhz * 1e6 / FRAME_HZ * (1.0 - machine_dma_fraction(&app.m))));
        }
        if (app.m.cpu.halted) break;
        if (app.m.vdu.width != app.last_w || app.m.vdu.height != app.last_h) {
            app.last_w = app.m.vdu.width;
            app.last_h = app.m.vdu.height;
            app.sel_valid = 0;
            fit_window();
        }
        draw();
        emulated += frame;
        if (shot && shot_at >= 0 && emulated >= shot_at) {
            int w, h;
            SDL_GetRendererOutputSize(renderer, &w, &h);
            uint32_t *buf = malloc((size_t)w * (size_t)h * 4);
            if (buf && SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_RGB888, buf, w * 4) == 0)
                png_write(shot, buf, w, h, 1);
            free(buf);
            app.quit = 1;
        }
        SDL_RenderPresent(renderer);

        t = now_s();
        if (t - app.stat_t0 >= 1.0) {
            app.stat_mhz = (double)(app.m.cpu.cycles - app.stat_c0) / (t - app.stat_t0) / 1e6;
            app.stat_t0 = t;
            app.stat_c0 = app.m.cpu.cycles;
            update_title();
        }
        if (app.notice[0] && t >= app.notice_until) { app.notice[0] = 0; update_title(); }
        next += frame;
        if (next < t - 0.1) next = t;                             /* recupera dopo una pausa */
    }
    machine_destroy(&app.m);
    SDL_Quit();
    return 0;
}
