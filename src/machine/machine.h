/*
 * machine.h - La macchina: assembla i moduli (bus, RAM, memoria video,
 * ROM con il modulo linguaggio, CPU ARMv2, kernel HLE, VDU) e la avvia.
 *
 * Mappa della memoria (26 bit):
 *   &0000000  RAM: pagina zero del kernel, poi l'applicazione da &8000
 *   &2000000  memoria video (16 MB)
 *   &3800000  ROM: il modulo linguaggio (BASIC, ...)
 */
#ifndef MACHINE_H
#define MACHINE_H

#include <stdint.h>
#include <stddef.h>
#include "../cpu/arm2.h"
#include "../core/bus.h"
#include "../riscos/kernel.h"
#include "../riscos/vdu.h"
#include "../riscos/module.h"

#define MACHINE_SCREEN_ADDR 0x2000000u
#define MACHINE_ROM_ADDR    0x3800000u

typedef struct MachineConfig {
    const char *rom_path;         /* modulo linguaggio */
    uint32_t    ram_mb;           /* RAM, 1-16 MB */
    int         mode;             /* modo iniziale */
    int         trace_swi;
    const char *disc_dir;         /* cartella dell'host usata come disco (NULL = nessuna) */
    int         emulated_clock;   /* TIME/INKEY dai cicli emulati invece che dall'orologio */
} MachineConfig;

/* Ricava la cartella "disc" del progetto dal percorso della ROM
   (".../third_party/riscos/BASIC" -> ".../disc"). */
void machine_default_disc(const char *rom_path, char *out, size_t size);

typedef struct Machine {
    Bus          bus;
    Arm2         cpu;
    RiscosKernel kernel;
    Vdu          vdu;
    RiscosModule module;
    uint8_t     *ram, *screen, *rom;
    uint32_t     ram_size, rom_size;
} Machine;

int  machine_create(Machine *m, const MachineConfig *cfg, char *err, size_t errsize);
void machine_destroy(Machine *m);

/* Esegue fino a 'cycles' cicli. Si ferma prima se il programma aspetta un
   tasto (kernel.waiting) o termina (cpu.halted). Ritorna i cicli eseguiti. */
uint64_t machine_run(Machine *m, uint64_t cycles);

/* Quota della banda di memoria presa dal DMA video nel modo corrente, come
   su un Archimedes (0 per i modi a 16/32 bpp, che l'Archimedes non aveva):
   il frontend riduce di questa quota i cicli dati alla CPU. */
double machine_dma_fraction(const Machine *m);

#endif
