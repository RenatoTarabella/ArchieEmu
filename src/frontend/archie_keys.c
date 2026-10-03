/*
 * archie_keys.c - Traduzione della tastiera per la macchina Archimedes
 * (vedi archie_keys.h)
 */
#include "archie_keys.h"
#include "riscpc/ps2kbd.h"
#include <stdio.h>
#include <string.h>

/* codici virtuali di Windows usati qui (senza includere windows.h) */
enum {
    K_SHIFT = 0x10, K_CONTROL = 0x11, K_MENU = 0x12, K_NUMPAD0 = 0x60
};

/* Carattere -> tasto (codice virtuale UK) e Shift sull'Archimedes UK, la cui
   disposizione dei simboli e' quella americana piu' il tasto della sterlina
   (verificato facendo scrivere a RISC OS 3.11 ogni tasto). */
typedef struct CharKey { unsigned char ch, vk, shift; } CharKey;
static const CharKey char_keys[] = {
    { '\'', 0xC0, 0 }, { '"', 0xC0, 1 }, { '-', 0xBD, 0 }, { '_', 0xBD, 1 }, { '=', 0xBB, 0 }, { '+', 0xBB, 1 },
    { '[', 0xDB, 0 }, { '{', 0xDB, 1 }, { ']', 0xDD, 0 }, { '}', 0xDD, 1 }, { '\\', 0xDC, 0 }, { '|', 0xDC, 1 },
    { ';', 0xBA, 0 }, { ':', 0xBA, 1 }, { ',', 0xBC, 0 }, { '<', 0xBC, 1 }, { '.', 0xBE, 0 }, { '>', 0xBE, 1 },
    { '/', 0xBF, 0 }, { '?', 0xBF, 1 }, { '`', 0xDF, 0 }, { '~', 0xDF, 1 }, { 0xA3, 0xDE, 0 },
    { ')', '0', 1 }, { '!', '1', 1 }, { '@', '2', 1 }, { '#', '3', 1 }, { '$', '4', 1 },
    { '%', '5', 1 }, { '^', '6', 1 }, { '&', '7', 1 }, { '*', '8', 1 }, { '(', '9', 1 },
    { ' ', 0x20, 0 },
};

/* La tastiera PS/2 UK del Risc PC: disposizione PC britannica (verificata
   con riscpc_boot facendo scrivere a RISC OS 3.5 i simboli). */
static const CharKey char_keys_pc[] = {
    { '\'', 0xC0, 0 }, { '@', 0xC0, 1 }, { '-', 0xBD, 0 }, { '_', 0xBD, 1 }, { '=', 0xBB, 0 }, { '+', 0xBB, 1 },
    { '[', 0xDB, 0 }, { '{', 0xDB, 1 }, { ']', 0xDD, 0 }, { '}', 0xDD, 1 }, { '\\', 0xDC, 0 }, { '|', 0xDC, 1 },
    { ';', 0xBA, 0 }, { ':', 0xBA, 1 }, { ',', 0xBC, 0 }, { '<', 0xBC, 1 }, { '.', 0xBE, 0 }, { '>', 0xBE, 1 },
    { '/', 0xBF, 0 }, { '?', 0xBF, 1 }, { '`', 0xDF, 0 }, { 0xAC, 0xDF, 1 }, { '#', 0xDE, 0 }, { '~', 0xDE, 1 },
    { ')', '0', 1 }, { '!', '1', 1 }, { '"', '2', 1 }, { 0xA3, '3', 1 }, { '$', '4', 1 },
    { '%', '5', 1 }, { '^', '6', 1 }, { '&', '7', 1 }, { '*', '8', 1 }, { '(', '9', 1 },
    { ' ', 0x20, 0 },
};

static int archie_code(int vk, int extended) { return kbd_code_from_vk(vk, extended); }
static void archie_send(void *ctx, int code, int down) { kbd_key((Kbd *)ctx, code, down); }

void keys_init(ArchieKeys *k, Kbd *kbd)
{
    memset(k, 0, sizeof *k);
    k->kbd = kbd;
    k->code_of = archie_code;
    k->send_fn = archie_send;
    k->send_ctx = kbd;
    k->chars = char_keys;
    k->nchars = (int)(sizeof char_keys / sizeof char_keys[0]);
}

static int ps2_code(int vk, int extended)
{
    uint32_t c = ps2_code_from_vk(vk, extended);
    return c ? (int)c : -1;
}

void keys_init_ps2(ArchieKeys *k, KeysSendFn send, void *ctx)
{
    memset(k, 0, sizeof *k);
    k->code_of = ps2_code;
    k->send_fn = send;
    k->send_ctx = ctx;
    k->chars = char_keys_pc;
    k->nchars = (int)(sizeof char_keys_pc / sizeof char_keys_pc[0]);
}

static void send_now(ArchieKeys *k, int code, int down)
{
    if (k->trace) fprintf(stderr, "    tasto &%02X %s\n", code, down ? "giu'" : "su");
    if (code >= 0) k->send_fn(k->send_ctx, code, down);
}

int keys_pending(const ArchieKeys *k) { return (k->out_tail - k->out_head) & 255; }

/* in coda: escono a ritmo con keys_tick */
static void send(ArchieKeys *k, int code, int down)
{
    if (code < 0) return;
    if (!keys_pending(k) && (int32_t)(k->now - k->out_next) >= 0) {
        send_now(k, code, down);
        k->out_next = k->now + KEYS_GAP_MS;
        return;
    }
    if (((k->out_tail + 1) & 255) == k->out_head) return;       /* coda piena */
    k->out_code[k->out_tail] = code;
    k->out_down[k->out_tail] = (uint8_t)down;
    k->out_tail = (k->out_tail + 1) & 255;
}

/* tasto fisico: subito, se non c'e' una sequenza in uscita */
static void send_direct(ArchieKeys *k, int code, int down)
{
    if (code < 0) return;
    /* dietro a una sequenza tradotta (anche appena finita) si aspetta il turno */
    if (keys_pending(k) || (int32_t)(k->now - k->out_next) < 0) { send(k, code, down); return; }
    send_now(k, code, down);
}

static void tap(ArchieKeys *k, int code)
{
    send(k, code, 1);
    send(k, code, 0);
}

/* porta lo Shift della macchina allo stato 'want' per un tasto, poi lo rimette */
static void tap_with_shift(ArchieKeys *k, int code, int want)
{
    int sc = k->code_of(K_SHIFT, 0);
    if (want != (k->shift_held != 0)) send(k, sc, want);
    tap(k, code);
    if (want != (k->shift_held != 0)) send(k, sc, k->shift_held != 0);
}

void keys_char(ArchieKeys *k, unsigned ch)
{
    /* un tasto puo' dare piu' caratteri (tasto morto che non si combina:
       "~" e poi "n"): si accettano tutti fino alla prossima pressione */
    k->dead_pending = 0;
    if (!k->char_expected || ch < 32) return;

    for (int i = 0; i < k->nchars; i++) {
        if (k->chars[i].ch == ch) {
            tap_with_shift(k, k->code_of(k->chars[i].vk, 0), k->chars[i].shift);
            return;
        }
    }
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
        int upper = ch >= 'A' && ch <= 'Z';
        int vk = ch >= 'a' ? (int)ch - 32 : (int)ch;
        tap_with_shift(k, k->code_of(vk, 0), upper);
        return;
    }
    if (ch >= 128 && ch < 256) {
        /* Alt + codice decimale sul tastierino numerico (Latin-1) */
        int sc = k->code_of(K_SHIFT, 0), alt = k->code_of(K_MENU, 0);
        char digits[4];
        snprintf(digits, sizeof digits, "%u", ch);
        if (k->shift_held) send(k, sc, 0);
        send(k, alt, 1);
        for (char *d = digits; *d; d++) tap(k, k->code_of(K_NUMPAD0 + (*d - '0'), 0));
        send(k, alt, 0);
        if (k->shift_held) send(k, sc, 1);
    }
}

void keys_deadchar(ArchieKeys *k)
{
    k->dead_pending = 1;
    k->char_expected = 0;
}

/* tasti che producono simboli: si aspetta il carattere */
static int is_char_key(const ArchieKeys *k, int vk)
{
    if ((vk >= 0xBA && vk <= 0xC0) || (vk >= 0xDB && vk <= 0xDF) || vk == 0xE2) return 1;
    if (vk >= '0' && vk <= '9' && (k->shift_held || k->altgr_held)) return 1;
    if (vk >= 'A' && vk <= 'Z' && (k->altgr_held || k->dead_pending)) return 1;
    return 0;
}

static void flush_ctrl(ArchieKeys *k)
{
    if (k->ctrl_pending) {
        k->ctrl_pending = 0;
        k->ctrl_sent = 1;
        send_direct(k, k->code_of(K_CONTROL, 0), 1);
    }
}

void keys_tick(ArchieKeys *k, uint32_t time)
{
    k->now = time;
    if (k->ctrl_pending && time - k->ctrl_time > 30) flush_ctrl(k);
    while (keys_pending(k) && (int32_t)(time - k->out_next) >= 0) {
        send_now(k, k->out_code[k->out_head], k->out_down[k->out_head]);
        k->out_head = (k->out_head + 1) & 255;
        k->out_next += KEYS_GAP_MS;
        if ((int32_t)(time - k->out_next) > KEYS_GAP_MS) k->out_next = time;
    }
}

void keys_key(ArchieKeys *k, int vk, int extended, int down, int repeat, uint32_t time)
{
    k->now = time;
    if (down && !repeat && vk != K_SHIFT && vk != K_CONTROL && vk != K_MENU) k->char_expected = 0;
    if (down && repeat) return;                          /* la ripetizione la fa la macchina */

    /* Ctrl sinistro: potrebbe essere il primo mezzo di AltGr */
    if (vk == K_CONTROL && !extended) {
        if (down) {
            if (!k->ctrl_pending && !k->ctrl_sent) { k->ctrl_pending = 1; k->ctrl_time = time; k->ctrl_fake = 0; }
        } else {
            if (k->ctrl_pending) flush_ctrl(k);          /* Ctrl premuto e rilasciato da solo */
            if (k->ctrl_sent) send_direct(k, k->code_of(K_CONTROL, 0), 0);
            k->ctrl_pending = k->ctrl_sent = k->ctrl_fake = 0;
        }
        return;
    }
    if (vk == K_MENU && extended) {                      /* AltGr */
        if (down && k->ctrl_pending && time - k->ctrl_time <= 30) {
            k->ctrl_pending = 0;                         /* il Ctrl era finto */
            k->ctrl_fake = 1;
        }
        k->altgr_held = down;
        return;
    }
    flush_ctrl(k);

    if (vk == K_SHIFT) k->shift_held = down;
    if (down && is_char_key(k, vk)) {
        k->suppressed[vk & 255] = 1;
        k->char_expected = 1;
        return;
    }
    if (!down && k->suppressed[vk & 255]) { k->suppressed[vk & 255] = 0; return; }
    if (down && vk >= 'A' && vk <= 'Z') k->dead_pending = 0;
    send_direct(k, k->code_of(vk, extended), down);
}
