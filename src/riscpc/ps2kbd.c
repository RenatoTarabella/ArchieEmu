/*
 * ps2kbd.c - Tastiera PS/2 (vedi ps2kbd.h)
 */
#include "ps2kbd.h"
#include <string.h>

static void put(Ps2Kbd *k, uint8_t b)
{
    if (k->out_w - k->out_r >= PS2_QUEUE) return;     /* coda piena: si perde */
    k->out[k->out_w++ % PS2_QUEUE] = b;
}

void ps2kbd_reset(Ps2Kbd *k)
{
    memset(k, 0, sizeof *k);
    k->enabled = 1;
    k->set = 2;
}

void ps2kbd_rx(Ps2Kbd *k, uint8_t b)
{
    if (k->pending) {                                   /* argomento di un comando */
        int cmd = k->pending;
        k->pending = 0;
        if (cmd == 0xED) k->leds = b & 7;
        if (cmd == 0xF0) {
            if (b == 0) { put(k, 0xFA); put(k, (uint8_t)k->set); return; }
            if (b <= 3) k->set = b;
        }
        put(k, 0xFA);
        return;
    }
    switch (b) {
    case 0xFF:                                          /* reset e autotest */
        k->out_r = k->out_w = 0;
        k->enabled = 1;
        k->set = 2;
        put(k, 0xFA);
        put(k, 0xAA);
        break;
    case 0xFE: put(k, k->last); break;                  /* ripeti */
    case 0xEE: put(k, 0xEE); break;                     /* eco */
    case 0xF2: put(k, 0xFA); put(k, 0xAB); put(k, 0x83); break;   /* identita' */
    case 0xED: case 0xF0: case 0xF3:
        k->pending = b;
        put(k, 0xFA);
        break;
    case 0xF4: k->enabled = 1; put(k, 0xFA); break;
    case 0xF5: k->enabled = 0; put(k, 0xFA); break;
    case 0xF6: k->enabled = 1; k->set = 2; put(k, 0xFA); break;
    default:   put(k, 0xFA); break;
    }
}

int ps2kbd_tx(Ps2Kbd *k, uint8_t *b)
{
    if (k->out_r == k->out_w) return 0;
    *b = k->out[k->out_r++ % PS2_QUEUE];
    k->last = *b;
    return 1;
}

void ps2kbd_key(Ps2Kbd *k, uint32_t code, int down)
{
    if (!k->enabled || !code) return;
    if (code == 0xE1) {                                 /* Pausa: solo la pressione */
        static const uint8_t pause[8] = { 0xE1, 0x14, 0x77, 0xE1, 0xF0, 0x14, 0xF0, 0x77 };
        if (down) for (int i = 0; i < 8; i++) put(k, pause[i]);
        return;
    }
    if (code & 0xE000) put(k, 0xE0);
    if (!down) put(k, 0xF0);
    put(k, (uint8_t)code);
}

uint32_t ps2_code_from_vk(int vk, int ext)
{
    if (vk >= 'A' && vk <= 'Z') {
        static const uint8_t letters[26] = {
            0x1C, 0x32, 0x21, 0x23, 0x24, 0x2B, 0x34, 0x33, 0x43, 0x3B, 0x42, 0x4B, 0x3A,
            0x31, 0x44, 0x4D, 0x15, 0x2D, 0x1B, 0x2C, 0x3C, 0x2A, 0x1D, 0x22, 0x35, 0x1A
        };
        return letters[vk - 'A'];
    }
    if (vk >= '0' && vk <= '9') {
        static const uint8_t digits[10] = { 0x45, 0x16, 0x1E, 0x26, 0x25, 0x2E, 0x36, 0x3D, 0x3E, 0x46 };
        return digits[vk - '0'];
    }
    if (vk >= 0x70 && vk <= 0x7B) {                     /* F1-F12 */
        static const uint8_t fkeys[12] = { 0x05, 0x06, 0x04, 0x0C, 0x03, 0x0B, 0x83, 0x0A, 0x01, 0x09, 0x78, 0x07 };
        return fkeys[vk - 0x70];
    }
    if (vk >= 0x60 && vk <= 0x69) {                     /* tastierino 0-9 */
        static const uint8_t pad[10] = { 0x70, 0x69, 0x72, 0x7A, 0x6B, 0x73, 0x74, 0x6C, 0x75, 0x7D };
        return pad[vk - 0x60];
    }
    /* tasti di navigazione: estesi, oppure quelli del tastierino senza Num Lock */
    switch (vk) {
    case 0x2D: return ext ? 0xE070 : 0x70;              /* Insert   */
    case 0x24: return ext ? 0xE06C : 0x6C;              /* Home     */
    case 0x21: return ext ? 0xE07D : 0x7D;              /* PgUp     */
    case 0x2E: return ext ? 0xE071 : 0x71;              /* Delete   */
    case 0x23: return ext ? 0xE069 : 0x69;              /* End      */
    case 0x22: return ext ? 0xE07A : 0x7A;              /* PgDn     */
    case 0x26: return ext ? 0xE075 : 0x75;              /* su       */
    case 0x25: return ext ? 0xE06B : 0x6B;              /* sinistra */
    case 0x28: return ext ? 0xE072 : 0x72;              /* giu'     */
    case 0x27: return ext ? 0xE074 : 0x74;              /* destra   */
    case 0x0C: return 0x73;                             /* 5 del tastierino senza Num Lock */
    case 0x0D: return ext ? 0xE05A : 0x5A;              /* Invio    */
    case 0x10: case 0xA0: return 0x12;                  /* Shift    */
    case 0xA1: return 0x59;                             /* Shift destro */
    case 0x11: return ext ? 0xE014 : 0x14;              /* Ctrl     */
    case 0xA2: return 0x14;
    case 0xA3: return 0xE014;
    case 0x12: return ext ? 0xE011 : 0x11;              /* Alt      */
    case 0xA4: return 0x11;
    case 0xA5: return 0xE011;
    case 0x08: return 0x66;                             /* Backspace */
    case 0x09: return 0x0D;                             /* Tab      */
    case 0x14: return 0x58;                             /* Caps Lock */
    case 0x1B: return 0x76;                             /* Esc      */
    case 0x20: return 0x29;                             /* spazio   */
    case 0x90: return 0x77;                             /* Num Lock */
    case 0x91: return 0x7E;                             /* Scroll Lock */
    case 0x13: return 0xE1;                             /* Pausa    */
    case 0x2C: return 0xE07C;                           /* Stampa   */
    case 0x6A: return 0x7C;                             /* tastierino *  */
    case 0x6B: return 0x79;                             /* tastierino +  */
    case 0x6D: return 0x7B;                             /* tastierino -  */
    case 0x6E: return 0x71;                             /* tastierino .  */
    case 0x6F: return 0xE04A;                           /* tastierino /  */
    case 0x5B: return 0xE01F;                           /* Windows sinistro */
    case 0x5C: return 0xE027;                           /* Windows destro   */
    case 0x5D: return 0xE02F;                           /* menu     */
    /* tastiera UK */
    case 0xBA: return 0x4C;                             /* ; :      */
    case 0xBB: return 0x55;                             /* = +      */
    case 0xBC: return 0x41;                             /* , <      */
    case 0xBD: return 0x4E;                             /* - _      */
    case 0xBE: return 0x49;                             /* . >      */
    case 0xBF: return 0x4A;                             /* / ?      */
    case 0xC0: return 0x52;                             /* ' @      */
    case 0xDB: return 0x54;                             /* [ {      */
    case 0xDC: return 0x61;                             /* \ |      */
    case 0xDD: return 0x5B;                             /* ] }      */
    case 0xDE: return 0x5D;                             /* # ~      */
    case 0xDF: return 0x0E;                             /* ` ¬      */
    case 0xE2: return 0x61;                             /* \ | (102° tasto) */
    default:   return 0;
    }
}
