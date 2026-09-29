/*
 * win32_main.c - Frontend a finestra (Win32/GDI, nessuna dipendenza).
 *
 *   armwin [--rom modulo] [--mode n] [--ram MB] [--mhz N] [--turbo]
 *
 * Il clock della CPU e' limitato (default 8 MHz, come un ARM2) eseguendo a
 * ogni frame da 1/50 s i cicli corrispondenti. F12 passa al turbo e
 * ritorno. Ctrl+V incolla il testo degli appunti come se fosse digitato.
 *
 * Selezione: trascinando col mouse si selezionano celle di testo (doppio
 * clic: una parola); Ctrl+C copia, il tasto destro copia o incolla. Il
 * testo viene riconosciuto dai pixel confrontandoli con il font.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "machine/machine.h"

#define FRAME_HZ     50
#define FLASH_CS     25            /* periodo del lampeggio (come *FX 9/10) */

typedef struct App {
    Machine   m;
    HWND      hwnd;
    uint32_t *pixels;
    int       pix_w, pix_h;
    double    mhz;
    int       turbo;
    char     *paste;               /* testo da incollare ancora da inviare */
    size_t    paste_pos, paste_len;
    uint64_t  stat_cycles;
    double    stat_time, stat_mhz;
    char      title[128];
    char      notice[64];          /* messaggio temporaneo nel titolo */
    double    notice_until;

    /* posizione dell'immagine nella finestra (letterbox) */
    int       img_x, img_y, img_w, img_h;

    /* selezione, in celle di carattere dello schermo */
    int       sel_valid, sel_dragging;
    int       sel_c0, sel_r0, sel_c1, sel_r1;   /* ancora e fine */
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

static void update_title(void)
{
    char t[128];
    if (app.notice[0] && now_s() < app.notice_until)
        snprintf(t, sizeof t, "ARM - BBC BASIC V   [%s]", app.notice);
    else if (app.turbo)
        snprintf(t, sizeof t, "ARM - BBC BASIC V   [TURBO: %.0f MHz, F12 per %.0f MHz]", app.stat_mhz, app.mhz);
    else
        snprintf(t, sizeof t, "ARM - BBC BASIC V   [ARM2 %.0f MHz, F12 per il turbo]", app.mhz);
    if (strcmp(t, app.title)) {
        strcpy(app.title, t);
        SetWindowTextA(app.hwnd, t);
    }
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
    RECT work;
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    int scale = 2;
    while (scale > 1 && (w * scale > (work.right - work.left) * 9 / 10 ||
                         h * scale > (work.bottom - work.top) * 9 / 10)) scale--;
    RECT r = { 0, 0, w * scale, h * scale };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(app.hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}

/* ------------------------------------------------------------------ */
/* selezione e appunti                                                */
/* ------------------------------------------------------------------ */

/* estremi della selezione in ordine di lettura */
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

/* inverte i colori delle celle selezionate: si vede anche solo dalla luminosita' */
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

/* coordinate della finestra -> cella di testo */
static void mouse_to_cell(int mx, int my, int *col, int *row)
{
    const Vdu *v = &app.m.vdu;
    int px = app.img_w ? (mx - app.img_x) * v->width / app.img_w : 0;
    int py = app.img_h ? (my - app.img_y) * v->height / app.img_h : 0;
    *col = px < 0 ? 0 : px / 8 >= v->cols ? v->cols - 1 : px / 8;
    *row = py < 0 ? 0 : py / 8 >= v->rows ? v->rows - 1 : py / 8;
}

static void set_notice(const char *msg)
{
    snprintf(app.notice, sizeof app.notice, "%s", msg);
    app.notice_until = now_s() + 2.0;
    update_title();
}

static void copy_selection(void)
{
    const Vdu *v = &app.m.vdu;
    int c0, r0, c1, r1;
    sel_range(&c0, &r0, &c1, &r1);
    size_t cap = (size_t)(r1 - r0 + 1) * (v->cols + 2) + 1, n = 0, chars = 0;
    wchar_t *text = malloc(cap * sizeof(wchar_t));
    if (!text) return;
    for (int row = r0; row <= r1; row++) {
        int from = row == r0 ? c0 : 0, to = row == r1 ? c1 : v->cols - 1;
        size_t line_start = n;
        for (int col = from; col <= to; col++) {
            int ch = vdu_char_at(v, col, row);
            text[n++] = (wchar_t)(ch ? ch : ' ');             /* Latin-1 = primi 256 Unicode */
        }
        while (n > line_start && text[n - 1] == L' ') n--;   /* spazi finali */
        chars += n - line_start;
        if (row < r1) { text[n++] = L'\r'; text[n++] = L'\n'; }
    }
    text[n++] = 0;

    if (OpenClipboard(app.hwnd)) {
        EmptyClipboard();
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n * sizeof(wchar_t));
        if (h) {
            memcpy(GlobalLock(h), text, n * sizeof(wchar_t));
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        }
        CloseClipboard();
        char msg[64];
        snprintf(msg, sizeof msg, "copiati %u caratteri", (unsigned)chars);
        set_notice(msg);
    }
    free(text);
}

/* doppio clic: la parola sotto il mouse */
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

static void present(HDC dc)
{
    Vdu *v = &app.m.vdu;
    if (v->width != app.pix_w || v->height != app.pix_h) {
        free(app.pixels);
        app.pixels = malloc((size_t)v->width * v->height * 4);
        app.pix_w = v->width;
        app.pix_h = v->height;
        if (!app.pixels) return;
    }
    uint64_t cs = kernel_time_cs(&app.m.kernel);
    int phase = (int)((cs / FLASH_CS) & 1);
    vdu_render(v, app.pixels, phase, !phase && !app.sel_valid);

    RECT rc;
    GetClientRect(app.hwnd, &rc);
    int cw = rc.right, ch = rc.bottom, dw, dh;
    display_size(&dw, &dh);
    /* scala mantenendo le proporzioni, bordo nel colore del border */
    int sw = cw, sh = cw * dh / dw;
    if (sh > ch) { sh = ch; sw = ch * dw / dh; }
    int ox = (cw - sw) / 2, oy = (ch - sh) / 2;
    app.img_x = ox; app.img_y = oy; app.img_w = sw; app.img_h = sh;
    if (app.sel_valid) highlight_selection();
    HBRUSH br = CreateSolidBrush(RGB((v->border >> 16) & 255, (v->border >> 8) & 255, v->border & 255));
    RECT bands[4] = { { 0, 0, cw, oy }, { 0, oy + sh, cw, ch }, { 0, oy, ox, oy + sh }, { ox + sw, oy, cw, oy + sh } };
    for (int i = 0; i < 4; i++) FillRect(dc, &bands[i], br);
    DeleteObject(br);

    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = v->width;
    bi.bmiHeader.biHeight = -v->height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, ox, oy, sw, sh, 0, 0, v->width, v->height, app.pixels, &bi, DIB_RGB_COLORS, SRCCOPY);
}

static void start_paste(void)
{
    if (!OpenClipboard(app.hwnd)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t *w = GlobalLock(h);
        if (w) {
            size_t n = wcslen(w);
            char *s = malloc(n * 3 + 1);          /* "..." puo' triplicare */
            size_t o = 0;
            if (s) {
                for (size_t i = 0; i < n; i++) {
                    wchar_t c = w[i];
                    if (c == L'\r') continue;
                    if (c == L'\n') s[o++] = 13;
                    else if (c == L'\t') s[o++] = ' ';
                    else if (c == 0xA0) s[o++] = ' ';                    /* spazio non separabile */
                    else if (c < 256) s[o++] = (char)c;
                    /* tipografia di chat e pagine web -> ASCII */
                    else if (c == 0x201C || c == 0x201D || c == 0x201E || c == 0x2033) s[o++] = '"';
                    else if (c == 0x2018 || c == 0x2019 || c == 0x201A || c == 0x2032) s[o++] = '\'';
                    else if (c == 0x2013 || c == 0x2014 || c == 0x2212) s[o++] = '-';
                    else if (c == 0x2026) { s[o++] = '.'; s[o++] = '.'; s[o++] = '.'; }
                    else if (c >= 0x2000 && c <= 0x200B) s[o++] = ' ';
                    else if (c == 0xFEFF) continue;                       /* BOM */
                    else s[o++] = '?';
                }
                free(app.paste);
                app.paste = s;
                app.paste_len = o;
                app.paste_pos = 0;
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
}

static void feed_paste(void)
{
    while (app.paste && app.paste_pos < app.paste_len && kernel_keys_pending(&app.m.kernel) < 200)
        kernel_key(&app.m.kernel, (uint8_t)app.paste[app.paste_pos++]);
    if (app.paste && app.paste_pos >= app.paste_len) { free(app.paste); app.paste = NULL; }
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CHAR:
        if (wp == 27) return 0;                           /* Escape da WM_KEYDOWN */
        if (wp == 22) { start_paste(); return 0; }        /* Ctrl+V */
        if (wp == 3 && app.sel_valid) {                   /* Ctrl+C con selezione */
            copy_selection();
            app.sel_valid = 0;
            return 0;
        }
        app.sel_valid = 0;
        if (wp < 256) kernel_key(&app.m.kernel, (uint8_t)wp);
        return 0;
    case WM_LBUTTONDOWN: {
        int col, row;
        mouse_to_cell((short)LOWORD(lp), (short)HIWORD(lp), &col, &row);
        app.sel_c0 = app.sel_c1 = col;
        app.sel_r0 = app.sel_r1 = row;
        app.sel_dragging = 1;
        app.sel_valid = 0;
        SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (app.sel_dragging) {
            int col, row;
            mouse_to_cell((short)LOWORD(lp), (short)HIWORD(lp), &col, &row);
            if (col != app.sel_c0 || row != app.sel_r0) app.sel_valid = 1;
            app.sel_c1 = col;
            app.sel_r1 = row;
        }
        return 0;
    case WM_LBUTTONUP:
        app.sel_dragging = 0;
        ReleaseCapture();
        return 0;
    case WM_LBUTTONDBLCLK: {
        int col, row;
        mouse_to_cell((short)LOWORD(lp), (short)HIWORD(lp), &col, &row);
        select_word(col, row);
        return 0;
    }
    case WM_RBUTTONUP:
        if (app.sel_valid) { copy_selection(); app.sel_valid = 0; }
        else start_paste();
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        switch (wp) {
        case VK_INSERT:
            if (GetKeyState(VK_CONTROL) < 0 && app.sel_valid) { copy_selection(); app.sel_valid = 0; }
            else if (GetKeyState(VK_SHIFT) < 0) start_paste();
            return 0;
        case VK_ESCAPE:
            if (app.sel_valid) { app.sel_valid = 0; return 0; }   /* prima annulla la selezione */
            free(app.paste); app.paste = NULL;
            kernel_key(&app.m.kernel, 27);
            return 0;
        case VK_F12:
            app.turbo = !app.turbo;
            update_title();
            return 0;
        case VK_LEFT:   kernel_key(&app.m.kernel, 0x8C); return 0;
        case VK_RIGHT:  kernel_key(&app.m.kernel, 0x8D); return 0;
        case VK_DOWN:   kernel_key(&app.m.kernel, 0x8E); return 0;
        case VK_UP:     kernel_key(&app.m.kernel, 0x8F); return 0;
        case VK_DELETE: kernel_key(&app.m.kernel, 127); return 0;
        case VK_HOME:   kernel_key(&app.m.kernel, 30); return 0;
        case VK_F10:    return 0;                          /* niente menu di sistema */
        default: break;
        }
        break;
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

static const char *find_rom(void)
{
    static char path[MAX_PATH];
    static const char *rel[] = { "third_party\\riscos\\BASIC", "..\\third_party\\riscos\\BASIC",
                                 "..\\..\\third_party\\riscos\\BASIC", "..\\..\\..\\third_party\\riscos\\BASIC" };
    for (size_t i = 0; i < sizeof rel / sizeof rel[0]; i++)
        if (GetFileAttributesA(rel[i]) != INVALID_FILE_ATTRIBUTES) return rel[i];
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *slash = n ? strrchr(exe, '\\') : NULL;
    if (slash) {
        slash[1] = 0;
        for (size_t i = 0; i < sizeof rel / sizeof rel[0]; i++) {
            snprintf(path, sizeof path, "%s%s", exe, rel[i]);
            if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return path;
        }
    }
    return rel[0];
}

int main(int argc, char **argv)
{
    MachineConfig cfg = { NULL, 4, -1, 0, NULL, 0 };
    char disc[MAX_PATH];
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
    if (!cfg.disc_dir) { machine_default_disc(cfg.rom_path, disc, sizeof disc); cfg.disc_dir = disc; }

    char err[256];
    if (!machine_create(&app.m, &cfg, err, sizeof err)) {
        MessageBoxA(NULL, err, "ARM", MB_ICONERROR);
        return 1;
    }

    HINSTANCE inst = GetModuleHandleA(NULL);
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.style = CS_DBLCLKS;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(1));
    wc.lpszClassName = "ARMEmuWindow";
    RegisterClassA(&wc);
    app.hwnd = CreateWindowA(wc.lpszClassName, "ARM", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             1280, 960, NULL, NULL, inst, NULL);
    fit_window();
    update_title();
    ShowWindow(app.hwnd, SW_SHOW);

    timeBeginPeriod(1);
    int last_w = app.m.vdu.width, last_h = app.m.vdu.height;
    double next = now_s(), stat_t0 = next;
    uint64_t stat_c0 = app.m.cpu.cycles;
    MSG msg;
    int running = 1;
    while (running) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = 0; break; }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!running) break;

        feed_paste();
        double frame = 1.0 / FRAME_HZ;
        if (app.turbo) {
            /* esegue finche' resta tempo nel frame, poi disegna */
            double until = now_s() + frame * 0.8;
            while (!app.m.cpu.halted && now_s() < until) {
                machine_run(&app.m, 200000);
                if (app.m.kernel.waiting) break;
            }
        } else {
            machine_run(&app.m, (uint64_t)(app.mhz * 1e6 / FRAME_HZ * (1.0 - machine_dma_fraction(&app.m))));
        }
        if (app.m.cpu.halted) break;

        if (app.m.vdu.width != last_w || app.m.vdu.height != last_h) {
            last_w = app.m.vdu.width;
            last_h = app.m.vdu.height;
            app.sel_valid = 0;
            fit_window();
        }
        HDC dc = GetDC(app.hwnd);
        present(dc);
        ReleaseDC(app.hwnd, dc);

        double t = now_s();
        if (t - stat_t0 >= 1.0) {
            app.stat_mhz = (double)(app.m.cpu.cycles - stat_c0) / (t - stat_t0) / 1e6;
            stat_t0 = t;
            stat_c0 = app.m.cpu.cycles;
            update_title();
        }
        if (app.notice[0] && t >= app.notice_until) { app.notice[0] = 0; update_title(); }
        next += frame;
        if (next < t - 0.1) next = t;                    /* recupera dopo una pausa */
        double wait = next - now_s();
        if (wait > 0) Sleep((DWORD)(wait * 1000));
    }
    timeEndPeriod(1);
    machine_destroy(&app.m);
    return 0;
}
