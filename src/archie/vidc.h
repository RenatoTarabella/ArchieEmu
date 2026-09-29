/*
 * vidc.h - VIDC1a (Video and Sound Controller) dell'Archimedes.
 *
 * Il VIDC riceve parole a 32 bit scritte dalla CPU nell'area &3400000:
 * i bit 31-26 scelgono il registro (palette, bordo, cursore, stereo,
 * temporizzazioni orizzontali e verticali, controllo, frequenza audio).
 * Non ha memoria propria: i dati dello schermo arrivano dal MEMC via DMA,
 * qui simulato leggendo direttamente la RAM fisica.
 */
#ifndef ARCHIE_VIDC_H
#define ARCHIE_VIDC_H

#include <stdint.h>
#include "archie_time.h"

typedef struct Vidc {
    uint16_t palette[16];        /* 13 bit: bit 12 = supremacy, 11-8 blu, 7-4 verde, 3-0 rosso */
    uint16_t border;
    uint16_t cursor_palette[3];  /* colori 1-3 del cursore hardware */
    uint8_t  stereo[8];

    /* registri di temporizzazione, valori grezzi (bit 23-14 o 23-13 del dato) */
    uint32_t hcr, hswr, hbsr, hdsr, hder, hber, hcsr, hir;
    uint32_t vcr, vswr, vbsr, vdsr, vder, vber, vcsr, vcer;
    uint32_t control;            /* bit 1-0 clock pixel, 3-2 bpp, 5-4 DMA, 6 interlace, 7 sync */
    uint32_t sound_freq;
} Vidc;

/* Geometria del modo corrente, ricavata dai registri. */
typedef struct VidcTiming {
    int     valid;               /* 0 se i registri non descrivono ancora un modo */
    int     width, height;       /* area di display in pixel */
    int     log2bpp;             /* 0-3: 1, 2, 4, 8 bit per pixel */
    int     total_lines;         /* righe per frame (VCR) */
    int     display_start;       /* prima riga del display (dall'inizio del frame) */
    int     display_end;         /* riga dopo l'ultima del display */
    ArcTime line_time;           /* durata di una riga */
    ArcTime frame_time;          /* durata di un frame */
} VidcTiming;

void vidc_reset(Vidc *v);
void vidc_write(Vidc *v, uint32_t data);
void vidc_timing(const Vidc *v, VidcTiming *t);

/*
 * Disegna un frame in 'out' (pixel &00RRGGBB, 'stride' pixel per riga).
 * I dati dello schermo si leggono dalla RAM fisica 'ram' come farebbe il
 * DMA del MEMC: si parte da 'vinit' e, superato 'vend' (+16), si riparte
 * da 'vstart'. Tutti gli indirizzi sono offset nella RAM fisica.
 * Il cursore hardware (32 pixel di larghezza, 2 bpp) parte da 'cinit'.
 * In *w e *h torna la dimensione disegnata; 'out' deve poter contenere
 * almeno 1024x768 pixel. Pixel doppi in verticale NON vengono duplicati:
 * ci pensa il frontend.
 */
void vidc_render(const Vidc *v, const uint8_t *ram, uint32_t ram_size,
                 uint32_t vinit, uint32_t vstart, uint32_t vend,
                 uint32_t cinit, int cursor_enabled,
                 uint32_t *out, int stride, int *w, int *h);

/* Colore del bordo in &00RRGGBB */
uint32_t vidc_border_rgb(const Vidc *v);

#endif
