/*
 * kernel.h - Kernel RISC OS emulato ad alto livello (HLE)
 *
 * Le SWI arrivano dal gancio della CPU e sono eseguite in C: uscita sul
 * driver VDU, tastiera, ambiente dei gestori (OS_ChangeEnvironment),
 * MessageTrans, ResourceFS e le conversioni numeriche.
 *
 * Le SWI che aspettano un tasto non bloccano: riportano il PC sulla SWI e
 * alzano 'waiting'; il ciclo principale smette di eseguire fino al frame
 * successivo e la SWI viene ripetuta.
 */
#ifndef RISCOS_KERNEL_H
#define RISCOS_KERNEL_H

#include <stdint.h>
#include "../cpu/arm2.h"
#include "../core/bus.h"
#include "vdu.h"

/* indirizzi nella pagina zero, usati dal kernel emulato */
#define K_VECTORS        0x0000u
#define K_TRAMPOLINE     0x0FF0u   /* SWI di ritorno per le chiamate host -> ARM */
#define K_EXIT_STUB      0x0FF8u
#define K_ERRBLOCK       0x1000u   /* errori generati dal kernel (256 byte) */
#define K_MSGBUF         0x1100u   /* buffer interno di MessageTrans (1 KB) */
#define K_ENVSTRING      0x1500u   /* riga di comando per OS_GetEnv */
#define K_STARTTIME      0x15F8u
#define K_PRIVWORD       0x15FCu   /* parola privata del modulo linguaggio */
#define K_SCRATCH        0x1600u
#define K_SVC_STACK      0x7C00u
#define K_APP_BASE       0x8000u

#define K_SWI_RETURN     0xEE00FFu /* SWI privata del trampolino */
#define K_SWI_EXCEPTION  0xEE0000u /* +vettore: eccezione non gestita */

#define K_MAX_RESBLOCKS  8
#define K_MAX_FILES      16
#define K_KEYBUF         256

typedef struct KernelFile {
    int      used;
    int      host;                 /* 1 = file dell'host (HostFS), 0 = ResourceFS */
    void    *fp;                   /* FILE* per i file dell'host */
    uint32_t data, size, ptr;      /* file di ResourceFS: dati in memoria emulata */
} KernelFile;

typedef struct RiscosKernel {
    Arm2    *cpu;
    Bus     *bus;
    Vdu     *vdu;

    uint32_t handlers[17][3];      /* OS_ChangeEnvironment: indirizzo, R12, buffer */
    uint32_t app_limit;
    char     command_line[240];

    /* tastiera */
    uint8_t  keys[K_KEYBUF];
    int      key_head, key_tail;
    int      escape_pending;
    int      escape_enabled;

    /* stato delle SWI che aspettano */
    int      waiting;
    int      readline_active;
    uint32_t readline_len;
    int      inkey_active;
    uint64_t inkey_deadline;

    uint32_t resblocks[K_MAX_RESBLOCKS];
    int      nresblocks;
    KernelFile files[K_MAX_FILES];
    char     disc_dir[512];        /* cartella dell'host che fa da disco ("" = nessuna) */

    /* tempo: centesimi di secondo dell'host */
    uint64_t (*now_cs)(void *ctx);
    void     *now_ctx;
    uint64_t start_cs;
    int64_t  time_offset;          /* TIME = adesso - start + offset */

    int      call_done;            /* chiamata host -> ARM terminata */
    int      faithful;             /* addebita alla CPU il tempo delle SWI */
    int      exit_code;
    int      trace_swi;
    uint32_t last_unknown_swi;
} RiscosKernel;

void kernel_init(RiscosKernel *k, Arm2 *cpu, Bus *bus, Vdu *vdu, uint32_t app_limit);
int  kernel_swi(Arm2 *cpu, uint32_t comment, void *user);     /* gancio SWI */

/* Esegue codice ARM dall'host (init dei moduli, gestore di Escape).
   regs: R0-R12 in ingresso e in uscita (puo' essere NULL). */
int  kernel_call(RiscosKernel *k, uint32_t addr, int mode, uint32_t *regs);

/* Input dal frontend (codici RISC OS: 13 Return, 127 Delete, 27 Escape...) */
void kernel_key(RiscosKernel *k, uint8_t code);
int  kernel_keys_pending(const RiscosKernel *k);

uint64_t kernel_time_cs(RiscosKernel *k);

#endif
