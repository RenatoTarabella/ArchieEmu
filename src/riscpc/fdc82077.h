/*
 * fdc82077.h - Controller floppy di tipo PC (82077/765) nel Super I/O del
 * Risc PC, con le unita' e le immagini del controller dell'Archimedes
 * (archie/fdc.c: ADF D/E/F/L, DOS 720 KB e 1,44 MB, HFE).
 *
 * Porte (offset rispetto a &3F0):
 *   2 DOR: bit 0-1 unita', 2 /reset, 3 DMA e interrupt abilitati, 4-7 motori
 *   4 lettura MSR: 7 RQM, 6 DIO (verso la CPU), 5 esecuzione senza DMA,
 *     4 occupato, 0-3 unita' in ricerca; scrittura DSR: 7 reset, 1-0 velocita'
 *   5 dati (comandi, parametri, byte dei settori, risultati)
 *   7 lettura DIR: bit 7 disco cambiato; scrittura CCR: 1-0 velocita'
 * Velocita': 0 = 500 kbit/s (alta densita'), 2 = 250 kbit/s (doppia).
 *
 * Comandi: SPECIFY, SENSE DRIVE STATUS, RECALIBRATE, SENSE INTERRUPT, SEEK,
 * READ DATA, READ DELETED, WRITE DATA, READ ID, FORMAT TRACK, VERSION,
 * CONFIGURE, DUMPREG, LOCK, PERPENDICULAR. Il disco gira a 300 giri/min e
 * i settori passano sotto la testina al loro posto; i byte arrivano al
 * ritmo della velocita' scelta, ma se la CPU non li prende in tempo il
 * disco "aspetta" (nessun overrun).
 */
#ifndef RISCPC_FDC82077_H
#define RISCPC_FDC82077_H

#include <stdint.h>
#include "../archie/fdc.h"

typedef struct Fdc82077 {
    Fdc      media;              /* solo le unita' e le immagini */
    int      present[FDC_DRIVES];
    uint8_t  dor, rate, specify[2], config[3];
    int      nondma;             /* SPECIFY: niente DMA, un interrupt per byte */
    int      locked;

    int      phase;
    uint8_t  cmd[9];
    int      ncmd, nparams;
    uint8_t  result[10];
    int      nresult, rpos;

    /* esecuzione */
    int      drive, head, mt, skip;
    uint8_t  c, h, r, n, eot;
    int      fmt_count, fmt_sectors;
    uint8_t  fmt_ids[32][4], fmt_fill;
    uint8_t  buf[1024 * 36];
    int      pos, len, data_ready;
    ArcTime  next;               /* prossimo evento (byte, settore, fine ricerca) */
    int      st0, st1, st2;
    int      op;                 /* operazione in corso (lettura, scrittura, ID, formattazione) */
    FdcSectorView secs[64];      /* la traccia dell'ultima ricerca */
    int      nsecs, found;

    int      pcn[FDC_DRIVES];    /* cilindro di ogni unita' */
    int      seek_target[FDC_DRIVES], seeking[FDC_DRIVES];
    ArcTime  seek_done[FDC_DRIVES];
    uint8_t  pending_int[FDC_DRIVES + 1];  /* ST0 da riportare con SENSE INTERRUPT */
    int      npending;
    int      irq;                /* linea di interrupt */
    int      tc;                 /* terminal count ricevuto: fine dopo questo settore */
} Fdc82077;

void    fdc82077_init(Fdc82077 *f);
void    fdc82077_reset(Fdc82077 *f, ArcTime now);
uint8_t fdc82077_read(Fdc82077 *f, int port, ArcTime now);
void    fdc82077_write(Fdc82077 *f, int port, uint8_t v, ArcTime now);
void    fdc82077_update(Fdc82077 *f, ArcTime now);
ArcTime fdc82077_next_event(const Fdc82077 *f);
int     fdc82077_irq(const Fdc82077 *f);
int     fdc82077_drq(const Fdc82077 *f);
/* Accesso DMA (DACK): il byte in esecuzione senza passare dalla porta dati;
   tc = 1 per l'ultimo byte (terminal count, il comando finisce li') */
uint8_t fdc82077_dack_read(Fdc82077 *f, int tc, ArcTime now);
void    fdc82077_dack_write(Fdc82077 *f, uint8_t v, int tc, ArcTime now);

/* l'unita' selezionata gira con un disco: passano gli impulsi di indice */
int     fdc82077_spinning(const Fdc82077 *f);
#define FDC82077_REV ARC_MS(200)

#endif
