/*
 * hostfs.h - HostFS dell'Archimedes: una cartella dell'host vista da RISC OS.
 *
 * Il modulo HostFS (hostfs_module.s, nella ROM della scheda 0) si dichiara
 * a FileSwitch; ogni ingresso FSEntry_* esegue la SWI ARC_HOSTFS_SWI + n,
 * che la CPU passa a arc_hostfs_entry prima di prendere l'eccezione. Qui si
 * fa il lavoro sui file dell'host e si torna con i registri come li vuole
 * FileSwitch 2.08 (RISC OS 3.11): V acceso e R0 -> blocco d'errore in caso
 * di errore (il blocco sta nell'area di lavoro del modulo, puntata da R12).
 */
#ifndef ARC_HOSTFS_H
#define ARC_HOSTFS_H

#include <stdint.h>
#include "../cpu/arm2.h"

#define ARC_HOSTFS_SWI    0x56AC0u      /* + numero dell'ingresso (0..7) */
#define ARC_HOSTFS_FILES  64

/* offset nell'area di lavoro del modulo (come in hostfs_module.s) */
#define ARC_HOSTFS_WS_ERROR 64
#define ARC_HOSTFS_WS_NAME  576

struct Memc;

typedef struct ArcHostFS {
    char   root[512];
    void  *fp[ARC_HOSTFS_FILES];        /* FILE* aperti; l'indice + 1 e' la maniglia */
    char   path[ARC_HOSTFS_FILES][600];
    struct Memc *memc;
} ArcHostFS;

void arc_hostfs_init(ArcHostFS *h, const char *root, struct Memc *memc);
void arc_hostfs_close_all(ArcHostFS *h);
/* esegue l'ingresso 'entry' (0 Open, 1 GetBytes, 2 PutBytes, 3 Args, 4 Close, 5 File, 6 Func) */
void arc_hostfs_entry(ArcHostFS *h, Arm2 *cpu, int entry);

#endif
