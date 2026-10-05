/*
 * ps2mouse.h - Mouse PS/2 dell'A7000 (seconda porta dell'ARM7500), lato
 * mouse del protocollo: risponde ai comandi del computer (reset &FF ->
 * &FA &AA &00, abilita &F4, frequenza &F3, risoluzione &E8, identita' &F2,
 * stato &E9, ...) e, abilitato, manda pacchetti di 3 byte: tasti e segni,
 * spostamento X, spostamento Y (in su positivo).
 */
#ifndef RISCPC_PS2MOUSE_H
#define RISCPC_PS2MOUSE_H

#include <stdint.h>

#define PS2M_QUEUE 64

typedef struct Ps2Mouse {
    uint8_t  out[PS2M_QUEUE];      /* byte verso il computer */
    uint32_t out_r, out_w;
    int      pending;              /* comando in attesa del suo argomento */
    uint8_t  last;
    int      enabled;
    int      dx, dy;               /* movimento non ancora spedito */
    int      buttons, sent_buttons;  /* bit 0 sinistro, 1 destro, 2 centrale */
    uint8_t  rate, resolution;
} Ps2Mouse;

void ps2mouse_reset(Ps2Mouse *m);
void ps2mouse_rx(Ps2Mouse *m, uint8_t byte);      /* dal computer */
int  ps2mouse_tx(Ps2Mouse *m, uint8_t *byte);     /* verso il computer: 1 se c'e' */
/* movimento (y in su) e tasti di RISC OS: bit 0 Adjust, 1 Menu, 2 Select */
void ps2mouse_move(Ps2Mouse *m, int dx, int dy);
void ps2mouse_buttons(Ps2Mouse *m, int riscos_buttons);

#endif
