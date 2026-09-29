/*
 * memc.h - MEMC1a (Memory Controller) dell'Archimedes.
 *
 * Il MEMC sta tra la CPU e tutto il resto. Mappa degli indirizzi (26 bit):
 *   &0000000-&1FFFFFF  RAM logica: pagine tradotte dalla CAM, con protezione
 *   &2000000-&2FFFFFF  RAM fisica (solo supervisore)
 *   &3000000-&33FFFFF  I/O: IOC e periferiche (solo supervisore)
 *   &3400000-&37FFFFF  lettura: ROM bassa (assente); scrittura: VIDC
 *                      (&3400000) e registri del MEMC (&3600000)
 *   &3800000-&3FFFFFF  lettura: ROM alta (il sistema operativo);
 *                      scrittura: la CAM (tabella logico -> fisico)
 * All'accensione la ROM appare anche all'indirizzo 0 finche' la CPU non
 * accede alla zona alta o scrive la CAM.
 */
#ifndef ARCHIE_MEMC_H
#define ARCHIE_MEMC_H

#include <stdint.h>
#include "../cpu/arm2.h"

#define MEMC_LOGICAL_PAGES 8192          /* pagine da 4 KB in 32 MB */

typedef struct MemcIo {
    void     *ctx;
    /* area &3000000-&33FFFFF: 'addr' completo, 'is_byte' per LDRB/STRB */
    uint32_t (*io_read)(void *ctx, uint32_t addr, int is_byte);
    void     (*io_write)(void *ctx, uint32_t addr, uint32_t value, int is_byte);
    void     (*vidc_write)(void *ctx, uint32_t value);
    void     (*sound_changed)(void *ctx);   /* registri del DMA audio modificati */
} MemcIo;

typedef struct Memc {
    uint8_t       *ram;                  /* RAM fisica */
    uint32_t       ram_size;
    const uint8_t *rom;                  /* ROM alta, ripetuta fino a 8 MB */
    uint32_t       rom_size;
    const Arm2    *cpu;                  /* per sapere il modo (utente/supervisore) */
    MemcIo         io;

    /* CAM: per ogni pagina logica (della dimensione corrente) la pagina
       fisica, -1 se non mappata, e il livello di protezione */
    int16_t        cam_phys[MEMC_LOGICAL_PAGES];
    uint8_t        cam_ppl[MEMC_LOGICAL_PAGES];
    int            page_shift;            /* 12 (4 KB) .. 15 (32 KB) */

    /* tabella veloce a 4 KB: puntatore nella RAM fisica o NULL */
    uint8_t       *fast[MEMC_LOGICAL_PAGES];
    uint8_t        fast_ppl[MEMC_LOGICAL_PAGES];

    int            rom_latched;           /* ROM visibile a 0 dopo il reset */
    int            os_mode, video_dma, sound_dma;
    uint32_t       control;
    /* puntatori DMA (offset nella RAM fisica) */
    uint32_t       vinit, vstart, vend, cinit;
    uint32_t       sstart, send, sptr;
    int            cursor_enabled;
    int            sound_reg;             /* registro audio appena scritto (4, 6, 7) */

    uint32_t       aborts;                /* contatore diagnostico */
} Memc;

void memc_init(Memc *m, uint8_t *ram, uint32_t ram_size, const uint8_t *rom, uint32_t rom_size,
               const Arm2 *cpu, const MemcIo *io);
void memc_reset(Memc *m);
/* Interfaccia per la CPU */
void memc_attach_cpu(Memc *m, ArmBus *bus);

uint32_t memc_read32(Memc *m, uint32_t addr, int *abort);
uint8_t  memc_read8 (Memc *m, uint32_t addr, int *abort);
void     memc_write32(Memc *m, uint32_t addr, uint32_t v, int *abort);
void     memc_write8 (Memc *m, uint32_t addr, uint8_t v, int *abort);

/* Traduzione logico -> offset fisico (per il debug); -1 se non mappato */
int32_t  memc_translate(const Memc *m, uint32_t logical);

#endif
