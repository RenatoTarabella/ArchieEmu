/*
 * arm2.h - Modulo CPU: ARMv2a (ARM2/ARM3, Acorn Archimedes)
 *
 * Architettura a 26 bit: R15 contiene insieme PC e PSR.
 *
 *   31 30 29 28 27 26 25 ........................ 2  1  0
 *    N  Z  C  V  I  F  [        PC (parole)       ] M1 M0
 *
 * La CPU non conosce la macchina: parla con il mondo solo attraverso
 * l'interfaccia ArmBus e (facoltativamente) il gancio per le SWI.
 */
#ifndef ARM2_H
#define ARM2_H

#include <stdint.h>

/* ---- bit di R15 ---- */
#define ARM_N        0x80000000u
#define ARM_Z        0x40000000u
#define ARM_C        0x20000000u
#define ARM_V        0x10000000u
#define ARM_I        0x08000000u
#define ARM_F        0x04000000u
#define ARM_PC_MASK  0x03FFFFFCu
#define ARM_PSR_MASK 0xFC000003u
#define ARM_MODE_MASK 0x00000003u

enum { ARM_MODE_USR = 0, ARM_MODE_FIQ = 1, ARM_MODE_IRQ = 2, ARM_MODE_SVC = 3 };

/* Vettori delle eccezioni */
enum {
    ARM_VEC_RESET     = 0x00,
    ARM_VEC_UNDEF     = 0x04,
    ARM_VEC_SWI       = 0x08,
    ARM_VEC_PABORT    = 0x0C,
    ARM_VEC_DABORT    = 0x10,
    ARM_VEC_ADDRESS   = 0x14,
    ARM_VEC_IRQ       = 0x18,
    ARM_VEC_FIQ       = 0x1C
};

/* Interfaccia verso il bus (armbus.h). Gli indirizzi arrivano gia' a 26 bit. */
#include "armbus.h"

struct Arm2;

/*
 * Gancio SWI: chiamato prima di prendere l'eccezione SWI.
 * Ritorna 1 se la SWI e' stata gestita dall'host (emulazione ad alto
 * livello, es. le chiamate di RISC OS), 0 per l'eccezione normale.
 */
typedef int (*ArmSwiHook)(struct Arm2 *cpu, uint32_t comment, void *user);

typedef struct Arm2 {
    uint32_t r[16];          /* registri visibili; r[15] = PC|PSR          */

    /* banchi: copie dei registri non visibili nel modo corrente */
    uint32_t usr_r8_14[7];   /* R8-R14 utente (R8-R12 validi solo in FIQ)  */
    uint32_t fiq_r8_14[7];
    uint32_t irq_r13_14[2];
    uint32_t svc_r13_14[2];

    int      irq_line;       /* livelli delle linee di interrupt (1 = attiva) */
    int      fiq_line;

    uint64_t cycles;         /* tick del clock: cicli S/N/I pesati coi costi sotto */
    uint64_t instructions;

    /* Costo in tick di ogni tipo di ciclo, deciso dalla memoria della
       macchina. Col MEMC a 8 MHz: S = 1, N = 2. I fetch di istruzioni da
       indirizzi >= slow_base (la ROM, senza accesso sequenziale) costano
       slow_fetch_extra tick in piu'. extra_cycles: tick aggiunti una volta
       sola (es. il costo stimato di una SWI emulata dall'host). */
    uint32_t s_ticks, n_ticks;
    uint32_t slow_base, slow_fetch_extra;
    uint32_t extra_cycles;

    ArmBus     bus;
    ArmSwiHook swi_hook;
    void      *swi_user;
    /* facoltativo, per il debug: chiamato all'ingresso di ogni eccezione */
    void     (*exception_hook)(struct Arm2 *cpu, uint32_t vector, uint32_t link, void *user);
    void      *exception_user;
    /* facoltativo, per il debug: chiamato per ogni istruzione eseguita */
    void     (*trace_hook)(struct Arm2 *cpu, uint32_t pc, uint32_t instr);

    int      halted;         /* messo a 1 dall'host per fermare arm2_run  */

    /* segnale TRANS: 1 durante un LDRT/STRT, che il MEMC deve trattare
       come accesso in modo utente anche se la CPU e' privilegiata */
    int      trans_user;

    /* Pipeline a tre stadi: mentre si esegue l'istruzione all'indirizzo A,
       quelle ad A+4 e A+8 sono gia' state lette. Il codice che modifica le
       istruzioni immediatamente successive NON vede l'effetto (alcune
       protezioni anticopia contano su questo). Si svuota scrivendo il PC. */
    uint32_t pipe[2];
    uint8_t  pipe_abort[2];
    uint32_t pipe_addr;      /* indirizzo di pipe[0] */
    int      pipe_valid;
} Arm2;

void     arm2_init(Arm2 *cpu, const ArmBus *bus);
void     arm2_reset(Arm2 *cpu);
/* Esegue un'istruzione (o l'ingresso in un'eccezione). Ritorna i cicli. */
int      arm2_step(Arm2 *cpu);
/* Esegue fino a 'max_cycles' cicli o finche' halted. Ritorna i cicli fatti. */
uint64_t arm2_run(Arm2 *cpu, uint64_t max_cycles);

static inline uint32_t arm2_pc(const Arm2 *cpu)   { return cpu->r[15] & ARM_PC_MASK; }
static inline int      arm2_mode(const Arm2 *cpu) { return (int)(cpu->r[15] & ARM_MODE_MASK); }

/* Scrive R15 intero (PC e PSR) cambiando banco se cambia il modo. */
void     arm2_set_r15(Arm2 *cpu, uint32_t value);
void     arm2_set_pc(Arm2 *cpu, uint32_t pc);

/* Accesso ai registri del modo utente, qualunque sia il modo corrente. */
uint32_t arm2_get_user_reg(const Arm2 *cpu, int n);
void     arm2_set_user_reg(Arm2 *cpu, int n, uint32_t v);

const char *arm2_mode_name(int mode);

#endif
