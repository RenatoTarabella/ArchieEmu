/*
 * riscpc.h - La macchina Risc PC (1994) a basso livello: ARM610/710,
 * IOMD, VIDC20, CMOS, con una ROM vera di RISC OS 3.5-3.7.
 *
 * Mappa fisica:
 *   &00000000  ROM (2 o 4 MB, ripetuta nei 16 MB del banco 0)
 *   &01000000  ROM di estensione (assente)
 *   &02000000  VRAM (0, 1 o 2 MB, ripetuta)
 *   &03010000  Super I/O 82C711 (porte del PC * 4)
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

    uint8_t *rom, *ram, *vram;
    uint32_t rom_size, ram_size, vram_size;
    char     cmos_path[512];
    int      sda_in;

    ArcTime  now;
    uint64_t cycle_base;
    uint32_t units_per_cycle_q16;
    double   mhz;

    int      mouse_buttons;      /* premuti: bit 0 Adjust, 1 Menu, 2 Select */

    ArcTime  snd_next;          /* prossimo blocco di 16 byte del DMA del suono */

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

#endif
