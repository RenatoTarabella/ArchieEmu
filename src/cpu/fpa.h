/*
 * fpa.h - Modulo FPU: il coprocessore in virgola mobile FPA10 (Archimedes con
 * ARM3, come l'A5000) e FPA11 (dentro l'ARM7500FE dell'A7000+).
 *
 * Come il chip vero, esegue in hardware solo i casi comuni (numeri normali e
 * zeri, risultati senza eccezioni) e "rimbalza" il resto al vettore delle
 * istruzioni non definite, dove il codice di supporto del FPEmulator 4
 * (FPASC) finisce il lavoro. Tutti i rimbalzi sono sincroni (bit SB del
 * FPCR): il codice di supporto emula da capo l'istruzione che ha rimbalzato,
 * leggendo e scrivendo i registri della FPA. Il chip vero rimbalza in modo
 * asincrono (bit AB) le eccezioni aritmetiche, salvo nel modo "Synchronous
 * Operation"; per i programmi la differenza non si vede.
 *
 * Il modulo non conosce la CPU: le parla attraverso FpaHost.
 */
#ifndef FPA_H
#define FPA_H

#include <stdint.h>

enum { FPA_NONE = 0, FPA_FPA10, FPA_FPA11 };

/* identificativo di sistema nel byte alto del FPSR */
#define FPA_SYSID 0x81u

/* esito di fpa_exec */
enum {
    FPA_DONE = 0,       /* eseguita (*cycles = cicli della CPU in attesa) */
    FPA_BOUNCE,         /* la CPU prende l'eccezione delle istruzioni non definite */
    FPA_DATA_ABORT,     /* abort su un accesso alla memoria (LDF/STF/LFM/SFM) */
    FPA_ADDR_EXC,       /* indirizzo oltre i 26 bit (solo ARM2/ARM3) */
    FPA_NOT_FPA         /* non e' un'istruzione della FPA (coprocessore diverso) */
};

/* La CPU, vista dalla FPA */
typedef struct FpaHost {
    void *ctx;
    /* registro Rn come operando: R15 = indirizzo dell'istruzione + 8, solo
       il PC (senza i bit del PSR nel modo a 26 bit) */
    uint32_t (*get_reg)(void *ctx, int n);
    void     (*set_reg)(void *ctx, int n, uint32_t v);
    /* N Z C V (bit 31-28) nei flag della CPU, il resto del PSR invariato */
    void     (*set_flags)(void *ctx, uint32_t nzcv);
    /* accessi a parola; ritornano FPA_DONE, FPA_DATA_ABORT o FPA_ADDR_EXC */
    int      (*read32)(void *ctx, uint32_t addr, uint32_t *v);
    int      (*write32)(void *ctx, uint32_t addr, uint32_t v);
    int      (*privileged)(void *ctx);       /* la CPU non e' in modo utente */
} FpaHost;

/* Un registro nel formato interno (96 bit): w[0] = segno (31), uncommon
   (30), esponente (14-0, eccesso 16383); w[1] = unita' (31) e frazione alta;
   w[2] = frazione bassa. */
typedef struct { uint32_t w[3]; } FpaReg;

typedef struct Fpa {
    int      model;          /* FPA_FPA10 o FPA_FPA11 */
    FpaReg   f[8];
    uint32_t fpsr, fpcr;
    uint64_t bounces;        /* statistiche */
    uint64_t executed;
} Fpa;

void fpa_init(Fpa *f, int model);
void fpa_reset(Fpa *f);
/* Esegue l'istruzione di coprocessore 'instr' (condizione gia' verificata
   dalla CPU). */
int  fpa_exec(Fpa *f, uint32_t instr, const FpaHost *h, int *cycles);

#endif
