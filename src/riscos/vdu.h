/*
 * vdu.h - Driver VDU in stile RISC OS: interpreta il flusso di codici VDU
 * (OS_WriteC) e disegna nel framebuffer, che vive nella memoria emulata nel
 * formato nativo del modo (1/2/4/8 bpp con palette, oppure 32 bpp truecolor
 * &00BBGGRR). Il frontend converte in RGB a ogni frame con vdu_render().
 */
#ifndef RISCOS_VDU_H
#define RISCOS_VDU_H

#include <stdint.h>

#define VDU_MAX_BYTES (16u << 20)     /* memoria video massima */

typedef struct VduPalette {
    uint32_t first, second;           /* &RRGGBB; diversi = colore lampeggiante */
} VduPalette;

typedef struct Vdu {
    uint8_t  *screen;                 /* memoria video (host) */
    uint32_t  screen_addr;            /* indirizzo nella memoria emulata */

    /* modo corrente */
    int      mode;                    /* numero, -1 se scelto con un selettore */
    int      width, height;           /* pixel */
    int      log2bpp;                 /* 0,1,2,3 o 5 */
    int      xeig, yeig;
    int      line_bytes;
    uint32_t ncolour;                 /* colori - 1 (&FFFFFFFF per 16M) */
    VduPalette palette[256];
    uint32_t border;

    /* testo */
    int      cols, rows;              /* in caratteri */
    int      tx, ty;                  /* cursore di testo, relativo alla finestra */
    int      twl, twb, twr, twt;      /* finestra di testo (colonne/righe) */
    uint32_t tfg, tbg;                /* colori del testo (valore pixel) */
    int      cursor_on;
    int      vdu5;                    /* testo al cursore grafico */
    int      disabled;                /* VDU 21 */
    int      pending_wrap;            /* cursore oltre l'ultima colonna */
    uint8_t  font[256][8];

    /* grafica (unita' OS) */
    int      gwl, gwb, gwr, gwt;      /* finestra grafica, pixel */
    int      orgx, orgy;
    int      gx, gy, ox, oy, oox, ooy; /* cursore grafico e i due precedenti */
    uint32_t gfg, gbg;
    int      gfg_action, gbg_action;
    int      tint_tf, tint_tb, tint_gf, tint_gb;

    /* coda dei parametri dei comandi a piu' byte */
    uint8_t  queue[16];
    int      queue_need, queue_have;

    /* echo testuale facoltativo (frontend console) */
    void   (*echo)(void *ctx, int ch);
    void    *echo_ctx;
    int      bell;                    /* contatore di VDU 7 */

    /* Lavoro che il driver VDU vero di RISC OS 3.11 avrebbe fatto, in tick
       dell'ARM2 a 8 MHz (tarato sulla macchina Archimedes emulata): il
       kernel lo addebita alla CPU dopo ogni SWI. */
    uint32_t cost;
} Vdu;

void vdu_init(Vdu *v, uint8_t *screen, uint32_t screen_addr);
void vdu_write(Vdu *v, uint8_t ch);
int  vdu_set_mode(Vdu *v, int mode);                    /* 0 = modo sconosciuto */
int  vdu_set_mode_spec(Vdu *v, int w, int h, int log2bpp);
int  vdu_mode_valid(int mode);

/* colori in RGB diretto (ColourTrans, COLOUR r,g,b) */
void vdu_set_text_rgb(Vdu *v, uint32_t rgb, int background);
void vdu_set_gcol_rgb(Vdu *v, uint32_t rgb, int background, int action);

int32_t  vdu_read_variable(Vdu *v, int var);            /* OS_ReadVduVariables */
int      vdu_char_at_cursor(Vdu *v);                   /* OSBYTE &87 */
/* Riconosce il carattere nella cella (colonna, riga) dello schermo intero:
   32 per una cella uniforme, 0 se non e' un carattere del font. */
int      vdu_char_at(const Vdu *v, int col, int row);
int      vdu_point(Vdu *v, int x, int y, uint32_t *value);  /* POINT, unita' OS */
void     vdu_plot(Vdu *v, int k, int x, int y);

/* Converte lo schermo in pixel &00RRGGBB. flash_phase alterna i colori
   lampeggianti; cursor_visible disegna il cursore di testo. */
void vdu_render(const Vdu *v, uint32_t *out, int flash_phase, int cursor_visible);

#endif
