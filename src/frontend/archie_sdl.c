/*
 * archie_sdl.c - Finestra delle macchine Archimedes e Risc PC per Linux (SDL2).
 *
 *   archie [--rom file] [--floppy disco.adf] [--floppy2 disco.adf] [--ram MB]
 *          [--vram MB] [--arm710 | --strongarm] [--mhz N] [--cmos file]
 *          [--hostfs cartella] [--choose]
 *
 * Come archie_win32.c e archie_mac.m. Senza --rom una finestra iniziale
 * sceglie macchina, ROM, processore e memoria, ricordati in
 * ~/.config/ArchieEmu/ArchieEmu.ini. Le ROM si cercano nella cartella roms
 * accanto all'eseguibile (o piu' su, per l'albero dei sorgenti) e in
 * ~/Documents/ArchieEmu/roms; HostFS e ADF accanto all'eseguibile o in
 * ~/Documents/ArchieEmu.
 *
 * I comandi stanno in una barra dei menu disegnata in cima alla finestra
 * (Disc, Machine, Mouse), che si usa col mouse libero: nessun tasto e' tolto
 * a RISC OS. Ctrl+Alt premuti e rilasciati da soli liberano il mouse; un clic
 * nell'immagine lo cattura di nuovo. La tastiera passa per la traduzione di
 * Windows (archie_keys.c, sdl_keys.c); l'Alt destro fa da AltGr.
 *
 * Per le prove senza schermo (SDL_VIDEODRIVER=offscreen):
 * ARCHIE_SDL_SHOT=file.png salva la finestra dopo ARCHIE_SDL_SHOT_MS ms
 * (0 = la finestra iniziale) ed esce.
 */
#include <SDL.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "archie/archie.h"
#include "archie_keys.h"
#include "png.h"
#include "riscpc/hdformat.h"
#include "riscpc/riscpc.h"
#include "romlist.h"
#include "sdl_keys.h"
#include "sdl_ui.h"

#define FRAME_HZ   50
#define TURBO_MHZ  64.0
#define BUF_W      2048
#define BUF_H      2048
#define AUDIO_AHEAD (ARCHIE_AUDIO_HZ / 12)     /* oltre ~80 ms in coda si scarta */

typedef struct App {
    Archie    a;
    RiscPc    r;
    int       rpc;
    int       rpc_buttons;
    uint32_t  pixels[BUF_W * BUF_H];
    int       disp_w, disp_h;            /* ultima immagine */
    uint32_t  border;
    double    mhz;
    int       turbo, muted;
    char      rom_name[64];
    char      floppy_name[2][64];
    int       captured, right_menu;
    int       free_armed;                /* Ctrl+Alt premuti da soli */
    int       ctrl_down, alt_down;
    int       got_text;                  /* SDL_TEXTINPUT dopo l'ultimo tasto */
    int       quit, choose;
    SDL_Rect  img;                       /* dove sta l'immagine nella finestra */
} App;

static App app;
static ArchieKeys keys;
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *screen_tex;
static int tex_w, tex_h;
static SDL_AudioDeviceID audio_dev;
static char exe_dir[PATH_MAX], docs_dir[PATH_MAX], adf_dir[PATH_MAX];
static char exe_path[PATH_MAX];

static int menubar_h(void) { return ui_line_h(); }

/* ------------------------------------------------------------------ */
/* impostazioni: ~/.config/ArchieEmu/ArchieEmu.ini (chiave=valore)      */
/* ------------------------------------------------------------------ */

#define MAX_SETTINGS 32
static struct { char key[32]; char val[PATH_MAX]; } settings[MAX_SETTINGS];
static int nsettings;
static char settings_path[PATH_MAX];

static void make_dirs(const char *path)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

static void settings_load(void)
{
    const char *cfg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char dir[PATH_MAX];
    if (cfg && cfg[0]) snprintf(dir, sizeof dir, "%s/ArchieEmu", cfg);
    else snprintf(dir, sizeof dir, "%s/.config/ArchieEmu", home ? home : ".");
    make_dirs(dir);
    snprintf(settings_path, sizeof settings_path, "%s/ArchieEmu.ini", dir);
    FILE *f = fopen(settings_path, "r");
    if (!f) return;
    char line[PATH_MAX + 64];
    while (fgets(line, sizeof line, f) && nsettings < MAX_SETTINGS) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '[' || line[0] == '#') continue;
        *eq = 0;
        snprintf(settings[nsettings].key, sizeof settings[0].key, "%s", line);
        snprintf(settings[nsettings].val, sizeof settings[0].val, "%s", eq + 1);
        nsettings++;
    }
    fclose(f);
}

static const char *setting(const char *key)
{
    for (int i = 0; i < nsettings; i++)
        if (!strcmp(settings[i].key, key)) return settings[i].val;
    return NULL;
}

static int setting_int(const char *key, int def)
{
    const char *v = setting(key);
    return v && v[0] ? atoi(v) : def;
}

static void set_setting(const char *key, const char *val)
{
    int i;
    for (i = 0; i < nsettings; i++)
        if (!strcmp(settings[i].key, key)) break;
    if (i == nsettings) {
        if (nsettings == MAX_SETTINGS) return;
        snprintf(settings[nsettings++].key, sizeof settings[0].key, "%s", key);
    }
    snprintf(settings[i].val, sizeof settings[0].val, "%s", val ? val : "");
    FILE *f = fopen(settings_path, "w");
    if (!f) return;
    fprintf(f, "[Machine]\n");
    for (int k = 0; k < nsettings; k++)
        if (settings[k].val[0]) fprintf(f, "%s=%s\n", settings[k].key, settings[k].val);
    fclose(f);
}

static void set_setting_int(const char *key, int v)
{
    char b[16];
    snprintf(b, sizeof b, "%d", v);
    set_setting(key, b);
}

/* ------------------------------------------------------------------ */
/* cartelle                                                           */
/* ------------------------------------------------------------------ */

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_file(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void find_folders(void)
{
    ssize_t n = readlink("/proc/self/exe", exe_path, sizeof exe_path - 1);
    if (n > 0) exe_path[n] = 0;
    else snprintf(exe_path, sizeof exe_path, "./archie");
    snprintf(exe_dir, sizeof exe_dir, "%s", exe_path);
    char *slash = strrchr(exe_dir, '/');
    if (slash) *slash = 0;
    else snprintf(exe_dir, sizeof exe_dir, ".");

    const char *home = getenv("HOME");
    char docs[PATH_MAX];
    snprintf(docs, sizeof docs, "%s/Documents", home ? home : ".");
    snprintf(docs_dir, sizeof docs_dir, "%s/ArchieEmu", is_dir(docs) ? docs : home ? home : ".");
}

/* la cartella 'name' accanto all'eseguibile (o piu' su), altrimenti in
   ~/Documents/ArchieEmu (creata) */
static void find_folder(const char *name, char *out, size_t size)
{
    static const char *up[] = { "", "/..", "/../..", "/../../.." };
    for (size_t i = 0; i < sizeof up / sizeof up[0]; i++) {
        char p[PATH_MAX], real[PATH_MAX];
        snprintf(p, sizeof p, "%s%s/%s", exe_dir, up[i], name);
        if (is_dir(p)) {
            snprintf(out, size, "%s", realpath(p, real) ? real : p);
            return;
        }
    }
    snprintf(out, size, "%s/%s", docs_dir, name);
    make_dirs(out);
}

/* ------------------------------------------------------------------ */
/* ROM                                                                */
/* ------------------------------------------------------------------ */

typedef struct Rom { char path[PATH_MAX]; char label[160]; int version, riscpc; } Rom;
static Rom roms[128];
static int nroms;

static int cmp_rom(const void *a, const void *b)
{
    const Rom *x = a, *y = b;
    return x->version != y->version ? x->version - y->version : strcmp(x->path, y->path);
}

static void scan_rom_dir(const char *dir, const char *sub)
{
    char d[PATH_MAX];
    if (sub) snprintf(d, sizeof d, "%s/%s", dir, sub);
    else snprintf(d, sizeof d, "%s", dir);
    DIR *dd = opendir(d);
    if (!dd) return;
    struct dirent *de;
    while ((de = readdir(dd)) && nroms < 128) {
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", d, de->d_name);
        if (!is_file(full)) continue;
        int rpc, v = romlist_version(de->d_name, &rpc);
        if (!v) continue;
        Rom *r = &roms[nroms++];
        snprintf(r->path, sizeof r->path, "%s", full);
        char name[64];
        romlist_name(v, name, sizeof name);
        const char *extra = de->d_name + 6;
        snprintf(r->label, sizeof r->label, "%s%s%.40s   (%.30s)", name, *extra ? " " : "", extra, sub ? sub : "roms");
        r->version = v;
        r->riscpc = rpc;
    }
    closedir(dd);
}

static void scan_roms(void)
{
    char dirs[5][PATH_MAX], seen[5][PATH_MAX];
    int nd = 0, ns = 0;
    static const char *up[] = { "", "/..", "/../..", "/../../.." };
    for (size_t i = 0; i < sizeof up / sizeof up[0]; i++)
        snprintf(dirs[nd++], PATH_MAX, "%s%s/roms", exe_dir, up[i]);
    snprintf(dirs[nd++], PATH_MAX, "%s/roms", docs_dir);
    nroms = 0;
    for (int i = 0; i < nd; i++) {
        char real[PATH_MAX];
        if (!is_dir(dirs[i]) || !realpath(dirs[i], real)) continue;
        int dup = 0;
        for (int k = 0; k < ns; k++) if (!strcmp(seen[k], real)) dup = 1;
        if (dup) continue;
        snprintf(seen[ns++], PATH_MAX, "%s", real);
        scan_rom_dir(real, NULL);
        DIR *dd = opendir(real);
        if (!dd) continue;
        struct dirent *de;
        while ((de = readdir(dd))) {
            if (de->d_name[0] == '.' || strstr(de->d_name, "NCOS")) continue;   /* Network Computer */
            char full[PATH_MAX];
            snprintf(full, sizeof full, "%s/%s", real, de->d_name);
            if (is_dir(full)) scan_rom_dir(real, de->d_name);
        }
        closedir(dd);
    }
    qsort(roms, (size_t)nroms, sizeof roms[0], cmp_rom);
}

/* ------------------------------------------------------------------ */
/* finestra iniziale                                                   */
/* ------------------------------------------------------------------ */

typedef struct Choice {
    int  riscpc, cpu, ram_mb, vram_mb;   /* cpu: 0 ARM610, 1 ARM710, 2 StrongARM */
    char rom[PATH_MAX], rom_name[64];
} Choice;

static const char *cpu_names[] = { "ARM610, 30 MHz (Risc PC 600)", "ARM710, 40 MHz (Risc PC 700)",
                                   "StrongARM SA-110 (RISC OS 3.7 only)" };
static const char *vram_names[] = { "None (screen in DRAM)", "1 MB", "2 MB" };
static const int ram_archie[] = { 1, 2, 4 }, ram_rpc[] = { 4, 8, 16, 32, 64 };

typedef struct Splash {
    int  have[2], rpc;
    int  rom_idx[128], nrom, rom_sel;
    int  cpu, ram_sel, vram;
    /* geometria */
    int  x, y, w, h;
    SDL_Rect radio[2], field[4], start, quit;
} Splash;

static void splash_fill(Splash *s)
{
    s->nrom = 0;
    const char *want = setting(s->rpc ? "rom_riscpc" : "rom_archie");
    int sel = -1, fallback = -1;
    for (int i = 0; i < nroms; i++) {
        if (roms[i].riscpc != s->rpc) continue;
        if (want && !strcmp(want, roms[i].path)) sel = s->nrom;
        if (roms[i].version == (s->rpc ? 350 : 311) && fallback < 0) fallback = s->nrom;
        s->rom_idx[s->nrom++] = i;
    }
    s->rom_sel = sel >= 0 ? sel : fallback >= 0 ? fallback : s->nrom - 1;
    int want_ram = setting_int(s->rpc ? "ram_riscpc" : "ram_archie", s->rpc ? 16 : 4);
    const int *sizes = s->rpc ? ram_rpc : ram_archie;
    int n = s->rpc ? 5 : 3;
    s->ram_sel = n - 1;
    for (int i = 0; i < n; i++) if (sizes[i] == want_ram) s->ram_sel = i;
    s->cpu = setting_int("cpu", 0);
    if (s->cpu < 0 || s->cpu > 2) s->cpu = 0;
    s->vram = setting_int("vram", 2);
    if (s->vram < 0 || s->vram > 2) s->vram = 2;
}

static void field_value(const Splash *s, int f, char *out, size_t size)
{
    switch (f) {
    case 0: snprintf(out, size, "%s", s->rom_sel >= 0 ? roms[s->rom_idx[s->rom_sel]].label : "(no ROM)"); break;
    case 1: snprintf(out, size, "%s", s->rpc ? cpu_names[s->cpu] : "ARM2, 8 MHz"); break;
    case 2: {
        int mb = s->rpc ? ram_rpc[s->ram_sel] : ram_archie[s->ram_sel];
        snprintf(out, size, "%d MB%s", mb, !s->rpc && mb == 1 ? " (A3000)" : "");
        break;
    }
    default: snprintf(out, size, "%s", s->rpc ? vram_names[s->vram] : "-"); break;
    }
}

static void splash_paint(UiCanvas *c, void *ctx)
{
    Splash *s = ctx;
    int lh = ui_line_h(), cw = ui_char_w();
    ui_fill(c, 0, 0, c->w, c->h, 0xFF303438);
    ui_fill(c, s->x, s->y, s->w, s->h, UI_BG);
    ui_frame(c, s->x, s->y, s->w, s->h, UI_TEXT);
    ui_text(c, s->x + 20, s->y + 16, "ArchieEmu", UI_HILITE);
    ui_text(c, s->x + 20, s->y + 16 + lh, "Choose the machine to start", UI_TEXT);
    static const char *machines[2] = { "Acorn Archimedes (ARM2, Arthur and RISC OS up to 3.11)",
                                       "Acorn Risc PC (ARM6/ARM7/StrongARM, RISC OS 3.5 to 3.7)" };
    for (int i = 0; i < 2; i++) {
        SDL_Rect r = s->radio[i];
        uint32_t col = s->have[i] ? UI_TEXT : UI_GREY;
        int k = ui_scale();
        ui_fill(c, r.x, r.y + 2 * k, 6 * k, 6 * k, UI_WHITE);
        ui_frame(c, r.x, r.y + 2 * k, 6 * k, 6 * k, col);
        if (s->rpc == i) ui_fill(c, r.x + 2 * k, r.y + 4 * k, 2 * k, 2 * k, UI_HILITE);
        ui_text(c, r.x + 9 * k, r.y, machines[i], col);
    }
    static const char *labels[4] = { "RISC OS", "Processor", "RAM", "VRAM" };
    for (int f = 0; f < 4; f++) {
        SDL_Rect r = s->field[f];
        int enabled = f == 0 || f == 2 || s->rpc;
        ui_text(c, s->x + 20, r.y + 5, labels[f], enabled ? UI_TEXT : UI_GREY);
        ui_fill(c, r.x, r.y, r.w, r.h, enabled ? UI_WHITE : UI_BG);
        ui_frame(c, r.x, r.y, r.w, r.h, UI_GREY);
        char v[200];
        field_value(s, f, v, sizeof v);
        int maxc = (r.w - 3 * cw) / cw;
        if ((int)strlen(v) > maxc) v[maxc] = 0;
        ui_text(c, r.x + 8, r.y + 5, v, enabled ? UI_TEXT : UI_GREY);
        if (enabled) ui_text(c, r.x + r.w - cw - 6, r.y + 5, "v", UI_TEXT);
    }
    SDL_Rect b = s->start, q = s->quit;
    ui_fill(c, b.x, b.y, b.w, b.h, s->nrom ? UI_HILITE : UI_GREY);
    ui_text(c, b.x + (b.w - ui_text_w("Start")) / 2, b.y + 5, "Start", UI_WHITE);
    ui_fill(c, q.x, q.y, q.w, q.h, UI_WHITE);
    ui_frame(c, q.x, q.y, q.w, q.h, UI_GREY);
    ui_text(c, q.x + (q.w - ui_text_w("Quit")) / 2, q.y + 5, "Quit", UI_TEXT);
    ui_text(c, s->x + 20, s->y + s->h - lh, "Enter: Start   Esc: Quit", UI_GREY);
}

static void splash_layout(Splash *s)
{
    int ww, wh, lh = ui_line_h(), cw = ui_char_w();
    SDL_GetRendererOutputSize(renderer, &ww, &wh);
    s->w = 60 * cw;
    if (s->w > ww - 8) s->w = ww - 8;
    s->h = 12 * lh + 40;
    s->x = (ww - s->w) / 2;
    s->y = (wh - s->h) / 2;
    if (s->y < 0) s->y = 0;
    int y = s->y + 16 + 3 * lh;
    for (int i = 0; i < 2; i++, y += lh) s->radio[i] = (SDL_Rect){ s->x + 20, y, s->w - 40, lh - 4 };
    y += lh / 2;
    int fx = s->x + 20 + 11 * cw;
    for (int f = 0; f < 4; f++, y += lh + 6) s->field[f] = (SDL_Rect){ fx, y, s->x + s->w - 20 - fx, lh };
    int bw = 10 * cw;
    s->quit = (SDL_Rect){ s->x + s->w - 20 - bw, s->y + s->h - 2 * lh - 8, bw, lh };
    s->start = (SDL_Rect){ s->quit.x - bw - 12, s->quit.y, bw, lh };
}

static void dark_backdrop(void *ctx)
{
    (void)ctx;
    SDL_SetRenderDrawColor(renderer, 0x30, 0x34, 0x38, 255);
    SDL_RenderClear(renderer);
}

static int hit(const SDL_Rect *r, int x, int y) { return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h; }

static void save_shot(const char *path);

/* 1 scelto, 0 rinuncia */
static int run_splash(Choice *ch)
{
    Splash s;
    memset(&s, 0, sizeof s);
    for (int i = 0; i < nroms; i++) s.have[roms[i].riscpc] = 1;
    s.rpc = setting_int("riscpc", 0) ? 1 : 0;
    if (!s.have[s.rpc]) s.rpc = !s.rpc;
    splash_fill(&s);
    ui_set_backdrop(dark_backdrop, NULL);
    const char *shot = getenv("ARCHIE_SDL_SHOT"), *shot_ms = getenv("ARCHIE_SDL_SHOT_MS");
    for (;;) {
        splash_layout(&s);
        dark_backdrop(NULL);
        UiCanvas *c = ui_begin();
        splash_paint(c, &s);
        ui_end();
        if (shot && (!shot_ms || atoi(shot_ms) == 0)) { save_shot(shot); return 0; }
        SDL_RenderPresent(renderer);
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 100)) continue;
        int start = 0;
        if (e.type == SDL_QUIT) return 0;
        if (e.type == SDL_KEYDOWN) {
            if (e.key.keysym.sym == SDLK_ESCAPE) return 0;
            if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) start = 1;
        }
        if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
            int mx = e.button.x, my = e.button.y;
            for (int i = 0; i < 2; i++)
                if (hit(&s.radio[i], mx, my) && s.have[i] && s.rpc != i) { s.rpc = i; splash_fill(&s); }
            if (hit(&s.quit, mx, my)) return 0;
            if (hit(&s.start, mx, my)) start = 1;
            for (int f = 0; f < 4; f++) {
                if (!hit(&s.field[f], mx, my) || (!s.rpc && (f == 1 || f == 3))) continue;
                UiItem items[128];
                int n = 0, cur = 0;
                char ram_labels[5][32];
                if (f == 0) {
                    for (int i = 0; i < s.nrom; i++) items[n++] = (UiItem){ roms[s.rom_idx[i]].label, 0 };
                    cur = s.rom_sel;
                } else if (f == 1) {
                    for (int i = 0; i < 3; i++) items[n++] = (UiItem){ cpu_names[i], 0 };
                    cur = s.cpu;
                } else if (f == 2) {
                    const int *sizes = s.rpc ? ram_rpc : ram_archie;
                    for (int i = 0; i < (s.rpc ? 5 : 3); i++) {
                        snprintf(ram_labels[i], sizeof ram_labels[i], "%d MB%s", sizes[i],
                                 !s.rpc && sizes[i] == 1 ? " (A3000)" : "");
                        items[n++] = (UiItem){ ram_labels[i], 0 };
                    }
                    cur = s.ram_sel;
                } else {
                    for (int i = 0; i < 3; i++) items[n++] = (UiItem){ vram_names[i], 0 };
                    cur = s.vram;
                }
                if (cur >= 0 && cur < n) items[cur].flags |= UI_CHECKED;
                int pick = ui_menu(items, n, s.field[f].x, s.field[f].y + s.field[f].h);
                if (pick >= 0) {
                    if (f == 0) s.rom_sel = pick;
                    else if (f == 1) s.cpu = pick;
                    else if (f == 2) s.ram_sel = pick;
                    else s.vram = pick;
                }
            }
        }
        if (!start || s.rom_sel < 0 || !s.nrom) continue;
        const Rom *rom = &roms[s.rom_idx[s.rom_sel]];
        /* lo StrongARM vuole RISC OS 3.7: con le ROM precedenti non parte (come sul vero) */
        if (s.rpc && s.cpu == 2 && rom->version < 370) {
            ui_message("The StrongARM needs RISC OS 3.7",
                       "RISC OS 3.5 and 3.6 do not run on a StrongARM, as on the real Risc PC.\n"
                       "Choose RISC OS 3.70 or 3.71, or an ARM610/ARM710 processor.");
            continue;
        }
        ch->riscpc = s.rpc;
        snprintf(ch->rom, sizeof ch->rom, "%s", rom->path);
        romlist_name(rom->version, ch->rom_name, sizeof ch->rom_name);
        ch->cpu = s.rpc ? s.cpu : 0;
        ch->ram_mb = s.rpc ? ram_rpc[s.ram_sel] : ram_archie[s.ram_sel];
        ch->vram_mb = s.rpc ? s.vram : 0;
        set_setting_int("riscpc", ch->riscpc);
        set_setting(ch->riscpc ? "rom_riscpc" : "rom_archie", ch->rom);
        set_setting_int(ch->riscpc ? "ram_riscpc" : "ram_archie", ch->ram_mb);
        if (ch->riscpc) { set_setting_int("cpu", ch->cpu); set_setting_int("vram", ch->vram_mb); }
        return 1;
    }
}

/* ------------------------------------------------------------------ */
/* macchina                                                           */
/* ------------------------------------------------------------------ */

static Fdc *floppies(void) { return app.rpc ? &app.r.sio.fdc.media : &app.a.fdc; }

static void mouse_button(int code, int down)
{
    if (!app.rpc) { kbd_key(&app.a.kbd, code, down); return; }
    int bit = code == 0x70 ? 4 : code == 0x71 ? 2 : 1;
    app.rpc_buttons = down ? app.rpc_buttons | bit : app.rpc_buttons & ~bit;
    riscpc_mouse_buttons(&app.r, app.rpc_buttons);
}

static void mouse_move(int dx, int dy)
{
    if (app.rpc) riscpc_mouse_move(&app.r, dx, dy);
    else         kbd_mouse_move(&app.a.kbd, dx, dy);
}

static void rpc_key(void *ctx, int code, int down) { riscpc_key((RiscPc *)ctx, (uint32_t)code, down); }

static void update_title(void)
{
    char t[256];
    snprintf(t, sizeof t, "%s - %s%s%s", app.rpc ? "Risc PC" : "Archimedes", app.rom_name,
             app.turbo ? "   [Turbo]" : "", app.captured ? "   (Ctrl+Alt: free mouse)" : "");
    if (strcmp(t, SDL_GetWindowTitle(window))) SDL_SetWindowTitle(window, t);
}

static void capture_mouse(int on)
{
    if (on == app.captured) return;
    app.captured = on;
    SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
    update_title();
}

static void release_modifiers(void)
{
    uint32_t t = SDL_GetTicks();
    keys_key(&keys, 0x10, 0, 0, 0, t);
    keys_key(&keys, 0x11, 0, 0, 0, t);
    keys_key(&keys, 0x11, 1, 0, 0, t);
    keys_key(&keys, 0x12, 0, 0, 0, t);
    keys_key(&keys, 0x12, 1, 0, 0, t);
    app.ctrl_down = app.alt_down = app.free_armed = 0;
}

/* dimensione a schermo: i modi da 256 righe hanno pixel alti il doppio */
static void display_size(int w, int h, int *dw, int *dh)
{
    *dw = w < 640 ? 640 : w;
    *dh = h <= 300 ? h * 2 : h;
}

static void fit_window(void)
{
    int dw, dh;
    display_size(app.disp_w ? app.disp_w : 640, app.disp_h ? app.disp_h : 256, &dw, &dh);
    SDL_Rect work;
    if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(window), &work) != 0) work = (SDL_Rect){ 0, 0, 1920, 1080 };
    int scale = 2;
    while (scale > 1 && (dw * scale > work.w * 9 / 10 || dh * scale + menubar_h() > work.h * 9 / 10)) scale--;
    SDL_SetWindowSize(window, dw * scale, dh * scale + menubar_h());
}

static void render_machine(void)
{
    int w = 0, h = 0;
    if (app.rpc) {
        riscpc_render(&app.r, app.pixels, BUF_W, &w, &h);
        app.border = riscpc_border_rgb(&app.r);
    } else {
        VidcTiming t;
        vidc_timing(&app.a.vidc, &t);
        if (t.valid) archie_render(&app.a, app.pixels, BUF_W, &w, &h);
        app.border = vidc_border_rgb(&app.a.vidc);
    }
    if (w <= 0 || h <= 0) { w = 640; h = app.rpc ? 480 : 256; memset(app.pixels, 0, (size_t)BUF_W * (size_t)h * 4); }
    if (w != app.disp_w || h != app.disp_h) {
        app.disp_w = w;
        app.disp_h = h;
        fit_window();
    }
    if (!screen_tex || w != tex_w || h != tex_h) {
        if (screen_tex) SDL_DestroyTexture(screen_tex);
        screen_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, w, h);
        tex_w = w;
        tex_h = h;
    }
    if (screen_tex) SDL_UpdateTexture(screen_tex, NULL, app.pixels, BUF_W * 4);
}

/* barra dei menu */
static const char *menu_titles[] = { "Disc", "Machine", "Mouse" };
#define NMENUS 3

static int menu_x(int i)
{
    int x = 8;
    for (int k = 0; k < i; k++) x += ui_text_w(menu_titles[k]) + 2 * ui_char_w();
    return x;
}

static void draw_frame(void *ctx)
{
    (void)ctx;
    int ww, wh, mb = menubar_h();
    SDL_GetRendererOutputSize(renderer, &ww, &wh);
    SDL_SetRenderDrawColor(renderer, (app.border >> 16) & 255, (app.border >> 8) & 255, app.border & 255, 255);
    SDL_RenderClear(renderer);
    int dw, dh;
    display_size(tex_w ? tex_w : 640, tex_h ? tex_h : 256, &dw, &dh);
    int aw = ww, ah = wh - mb;
    int sw = aw, sh = (int)((double)aw * dh / dw);
    if (sh > ah) { sh = ah; sw = (int)((double)ah * dw / dh); }
    app.img = (SDL_Rect){ (aw - sw) / 2, mb + (ah - sh) / 2, sw, sh };
    if (screen_tex) SDL_RenderCopy(renderer, screen_tex, NULL, &app.img);
    UiCanvas *c = ui_begin();
    ui_fill(c, 0, 0, c->w, mb, UI_BG);
    ui_fill(c, 0, mb - 1, c->w, 1, UI_GREY);
    for (int i = 0; i < NMENUS; i++) ui_text(c, menu_x(i), (mb - 8 * ui_scale()) / 2, menu_titles[i], UI_TEXT);
    const char *hint = app.captured ? "Ctrl+Alt: free the mouse" : "Click the screen to capture the mouse";
    int hx = c->w - ui_text_w(hint) - 10;
    if (hx > menu_x(NMENUS)) ui_text(c, hx, (mb - 8 * ui_scale()) / 2, hint, UI_GREY);
    ui_end();
}

static void save_shot(const char *path)
{
    int w, h;
    SDL_GetRendererOutputSize(renderer, &w, &h);
    uint32_t *buf = malloc((size_t)w * (size_t)h * 4);
    if (!buf) return;
    if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_RGB888, buf, w * 4) == 0) {
        png_write(path, buf, w, h, 1);
        fprintf(stderr, "finestra %dx%d in %s\n", w, h, path);
    }
    free(buf);
}

static void insert_floppy(int drive, const char *path)
{
    fdc_eject(floppies(), drive);
    if (fdc_insert(floppies(), drive, path)) {
        snprintf(app.floppy_name[drive], sizeof app.floppy_name[drive], "%s", base_name(path));
        snprintf(adf_dir, sizeof adf_dir, "%s", path);
        char *slash = strrchr(adf_dir, '/');
        if (slash) *slash = 0;
    } else {
        app.floppy_name[drive][0] = 0;
        char msg[PATH_MAX + 200];
        snprintf(msg, sizeof msg, "Unrecognised image:\n%s\n\nAn ADFS .adf image (800 KB or 640 KB) or an .hfe is "
                 "needed; on the Risc PC also ADFS F (1.6 MB) and DOS (720 KB, 1.44 MB).", path);
        ui_message(app.rpc ? "Risc PC" : "Archimedes", msg);
    }
}

static void choose_floppy(int drive)
{
    capture_mouse(0);
    char title[64], path[PATH_MAX];
    snprintf(title, sizeof title, "Floppy disc for drive %d", drive);
    if (ui_file(title, adf_dir, 0, NULL, path, sizeof path)) insert_floppy(drive, path);
    release_modifiers();
}

/* Disco fisso del Risc PC: un'immagine esistente (new_mb = 0) o una nuova
   di new_mb MB gia' formattata ADFS (hdformat.c); la macchina riparte. */
static void choose_hd(int new_mb)
{
    capture_mouse(0);
    char path[PATH_MAX];
    int ok = new_mb ? ui_file("New hard disc image", docs_dir, 1, "HardDisc4.hdf", path, sizeof path)
                    : ui_file("Hard disc image for drive :4", docs_dir, 0, NULL, path, sizeof path);
    release_modifiers();
    if (!ok) return;
    if (new_mb && !hdf_create(path, (uint32_t)new_mb, "HardDisc4", 0)) {
        ui_message("Risc PC", "Could not create the image.");
        return;
    }
    if (!riscpc_attach_hd(&app.r, path)) {
        ui_message("Risc PC", "Not a hard disc image.");
        return;
    }
    set_setting("hd_riscpc", path);
    riscpc_reset(&app.r);
}

static void toggle_turbo(void)
{
    app.turbo = !app.turbo;
    if (app.rpc) riscpc_set_mhz(&app.r, app.turbo ? 200.0 : app.mhz);
    else         archie_set_mhz(&app.a, app.turbo ? TURBO_MHZ : app.mhz);
    update_title();
}

enum {
    CMD_INS0 = 1, CMD_INS1, CMD_EJ0, CMD_EJ1, CMD_HD, CMD_HD64, CMD_HD128, CMD_HD256, CMD_HD512, CMD_HDREM,
    CMD_TURBO, CMD_SOUND, CMD_RAM1, CMD_RAM2, CMD_RAM4, CMD_RESET, CMD_CHOOSE, CMD_QUIT,
    CMD_RIGHTMENU, CMD_FREE
};

static void run_command(int cmd)
{
    switch (cmd) {
    case CMD_INS0: choose_floppy(0); break;
    case CMD_INS1: choose_floppy(1); break;
    case CMD_EJ0: case CMD_EJ1: {
        int d = cmd == CMD_EJ1;
        fdc_eject(floppies(), d);
        app.floppy_name[d][0] = 0;
        break;
    }
    case CMD_HD: choose_hd(0); break;
    case CMD_HD64: choose_hd(64); break;
    case CMD_HD128: choose_hd(128); break;
    case CMD_HD256: choose_hd(256); break;
    case CMD_HD512: choose_hd(512); break;
    case CMD_HDREM:
        if (ui_confirm("Risc PC", "Removing the hard disc restarts the machine.", "Restart")) {
            riscpc_detach_hd(&app.r);
            set_setting("hd_riscpc", "");
            riscpc_reset(&app.r);
        }
        break;
    case CMD_TURBO: toggle_turbo(); break;
    case CMD_SOUND: app.muted = !app.muted; break;
    case CMD_RAM1: case CMD_RAM2: case CMD_RAM4: {
        uint32_t mb = cmd == CMD_RAM1 ? 1 : cmd == CMD_RAM2 ? 2 : 4;
        if (mb != app.a.ram_size >> 20 &&
            ui_confirm("Archimedes", "Changing the RAM restarts the machine.", "Restart")) {
            archie_set_ram(&app.a, mb);
            set_setting_int("ram_archie", (int)mb);
        }
        break;
    }
    case CMD_RESET:
        if (app.rpc) riscpc_reset(&app.r);
        else         archie_reset(&app.a);
        break;
    case CMD_CHOOSE: app.choose = 1; app.quit = 1; break;
    case CMD_QUIT: app.quit = 1; break;
    case CMD_RIGHTMENU:
        app.right_menu = !app.right_menu;
        set_setting_int("right_menu", app.right_menu);
        break;
    case CMD_FREE: capture_mouse(0); break;
    default: break;
    }
}

static void open_menu(int which)
{
    UiItem it[24];
    int cmd[24], n = 0;
#define ADD(label, flags, c) do { it[n] = (UiItem){ label, flags }; cmd[n++] = c; } while (0)
#define SEP() ADD("", UI_SEPARATOR, 0)
    if (which == 0) {
        ADD("Insert floppy in :0...", 0, CMD_INS0);
        ADD("Insert floppy in :1...", 0, CMD_INS1);
        ADD("Eject :0", app.floppy_name[0][0] ? 0 : UI_DISABLED, CMD_EJ0);
        ADD("Eject :1", app.floppy_name[1][0] ? 0 : UI_DISABLED, CMD_EJ1);
        if (app.rpc) {
            SEP();
            ADD("Hard disc image (:4)...", 0, CMD_HD);
            ADD("New hard disc 64 MB...", 0, CMD_HD64);
            ADD("New hard disc 128 MB...", 0, CMD_HD128);
            ADD("New hard disc 256 MB...", 0, CMD_HD256);
            ADD("New hard disc 512 MB...", 0, CMD_HD512);
            ADD("Remove hard disc", app.r.sio.ide.fp ? 0 : UI_DISABLED, CMD_HDREM);
        }
    } else if (which == 1) {
        ADD("Turbo", app.turbo ? UI_CHECKED : 0, CMD_TURBO);
        ADD("Sound", app.muted ? 0 : UI_CHECKED, CMD_SOUND);
        if (!app.rpc) {
            SEP();
            uint32_t mb = app.a.ram_size >> 20;
            ADD("RAM 1 MB (A3000)", mb == 1 ? UI_CHECKED : 0, CMD_RAM1);
            ADD("RAM 2 MB", mb == 2 ? UI_CHECKED : 0, CMD_RAM2);
            ADD("RAM 4 MB", mb == 4 ? UI_CHECKED : 0, CMD_RAM4);
        }
        SEP();
        ADD("Reset", 0, CMD_RESET);
        ADD("Choose another machine...", 0, CMD_CHOOSE);
        SEP();
        ADD("Quit", 0, CMD_QUIT);
    } else {
        ADD("Right button is Menu", app.right_menu ? UI_CHECKED : 0, CMD_RIGHTMENU);
        ADD("Free the mouse (Ctrl+Alt)", app.captured ? 0 : UI_DISABLED, CMD_FREE);
    }
#undef SEP
#undef ADD
    int pick = ui_menu(it, n, menu_x(which) - 4, menubar_h());
    if (pick >= 0) run_command(cmd[pick]);
}

static void handle_key(const SDL_KeyboardEvent *k, int down)
{
    uint32_t t = SDL_GetTicks();
    SDL_Scancode sc = k->keysym.scancode;
    int is_ctrl = sc == SDL_SCANCODE_LCTRL || sc == SDL_SCANCODE_RCTRL;
    int is_alt = sc == SDL_SCANCODE_LALT || sc == SDL_SCANCODE_RALT;
    /* Ctrl+Alt premuti e rilasciati da soli liberano il mouse */
    if (is_ctrl) app.ctrl_down = down;
    if (is_alt) app.alt_down = down;
    if (down && (is_ctrl || is_alt) && app.ctrl_down && app.alt_down) app.free_armed = 1;
    else if (down && !is_ctrl && !is_alt) app.free_armed = 0;
    else if (!down && app.free_armed && (is_ctrl || is_alt)) { app.free_armed = 0; capture_mouse(0); }

    if (k->repeat) return;                                       /* la ripetizione la fa la macchina */
    int ext, vk = sdl_scancode_to_vk(sc, &ext);
    if (vk < 0) return;
    int modifier = vk == 0x10 || vk == 0x11 || vk == 0x12 || vk == 0x14;
    if (down && !modifier) {
        /* il tasto precedente aspettava un carattere che non e' arrivato:
           era un tasto morto (l'accento si compone col prossimo) */
        if (keys.char_expected && !app.got_text) keys_deadchar(&keys);
        app.got_text = 0;
    }
    keys_key(&keys, vk, ext, down, 0, t);
}

static void handle_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_QUIT: app.quit = 1; break;
    case SDL_KEYDOWN: handle_key(&e->key, 1); break;
    case SDL_KEYUP: handle_key(&e->key, 0); break;
    case SDL_TEXTINPUT: {
        app.got_text = 1;
        const char *s = e->text.text;
        uint32_t ch;
        while ((ch = sdl_utf8_next(&s))) keys_char(&keys, ch);
        break;
    }
    case SDL_MOUSEMOTION:
        if (app.captured && (e->motion.xrel || e->motion.yrel)) mouse_move(e->motion.xrel, -e->motion.yrel);
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        int down = e->type == SDL_MOUSEBUTTONDOWN;
        if (!app.captured) {
            if (!down || e->button.button != SDL_BUTTON_LEFT) break;
            if (e->button.y < menubar_h()) {
                for (int i = NMENUS - 1; i >= 0; i--)
                    if (e->button.x >= menu_x(i) - 4) { open_menu(i); break; }
            } else {
                capture_mouse(1);                                /* il primo clic cattura soltanto */
            }
            break;
        }
        /* come sull'Archimedes: sinistro Select, centrale Menu, destro Adjust */
        int code = -1;
        if (e->button.button == SDL_BUTTON_LEFT) code = 0x70;
        else if (e->button.button == SDL_BUTTON_MIDDLE) code = app.right_menu ? 0x72 : 0x71;
        else if (e->button.button == SDL_BUTTON_RIGHT) code = app.right_menu ? 0x71 : 0x72;
        if (code >= 0) mouse_button(code, down);
        break;
    }
    case SDL_DROPFILE: {
        int drive = (SDL_GetModState() & KMOD_SHIFT) ? 1 : 0;
        insert_floppy(drive, e->drop.file);
        SDL_free(e->drop.file);
        break;
    }
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) { capture_mouse(0); release_modifiers(); }
        break;
    default: break;
    }
}

/* dischetti cambiati da RISC OS (*HostFS_Insert) */
static void sync_floppy_names(void)
{
    for (int d = 0; d < 2; d++) {
        const FdcDrive *fd = &floppies()->drive[d];
        const char *name = fd->image ? base_name(fd->path) : "";
        if (strcmp(name, app.floppy_name[d])) snprintf(app.floppy_name[d], sizeof app.floppy_name[d], "%s", name);
    }
}

static void audio_open(void)
{
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof want);
    want.freq = ARCHIE_AUDIO_HZ;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio_dev) SDL_PauseAudioDevice(audio_dev, 0);
}

static void audio_pump(void)
{
    int16_t tmp[ARCHIE_AUDIO_HZ / 50 * 2];
    uint32_t got;
    for (;;) {
        got = app.rpc ? riscpc_audio_read(&app.r, tmp, ARCHIE_AUDIO_HZ / 50)
                      : archie_audio_read(&app.a, tmp, ARCHIE_AUDIO_HZ / 50);
        if (!got) break;
        if (!audio_dev) continue;
        if (SDL_GetQueuedAudioSize(audio_dev) / 4 + got > AUDIO_AHEAD) continue;   /* troppo avanti */
        if (app.muted) memset(tmp, 0, got * 4);
        SDL_QueueAudio(audio_dev, tmp, got * 4);
    }
}

static double now_s(void) { return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }

int main(int argc, char **argv)
{
    ArchieConfig cfg = { NULL, 4, NULL, { NULL, NULL }, 8, NULL };
    int force_rpc = 0, arm710 = 0, strongarm = 0, choose = 0, vram = -1;
    uint32_t ram = 0;
    double mhz = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) cfg.rom_path = argv[++i];
        else if (!strcmp(argv[i], "--floppy") && i + 1 < argc) cfg.floppy[0] = argv[++i];
        else if (!strcmp(argv[i], "--floppy2") && i + 1 < argc) cfg.floppy[1] = argv[++i];
        else if (!strcmp(argv[i], "--ram") && i + 1 < argc) ram = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--vram") && i + 1 < argc) vram = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mhz") && i + 1 < argc) mhz = atof(argv[++i]);
        else if (!strcmp(argv[i], "--cmos") && i + 1 < argc) cfg.cmos_path = argv[++i];
        else if (!strcmp(argv[i], "--hostfs") && i + 1 < argc) cfg.hostfs_dir = argv[++i];
        else if (!strcmp(argv[i], "--riscpc")) force_rpc = 1;
        else if (!strcmp(argv[i], "--arm710")) arm710 = 1;
        else if (!strcmp(argv[i], "--strongarm")) strongarm = 1;
        else if (!strcmp(argv[i], "--choose")) choose = 1;
        else if (argv[i][0] != '-' && !cfg.floppy[0]) cfg.floppy[0] = argv[i];
    }
    find_folders();
    settings_load();
    app.right_menu = setting_int("right_menu", 0);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    window = SDL_CreateWindow("ArchieEmu", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1024, 640,
                              SDL_WINDOW_RESIZABLE);
    if (!window) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, "0");
    ui_init(window, renderer);

    /* quale macchina: la finestra iniziale, oppure --rom (la macchina dal nome della ROM) */
    static char rom_buf[PATH_MAX], cmos_buf[PATH_MAX + 8];
    if (!cfg.rom_path && (choose || !cfg.floppy[0])) {
        scan_roms();
        if (!nroms) {
            char msg[3 * PATH_MAX];
            snprintf(msg, sizeof msg, "No RISC OS ROMs found.\n\nPut the ROM images, named by version (ROM311, ROM350, "
                     "ROM371...), in a folder called \"roms\" next to the program or in\n%s/roms", docs_dir);
            char roms_dir[PATH_MAX + 8];
            snprintf(roms_dir, sizeof roms_dir, "%s/roms", docs_dir);
            make_dirs(roms_dir);
            ui_set_backdrop(dark_backdrop, NULL);
            ui_message("ArchieEmu", msg);
            SDL_Quit();
            return 1;
        }
        Choice ch;
        memset(&ch, 0, sizeof ch);
        if (!run_splash(&ch)) { SDL_Quit(); return 0; }
        snprintf(rom_buf, sizeof rom_buf, "%s", ch.rom);
        cfg.rom_path = rom_buf;
        app.rpc = ch.riscpc;
        if (!ram) ram = (uint32_t)ch.ram_mb;
        if (vram < 0) vram = ch.vram_mb;
        arm710 = ch.cpu == 1;
        strongarm = ch.cpu == 2;
        snprintf(app.rom_name, sizeof app.rom_name, "%s", ch.rom_name);
    }
    if (!cfg.rom_path) {
        const char *r = setting("rom_archie");
        if (!r || !is_file(r)) { fprintf(stderr, "nessuna ROM: usare --rom\n"); return 1; }
        snprintf(rom_buf, sizeof rom_buf, "%s", r);
        cfg.rom_path = rom_buf;
    }
    if (!app.rom_name[0]) {
        int rpc = 0, v = romlist_version(base_name(cfg.rom_path), &rpc);
        app.rpc = rpc || force_rpc;
        if (v) romlist_name(v, app.rom_name, sizeof app.rom_name);
        else   snprintf(app.rom_name, sizeof app.rom_name, "%s", base_name(cfg.rom_path));
    }
    if (!cfg.cmos_path) {
        /* la CMOS si salva accanto alla ROM, una per ogni versione */
        snprintf(cmos_buf, sizeof cmos_buf, "%s.cmos", cfg.rom_path);
        cfg.cmos_path = cmos_buf;
    }
    for (int d = 0; d < 2; d++)
        if (cfg.floppy[d]) snprintf(app.floppy_name[d], sizeof app.floppy_name[d], "%s", base_name(cfg.floppy[d]));
    find_folder("ADF", adf_dir, sizeof adf_dir);
    static char hostfs_buf[PATH_MAX];
    if (!cfg.hostfs_dir) {
        find_folder("HostFS", hostfs_buf, sizeof hostfs_buf);
        cfg.hostfs_dir = hostfs_buf;
    }

    char err[300];
    if (app.rpc) {
        RiscPcConfig rcfg;
        memset(&rcfg, 0, sizeof rcfg);
        rcfg.rom_path = cfg.rom_path;
        rcfg.cmos_path = cfg.cmos_path;
        rcfg.ram_mb = ram ? ram : 16;
        rcfg.vram_mb = vram >= 0 ? (uint32_t)vram : 2;
        rcfg.arm710 = arm710;
        rcfg.strongarm = strongarm;
        rcfg.mhz = mhz > 0 ? mhz : strongarm ? 100 : arm710 ? 40 : 30;
        rcfg.hostfs_dir = cfg.hostfs_dir;
        app.mhz = rcfg.mhz;
        if (!riscpc_create(&app.r, &rcfg, err, sizeof err)) {
            ui_set_backdrop(dark_backdrop, NULL);
            ui_message("Risc PC", err);
            return 1;
        }
        for (int d = 0; d < 2; d++)
            if (cfg.floppy[d]) riscpc_insert_floppy(&app.r, d, cfg.floppy[d]);
        const char *hd = setting("hd_riscpc");
        if (hd && hd[0]) riscpc_attach_hd(&app.r, hd);
        keys_init_ps2(&keys, rpc_key, &app.r);
    } else {
        cfg.ram_mb = ram ? ram : 4;
        app.mhz = mhz > 0 ? mhz : 8;
        cfg.mhz = app.mhz;
        if (!archie_create(&app.a, &cfg, err, sizeof err)) {
            ui_set_backdrop(dark_backdrop, NULL);
            ui_message("Archimedes", err);
            return 1;
        }
        keys_init(&keys, &app.a.kbd);
    }

    ui_set_backdrop(draw_frame, NULL);
    render_machine();
    fit_window();
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    update_title();
    audio_open();
    SDL_StartTextInput();

    const char *shot = getenv("ARCHIE_SDL_SHOT");
    double shot_at = shot && getenv("ARCHIE_SDL_SHOT_MS") ? atof(getenv("ARCHIE_SDL_SHOT_MS")) / 1000.0 : -1;
    double frame = 1.0 / FRAME_HZ, next = now_s(), emulated = 0;
    while (!app.quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) handle_event(&e);
        double t = now_s();
        if (t < next) {
            SDL_Delay(1);
            continue;
        }
        keys_tick(&keys, SDL_GetTicks());
        if (app.rpc) riscpc_run(&app.r, ARC_MS(1000 / FRAME_HZ));
        else         archie_run(&app.a, ARC_MS(1000 / FRAME_HZ));
        emulated += frame;
        audio_pump();
        sync_floppy_names();
        render_machine();
        draw_frame(NULL);
        if (shot && shot_at >= 0 && emulated >= shot_at) { save_shot(shot); app.quit = 1; }
        SDL_RenderPresent(renderer);
        next += frame;
        if (next < t - 0.1) next = t;                             /* recupera dopo una pausa */
    }

    capture_mouse(0);
    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    if (app.rpc) riscpc_destroy(&app.r);                          /* salva la CMOS */
    else         archie_destroy(&app.a);
    ui_quit();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (app.choose) {
        /* un'istanza nuova con la finestra iniziale */
        char *args[] = { exe_path, "--choose", NULL };
        execv(exe_path, args);
    }
    return 0;
}
