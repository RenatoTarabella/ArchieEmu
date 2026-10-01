/*
 * archie_win32.c - Finestra della macchina Archimedes (Win32/GDI).
 *
 *   archie [--rom file] [--floppy disco.adf] [--floppy2 disco.adf]
 *          [--ram MB] [--mhz N] [--cmos file] [--hostfs cartella] [--right-menu]
 *
 * HostFS: la cartella "HostFS" accanto all'eseguibile (creata se manca)
 * compare in RISC OS come disco, con l'icona sulla barra.
 *
 * Tastiera: lettere, cifre, frecce, tasti funzione e modificatori vanno per
 * posizione (come servono ai giochi). I simboli invece seguono il carattere
 * prodotto dalla disposizione di Windows (spagnola, italiana...): si premono
 * sull'Archimedes UK i tasti che danno quel carattere, e le lettere accentate
 * si compongono con Alt + codice sul tastierino, come in RISC OS.
 * Mouse come su un Archimedes vero:
 *   tasto sinistro = Select, centrale = Menu, destro = Adjust
 *   (--right-menu o il menu Mouse: destro = Menu, per i mouse a due tasti).
 * Un clic nella finestra cattura il mouse (serve ai giochi come Zarch,
 * che leggono solo il movimento): Ctrl+Alt, premuti e rilasciati da soli,
 * lo liberano. I comandi dell'emulatore (dischetti, turbo, audio, reset)
 * stanno nella barra dei menu, come sul Mac: nessun tasto viene tolto a
 * RISC OS. Il reset si fa anche con Ctrl+Break (Ctrl+Pausa), come
 * sull'Archimedes. Trascinando un'immagine .adf/.hfe sulla finestra la si
 * inserisce nell'unita' 0 (con Shift nell'unita' 1).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "archie/archie.h"
#include "archie_keys.h"

#define FRAME_HZ   50
#define TURBO_MHZ  64.0
#define BUF_W      1024
#define BUF_H      768

/* ------------------------------------------------------------------ */
/* audio: waveOut, buffer da 20 ms                                    */
/* ------------------------------------------------------------------ */

#define AUDIO_BUFS    8
#define AUDIO_FRAMES  (ARCHIE_AUDIO_HZ / 50)
#define AUDIO_AHEAD   4                  /* buffer in coda oltre i quali si scarta */

typedef struct Audio {
    HWAVEOUT  out;
    WAVEHDR   hdr[AUDIO_BUFS];
    int16_t   data[AUDIO_BUFS][AUDIO_FRAMES * 2];
    int16_t   pending[AUDIO_FRAMES * 2 * 8];
    uint32_t  npending;
    int       ok, muted;
} Audio;

static Audio audio;

static void audio_open(void)
{
    WAVEFORMATEX f;
    memset(&f, 0, sizeof f);
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 2;
    f.nSamplesPerSec = ARCHIE_AUDIO_HZ;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 4;
    f.nAvgBytesPerSec = ARCHIE_AUDIO_HZ * 4;
    if (waveOutOpen(&audio.out, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return;
    for (int i = 0; i < AUDIO_BUFS; i++) {
        audio.hdr[i].lpData = (LPSTR)audio.data[i];
        audio.hdr[i].dwBufferLength = AUDIO_FRAMES * 4;
        waveOutPrepareHeader(audio.out, &audio.hdr[i], sizeof(WAVEHDR));
        audio.hdr[i].dwFlags |= WHDR_DONE;
    }
    audio.ok = 1;
}

static void audio_close(void)
{
    if (!audio.ok) return;
    waveOutReset(audio.out);
    for (int i = 0; i < AUDIO_BUFS; i++) waveOutUnprepareHeader(audio.out, &audio.hdr[i], sizeof(WAVEHDR));
    waveOutClose(audio.out);
    audio.ok = 0;
}

/* prende i campioni prodotti dalla macchina e li manda alla scheda audio */
static void audio_pump(Archie *a)
{
    int16_t tmp[AUDIO_FRAMES * 2];
    uint32_t got;
    while ((got = archie_audio_read(a, tmp, AUDIO_FRAMES)) > 0) {
        uint32_t room = (uint32_t)(sizeof audio.pending / sizeof audio.pending[0]) / 2 - audio.npending;
        if (got > room) got = room;                     /* troppo indietro: si scarta */
        memcpy(audio.pending + audio.npending * 2, tmp, got * 4);
        audio.npending += got;
    }
    if (!audio.ok) { audio.npending = 0; return; }
    int queued = 0;
    for (int i = 0; i < AUDIO_BUFS; i++) if (!(audio.hdr[i].dwFlags & WHDR_DONE)) queued++;
    while (audio.npending >= AUDIO_FRAMES) {
        int free_buf = -1;
        for (int i = 0; i < AUDIO_BUFS; i++) if (audio.hdr[i].dwFlags & WHDR_DONE) { free_buf = i; break; }
        if (free_buf < 0 || queued >= AUDIO_AHEAD) {
            /* coda piena: si tiene solo l'ultimo buffer, per non accumulare ritardo */
            if (audio.npending > AUDIO_FRAMES * 2) {
                memmove(audio.pending, audio.pending + (audio.npending - AUDIO_FRAMES) * 2, AUDIO_FRAMES * 4);
                audio.npending = AUDIO_FRAMES;
            }
            break;
        }
        WAVEHDR *h = &audio.hdr[free_buf];
        if (audio.muted) memset(audio.data[free_buf], 0, AUDIO_FRAMES * 4);
        else memcpy(audio.data[free_buf], audio.pending, AUDIO_FRAMES * 4);
        audio.npending -= AUDIO_FRAMES;
        memmove(audio.pending, audio.pending + AUDIO_FRAMES * 2, audio.npending * 4);
        h->dwFlags &= ~WHDR_DONE;
        waveOutWrite(audio.out, h, sizeof(WAVEHDR));
        queued++;
    }
}

typedef struct App {
    Archie    a;
    HWND      hwnd;
    uint32_t  pixels[BUF_W * BUF_H];
    int       disp_w, disp_h;        /* ultima immagine */
    double    mhz;
    int       turbo;
    char      title[256];
    char      rom_name[64];
    char      floppy_name[2][64];
    /* mouse */
    int       have_last;
    int       last_x, last_y;
    double    acc_x, acc_y;
    int       img_x, img_y, img_w, img_h;
    int       captured;              /* mouse catturato: movimento relativo */
    int       right_menu;            /* tasto destro = Menu invece di Adjust */
    int       free_armed;            /* Ctrl+Alt premuti da soli: al rilascio si libera il mouse */
} App;

static App app;

static double now_s(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '\\'), *t = strrchr(p, '/');
    if (t && (!s || t > s)) s = t;
    return s ? s + 1 : p;
}

static void update_title(void)
{
    char t[256];
    snprintf(t, sizeof t, "Archimedes - %s%s", app.rom_name, app.captured ? "   (Ctrl+Alt: free mouse)" : "");
    if (strcmp(t, app.title)) {
        strcpy(app.title, t);
        SetWindowTextA(app.hwnd, t);
    }
}

/* dimensione a schermo: i modi da 256 righe hanno pixel alti il doppio */
static void display_size(int w, int h, int *dw, int *dh)
{
    /* i modi larghi 320 o 160 pixel hanno pixel piu' larghi: la larghezza
       si porta a 640 senza toccare l'altezza (320x256 -> 640x512) */
    *dw = w < 640 ? 640 : w;
    *dh = h <= 300 ? h * 2 : h;
}

static void fit_window(void)
{
    int dw, dh;
    display_size(app.disp_w ? app.disp_w : 640, app.disp_h ? app.disp_h : 256, &dw, &dh);
    RECT work;
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    int scale = 2;
    while (scale > 1 && (dw * scale > (work.right - work.left) * 9 / 10 ||
                         dh * scale > (work.bottom - work.top) * 9 / 10)) scale--;
    RECT r = { 0, 0, dw * scale, dh * scale };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, TRUE);           /* con la barra dei menu */
    SetWindowPos(app.hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}

static void present(HDC dc)
{
    int w = 0, h = 0;
    VidcTiming t;
    vidc_timing(&app.a.vidc, &t);
    if (t.valid) archie_render(&app.a, app.pixels, BUF_W, &w, &h);
    if (w <= 0 || h <= 0) { w = 640; h = 256; memset(app.pixels, 0, sizeof app.pixels); }
    if (w != app.disp_w || h != app.disp_h) {
        app.disp_w = w;
        app.disp_h = h;
        fit_window();
    }

    RECT rc;
    GetClientRect(app.hwnd, &rc);
    int cw = rc.right, ch = rc.bottom, dw, dh;
    display_size(w, h, &dw, &dh);
    int sw = cw, sh = cw * dh / dw;
    if (sh > ch) { sh = ch; sw = ch * dw / dh; }
    int ox = (cw - sw) / 2, oy = (ch - sh) / 2;
    app.img_x = ox; app.img_y = oy; app.img_w = sw; app.img_h = sh;

    uint32_t border = vidc_border_rgb(&app.a.vidc);
    HBRUSH br = CreateSolidBrush(RGB((border >> 16) & 255, (border >> 8) & 255, border & 255));
    RECT bands[4] = { { 0, 0, cw, oy }, { 0, oy + sh, cw, ch }, { 0, oy, ox, oy + sh }, { ox + sw, oy, cw, oy + sh } };
    for (int i = 0; i < 4; i++) FillRect(dc, &bands[i], br);
    DeleteObject(br);

    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = BUF_W;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, ox, oy, sw, sh, 0, 0, w, h, app.pixels, &bi, DIB_RGB_COLORS, SRCCOPY);
}

static void insert_floppy(int drive, const char *path)
{
    fdc_eject(&app.a.fdc, drive);
    if (fdc_insert(&app.a.fdc, drive, path)) {
        snprintf(app.floppy_name[drive], sizeof app.floppy_name[drive], "%s", base_name(path));
    } else {
        app.floppy_name[drive][0] = 0;
        char msg[600];
        snprintf(msg, sizeof msg, "Immagine non riconosciuta:\n%s\n\nServe un'immagine ADFS .adf (800 KB o 640 KB) o un .hfe.", path);
        MessageBoxA(app.hwnd, msg, "Archimedes", MB_ICONWARNING);
    }
    update_title();
}

/* dischetti cambiati da RISC OS (*HostFS_Insert): il titolo li segue */
static void sync_floppy_names(void)
{
    for (int d = 0; d < 2; d++) {
        const FdcDrive *fd = &app.a.fdc.drive[d];
        const char *name = fd->image ? base_name(fd->path) : "";
        if (strcmp(name, app.floppy_name[d])) {
            snprintf(app.floppy_name[d], sizeof app.floppy_name[d], "%s", name);
            update_title();
        }
    }
}

static void mouse_button(int code, int down) { kbd_key(&app.a.kbd, code, down); }

static ArchieKeys keys;

static POINT client_center(void)
{
    RECT rc;
    GetClientRect(app.hwnd, &rc);
    POINT p = { rc.right / 2, rc.bottom / 2 };
    ClientToScreen(app.hwnd, &p);
    return p;
}

static void capture_mouse(int on)
{
    if (on == app.captured) return;
    app.captured = on;
    if (on) {
        RECT rc;
        GetClientRect(app.hwnd, &rc);
        MapWindowPoints(app.hwnd, NULL, (POINT *)&rc, 2);
        ClipCursor(&rc);
        POINT c = client_center();
        SetCursorPos(c.x, c.y);
    } else {
        ClipCursor(NULL);
    }
    update_title();
}

static const char *find_file(const char *const *rel, size_t n, char *buf, size_t size);

/* Ctrl+Alt+F9: scelta di un'immagine con la finestra di Windows, partendo dalla
   cartella ADF del progetto (o dall'ultima usata) */
static void choose_floppy(int drive)
{
    static char initial[MAX_PATH];
    static const char *dirs[] = { "ADF", "..\\ADF", "..\\..\\ADF", "..\\..\\..\\ADF" };
    char dir[MAX_PATH], full[MAX_PATH];
    if (!initial[0] && find_file(dirs, sizeof dirs / sizeof dirs[0], dir, sizeof dir) &&
        GetFullPathNameA(dir, MAX_PATH, full, NULL))
        snprintf(initial, sizeof initial, "%s", full);

    capture_mouse(0);
    char path[MAX_PATH] = "";
    char title[64];
    snprintf(title, sizeof title, "Dischetto per l'unita' %d", drive);
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = app.hwnd;
    ofn.lpstrFilter = "Dischetti (*.adf, *.adl, *.hfe)\0*.adf;*.adl;*.hfe\0Tutti i file\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = initial[0] ? initial : NULL;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    int ok = GetOpenFileNameA(&ofn);
    /* i rilasci di Ctrl e Shift sono andati alla finestra di dialogo */
    keys_key(&keys, VK_SHIFT, 0, 0, 0, GetTickCount());
    keys_key(&keys, VK_CONTROL, 0, 0, 0, GetTickCount());
    keys_key(&keys, VK_MENU, 0, 0, 0, GetTickCount());
    if (!ok) return;
    insert_floppy(drive, path);
    /* la prossima volta si riparte da questa cartella */
    snprintf(initial, sizeof initial, "%s", path);
    char *slash = strrchr(initial, '\\');
    if (slash) *slash = 0;
}

/* barra dei menu, come sul Mac */
enum { CMD_INSERT0 = 100, CMD_INSERT1, CMD_EJECT0, CMD_EJECT1, CMD_TURBO, CMD_SOUND, CMD_RESET, CMD_RIGHT_MENU, CMD_QUIT };

static HMENU build_menu(void)
{
    HMENU bar = CreateMenu(), disc = CreatePopupMenu(), mach = CreatePopupMenu(), mouse = CreatePopupMenu();
    AppendMenuA(disc, MF_STRING, CMD_INSERT0, "Insert floppy in :0...");
    AppendMenuA(disc, MF_STRING, CMD_INSERT1, "Insert floppy in :1...");
    AppendMenuA(disc, MF_SEPARATOR, 0, NULL);
    AppendMenuA(disc, MF_STRING, CMD_EJECT0, "Eject :0");
    AppendMenuA(disc, MF_STRING, CMD_EJECT1, "Eject :1");
    AppendMenuA(mach, MF_STRING, CMD_TURBO, "Turbo");
    AppendMenuA(mach, MF_STRING, CMD_SOUND, "Sound");
    AppendMenuA(mach, MF_SEPARATOR, 0, NULL);
    AppendMenuA(mach, MF_STRING, CMD_RESET, "Reset\tCtrl+Break");
    AppendMenuA(mach, MF_SEPARATOR, 0, NULL);
    AppendMenuA(mach, MF_STRING, CMD_QUIT, "Quit");
    AppendMenuA(mouse, MF_STRING, CMD_RIGHT_MENU, "Right button is Menu");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)disc, "&Disc");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)mach, "&Machine");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)mouse, "M&ouse");
    return bar;
}

static void menu_command(int id)
{
    switch (id) {
    case CMD_INSERT0: choose_floppy(0); break;
    case CMD_INSERT1: choose_floppy(1); break;
    case CMD_EJECT0: case CMD_EJECT1: {
        int d = id == CMD_EJECT1;
        fdc_eject(&app.a.fdc, d);
        app.floppy_name[d][0] = 0;
        break;
    }
    case CMD_TURBO:
        app.turbo = !app.turbo;
        archie_set_mhz(&app.a, app.turbo ? TURBO_MHZ : app.mhz);
        break;
    case CMD_SOUND: audio.muted = !audio.muted; break;
    case CMD_RESET: archie_reset(&app.a); break;
    case CMD_RIGHT_MENU: app.right_menu = !app.right_menu; break;
    case CMD_QUIT: DestroyWindow(app.hwnd); break;
    default: break;
    }
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        menu_command(LOWORD(wp));
        return 0;
    case WM_INITMENUPOPUP: {
        HMENU m = (HMENU)wp;
        CheckMenuItem(m, CMD_TURBO, app.turbo ? MF_CHECKED : MF_UNCHECKED);
        CheckMenuItem(m, CMD_SOUND, audio.muted ? MF_UNCHECKED : MF_CHECKED);
        CheckMenuItem(m, CMD_RIGHT_MENU, app.right_menu ? MF_CHECKED : MF_UNCHECKED);
        EnableMenuItem(m, CMD_EJECT0, app.a.fdc.drive[0].image ? MF_ENABLED : MF_GRAYED);
        EnableMenuItem(m, CMD_EJECT1, app.a.fdc.drive[1].image ? MF_ENABLED : MF_GRAYED);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        if (down && (lp & (1 << 30))) return 0;             /* ripetizione: la fa la macchina */
        /* Ctrl+Alt premuti e rilasciati senza altri tasti liberano il mouse
           (come nelle macchine virtuali); gli altri comandi stanno nel menu */
        if (down && (wp == VK_CONTROL || wp == VK_MENU))
            app.free_armed = GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) < 0 ? 1 : app.free_armed;
        else if (down)
            app.free_armed = 0;
        else if ((wp == VK_CONTROL || wp == VK_MENU) && app.free_armed) {
            app.free_armed = 0;
            capture_mouse(0);
        }
        keys_key(&keys, (int)wp, (int)((lp >> 24) & 1), down, 0, (uint32_t)GetMessageTime());
        return 0;                                              /* niente menu di sistema con Alt/F10 */
    }
    case WM_DEADCHAR:
    case WM_SYSDEADCHAR:
        keys_deadchar(&keys);
        return 0;
    case WM_CHAR:
    case WM_SYSCHAR:
        keys_char(&keys, (unsigned)wp);
        return 0;
    case WM_MOUSEMOVE: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        if (app.captured) {
            /* movimento relativo: si misura dal centro e si riporta il cursore li' */
            POINT c = client_center();
            ScreenToClient(hwnd, &c);
            int dx = x - c.x, dy = y - c.y;
            if (dx || dy) {
                kbd_mouse_move(&app.a.kbd, dx, -dy);
                POINT s = client_center();
                SetCursorPos(s.x, s.y);
            }
            return 0;
        }
        if (app.have_last && app.img_w > 0) {
            /* unita' del mouse Archimedes: circa 2 unita' OS per passo; lo
               schermo e' largo 1280 unita' OS */
            double k = 1280.0 / 2.0 / app.img_w;
            app.acc_x += (x - app.last_x) * k;
            app.acc_y -= (y - app.last_y) * k;
            int dx = (int)app.acc_x, dy = (int)app.acc_y;
            if (dx || dy) {
                kbd_mouse_move(&app.a.kbd, dx, dy);
                app.acc_x -= dx;
                app.acc_y -= dy;
            }
        }
        app.last_x = x; app.last_y = y; app.have_last = 1;
        return 0;
    }
    case WM_MOUSELEAVE:
        app.have_last = 0;
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(NULL); return TRUE; }   /* si vede il puntatore di RISC OS */
        break;
    case WM_LBUTTONDOWN:
        if (!app.captured) { capture_mouse(1); return 0; }   /* il primo clic cattura soltanto */
        mouse_button(0x70, 1);
        return 0;
    case WM_LBUTTONUP:   mouse_button(0x70, 0); return 0;
    case WM_KILLFOCUS:   capture_mouse(0); return 0;
    /* come sull'Archimedes: centrale = Menu, destro = Adjust (con --right-menu
       o Ctrl+Alt+F7 il destro fa Menu, per i mouse senza tasto centrale) */
    case WM_RBUTTONDOWN: mouse_button(app.right_menu ? 0x71 : 0x72, 1); return 0;
    case WM_RBUTTONUP:   mouse_button(app.right_menu ? 0x71 : 0x72, 0); return 0;
    case WM_MBUTTONDOWN: mouse_button(app.right_menu ? 0x72 : 0x71, 1); return 0;
    case WM_MBUTTONUP:   mouse_button(app.right_menu ? 0x72 : 0x71, 0); return 0;
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        char path[MAX_PATH];
        if (DragQueryFileA(drop, 0, path, MAX_PATH)) insert_floppy(GetKeyState(VK_SHIFT) < 0 ? 1 : 0, path);
        DragFinish(drop);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        present(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* cerca un file relativo alla cartella corrente o a quella dell'eseguibile */
static const char *find_file(const char *const *rel, size_t n, char *buf, size_t size)
{
    for (size_t i = 0; i < n; i++)
        if (GetFileAttributesA(rel[i]) != INVALID_FILE_ATTRIBUTES) { snprintf(buf, size, "%s", rel[i]); return buf; }
    char exe[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *slash = len ? strrchr(exe, '\\') : NULL;
    if (slash) {
        slash[1] = 0;
        for (size_t i = 0; i < n; i++) {
            snprintf(buf, size, "%s%s", exe, rel[i]);
            if (GetFileAttributesA(buf) != INVALID_FILE_ATTRIBUTES) return buf;
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    ArchieConfig cfg = { NULL, 4, NULL, { NULL, NULL }, 8, NULL };
    app.mhz = 8;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) cfg.rom_path = argv[++i];
        else if (!strcmp(argv[i], "--floppy") && i + 1 < argc) cfg.floppy[0] = argv[++i];
        else if (!strcmp(argv[i], "--floppy2") && i + 1 < argc) cfg.floppy[1] = argv[++i];
        else if (!strcmp(argv[i], "--ram") && i + 1 < argc) cfg.ram_mb = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mhz") && i + 1 < argc) app.mhz = atof(argv[++i]);
        else if (!strcmp(argv[i], "--cmos") && i + 1 < argc) cfg.cmos_path = argv[++i];
        else if (!strcmp(argv[i], "--hostfs") && i + 1 < argc) cfg.hostfs_dir = argv[++i];
        else if (!strcmp(argv[i], "--right-menu")) app.right_menu = 1;
        else if (argv[i][0] != '-' && !cfg.floppy[0]) cfg.floppy[0] = argv[i];   /* file aperto con l'eseguibile */
    }
    if (app.mhz <= 0) app.mhz = 8;
    cfg.mhz = app.mhz;

    static char rom_buf[MAX_PATH], cmos_buf[MAX_PATH];
    if (!cfg.rom_path) {
        static const char *roms[] = { "roms\\1. Major\\ROM311", "..\\roms\\1. Major\\ROM311",
                                      "..\\..\\roms\\1. Major\\ROM311", "..\\..\\..\\roms\\1. Major\\ROM311" };
        cfg.rom_path = find_file(roms, sizeof roms / sizeof roms[0], rom_buf, sizeof rom_buf);
        if (!cfg.rom_path) {
            MessageBoxA(NULL, "ROM non trovata: usa --rom percorso\\ROM311", "Archimedes", MB_ICONERROR);
            return 1;
        }
    }
    if (!cfg.cmos_path) {
        /* la CMOS si salva accanto alla ROM, una per ogni versione */
        snprintf(cmos_buf, sizeof cmos_buf, "%s.cmos", cfg.rom_path);
        cfg.cmos_path = cmos_buf;
    }
    static char hostfs_buf[MAX_PATH];
    if (!cfg.hostfs_dir) {
        /* la cartella HostFS: quella del progetto o accanto all'eseguibile */
        static const char *dirs[] = { "HostFS", "..\\HostFS", "..\\..\\HostFS", "..\\..\\..\\HostFS" };
        cfg.hostfs_dir = find_file(dirs, sizeof dirs / sizeof dirs[0], hostfs_buf, sizeof hostfs_buf);
        if (!cfg.hostfs_dir) {
            DWORD n = GetModuleFileNameA(NULL, hostfs_buf, MAX_PATH);
            char *slash = n ? strrchr(hostfs_buf, '\\') : NULL;
            if (slash) {
                strcpy(slash + 1, "HostFS");
                CreateDirectoryA(hostfs_buf, NULL);
                cfg.hostfs_dir = hostfs_buf;
            }
        }
    }
    snprintf(app.rom_name, sizeof app.rom_name, "%s", base_name(cfg.rom_path));
    if (!strcmp(app.rom_name, "ROM311")) snprintf(app.rom_name, sizeof app.rom_name, "RISC OS 3.11");
    for (int d = 0; d < 2; d++)
        if (cfg.floppy[d]) snprintf(app.floppy_name[d], sizeof app.floppy_name[d], "%s", base_name(cfg.floppy[d]));

    char err[300];
    if (!archie_create(&app.a, &cfg, err, sizeof err)) {
        MessageBoxA(NULL, err, "Archimedes", MB_ICONERROR);
        return 1;
    }

    HINSTANCE inst = GetModuleHandleA(NULL);
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(1));
    wc.lpszClassName = "ArchieWindow";
    RegisterClassA(&wc);
    app.hwnd = CreateWindowA(wc.lpszClassName, "Archimedes", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             1280, 1024, NULL, build_menu(), inst, NULL);
    DragAcceptFiles(app.hwnd, TRUE);
    fit_window();
    update_title();
    ShowWindow(app.hwnd, SW_SHOW);

    keys_init(&keys, &app.a.kbd);
    timeBeginPeriod(1);
    audio_open();
    double next = now_s();
    MSG msg;
    int running = 1;
    while (running) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = 0; break; }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!running) break;

        keys_tick(&keys, GetTickCount());
        archie_run(&app.a, ARC_MS(1000 / FRAME_HZ));
        audio_pump(&app.a);
        sync_floppy_names();
        HDC dc = GetDC(app.hwnd);
        present(dc);
        ReleaseDC(app.hwnd, dc);

        double t = now_s();
        next += 1.0 / FRAME_HZ;
        if (next < t - 0.1) next = t;                    /* recupera dopo una pausa */
        double wait = next - now_s();
        if (wait > 0) Sleep((DWORD)(wait * 1000));
    }
    audio_close();
    timeEndPeriod(1);
    archie_destroy(&app.a);
    return 0;
}
