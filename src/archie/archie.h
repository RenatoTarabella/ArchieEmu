/*
 * archie.h - La macchina Archimedes (stile A310/A3000) a basso livello:
 * CPU ARMv2, MEMC, IOC, VIDC, tastiera, CMOS e floppy, con una ROM vera
 * di Arthur o RISC OS (fino alla 3.11).
 *
 * A differenza della macchina "BASIC" (src/machine), qui il sistema
 * operativo e' quello originale: la CPU esegue la ROM e parla con i chip.
 */
#ifndef ARCHIE_H
#define ARCHIE_H

#include <stdint.h>
#include <stddef.h>
#include "../cpu/arm2.h"
#include "archie_time.h"
#include "memc.h"
#include "ioc.h"
#include "vidc.h"
#include "kbd.h"
#include "cmos.h"
#include "fdc.h"

typedef struct ArchieConfig {
    const char *rom_path;        /* immagine della ROM (512 KB - 2 MB) */
    uint32_t    ram_mb;          /* 1, 2 o 4 */
    const char *cmos_path;       /* dove leggere/salvare la CMOS (NULL = predefinita) */
    const char *floppy[2];       /* immagini .adf nelle unita' 0 e 1 (NULL = vuote) */
    double      mhz;             /* clock della CPU (8 = ARM2) */
    const char *hostfs_dir;      /* cartella dell'host vista come HostFS (NULL = niente scheda) */
} ArchieConfig;

typedef struct Archie {
    Arm2     cpu;
    Memc     memc;
    Ioc      ioc;
    Vidc     vidc;
    Kbd      kbd;
    Cmos     cmos;
    Fdc      fdc;

    uint8_t *ram, *rom;
    uint32_t ram_size, rom_size;
    char     cmos_path[512];

    /* tempo: 'now' corrisponde a cpu.cycles == cycle_base */
    ArcTime  now;
    uint64_t cycle_base;
    uint32_t units_per_cycle_q16;  /* unita' da 24 MHz per tick CPU, virgola fissa 16.16 */
    double   mhz;                  /* clock della CPU */
    int      faithful;             /* tempi fedeli: cicli N, ROM lenta, DMA video */
    double   dma_fraction;         /* quota di banda presa dal DMA video */

    /* video */
    ArcTime  frame_start;
    int      flyback;
    uint64_t frames;

    /* audio: DMA del MEMC verso il VIDC */
    uint32_t sound_sfr;          /* periodo di un byte: SFR + 2 microsecondi */
    uint32_t snd_ptr, snd_end;   /* buffer corrente (offset nella RAM fisica) */
    ArcTime  snd_next;           /* istante del prossimo byte */
    ArcTime  out_t;              /* inizio del campione d'uscita in costruzione */
    double   acc_l, acc_r;
    int16_t *audio;              /* anello di campioni stereo a ARCHIE_AUDIO_HZ */
    uint32_t audio_w, audio_r;

    uint8_t  latch_a, latch_b;
    int      sda_in;

    /* scheda di espansione 0: ROM con il modulo HostFS (vuota se hostfs_dir manca) */
    uint8_t *podule_rom;
    uint32_t podule_size;
    struct ArcHostFS *hostfs;

    /* facoltativo, per il debug: scrittura del latch A (il POST di
       RISC OS 3 ci manda il suo rapporto in seriale sul bit 0) */
    void   (*latch_a_hook)(void *user, uint8_t value, ArcTime now);
    void    *hook_user;
} Archie;

int  archie_create(Archie *a, const ArchieConfig *cfg, char *err, size_t errsize);
void archie_destroy(Archie *a);
void archie_reset(Archie *a);

/* Esegue la macchina per 'duration' unita' di tempo emulato. */
void archie_run(Archie *a, ArcTime duration);
/* Istante corrente, con i cicli CPU gia' eseguiti. */
ArcTime archie_now(const Archie *a);

/* Disegna lo schermo; *w e *h: dimensione in pixel. */
void archie_render(Archie *a, uint32_t *out, int stride, int *w, int *h);

void archie_set_mhz(Archie *a, double mhz);

/* Audio: campioni stereo a 16 bit, interlacciati (sinistro, destro). */
#define ARCHIE_AUDIO_HZ     48000
#define ARCHIE_AUDIO_FRAMES 65536u               /* capacita' dell'anello */
/* Copia fino a 'max' campioni stereo prodotti; ritorna quanti. */
uint32_t archie_audio_read(Archie *a, int16_t *out, uint32_t max);
/* 1 = tempi fedeli dell'ARM2 sul MEMC (predefinito), 0 = un ciclo per tick */
void archie_set_faithful(Archie *a, int on);

#endif
