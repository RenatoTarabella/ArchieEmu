/*
 * riscos_swi.h - Emulazione ad alto livello (HLE) di alcune SWI di RISC OS.
 *
 * Invece di eseguire il sistema operativo in codice ARM, l'host intercetta
 * le SWI e le esegue direttamente in C. Per ora c'e' il minimo per far
 * parlare un programma con il terminale.
 */
#ifndef RISCOS_SWI_H
#define RISCOS_SWI_H

#include "../cpu/arm2.h"
#include "../core/bus.h"

typedef struct RiscosHle {
    Bus      *bus;
    uint32_t  ram_limit;     /* restituito da OS_GetEnv              */
    int       exit_code;
    int       trace;         /* stampa le SWI su stderr               */
} RiscosHle;

/* Da installare come cpu->swi_hook, con cpu->swi_user = RiscosHle* */
int riscos_swi_hook(Arm2 *cpu, uint32_t comment, void *user);

#endif
