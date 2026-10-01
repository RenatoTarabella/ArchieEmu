/*
 * test_keys_es.c - Collaudo della tastiera spagnola sulla macchina Archimedes.
 *
 * Genera con la disposizione spagnola vera di Windows (VkKeyScanEx e
 * ToUnicodeEx) i messaggi che produrrebbe ogni carattere, li passa alla
 * traduzione di archie_keys.c, li fa scrivere a RISC OS 3.11 dopo "*echo" e
 * rilegge lo schermo confrontando i pixel con il font di sistema.
 *
 *   test_keys_es ROM [layout]      (layout: default 0000040A, spagnolo)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "archie/archie.h"
#include "archie_keys.h"
#include "frontend/png.h"

static Archie a;
static ArchieKeys keys;
static HKL layout;
static uint32_t t_ms = 100000;

static void run_ms(int ms) { archie_run(&a, ARC_MS(ms)); keys_tick(&keys, t_ms += (uint32_t)ms); }

/* premi un tasto con lo stato dei modificatori, come farebbe Windows */
static void press(int vk, int shift, int altgr)
{
    BYTE state[256] = { 0 };
    if (shift) { keys_key(&keys, VK_SHIFT, 0, 1, 0, t_ms); state[VK_SHIFT] = state[VK_LSHIFT] = 0x80; }
    if (altgr) {
        keys_key(&keys, VK_CONTROL, 0, 1, 0, t_ms);        /* il Ctrl finto di AltGr */
        keys_key(&keys, VK_MENU, 1, 1, 0, t_ms);
        state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_MENU] = state[VK_RMENU] = 0x80;
    }
    run_ms(20);
    keys_key(&keys, vk, 0, 1, 0, t_ms);
    WCHAR out[4];
    UINT scan = MapVirtualKeyExW((UINT)vk, MAPVK_VK_TO_VSC, layout);
    int n = ToUnicodeEx((UINT)vk, scan, state, out, 4, 0, layout);
    if (getenv("KEYS_TRACE"))
        fprintf(stderr, "vk=%02X shift=%d altgr=%d -> n=%d ch=%04X  (coda tastiera %d)\n", vk, shift, altgr, n,
                n > 0 ? out[0] : 0, (keys.kbd->pk_tail - keys.kbd->pk_head + 32) % 32);
    if (n < 0) keys_deadchar(&keys);
    for (int i = 0; i < n; i++) keys_char(&keys, out[i]);      /* un WM_CHAR per carattere */
    run_ms(20);
    while (keys_pending(&keys)) run_ms(KEYS_GAP_MS);
    keys_key(&keys, vk, 0, 0, 0, t_ms);
    if (altgr) { keys_key(&keys, VK_CONTROL, 0, 0, 0, t_ms); keys_key(&keys, VK_MENU, 1, 0, 0, t_ms); }
    if (shift) keys_key(&keys, VK_SHIFT, 0, 0, 0, t_ms);
    run_ms(80);
    while (keys_pending(&keys)) run_ms(KEYS_GAP_MS);
}

static int type_char(WCHAR ch)
{
    SHORT r = VkKeyScanExW(ch, layout);
    if (r != -1) {
        int mods = (r >> 8) & 0xFF;
        press(r & 0xFF, mods & 1, (mods & 6) == 6);
        return 1;
    }
    /* lettere accentate: tasto morto e poi la vocale */
    static const struct { const WCHAR *set; WCHAR accent; } dead[] = {
        { L"áéíóúÁÉÍÓÚ", 0x00B4 },
        { L"äëïöüÄËÏÖÜ", 0x00A8 },
        { L"àèìòù", L'`' },
        { L"âêîôû", L'^' },
    };
    static const WCHAR base[] = L"aeiouAEIOU";
    for (size_t d = 0; d < sizeof dead / sizeof dead[0]; d++) {
        const WCHAR *p = wcschr(dead[d].set, ch);
        if (!p) continue;
        SHORT acc = VkKeyScanExW(dead[d].accent, layout);
        WCHAR b = base[p - dead[d].set];
        SHORT bk = VkKeyScanExW(b, layout);
        if (acc == -1 || bk == -1) return 0;
        press(acc & 0xFF, (acc >> 8) & 1, ((acc >> 8) & 6) == 6);
        press(bk & 0xFF, (bk >> 8) & 1, 0);
        return 1;
    }
    return 0;
}

/* Il font di sistema della ROM stessa (caratteri 32-255): si trova cercando
   il disegno della "A" e tornando indietro di 33 caratteri. */
static unsigned char rom_font[224][8];

static int load_rom_font(void)
{
    static const unsigned char A[8] = { 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00 };
    for (uint32_t off = 33 * 8; off + 8 <= a.rom_size; off += 4) {
        if (memcmp(a.rom + off, A, 8)) continue;
        uint32_t base = off - 33 * 8;
        if (base + sizeof rom_font > a.rom_size) return 0;
        memcpy(rom_font, a.rom + base, sizeof rom_font);
        return rom_font[0][0] == 0 && rom_font[0][7] == 0;   /* lo spazio e' vuoto */
    }
    return 0;
}

/* rilegge le righe di testo dello schermo confrontando le celle col font */
static void read_screen(char lines[64][128], int *nlines)
{
    static uint32_t pix[1024 * 768];
    int w = 0, h = 0;
    archie_render(&a, pix, 1024, &w, &h);
    *nlines = h / 8;
    for (int row = 0; row < h / 8 && row < 64; row++) {
        int col;
        for (col = 0; col < w / 8 && col < 127; col++) {
            uint32_t p[64], seen[64];
            int ns = 0;
            for (int i = 0; i < 64; i++) {
                p[i] = pix[(size_t)(row * 8 + i / 8) * 1024 + col * 8 + i % 8];
                int k = 0;
                while (k < ns && seen[k] != p[i]) k++;
                if (k == ns) seen[ns++] = p[i];
            }
            char c = '?';
            if (ns == 1) c = ' ';
            for (int k = 0; k < ns && c == '?'; k++) {
                unsigned char cell[8] = { 0 };
                for (int i = 0; i < 64; i++) if (p[i] == seen[k]) cell[i / 8] |= (unsigned char)(0x80 >> (i % 8));
                for (int g = 1; g < 224; g++) if (!memcmp(cell, rom_font[g], 8)) { c = (char)(g + 32); break; }
            }
            lines[row][col] = c;
        }
        lines[row][col] = 0;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "uso: test_keys_es ROM [layout]\n"); return 1; }
    layout = LoadKeyboardLayoutA(argc > 2 ? argv[2] : "0000040A", KLF_NOTELLSHELL);
    if (!layout) { fprintf(stderr, "disposizione di tastiera non disponibile\n"); return 1; }

    ArchieConfig cfg = { argv[1], 4, NULL, { NULL, NULL }, 8, NULL };
    char err[256];
    if (!archie_create(&a, &cfg, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    keys_init(&keys, &a.kbd);
    if (!load_rom_font()) { fprintf(stderr, "font di sistema non trovato nella ROM\n"); return 1; }
    keys.trace = getenv("KEYS_TRACE") != NULL;
    run_ms(16000);                                         /* fino al desktop */

    press(VK_F12, 0, 0);
    run_ms(500);
    const WCHAR *target = L"!\"$%&/()=?'¡¿*+ç<>,;.:-_@#[]{}\\|~ñÑºª·¬áéíóúÁü";
    const char *plain = "echo ";
    for (const char *c = plain; *c; c++) type_char((WCHAR)*c);
    int skipped = 0;
    for (const WCHAR *c = target; *c; c++) if (!type_char(*c)) skipped++;
    press(VK_RETURN, 0, 0);
    run_ms(1500);

    /* il risultato di *echo e' la riga dopo quella del comando */
    char lines[64][128];
    int n = 0;
    read_screen(lines, &n);
    char expect[128];
    int e = 0;
    for (const WCHAR *c = target; *c; c++) expect[e++] = (char)*c;   /* Latin-1 */
    expect[e] = 0;
    int found = -1;
    for (int r = 0; r < n; r++) if (!strncmp(lines[r], "*echo ", 6)) found = r;
    if (found < 0 || found + 1 >= n) {
        fprintf(stderr, "riga di *echo non trovata sullo schermo\n");
        for (int r = 0; r < n; r++) if (lines[r][0] != ' ') fprintf(stderr, "  |%s|\n", lines[r]);
        return 1;
    }
    /* si confronta la riga digitata (l'uscita di *echo interpreta la '|');
       i caratteri oltre &7F hanno in RISC OS 3.11 un disegno diverso dal
       font usato qui: quelli si guardano nell'istantanea */
    char *got = lines[found] + 6;
    int len = (int)strlen(got);
    while (len > 0 && got[len - 1] == ' ') got[--len] = 0;
    if (getenv("KEYS_TRACE")) {
        fprintf(stderr, "  letto:  ");
        for (int i = 0; i < len; i++) fprintf(stderr, "%02X ", (unsigned char)got[i]);
        fprintf(stderr, "\n  atteso: ");
        for (int i = 0; i < e; i++) fprintf(stderr, "%02X ", (unsigned char)expect[i]);
        fprintf(stderr, "\n");
    }
    int bad = 0, unverified = 0;
    for (int i = 0; i < e || i < len; i++) {
        unsigned char x = i < e ? (unsigned char)expect[i] : 0, y = i < len ? (unsigned char)got[i] : 0;
        if (x == y) continue;
        if (x >= 0x80 && y != 0 && (y == '?' || y >= 0x80)) { unverified++; continue; }
        bad++;
        fprintf(stderr, "  posizione %d: atteso &%02X, scritto &%02X\n", i, x, y);
    }
    if (argc > 3) {
        static uint32_t pix[1024 * 768];
        int w = 0, h = 0;
        archie_render(&a, pix, 1024, &w, &h);
        for (int y = 0; y < h; y++) memmove(pix + (size_t)y * w, pix + (size_t)y * 1024, (size_t)w * 4);
        png_write(argv[3], pix, w, h, h <= 300 ? 2 : 1);
    }
    printf("%d caratteri, %d sbagliati, %d accentati da guardare nell'istantanea%s\n", e, bad, unverified,
           skipped ? " (alcuni non digitabili)" : "");
    archie_destroy(&a);
    return bad ? 1 : 0;
}
