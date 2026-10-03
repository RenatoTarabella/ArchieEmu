/*
 * riscpc.h - La macchina Risc PC (1994) a basso livello: ARM610/710,
 * IOMD, VIDC20, CMOS, con una ROM vera di RISC OS 3.5-3.7.
 *
 * Mappa fisica:
 *   &00000000  ROM (2 o 4 MB, ripetuta nei 16 MB del banco 0)
 *   &01000000  ROM di estensione (assente)
 *   &02000000  VRAM (0, 1 o 2 MB, ripetuta)
 *   &03010000  Super I/O 82C711 (porte del PC * 4)
 *   &03012000  DACK del floppy (il dato del DMA); &0302A000 lo stesso con TC
 *   &03200000  IOMD
 *   &03310000  tasti del mouse (bit 4-6, attivi bassi)
 *   &033C0000  identita' delle schede (assenti)
 *   &03400000  VIDC20
 *   &08000000  spazio EASI delle schede
 *   &10000000  DRAM: quattro banchi da 64 MB (&10, &14, &18, &1C000000)
 */
#ifndef RISCPC_H
#define RISCPC_H

#include <stdint.h>
#include <stddef.h>
#include "../cpu/arm6.h"
#include "../archie/archie_time.h"
#include "../archie/cmos.h"
#include "iomd.h"
#include "vidc20.h"
#include "ps2kbd.h"
#include "superio.h"

typedef struct RiscPcConfig {
    const char *rom_path;
    uint32_t    ram_mb;        /* DRAM nel banco 0: 4-64 MB (potenza di due) */
    uint32_t    vram_mb;       /* 0, 1 o 2 */
    const char *cmos_path;
    double      mhz;           /* clock della CPU: 30 (ARM610) o 40 (ARM710) */
    int         arm710;
} RiscPcConfig;

/* accesso a un indirizzo che nessun dispositivo riconosce (per il debug) */
typedef void (*RiscPcIoHook)(void *user, int write, uint32_t addr, uint32_t value, int size);

typedef struct RiscPc {
    Arm6     cpu;
    Iomd     iomd;
    Vidc20   vidc;
    Cmos     cmos;
    Ps2Kbd   kbd;
    SuperIo  sio;

    uint8_t *rom, *ram, *vram;
    uint32_t rom_size, ram_size, vram_size;
    char     cmos_path[512];
    int      sda_in;

    ArcTime  now;
    uint64_t cycle_base;
    uint32_t units_per_cycle_q16;
    double   mhz;

    int      mouse_buttons;      /* premuti: bit 0 Adjust, 1 Menu, 2 Select */

    uint64_t slice_stop;         /* fine della fetta di CPU in corso (cicli) */
    ArcTime  index_next;         /* prossimo impulso di indice del floppy */
    ArcTime  snd_next;          /* prossimo blocco di 16 byte del DMA del suono */
    /* uscita audio: campioni stereo a RISCPC_AUDIO_HZ in un anello */
    ArcTime  out_t;              /* inizio del campione d'uscita in costruzione */
    double   acc_l, acc_r;
    int16_t *audio;
    uint32_t audio_w, audio_r;

    ArcTime  frame_start;
    int      flyback;
    uint64_t frames;

    RiscPcIoHook io_hook;
    void        *hook_user;
    /* facoltativo: ogni accesso I/O fra trace_lo e trace_hi (write 0/1, valore letto o scritto) */
    RiscPcIoHook io_trace;
    uint32_t     trace_lo, trace_hi;
} RiscPc;

int  riscpc_create(RiscPc *m, const RiscPcConfig *cfg, char *err, size_t errsize);
void riscpc_destroy(RiscPc *m);
void riscpc_reset(RiscPc *m);
void riscpc_run(RiscPc *m, ArcTime duration);
ArcTime riscpc_now(const RiscPc *m);

/* Puntatore alla memoria fisica (RAM, VRAM o ROM) per 'len' byte, o NULL */
const uint8_t *riscpc_phys(RiscPc *m, uint32_t addr, uint32_t len);

void riscpc_render(RiscPc *m, uint32_t *out, int stride, int *w, int *h);
/* Audio: campioni stereo a 16 bit interlacciati (sinistro, destro). */
#define RISCPC_AUDIO_HZ     48000
#define RISCPC_AUDIO_FRAMES 65536u
uint32_t riscpc_audio_read(RiscPc *m, int16_t *out, uint32_t max);

/* colore del bordo, 0x00RRGGBB */
uint32_t riscpc_border_rgb(const RiscPc *m);
void riscpc_set_mhz(RiscPc *m, double mhz);

/* Floppy: immagine nell'unita' 0 o 1 (0 = errore), espulsione (salva) */
int  riscpc_insert_floppy(RiscPc *m, int drive, const char *path);
void riscpc_eject_floppy(RiscPc *m, int drive);
/* Disco fisso IDE: immagine .hdf (0 = errore) */
int  riscpc_attach_hd(RiscPc *m, const char *path);
void riscpc_detach_hd(RiscPc *m);

/* Tastiera: codice del set 2 (vedi ps2kbd.h), premuto o rilasciato */
void riscpc_key(RiscPc *m, uint32_t code, int down);
/* Mouse: movimento (y verso l'alto) e tasti premuti (bit 0 Adjust, 1 Menu, 2 Select) */
void riscpc_mouse_move(RiscPc *m, int dx, int dy);
void riscpc_mouse_buttons(RiscPc *m, int buttons);

#endif
