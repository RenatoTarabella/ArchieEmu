/*
 * arm6.h - Modulo CPU: ARMv3 (ARM610 e ARM710, Risc PC)
 *
 * Spazio d'indirizzi a 32 bit, CPSR separato dal PC e un SPSR per ogni modo
 * privilegiato. Restano i modi a 26 bit dell'ARM2 (R15 letto come PC + PSR),
 * nei quali RISC OS 3.5-3.7 passa quasi tutto il tempo.
 *
 *   CPSR:  31 30 29 28 27 ...... 8  7  6  5  4 3 2 1 0
 *           N  Z  C  V  [ zero ]    I  F  0  [  modo  ]
 *
 * Il coprocessore 15 (ID, controllo, MMU) sta dentro il core: la macchina
 * vede sul bus solo indirizzi fisici. Gli altri coprocessori non ci sono
 * (le istruzioni FPA vanno al vettore delle istruzioni non definite, dove
 * le aspetta il FPEmulator).
 */
#ifndef ARM6_H
#define ARM6_H

#include <stdint.h>
#include "armbus.h"

/* ---- bit del CPSR ---- */
#define ARM6_N         0x80000000u
#define ARM6_Z         0x40000000u
#define ARM6_C         0x20000000u
#define ARM6_V         0x10000000u
#define ARM6_I         0x00000080u
#define ARM6_F         0x00000040u
#define ARM6_MODE_MASK 0x0000001Fu
#define ARM6_PSR_BITS  0xF00000DFu   /* i bit che esistono su ARMv3 */

/* modi: i quattro dell'ARM2 (0-3) e i sei a 32 bit (bit 4 = 1) */
enum {
    ARM6_USR26 = 0x00, ARM6_FIQ26 = 0x01, ARM6_IRQ26 = 0x02, ARM6_SVC26 = 0x03,
    ARM6_USR32 = 0x10, ARM6_FIQ32 = 0x11, ARM6_IRQ32 = 0x12, ARM6_SVC32 = 0x13,
    ARM6_ABT32 = 0x17, ARM6_UND32 = 0x1B
};

/* vettori delle eccezioni (come sull'ARM2; 0x14 solo coi dati a 26 bit) */
enum {
    ARM6_VEC_RESET   = 0x00, ARM6_VEC_UNDEF  = 0x04, ARM6_VEC_SWI   = 0x08,
    ARM6_VEC_PABORT  = 0x0C, ARM6_VEC_DABORT = 0x10, ARM6_VEC_ADDRESS = 0x14,
    ARM6_VEC_IRQ     = 0x18, ARM6_VEC_FIQ    = 0x1C
};

/* registro di controllo di CP15 (c1) */
#define ARM6_CTRL_M  0x001u    /* MMU                                   */
#define ARM6_CTRL_A  0x002u    /* controllo dell'allineamento           */
#define ARM6_CTRL_C  0x004u    /* cache                                 */
#define ARM6_CTRL_W  0x008u    /* write buffer                          */
#define ARM6_CTRL_P  0x010u    /* programma a 32 bit: eccezioni nei modi a 32 bit */
#define ARM6_CTRL_D  0x020u    /* dati a 32 bit: niente address exception */
#define ARM6_CTRL_L  0x040u    /* abort "tardivi" (ARM710): la base si aggiorna */
#define ARM6_CTRL_B  0x080u    /* big endian (non emulato)              */
#define ARM6_CTRL_S  0x100u    /* protezione di sistema                 */
#define ARM6_CTRL_R  0x200u    /* protezione della ROM                  */

/* ID letti da MRC p15,0,Rd,c0,c0 */
#define ARM6_ID_ARM610 0x41560610u
#define ARM6_ID_ARM710 0x41007100u

/* TLB software: una voce per blocco da 1 KB (la granularita' delle
   sottopagine), mappata direttamente */
#define ARM6_TLB_BITS 12
#define ARM6_TLB_SIZE (1u << ARM6_TLB_BITS)

typedef struct Arm6TlbEntry {
    uint32_t tag;            /* (va >> 10) | 1, 0 = vuota                  */
    uint32_t pa;             /* indirizzo fisico del blocco da 1 KB        */
    uint8_t  fsr[4];         /* esito per [utente*2 + scrittura]: 0 = ok,
                                altrimenti lo stato per l'FSR             */
} Arm6TlbEntry;

struct Arm6;

/* Come per l'ARM2: ritorna 1 se la SWI e' stata gestita dall'host. */
typedef int (*Arm6SwiHook)(struct Arm6 *cpu, uint32_t comment, void *user);

typedef struct Arm6 {
    uint32_t r[16];          /* registri visibili; r[15] = PC (senza PSR)  */
    uint32_t cpsr;
    uint32_t spsr[6];        /* per banco: [0] (utente) non esiste          */

    /* banchi: copie dei registri non visibili nel modo corrente */
    uint32_t usr_r8_12[5];
    uint32_t fiq_r8_12[5];
    uint32_t r13_14[6][2];   /* per banco: USR FIQ IRQ SVC ABT UND           */

    int      irq_line;
    int      fiq_line;

    uint64_t cycles;
    uint64_t instructions;
    uint32_t s_ticks, n_ticks;
    uint32_t extra_cycles;

    /* CP15 */
    uint32_t cp15_id;
    uint32_t ctrl;           /* c1 */
    uint32_t ttb;            /* c2 */
    uint32_t dacr;           /* c3 */
    uint32_t fsr;            /* c5 */
    uint32_t far;            /* c6 */
    Arm6TlbEntry tlb[ARM6_TLB_SIZE];

    ArmBus      bus;
    Arm6SwiHook swi_hook;
    void       *swi_user;
    void      (*exception_hook)(struct Arm6 *cpu, uint32_t vector, uint32_t link, void *user);
    void       *exception_user;
    void      (*trace_hook)(struct Arm6 *cpu, uint32_t pc, uint32_t instr);

    int      halted;

    /* pipeline a tre stadi come sull'ARM2 (vedi arm2.h) */
    uint32_t pipe[2];
    uint8_t  pipe_abort[2];
    uint32_t pipe_addr;
    int      pipe_valid;
} Arm6;

/* cpu_id: ARM6_ID_ARM610 o ARM6_ID_ARM710 */
void     arm6_init(Arm6 *cpu, const ArmBus *bus, uint32_t cpu_id);
/* Reset: SVC26, IRQ e FIQ disabilitati, PC = 0, CP15 azzerato (MMU spenta,
   configurazione a 26 bit) */
void     arm6_reset(Arm6 *cpu);
int      arm6_step(Arm6 *cpu);
uint64_t arm6_run(Arm6 *cpu, uint64_t max_cycles);

static inline int      arm6_mode(const Arm6 *c)  { return (int)(c->cpsr & ARM6_MODE_MASK); }
static inline int      arm6_is26(const Arm6 *c)  { return !(c->cpsr & 0x10u); }
static inline uint32_t arm6_pc(const Arm6 *c)    { return c->r[15]; }

/* R15 come lo vede un programma a 26 bit (PC + PSR), qualunque sia il modo */
uint32_t arm6_r15_26(const Arm6 *cpu);
/* Cambia il CPSR intero, scambiando i banchi se cambia il modo */
void     arm6_set_cpsr(Arm6 *cpu, uint32_t psr);
void     arm6_set_pc(Arm6 *cpu, uint32_t pc);

uint32_t arm6_get_user_reg(const Arm6 *cpu, int n);
void     arm6_set_user_reg(Arm6 *cpu, int n, uint32_t v);

/* Traduzione di un indirizzo virtuale (per il debug e per i frontend):
   ritorna 0 e mette in *pa l'indirizzo fisico, o lo stato del fault. */
int      arm6_translate(Arm6 *cpu, uint32_t va, int write, int user, uint32_t *pa);
void     arm6_tlb_flush(Arm6 *cpu);

const char *arm6_mode_name(int mode);

#endif
