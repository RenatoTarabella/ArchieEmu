/*
 * riscpc_keys.h - Digitazione e mouse simulati per riscpc_boot.
 *
 * Testo normale (tastiera UK) e token:
 *   {F12} {ENTER} {ESC} {TAB} {UP} {DOWN} {LEFT} {RIGHT} {VKxx} {SVKxx}
 *   {WAIT} (5 s)  {MOUSE dx,dy} (y verso l'alto)  {SELECT} {MENU} {ADJUST} (clic)
 * Un passo ogni 40 ms (RISC OS legge la tastiera a ogni centesimo).
 */
#ifndef RISCPC_KEYS_H
#define RISCPC_KEYS_H

#include <stdlib.h>
#include <string.h>
#include "riscpc/riscpc.h"

enum { STEP_KEY, STEP_WAIT, STEP_MOUSE, STEP_BUTTONS };
typedef struct KeyStep { int kind; uint32_t code; int a, b; } KeyStep;
static KeyStep key_steps[8192];
static int key_count, key_next;

static void push_step(int kind, uint32_t code, int a, int b)
{
    if (key_count < 8192) key_steps[key_count++] = (KeyStep){ kind, code, a, b };
}

static void push_vk(int vk, int shift, int ext)
{
    uint32_t c = ps2_code_from_vk(vk, ext), sh = ps2_code_from_vk(0xA0, 0);
    if (!c) return;
    if (shift) push_step(STEP_KEY, sh, 1, 0);
    push_step(STEP_KEY, c, 1, 0);
    push_step(STEP_KEY, c, 0, 0);
    if (shift) push_step(STEP_KEY, sh, 0, 0);
}

static void push_char(char ch)
{
    if (ch >= 'a' && ch <= 'z') { push_vk(ch - 32, 0, 0); return; }
    if (ch >= 'A' && ch <= 'Z') { push_vk(ch, 1, 0); return; }
    if (ch >= '0' && ch <= '9') { push_vk(ch, 0, 0); return; }
    static const struct { char ch; int vk, shift; } map[] = {
        { ' ', 0x20, 0 }, { '.', 0xBE, 0 }, { ',', 0xBC, 0 }, { '-', 0xBD, 0 }, { ':', 0xBA, 1 },
        { ';', 0xBA, 0 }, { '$', '4', 1 },  { '!', '1', 1 },  { '*', '8', 1 },  { '"', '2', 1 },
        { '&', '7', 1 },  { '_', 0xBD, 1 }, { '%', '5', 1 },  { '(', '9', 1 },  { ')', '0', 1 },
        { '=', 0xBB, 0 }, { '+', 0xBB, 1 }, { '/', 0xBF, 0 }, { '<', 0xBC, 1 }, { '>', 0xBE, 1 },
        { '@', 0xC0, 1 }, { '\'', 0xC0, 0 }, { '#', 0xDE, 0 }, { '~', 0xDE, 1 }, { '?', 0xBF, 1 },
        { '[', 0xDB, 0 }, { ']', 0xDD, 0 }, { '^', '6', 1 },  { '|', 0xDC, 1 }, { '\\', 0xDC, 0 },
    };
    for (size_t k = 0; k < sizeof map / sizeof map[0]; k++)
        if (map[k].ch == ch) { push_vk(map[k].vk, map[k].shift, 0); return; }
}

static void parse_keys(const char *s)
{
    static const struct { const char *name; int vk, ext; } named[] = {
        { "{F12}", 0x7B, 0 }, { "{ENTER}", 0x0D, 0 }, { "{ESC}", 0x1B, 0 }, { "{TAB}", 0x09, 0 },
        { "{UP}", 0x26, 1 }, { "{DOWN}", 0x28, 1 }, { "{LEFT}", 0x25, 1 }, { "{RIGHT}", 0x27, 1 },
    };
    while (*s) {
        int done = 0;
        for (size_t k = 0; k < sizeof named / sizeof named[0] && !done; k++) {
            size_t n = strlen(named[k].name);
            if (!strncmp(s, named[k].name, n)) { push_vk(named[k].vk, 0, named[k].ext); s += n; done = 1; }
        }
        if (done) continue;
        if (!strncmp(s, "{WAIT}", 6)) {
            for (int w = 0; w < 125; w++) push_step(STEP_WAIT, 0, 0, 0);
            s += 6;
        } else if (!strncmp(s, "{MOUSE", 6)) {
            char *e;
            int dx = (int)strtol(s + 6, &e, 10), dy = 0;
            if (*e == ',') dy = (int)strtol(e + 1, &e, 10);
            /* a piccoli passi, come un mouse vero */
            int n = (abs(dx) > abs(dy) ? abs(dx) : abs(dy)) / 32 + 1;
            for (int k = 0; k < n; k++)
                push_step(STEP_MOUSE, 0, dx * (k + 1) / n - dx * k / n, dy * (k + 1) / n - dy * k / n);
            s = *e == '}' ? e + 1 : e;
        } else if (!strncmp(s, "{SELECT}", 8) || !strncmp(s, "{MENU}", 6) || !strncmp(s, "{ADJUST}", 8)) {
            int b = s[1] == 'S' ? 4 : s[1] == 'M' ? 2 : 1;
            push_step(STEP_BUTTONS, 0, b, 0);
            push_step(STEP_BUTTONS, 0, 0, 0);
            s = strchr(s, '}') + 1;
        } else if (!strncmp(s, "{VK", 3) || !strncmp(s, "{SVK", 4)) {
            int shift = s[1] == 'S';
            push_vk((int)strtol(s + (shift ? 4 : 3), NULL, 16), shift, 0);
            while (*s && *s != '}') s++;
            if (*s) s++;
        } else {
            push_char(*s++);
        }
    }
}

/* audio raccolto durante l'esecuzione (--wav) */
static FILE *wav_file;
static uint32_t wav_frames;
static double wav_peak, wav_energy;

static void drain_audio(RiscPc *m)
{
    int16_t buf[2 * 4096];
    uint32_t got;
    while ((got = riscpc_audio_read(m, buf, 4096)) > 0) {
        for (uint32_t k = 0; k < 2 * got; k++) {
            double v = buf[k] < 0 ? -buf[k] : buf[k];
            if (v > wav_peak) wav_peak = v;
            wav_energy += (double)buf[k] * buf[k];
        }
        if (wav_file) fwrite(buf, 4, got, wav_file);
        wav_frames += got;
    }
}

/* esegue la macchina per 'duration' facendo i passi a partire da 'start' */
static void run_with_keys(RiscPc *m, ArcTime duration, ArcTime start)
{
    ArcTime end = riscpc_now(m) + duration;
    while (riscpc_now(m) < end && !m->cpu.halted) {
        ArcTime now = riscpc_now(m);
        ArcTime slice = ARC_MS(40);
        if (now + slice > end) slice = end - now;
        riscpc_run(m, slice);
        drain_audio(m);
        now = riscpc_now(m);
        if (key_next < key_count && now >= start + (ArcTime)key_next * ARC_MS(40)) {
            KeyStep *k = &key_steps[key_next++];
            switch (k->kind) {
            case STEP_KEY:     riscpc_key(m, k->code, k->a); break;
            case STEP_MOUSE:   riscpc_mouse_move(m, k->a, k->b); break;
            case STEP_BUTTONS: riscpc_mouse_buttons(m, k->a); break;
            default: break;
            }
        }
    }
}

#endif
