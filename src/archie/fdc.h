/*
 * fdc.h - Controller floppy WD1772 dell'A310/A3000 e unita' da 3,5" con
 * immagini disco ADFS (.adf: formati D/E da 800 KB, 80 tracce, 2 facce,
 * 5 settori da 1024 byte; anche L da 640 KB e 16 settori da 256 byte).
 *
 * Collegamento sulla macchina (lo fa archie.c):
 *   registri 0-3 (stato/comando, traccia, settore, dato) all'IOC banco 1;
 *   latch A: bit 0-3 selezione unita' (attivi bassi), 4 faccia (0 = faccia 1),
 *            5 motore (attivo basso, come MON* in MAME), 6 "in use";
 *   latch B: bit 1 densita' (0 = doppia), bit 3 reset del controller (attivo basso);
 *   DRQ -> FIQ dell'IOC (FH0), INTRQ -> FIQ dell'IOC (FH1).
 * Il trasferimento dei byte lo fa il gestore FIQ di RISC OS: fdc_update
 * deve alzare DRQ con la cadenza di un disco vero (circa 32 us per byte in
 * doppia densita') e segnalare "lost data" se il byte non viene letto.
 */
#ifndef ARCHIE_FDC_H
#define ARCHIE_FDC_H

#include <stdint.h>
#include "archie_time.h"

#define FDC_DRIVES 4
#define FDC_MAX_CYL 84           /* posizioni fisiche della testina (0-83) */

struct FdcCustomTrack;           /* traccia riformattata con geometria non standard */

typedef struct FdcDrive {
    uint8_t *image;              /* contenuto dell'immagine (malloc), NULL = vuota */
    uint32_t size;
    int      sides, tracks, sectors, sector_size, first_sector;
    int      write_protect;
    int      dirty;
    char     path[512];
    int      track;              /* posizione della testina */
    int      disc_changed;
    struct FdcCustomTrack *custom[FDC_MAX_CYL * 2];  /* privato: tracce non standard */
} FdcDrive;

typedef struct Fdc {
    FdcDrive drive[FDC_DRIVES];
    int      selected;           /* -1 = nessuna */
    int      side, motor, dden, reset;
    /* registri del WD1772 */
    uint8_t  status, track, sector, data, command;
    int      direction;
    /* esecuzione del comando in corso (privato) */
    int      phase;
    ArcTime  next_event;
    int      pos, len;
    uint8_t  buffer[1024 * 16];
    int      drq, intrq;
    int      multi;
    /* privato */
    int      type1;              /* il registro di stato mostra i bit del tipo I */
    int      intrq_cond;         /* condizioni I0-I3 dell'ultimo Force Interrupt */
    int      steps, sec_idx, prev_mark, spin_flag;
    uint16_t crc;
    ArcTime  t0, motor_off_at, idx_next;
} Fdc;

void    fdc_reset(Fdc *f);
int     fdc_insert(Fdc *f, int drive, const char *path);   /* 0 = errore */
void    fdc_eject(Fdc *f, int drive);                      /* salva se modificata */
/* Salva subito l'immagine se modificata (1 = ok o niente da salvare). */
int     fdc_flush(Fdc *f, int drive);
uint8_t fdc_read(Fdc *f, int reg, ArcTime now);
void    fdc_write(Fdc *f, int reg, uint8_t value, ArcTime now);
void    fdc_latch_a(Fdc *f, uint8_t value);
void    fdc_latch_b(Fdc *f, uint8_t value);
/* Avanza il comando in corso fino all'istante 'now'. */
void    fdc_update(Fdc *f, ArcTime now);
/* Prossimo istante in cui fdc_update ha qualcosa da fare (o ~0 se niente). */
ArcTime fdc_next_event(const Fdc *f);
int     fdc_drq(const Fdc *f);
int     fdc_intrq(const Fdc *f);
/* Linea "disc changed" per il pin C4 dell'IOC (attiva bassa). */
int     fdc_disc_changed(const Fdc *f);

#endif
