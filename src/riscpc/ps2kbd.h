/*
 * ps2kbd.h - Tastiera PS/2 del Risc PC (lato tastiera del protocollo).
 *
 * Riceve i comandi dal computer (reset &FF, LED &ED, set di codici &F0,
 * ripetizione &F3, abilita &F4, ...) e risponde con &FA, poi manda i codici
 * dei tasti: set 2 (quello predefinito; RISC OS non lo cambia), rilascio
 * con il prefisso &F0, tasti estesi con &E0.
 */
#ifndef RISCPC_PS2KBD_H
#define RISCPC_PS2KBD_H

#include <stdint.h>

#define PS2_QUEUE 256

typedef struct Ps2Kbd {
    uint8_t  out[PS2_QUEUE];       /* byte verso il computer */
    uint32_t out_r, out_w;
    int      pending;              /* comando in attesa del suo argomento */
    uint8_t  last;                 /* ultimo byte spedito (per &FE) */
    uint8_t  leds;
    int      enabled;
    int      set;                  /* set di codici (1, 2 o 3) */
} Ps2Kbd;

void ps2kbd_reset(Ps2Kbd *k);
/* byte dal computer alla tastiera */
void ps2kbd_rx(Ps2Kbd *k, uint8_t byte);
/* byte dalla tastiera al computer: 1 se ce n'e' uno */
int  ps2kbd_tx(Ps2Kbd *k, uint8_t *byte);

/* Tasto premuto o rilasciato. code: codice del set 2, con 0xE000 per i
   tasti estesi (es. 0xE075 = freccia su). Pausa: 0xE1 (solo pressione). */
void ps2kbd_key(Ps2Kbd *k, uint32_t code, int down);

/* Codice del set 2 per un virtual key di Windows (VK_*), o 0. */
uint32_t ps2_code_from_vk(int vk, int extended);

#endif
