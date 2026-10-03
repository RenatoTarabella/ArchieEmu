/*
 * splash_win32.c - Finestra iniziale per scegliere la macchina (vedi splash_win32.h)
 */
#include "splash_win32.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* ROM disponibili                                                    */
/* ------------------------------------------------------------------ */

typedef struct RomEntry {
    char path[MAX_PATH];
    char label[128];
    int  version, riscpc;
} RomEntry;

static RomEntry roms[128];
static int nroms;

int splash_rom_version(const char *path, int *riscpc)
{
    const char *s = strrchr(path, '\\'), *t = strrchr(path, '/');
    if (t && (!s || t > s)) s = t;
    s = s ? s + 1 : path;
    *riscpc = 0;
    if (_strnicmp(s, "ROM", 3) || !isdigit((unsigned char)s[3]) || !isdigit((unsigned char)s[4]) ||
        !isdigit((unsigned char)s[5]))
        return 0;
    int v = (s[3] - '0') * 100 + (s[4] - '0') * 10 + (s[5] - '0');
    const char *rest = s + 6;
    if (!_stricmp(rest, ".cmos") || strstr(rest, ".cmos")) return 0;
    if (v >= 350) {
        /* 3.50-3.80 per ARM6/ARM7; la versione StrongARM e RISC OS 4+ non ancora */
        if (v > 380 || !_stricmp(rest, ".SA")) return 0;
        *riscpc = 1;
    }
    return v;
}

void splash_rom_name(int v, char *out, size_t size)
{
    if (v < 200) snprintf(out, size, "Arthur %d.%02d", v / 100, v % 100);
    else         snprintf(out, size, "RISC OS %d.%02d", v / 100, v % 100);
}

static int cmp_rom(const void *a, const void *b)
{
    const RomEntry *x = a, *y = b;
    if (x->version != y->version) return x->version - y->version;
    return strcmp(x->path, y->path);
}

static void scan_dir(const char *dir, const char *sub)
{
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s\\%s\\ROM*", dir, sub);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        int rpc, v = splash_rom_version(fd.cFileName, &rpc);
        if (!v || nroms >= 128) continue;
        RomEntry *e = &roms[nroms++];
        snprintf(e->path, sizeof e->path, "%s\\%s\\%s", dir, sub, fd.cFileName);
        char name[64];
        splash_rom_name(v, name, sizeof name);
        const char *extra = fd.cFileName + 6;
        snprintf(e->label, sizeof e->label, "%s%s%s   (%s)", name, *extra ? " " : "", extra, sub);
        e->version = v;
        e->riscpc = rpc;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* la cartella roms: accanto alla cartella corrente o all'eseguibile, o piu' su */
static int find_roms_dir(char *out, size_t size)
{
    static const char *rel[] = { "roms", "..\\roms", "..\\..\\roms", "..\\..\\..\\roms" };
    char base[2][MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, base[0]);
    DWORD n = GetModuleFileNameA(NULL, base[1], MAX_PATH);
    char *slash = n ? strrchr(base[1], '\\') : NULL;
    if (slash) *slash = 0;
    for (int b = 0; b < 2; b++) {
        for (size_t i = 0; i < sizeof rel / sizeof rel[0]; i++) {
            char p[MAX_PATH], full[MAX_PATH];
            snprintf(p, sizeof p, "%s\\%s", base[b], rel[i]);
            DWORD a = GetFileAttributesA(p);
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
                if (!GetFullPathNameA(p, (DWORD)size, full, NULL)) continue;
                snprintf(out, size, "%s", full);
                return 1;
            }
        }
    }
    return 0;
}

static void scan_roms(void)
{
    char dir[MAX_PATH];
    nroms = 0;
    if (!find_roms_dir(dir, sizeof dir)) return;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        if (strstr(fd.cFileName, "NCOS")) continue;            /* Network Computer: altro hardware */
        scan_dir(dir, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(roms, (size_t)nroms, sizeof roms[0], cmp_rom);
}

/* ------------------------------------------------------------------ */
/* impostazioni ricordate                                             */
/* ------------------------------------------------------------------ */

static char ini_path[MAX_PATH];

static void ini_init(void)
{
    DWORD n = GetModuleFileNameA(NULL, ini_path, MAX_PATH);
    char *slash = n ? strrchr(ini_path, '\\') : NULL;
    if (slash) strcpy(slash + 1, "ArchieEmu.ini");
    else       strcpy(ini_path, "ArchieEmu.ini");
}

static int ini_int(const char *key, int def)
{
    return (int)GetPrivateProfileIntA("Machine", key, def, ini_path);
}

static void ini_str(const char *key, char *out, DWORD size)
{
    GetPrivateProfileStringA("Machine", key, "", out, size, ini_path);
}

static void ini_put_int(const char *key, int v)
{
    char b[32];
    snprintf(b, sizeof b, "%d", v);
    WritePrivateProfileStringA("Machine", key, b, ini_path);
}

/* ------------------------------------------------------------------ */
/* la finestra                                                        */
/* ------------------------------------------------------------------ */

enum { ID_ARCHIE = 200, ID_RISCPC, ID_ROM, ID_CPU, ID_RAM, ID_VRAM, ID_VRAM_LABEL };

static const int ram_archie[] = { 1, 2, 4 };
static const int ram_riscpc[] = { 4, 8, 16, 32, 64 };

typedef struct Splash {
    HWND  wnd, archie, riscpc, rom, cpu, ram, vram, vram_label, start;
    HFONT font, big;
    int   done, ok, riscpc_on;
    int   rom_index[128];              /* voce della lista -> indice in roms[] */
    MachineChoice *c;
} Splash;

static Splash sp;

static void fill_lists(void)
{
    int rpc = sp.riscpc_on;
    char want[MAX_PATH];
    ini_str(rpc ? "rom_riscpc" : "rom_archie", want, sizeof want);

    SendMessageA(sp.rom, CB_RESETCONTENT, 0, 0);
    int sel = -1, n = 0, fallback = -1;
    for (int i = 0; i < nroms; i++) {
        if (roms[i].riscpc != rpc) continue;
        SendMessageA(sp.rom, CB_ADDSTRING, 0, (LPARAM)roms[i].label);
        sp.rom_index[n] = i;
        if (!_stricmp(roms[i].path, want)) sel = n;
        /* predefinite: 3.11 sull'Archimedes, 3.50 sul Risc PC */
        if (roms[i].version == (rpc ? 350 : 311) && fallback < 0) fallback = n;
        n++;
    }
    if (sel < 0) sel = fallback >= 0 ? fallback : n - 1;
    SendMessageA(sp.rom, CB_SETCURSEL, (WPARAM)sel, 0);

    SendMessageA(sp.cpu, CB_RESETCONTENT, 0, 0);
    if (rpc) {
        SendMessageA(sp.cpu, CB_ADDSTRING, 0, (LPARAM)"ARM610, 30 MHz (Risc PC 600)");
        SendMessageA(sp.cpu, CB_ADDSTRING, 0, (LPARAM)"ARM710, 40 MHz (Risc PC 700)");
        SendMessageA(sp.cpu, CB_SETCURSEL, ini_int("cpu", CPU_ARM610) == CPU_ARM710 ? 1 : 0, 0);
    } else {
        SendMessageA(sp.cpu, CB_ADDSTRING, 0, (LPARAM)"ARM2, 8 MHz");
        SendMessageA(sp.cpu, CB_SETCURSEL, 0, 0);
    }

    SendMessageA(sp.ram, CB_RESETCONTENT, 0, 0);
    const int *ram = rpc ? ram_riscpc : ram_archie;
    int nram = rpc ? 5 : 3, want_ram = ini_int(rpc ? "ram_riscpc" : "ram_archie", rpc ? 16 : 4), rsel = nram - 1;
    if (rpc && want_ram == 0) want_ram = 16;
    for (int i = 0; i < nram; i++) {
        char b[32];
        snprintf(b, sizeof b, "%d MB%s", ram[i], !rpc && ram[i] == 1 ? " (A3000)" : "");
        SendMessageA(sp.ram, CB_ADDSTRING, 0, (LPARAM)b);
        if (ram[i] == want_ram) rsel = i;
    }
    SendMessageA(sp.ram, CB_SETCURSEL, (WPARAM)rsel, 0);

    SendMessageA(sp.vram, CB_RESETCONTENT, 0, 0);
    SendMessageA(sp.vram, CB_ADDSTRING, 0, (LPARAM)"None (screen in DRAM)");
    SendMessageA(sp.vram, CB_ADDSTRING, 0, (LPARAM)"1 MB");
    SendMessageA(sp.vram, CB_ADDSTRING, 0, (LPARAM)"2 MB");
    int v = ini_int("vram", 2);
    SendMessageA(sp.vram, CB_SETCURSEL, (WPARAM)(v >= 0 && v <= 2 ? v : 2), 0);
    EnableWindow(sp.vram, rpc);
    EnableWindow(sp.vram_label, rpc);
    EnableWindow(sp.cpu, rpc);
    EnableWindow(sp.start, n > 0);
}

static void choose(int rpc)
{
    sp.riscpc_on = rpc;
    SendMessageA(sp.archie, BM_SETCHECK, rpc ? BST_UNCHECKED : BST_CHECKED, 0);
    SendMessageA(sp.riscpc, BM_SETCHECK, rpc ? BST_CHECKED : BST_UNCHECKED, 0);
    fill_lists();
}

static void splash_accept(void)
{
    MachineChoice *c = sp.c;
    int item = (int)SendMessageA(sp.rom, CB_GETCURSEL, 0, 0);
    if (item < 0) return;
    const RomEntry *r = &roms[sp.rom_index[item]];
    c->riscpc = sp.riscpc_on;
    snprintf(c->rom, sizeof c->rom, "%s", r->path);
    splash_rom_name(r->version, c->rom_name, sizeof c->rom_name);
    int ri = (int)SendMessageA(sp.ram, CB_GETCURSEL, 0, 0);
    c->ram_mb = c->riscpc ? ram_riscpc[ri < 0 ? 2 : ri] : ram_archie[ri < 0 ? 2 : ri];
    c->cpu = c->riscpc ? ((int)SendMessageA(sp.cpu, CB_GETCURSEL, 0, 0) == 1 ? CPU_ARM710 : CPU_ARM610) : CPU_ARM2;
    c->vram_mb = c->riscpc ? (int)SendMessageA(sp.vram, CB_GETCURSEL, 0, 0) : 0;

    ini_put_int("riscpc", c->riscpc);
    WritePrivateProfileStringA("Machine", c->riscpc ? "rom_riscpc" : "rom_archie", c->rom, ini_path);
    ini_put_int(c->riscpc ? "ram_riscpc" : "ram_archie", c->ram_mb);
    if (c->riscpc) { ini_put_int("cpu", c->cpu); ini_put_int("vram", c->vram_mb); }
    sp.ok = 1;
    sp.done = 1;
}

static LRESULT CALLBACK splash_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_ARCHIE: choose(0); return 0;
        case ID_RISCPC: choose(1); return 0;
        case IDOK:      splash_accept(); return 0;
        case IDCANCEL:  sp.done = 1; return 0;
        default: break;
        }
        break;
    case DM_GETDEFID:
        return MAKELRESULT(IDOK, DC_HASDEFID);
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_CLOSE:
        sp.done = 1;
        return 0;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND add(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id, HFONT font)
{
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, sp.wnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

int splash_choose(HINSTANCE inst, MachineChoice *c)
{
    ini_init();
    scan_roms();
    int have[2] = { 0, 0 };
    for (int i = 0; i < nroms; i++) have[roms[i].riscpc] = 1;
    if (!have[0] && !have[1]) {
        MessageBoxA(NULL, "No RISC OS ROMs found.\n\nPut the ROM images in a folder called \"roms\" "
                    "next to the program (for example roms\\1. Major\\ROM311).", "ArchieEmu", MB_ICONERROR);
        return 0;
    }

    memset(&sp, 0, sizeof sp);
    sp.c = c;
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = splash_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(1));
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = "ArchieSplash";
    RegisterClassA(&wc);

    HDC sdc = GetDC(NULL);
    int dpi = GetDeviceCaps(sdc, LOGPIXELSY);
    ReleaseDC(NULL, sdc);
#define S(v) MulDiv((v), dpi, 96)
    sp.font = CreateFontA(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    sp.big = CreateFontA(-S(24), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");

    RECT r = { 0, 0, S(470), S(330) };
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    RECT work;
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    sp.wnd = CreateWindowExA(0, wc.lpszClassName, "ArchieEmu", style,
                             work.left + (work.right - work.left - ww) / 2, work.top + (work.bottom - work.top - wh) / 2,
                             ww, wh, NULL, NULL, inst, NULL);

    add("STATIC", "ArchieEmu", 0, S(20), S(14), S(300), S(34), -1, sp.big);
    add("STATIC", "Choose the machine to start", 0, S(22), S(50), S(400), S(20), -1, sp.font);
    sp.archie = add("BUTTON", "Acorn Archimedes   (ARM2, Arthur and RISC OS up to 3.11)",
                    BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP, S(22), S(82), S(430), S(22), ID_ARCHIE, sp.font);
    sp.riscpc = add("BUTTON", "Acorn Risc PC   (ARM610/ARM710, RISC OS 3.5 to 3.8)",
                    BS_AUTORADIOBUTTON, S(22), S(106), S(430), S(22), ID_RISCPC, sp.font);
    EnableWindow(sp.archie, have[0]);
    EnableWindow(sp.riscpc, have[1]);

    int y = S(144), lx = S(22), cx = S(120), cw = S(328), row = S(32);
    add("STATIC", "RISC OS", 0, lx, y + S(3), S(95), S(20), -1, sp.font);
    sp.rom = add("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP, cx, y, cw, S(300), ID_ROM, sp.font);
    y += row;
    add("STATIC", "Processor", 0, lx, y + S(3), S(95), S(20), -1, sp.font);
    sp.cpu = add("COMBOBOX", "", CBS_DROPDOWNLIST | WS_TABSTOP, cx, y, cw, S(200), ID_CPU, sp.font);
    y += row;
    add("STATIC", "RAM", 0, lx, y + S(3), S(95), S(20), -1, sp.font);
    sp.ram = add("COMBOBOX", "", CBS_DROPDOWNLIST | WS_TABSTOP, cx, y, S(160), S(200), ID_RAM, sp.font);
    y += row;
    sp.vram_label = add("STATIC", "VRAM", 0, lx, y + S(3), S(95), S(20), ID_VRAM_LABEL, sp.font);
    sp.vram = add("COMBOBOX", "", CBS_DROPDOWNLIST | WS_TABSTOP, cx, y, S(160), S(200), ID_VRAM, sp.font);

    sp.start = add("BUTTON", "Start", BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP, S(258), S(286), S(92), S(28), IDOK, sp.font);
    add("BUTTON", "Quit", BS_PUSHBUTTON | WS_TABSTOP, S(358), S(286), S(92), S(28), IDCANCEL, sp.font);
#undef S

    int rpc = ini_int("riscpc", 0) ? 1 : 0;
    if (!have[rpc]) rpc = !rpc;
    choose(rpc);
    ShowWindow(sp.wnd, SW_SHOW);
    SetForegroundWindow(sp.wnd);
    SetFocus(sp.start);

    MSG msg;
    while (!sp.done && GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageA(sp.wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    DestroyWindow(sp.wnd);
    DeleteObject(sp.font);
    DeleteObject(sp.big);
    return sp.ok;
}
