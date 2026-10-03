/*
 * vidc20.h - VIDC20 del Risc PC: palette a 24 bit di 256 voci, modi da 1 a
 * 32 bit per pixel, cursore hardware, suono.
 *
 * Un solo registro in scrittura a &03400000: i bit alti della parola
 * scelgono il registro.
 *   &0xxxxxxx dato della palette (R bit 0-7, G 8-15, B 16-23), indirizzo +1
 *   &1xxxxxxx indirizzo della palette (bit 0-7)
 *   &4 bordo, &5-&7 colori del cursore 1-3
 *   &80-&87 orizzontali: ciclo, sync, inizio bordo, inizio display, fine
 *           display, fine bordo, cursore, interlacciato
 *   &90-&97 verticali: ciclo, sync, inizio bordo, inizio display, fine
 *           display, fine bordo, inizio e fine cursore
 *   &A0-&A7 immagine stereo, &B0 frequenza del suono, &B1 controllo suono
 *   &C esterno, &D sintetizzatore di frequenza, &E controllo, &F dati
 */
#ifndef RISCPC_VIDC20_H
#define RISCPC_VIDC20_H

#include <stdint.h>

typedef struct Vidc20 {
    uint32_t palette[256];
    uint8_t  pal_index;
    uint32_t border, cursor[3];
    uint32_t horiz[8], vert[8];
    uint32_t stereo[8];
    uint32_t sound_freq, sound_ctrl;
    uint32_t ext, fsyn, control, datactl;
} Vidc20;

void vidc20_reset(Vidc20 *v);
void vidc20_write(Vidc20 *v, uint32_t value);

/* bit per pixel (log2: 0 = 1 bpp ... 5 = 32 bpp) */
int  vidc20_log2bpp(const Vidc20 *v);

/* Tempi del video, in unita' da 24 MHz (ArcTime). Il clock dei pixel viene
   dal sintetizzatore (FSYN: V bit 8-13, R bit 0-5): 24 MHz * (V+1)/(R+1),
   diviso per (bit 4-2 del controllo) + 1; righe di HCR + 8 pixel, frame di
   VCR + 2 righe; il flyback comincia alla fine del display (VDER).
   Ritorna 0 se i registri non sono ancora programmati. */
typedef struct Vidc20Timing {
    double   pixel_hz;
    uint64_t line_time, frame_time, flyback_at;
} Vidc20Timing;
int  vidc20_timing(const Vidc20 *v, Vidc20Timing *t);
/* larghezza e altezza dell'area visibile, 0 se non programmata */
void vidc20_size(const Vidc20 *v, int *w, int *h);

/* Disegna lo schermo leggendo i pixel con 'read' a partire dall'indirizzo
   fisico 'start' (VIDINIT dell'IOMD). out: pixel 0x00RRGGBB. */
typedef const uint8_t *(*Vidc20Mem)(void *ctx, uint32_t addr, uint32_t len);
void vidc20_render(const Vidc20 *v, Vidc20Mem mem, void *ctx, uint32_t start,
                   uint32_t *out, int stride, int *w, int *h);

/* Cursore hardware: 32 pixel a 2 bpp, 8 byte per riga da 'data' (CURSINIT
   dell'IOMD); 0 trasparente, 1-3 i colori del cursore. Va disegnato sopra
   l'immagine prodotta da vidc20_render. */
int  vidc20_cursor_height(const Vidc20 *v);
void vidc20_draw_cursor(const Vidc20 *v, const uint8_t *data, uint32_t *out, int stride, int w, int h);

#endif
