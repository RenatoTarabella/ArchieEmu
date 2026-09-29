/*
 * test_arm2.c - Test del modulo CPU sui comportamenti specifici dell'ARMv2
 * (26 bit, banchi, eccezioni). La semantica ALU "comune" agli ARM moderni
 * e' verificata a parte contro Unicorn (tests/diff_unicorn.py).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu/arm2.h"
#include "cpu/arm2_disasm.h"
#include "core/bus.h"

/* ------------------------------------------------------------------ */
/* codificatori                                                       */
/* ------------------------------------------------------------------ */

enum { AND, EOR, SUB, RSB, ADD, ADC, SBC, RSC, TST, TEQ, CMP, CMN, ORR, MOV, BIC, MVN };
enum { LSL, LSR, ASR, ROR };
enum { EQ, NE, CS, CC, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, AL };
#define PC 15
#define LR 14
#define SP 13

static uint32_t enc_imm(uint32_t v)
{
    for (unsigned r = 0; r < 16; r++) {
        uint32_t x = r ? (v << (2 * r)) | (v >> (32 - 2 * r)) : v;
        if (x < 256) return (r << 8) | x;
    }
    fprintf(stderr, "immediato non codificabile: %08X\n", v);
    exit(2);
}

static uint32_t cond(int c, uint32_t i) { return (i & 0x0FFFFFFFu) | ((uint32_t)c << 28); }

static uint32_t DPI(int op, int s, int rn, int rd, uint32_t v)
{
    return 0xE2000000u | (uint32_t)op << 21 | (uint32_t)s << 20 | (uint32_t)rn << 16 | (uint32_t)rd << 12 | enc_imm(v);
}
static uint32_t DPR(int op, int s, int rn, int rd, int rm, int sh, int amt)
{
    return 0xE0000000u | (uint32_t)op << 21 | (uint32_t)s << 20 | (uint32_t)rn << 16 | (uint32_t)rd << 12
         | (uint32_t)amt << 7 | (uint32_t)sh << 5 | (uint32_t)rm;
}
static uint32_t DPRS(int op, int s, int rn, int rd, int rm, int sh, int rs)
{
    return 0xE0000010u | (uint32_t)op << 21 | (uint32_t)s << 20 | (uint32_t)rn << 16 | (uint32_t)rd << 12
         | (uint32_t)rs << 8 | (uint32_t)sh << 5 | (uint32_t)rm;
}
/* LDR/STR con offset immediato; pre=1 indicizzato prima, wb=1 scrive la base */
static uint32_t MEM(int l, int b, int rd, int rn, int off, int pre, int wb)
{
    uint32_t u = off >= 0, a = (uint32_t)(off >= 0 ? off : -off);
    return 0xE4000000u | (uint32_t)pre << 24 | u << 23 | (uint32_t)b << 22 | (uint32_t)wb << 21
         | (uint32_t)l << 20 | (uint32_t)rn << 16 | (uint32_t)rd << 12 | a;
}
static uint32_t BLK(int l, int p, int u, int s, int w, int rn, uint32_t list)
{
    return 0xE8000000u | (uint32_t)p << 24 | (uint32_t)u << 23 | (uint32_t)s << 22 | (uint32_t)w << 21
         | (uint32_t)l << 20 | (uint32_t)rn << 16 | list;
}
static uint32_t B(uint32_t from, uint32_t to, int link)
{
    return 0xEA000000u | (uint32_t)link << 24 | (((to - from - 8) >> 2) & 0xFFFFFFu);
}
static uint32_t SWI(uint32_t n) { return 0xEF000000u | n; }
static uint32_t MUL(int rd, int rm, int rs) { return 0xE0000090u | (uint32_t)rd << 16 | (uint32_t)rs << 8 | (uint32_t)rm; }
static uint32_t MLA(int rd, int rm, int rs, int rn) { return MUL(rd, rm, rs) | 1u << 21 | (uint32_t)rn << 12; }
static uint32_t SWP(int b, int rd, int rm, int rn)
{
    return 0xE1000090u | (uint32_t)b << 22 | (uint32_t)rn << 16 | (uint32_t)rd << 12 | (uint32_t)rm;
}
#define TEQP_IMM(v) (DPI(TEQ, 1, PC, PC, (v)))
#define EXIT        SWI(0x11)
#define MOVS_PC_LR  DPR(MOV, 1, 0, PC, LR, LSL, 0)

/* ------------------------------------------------------------------ */
/* macchina di prova: 64 KB di RAM, vettori che segnalano l'eccezione */
/* ------------------------------------------------------------------ */

#define RAM_SIZE 0x10000u
#define CODE     0x8000u
#define VEC_SWI_BASE 0xEE0000u

typedef struct T {
    Bus      bus;
    Arm2     cpu;
    uint8_t  ram[RAM_SIZE];
    int      exception;        /* vettore dell'ultima eccezione non gestita, o -1 */
    uint32_t pos;              /* prossimo indirizzo per emit() */
} T;

static int hook(Arm2 *cpu, uint32_t n, void *user)
{
    T *t = (T *)user;
    if (n == 0x11) { cpu->halted = 1; return 1; }
    if ((n & 0xFFFF00u) == VEC_SWI_BASE) { t->exception = (int)(n & 0xFF); cpu->halted = 1; return 1; }
    return 0;   /* tutte le altre SWI: eccezione vera */
}

static void poke(T *t, uint32_t a, uint32_t v)
{
    int ab = 0;
    bus_write32(&t->bus, a, v, &ab);
}
static uint32_t peek(T *t, uint32_t a)
{
    int ab = 0;
    return bus_read32(&t->bus, a, &ab);
}

static T *setup(int mode)
{
    static T t;
    memset(&t, 0, sizeof t);
    bus_init(&t.bus);
    bus_map_memory(&t.bus, 0, RAM_SIZE, t.ram, 0);
    ArmBus ab;
    bus_attach_cpu(&t.bus, &ab);
    arm2_init(&t.cpu, &ab);
    t.cpu.swi_hook = hook;
    t.cpu.swi_user = &t;
    t.exception = -1;
    for (uint32_t v = 0; v < 0x20; v += 4) poke(&t, v, SWI(VEC_SWI_BASE | v));
    arm2_set_r15(&t.cpu, (uint32_t)mode);
    arm2_set_pc(&t.cpu, CODE);
    t.cpu.r[13] = RAM_SIZE;
    t.pos = CODE;
    return &t;
}

static void emit(T *t, uint32_t i) { poke(t, t->pos, i); t->pos += 4; }

static void run(T *t)
{
    for (int n = 0; n < 100000 && !t->cpu.halted; n++) arm2_step(&t->cpu);
}

/* ------------------------------------------------------------------ */
/* mini framework                                                     */
/* ------------------------------------------------------------------ */

static int failures = 0, checks = 0;
static const char *current = "";

#define CHECK(cond_) do { checks++; if (!(cond_)) { failures++; \
    fprintf(stderr, "FALLITO [%s] %s:%d: %s\n", current, __FILE__, __LINE__, #cond_); } } while (0)
#define CHECK_EQ(a, b) do { uint32_t va_ = (uint32_t)(a), vb_ = (uint32_t)(b); checks++; \
    if (va_ != vb_) { failures++; fprintf(stderr, "FALLITO [%s] %s:%d: %s = %08X, atteso %08X\n", \
        current, __FILE__, __LINE__, #a, va_, vb_); } } while (0)
#define TEST(name) static void name(void)
#define RUN_TEST(name) do { current = #name; name(); } while (0)

/* ------------------------------------------------------------------ */
/* test                                                               */
/* ------------------------------------------------------------------ */

TEST(flags_add_overflow)
{
    T *t = setup(ARM_MODE_USR);
    emit(t, DPI(MVN, 0, 0, 0, 0x80000000u));       /* R0 = &7FFFFFFF */
    emit(t, DPI(ADD, 1, 0, 1, 1));                  /* ADDS R1,R0,#1  */
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->cpu.r[1], 0x80000000u);
    CHECK_EQ(t->cpu.r[15] & 0xF0000000u, ARM_N | ARM_V);
}

TEST(r15_as_rn_vs_rm)
{
    T *t = setup(ARM_MODE_USR);
    emit(t, DPI(MOV, 1, 0, 1, 0));                  /* 8000 MOVS R1,#0  -> Z   */
    emit(t, DPI(ADD, 0, PC, 0, 0));                 /* 8004 ADD R0,PC,#0       */
    emit(t, DPR(MOV, 0, 0, 2, PC, LSL, 0));         /* 8008 MOV R2,PC          */
    emit(t, DPRS(MOV, 0, 0, 3, PC, LSL, 1));        /* 800C MOV R3,PC,LSL R1   */
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->cpu.r[0], 0x800Cu);                 /* Rn: solo PC             */
    CHECK_EQ(t->cpu.r[2], 0x8010u | ARM_Z);         /* Rm: PC + PSR            */
    CHECK_EQ(t->cpu.r[3], 0x8018u | ARM_Z);         /* shift da registro: +12  */
}

TEST(branch_and_link_keeps_psr)
{
    T *t = setup(ARM_MODE_USR);
    emit(t, DPI(CMP, 1, 0, 0, 0));                  /* 8000 CMP R0,#0 -> Z,C   */
    emit(t, B(0x8004, 0x8010, 1));                  /* 8004 BL &8010           */
    emit(t, EXIT);                                  /* 8008                    */
    emit(t, 0);                                     /* 800C                    */
    emit(t, DPI(MOV, 0, 0, 5, 7));                  /* 8010 MOV R5,#7          */
    emit(t, MOVS_PC_LR);                            /* 8014 MOVS PC,R14        */
    run(t);
    CHECK_EQ(t->cpu.r[14], 0x8008u | ARM_Z | ARM_C);
    CHECK_EQ(t->cpu.r[5], 7);
    CHECK(t->cpu.halted && t->exception == -1);
    CHECK_EQ(t->cpu.r[15] & 0xF0000000u, ARM_Z | ARM_C);
}

TEST(branch_backwards_and_conditions)
{
    T *t = setup(ARM_MODE_USR);
    emit(t, DPI(MOV, 0, 0, 0, 10));                 /* 8000 MOV R0,#10         */
    emit(t, DPI(MOV, 0, 0, 1, 0));                  /* 8004 MOV R1,#0          */
    emit(t, DPR(ADD, 0, 1, 1, 0, LSL, 0));          /* 8008 ADD R1,R1,R0       */
    emit(t, DPI(SUB, 1, 0, 0, 1));                  /* 800C SUBS R0,R0,#1      */
    emit(t, cond(NE, B(0x8010, 0x8008, 0)));        /* 8010 BNE &8008          */
    emit(t, cond(EQ, DPI(MOV, 0, 0, 2, 1)));        /* MOVEQ R2,#1             */
    emit(t, cond(NE, DPI(MOV, 0, 0, 3, 1)));        /* MOVNE R3,#1 (saltata)   */
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->cpu.r[1], 55);
    CHECK_EQ(t->cpu.r[2], 1);
    CHECK_EQ(t->cpu.r[3], 0);
}

TEST(user_mode_cannot_change_mode_or_irq)
{
    T *t = setup(ARM_MODE_USR);
    /* R14 = &8018 | N | I | SVC: MOVS PC,R14 deve prendere solo PC e flag */
    emit(t, DPI(MOV, 0, 0, LR, 0x8000));            /* 8000                */
    emit(t, DPI(ORR, 0, LR, LR, 0x18));             /* 8004                */
    emit(t, DPI(ORR, 0, LR, LR, 0x80000000u));      /* 8008                */
    emit(t, DPI(ORR, 0, LR, LR, ARM_I | 3));        /* 800C                */
    emit(t, MOVS_PC_LR);                            /* 8010                */
    emit(t, DPI(MOV, 0, 0, 0, 1));                  /* 8014 (saltata)      */
    emit(t, EXIT);                                  /* 8018                */
    run(t);
    CHECK_EQ(arm2_mode(&t->cpu), ARM_MODE_USR);
    CHECK_EQ(t->cpu.r[15] & (ARM_N | ARM_I), ARM_N);
    CHECK_EQ(t->cpu.r[0], 0);
}

TEST(swi_exception_and_return)
{
    T *t = setup(ARM_MODE_USR);
    poke(t, 0x08, B(0x08, 0x200, 0));
    poke(t, 0x200, DPI(MOV, 0, 0, 5, 0x55));        /* handler: MOV R5,#&55     */
    poke(t, 0x204, DPR(MOV, 0, 0, 6, LR, LSL, 0));  /*          MOV R6,R14      */
    poke(t, 0x208, MOVS_PC_LR);                     /*          MOVS PC,R14     */
    emit(t, DPI(MOV, 0, 0, LR, 0x3400));            /* 8000 R14_usr = &1234     */
    emit(t, DPI(CMP, 1, 0, 0, 0));                  /* 8004 Z,C                 */
    emit(t, SWI(0x42));                             /* 8008                     */
    emit(t, DPI(MOV, 0, 0, 7, 1));                  /* 800C                     */
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->cpu.r[5], 0x55);
    CHECK_EQ(t->cpu.r[6], 0x800Cu | ARM_Z | ARM_C); /* R14_svc = ritorno + PSR  */
    CHECK_EQ(t->cpu.r[7], 1);
    CHECK_EQ(t->cpu.r[14], 0x3400);                 /* R14 utente intatto       */
    CHECK_EQ(arm2_mode(&t->cpu), ARM_MODE_USR);
    CHECK_EQ(t->cpu.r[15] & 0xFC000000u, ARM_Z | ARM_C);  /* I ripristinato a 0 */
}

TEST(teqp_mode_switch_and_banking)
{
    T *t = setup(ARM_MODE_SVC);
    emit(t, DPI(MOV, 0, 0, SP, 1));                 /* R13_svc = 1              */
    emit(t, DPI(MOV, 0, 0, 8, 8));                  /* R8 = 8                   */
    emit(t, TEQP_IMM(1));                           /* -> FIQ                   */
    emit(t, DPI(MOV, 0, 0, 0, 0));                  /* NOP (hazard dei banchi)  */
    emit(t, DPI(MOV, 0, 0, 8, 9));                  /* R8_fiq = 9               */
    emit(t, DPI(MOV, 0, 0, SP, 3));                 /* R13_fiq = 3              */
    emit(t, TEQP_IMM(ARM_C | 0));                   /* -> USR, C=1              */
    emit(t, DPI(MOV, 0, 0, 0, 0));
    emit(t, DPR(MOV, 0, 0, 1, 8, LSL, 0));          /* R1 = R8 utente           */
    emit(t, DPI(MOV, 0, 0, SP, 2));                 /* R13_usr = 2              */
    emit(t, EXIT);
    run(t);
    CHECK_EQ(arm2_mode(&t->cpu), ARM_MODE_USR);
    CHECK_EQ(t->cpu.r[1], 8);
    CHECK_EQ(t->cpu.r[13], 2);
    CHECK_EQ(t->cpu.svc_r13_14[0], 1);
    CHECK_EQ(t->cpu.fiq_r8_14[0], 9);
    CHECK_EQ(t->cpu.fiq_r8_14[5], 3);
    CHECK_EQ(t->cpu.r[15] & 0xFC000000u, ARM_C);
}

TEST(irq_entry_and_return)
{
    T *t = setup(ARM_MODE_USR);
    poke(t, 0x18, B(0x18, 0x300, 0));
    poke(t, 0x300, DPI(ADD, 0, 6, 6, 1));           /* R6++                      */
    poke(t, 0x304, DPR(MOV, 0, 0, 7, LR, LSL, 0));  /* R7 = R14_irq              */
    poke(t, 0x308, DPI(SUB, 1, LR, PC, 4));         /* SUBS PC,R14,#4            */
    for (int k = 0; k < 8; k++) emit(t, DPI(ADD, 0, 0, 0, 1));
    emit(t, EXIT);
    /* tre istruzioni, poi IRQ; la linea si abbassa appena entrati */
    for (int k = 0; k < 3; k++) arm2_step(&t->cpu);
    t->cpu.irq_line = 1;
    arm2_step(&t->cpu);
    CHECK_EQ(arm2_mode(&t->cpu), ARM_MODE_IRQ);
    CHECK_EQ(arm2_pc(&t->cpu), 0x18);
    CHECK(t->cpu.r[15] & ARM_I);
    t->cpu.irq_line = 0;
    run(t);
    CHECK_EQ(t->cpu.r[0], 8);                       /* nessuna istruzione persa  */
    CHECK_EQ(t->cpu.r[6], 1);
    CHECK_EQ(t->cpu.r[7], 0x8010u);                 /* R14 = prossima + 4        */
    CHECK_EQ(arm2_mode(&t->cpu), ARM_MODE_USR);
    CHECK(!(t->cpu.r[15] & ARM_I));
}

TEST(irq_masked)
{
    T *t = setup(ARM_MODE_SVC);                     /* setup: I=0 */
    emit(t, TEQP_IMM(ARM_I | 3));                   /* SVC con IRQ disabilitati */
    emit(t, DPI(MOV, 0, 0, 0, 1));
    emit(t, EXIT);
    arm2_step(&t->cpu);
    t->cpu.irq_line = 1;
    run(t);
    CHECK_EQ(t->cpu.r[0], 1);
    CHECK_EQ(t->exception, -1);
}

TEST(ldr_unaligned_rotates)
{
    T *t = setup(ARM_MODE_USR);
    poke(t, 0x1000, 0x44332211u);
    emit(t, DPI(MOV, 0, 0, 1, 0x1000));
    emit(t, MEM(1, 0, 0, 1, 1, 1, 0));              /* LDR R0,[R1,#1]  */
    emit(t, MEM(1, 0, 2, 1, 3, 1, 0));              /* LDR R2,[R1,#3]  */
    emit(t, MEM(1, 1, 3, 1, 2, 1, 0));              /* LDRB R3,[R1,#2] */
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->cpu.r[0], 0x11443322u);
    CHECK_EQ(t->cpu.r[2], 0x33221144u);
    CHECK_EQ(t->cpu.r[3], 0x33);
}

TEST(str_byte_and_indexing)
{
    T *t = setup(ARM_MODE_USR);
    poke(t, 0x1000, 0xFFFFFFFFu);
    emit(t, DPI(MOV, 0, 0, 1, 0x1000));
    emit(t, DPI(MOV, 0, 0, 0, 0xAB));
    emit(t, MEM(0, 1, 0, 1, 2, 1, 0));              /* STRB R0,[R1,#2]      */
    emit(t, DPI(MOV, 0, 0, 2, 0x1100));
    emit(t, MEM(0, 0, 0, 2, 4, 1, 1));              /* STR R0,[R2,#4]!      */
    emit(t, DPI(MOV, 0, 0, 3, 0x1200));
    emit(t, MEM(0, 0, 0, 3, -8, 0, 0));             /* STR R0,[R3],#-8      */
    emit(t, EXIT);
    run(t);
    CHECK_EQ(peek(t, 0x1000), 0xFFABFFFFu);
    CHECK_EQ(t->cpu.r[2], 0x1104);
    CHECK_EQ(peek(t, 0x1104), 0xAB);
    CHECK_EQ(t->cpu.r[3], 0x11F8);
    CHECK_EQ(peek(t, 0x1200), 0xAB);
}

TEST(str_pc_stores_plus12_with_psr)
{
    T *t = setup(ARM_MODE_USR);
    emit(t, DPI(MOV, 1, 0, 0, 0));                  /* 8000 MOVS R0,#0 -> Z */
    emit(t, DPI(MOV, 0, 0, 1, 0x1000));             /* 8004 */
    emit(t, MEM(0, 0, PC, 1, 0, 1, 0));             /* 8008 STR PC,[R1]     */
    emit(t, DPI(ADD, 0, 1, 1, 8));                  /* 800C                 */
    emit(t, BLK(0, 0, 1, 0, 0, 1, 1u << PC) | 4);   /* 8010 STMIA R1,{R2,PC}*/
    emit(t, EXIT);
    run(t);
    CHECK_EQ(peek(t, 0x1000), 0x8014u | ARM_Z);
    CHECK_EQ(peek(t, 0x100C), 0x801Cu | ARM_Z);
}

TEST(ldr_pc_keeps_flags)
{
    T *t = setup(ARM_MODE_USR);
    poke(t, 0x1000, 0xF0008010u);                   /* flag tutti a 1 nella parola */
    emit(t, DPI(MOV, 0, 0, 1, 0x1000));             /* 8000 */
    emit(t, MEM(1, 0, PC, 1, 0, 1, 0));             /* 8004 LDR PC,[R1] */
    emit(t, 0);
    emit(t, 0);
    emit(t, EXIT);                                  /* 8010 */
    run(t);
    CHECK_EQ(t->cpu.r[15] & 0xF0000000u, 0);
    CHECK(t->exception == -1);
}

TEST(stm_ldm_stack_with_psr_restore)
{
    T *t = setup(ARM_MODE_SVC);
    poke(t, 0x400, BLK(0, 1, 0, 0, 1, SP, 0x400F));  /* STMFD R13!,{R0-R3,R14}   */
    poke(t, 0x404, DPI(MOV, 0, 0, 0, 0));
    poke(t, 0x408, DPI(MOV, 0, 0, 1, 0));
    poke(t, 0x40C, BLK(1, 0, 1, 1, 1, SP, 0x800F));  /* LDMFD R13!,{R0-R3,PC}^   */
    emit(t, DPI(MOV, 0, 0, 0, 10));
    emit(t, DPI(MOV, 0, 0, 1, 11));
    emit(t, DPI(MOV, 0, 0, 2, 12));
    emit(t, DPI(MOV, 0, 0, 3, 13));
    emit(t, DPI(CMP, 1, 0, 0, 10));                 /* Z=1 C=1 */
    emit(t, DPR(MOV, 0, 0, LR, PC, LSL, 0));        /* 8014: R14 = &801C | flags | SVC */
    emit(t, B(0x8018, 0x400, 0));                   /* 8018 */
    emit(t, EXIT);                                  /* 801C */
    run(t);
    CHECK_EQ(t->cpu.r[0], 10);
    CHECK_EQ(t->cpu.r[1], 11);
    CHECK_EQ(t->cpu.r[3], 13);
    CHECK_EQ(t->cpu.r[13], RAM_SIZE);
    CHECK_EQ(t->cpu.r[15] & 0xF0000000u, ARM_Z | ARM_C);
    CHECK_EQ(arm2_mode(&t->cpu), ARM_MODE_SVC);
    CHECK_EQ(arm2_pc(&t->cpu), 0x8020);
}

TEST(stm_user_bank)
{
    T *t = setup(ARM_MODE_SVC);
    t->cpu.usr_r8_14[5] = 0xAAAA;                   /* R13 utente */
    emit(t, DPI(MOV, 0, 0, 1, 0x1000));
    emit(t, BLK(0, 0, 1, 1, 0, 1, 1u << SP));       /* STMIA R1,{R13}^ */
    emit(t, EXIT);
    run(t);
    CHECK_EQ(peek(t, 0x1000), 0xAAAA);
}

TEST(stm_base_in_list)
{
    T *t = setup(ARM_MODE_USR);
    emit(t, DPI(MOV, 0, 0, 0, 0x1000));
    emit(t, DPI(MOV, 0, 0, 1, 0x1100));
    emit(t, BLK(0, 0, 1, 0, 1, 0, 0x3));            /* STMIA R0!,{R0,R1}: R0 primo  */
    emit(t, BLK(0, 0, 1, 0, 1, 1, 0x3));            /* STMIA R1!,{R0,R1}: R1 secondo */
    emit(t, EXIT);
    run(t);
    CHECK_EQ(peek(t, 0x1000), 0x1000);              /* vecchio valore */
    CHECK_EQ(t->cpu.r[0], 0x1008);
    CHECK_EQ(peek(t, 0x1100), 0x1008);
    CHECK_EQ(peek(t, 0x1104), 0x1108);              /* nuovo valore   */
}

TEST(ldm_modes)
{
    T *t = setup(ARM_MODE_USR);
    for (uint32_t k = 0; k < 8; k++) poke(t, 0xFF0 + 4 * k, 100 + k);
    emit(t, DPI(MOV, 0, 0, 8, 0x1000));
    emit(t, BLK(1, 1, 0, 0, 0, 8, 0x3));            /* LDMDB R8,{R0,R1} -> 102,103 */
    emit(t, BLK(1, 0, 0, 0, 0, 8, 0xC));            /* LDMDA R8,{R2,R3} -> 103,104 */
    emit(t, BLK(1, 1, 1, 0, 0, 8, 0x30));           /* LDMIB R8,{R4,R5} -> 105,106 */
    emit(t, BLK(1, 0, 1, 0, 1, 8, 0x100 | 0x40));   /* LDMIA R8!,{R6,R8}: base caricata */
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->cpu.r[0], 102); CHECK_EQ(t->cpu.r[1], 103);
    CHECK_EQ(t->cpu.r[2], 103); CHECK_EQ(t->cpu.r[3], 104);
    CHECK_EQ(t->cpu.r[4], 105); CHECK_EQ(t->cpu.r[5], 106);
    CHECK_EQ(t->cpu.r[6], 104); CHECK_EQ(t->cpu.r[8], 105);
}

TEST(multiply)
{
    T *t = setup(ARM_MODE_USR);
    emit(t, DPI(MOV, 0, 0, 1, 1000));
    emit(t, DPI(MVN, 0, 0, 2, 2));                  /* R2 = -3 */
    emit(t, DPI(MOV, 0, 0, 3, 7));
    emit(t, MUL(4, 1, 2));                          /* R4 = -3000 */
    emit(t, MLA(5, 1, 3, 4));                       /* R5 = 7000 - 3000 */
    emit(t, MUL(6, 1, 2) | 1u << 20);               /* MULS -> N */
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->cpu.r[4], (uint32_t)-3000);
    CHECK_EQ(t->cpu.r[5], 4000);
    CHECK(t->cpu.r[15] & ARM_N);
}

TEST(swap)
{
    T *t = setup(ARM_MODE_USR);
    poke(t, 0x1000, 0x11223344u);
    emit(t, DPI(MOV, 0, 0, 1, 0x1000));
    emit(t, DPI(MOV, 0, 0, 2, 0x55));
    emit(t, SWP(0, 0, 2, 1));                       /* SWP R0,R2,[R1]  */
    emit(t, SWP(1, 3, 2, 1));                       /* SWPB R3,R2,[R1] */
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->cpu.r[0], 0x11223344u);
    CHECK_EQ(t->cpu.r[3], 0x55);
    CHECK_EQ(peek(t, 0x1000), 0x55);
}

TEST(address_exception)
{
    T *t = setup(ARM_MODE_USR);
    emit(t, DPI(MOV, 0, 0, 1, 0x04000000u));
    emit(t, MEM(1, 0, 0, 1, 0, 1, 0));              /* 8004 LDR R0,[R1] */
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->exception, ARM_VEC_ADDRESS);
    CHECK_EQ(arm2_mode(&t->cpu), ARM_MODE_SVC);
    CHECK_EQ(t->cpu.r[14] & ARM_PC_MASK, 0x800C);   /* istruzione + 8 */
}

TEST(data_abort_unmapped)
{
    T *t = setup(ARM_MODE_USR);
    emit(t, DPI(MOV, 0, 0, 1, 0x100000));
    emit(t, DPI(MOV, 0, 0, 0, 5));
    emit(t, MEM(1, 0, 0, 1, 0, 1, 0));              /* 8008 LDR R0,[R1] */
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->exception, ARM_VEC_DABORT);
    CHECK_EQ(t->cpu.r[14] & ARM_PC_MASK, 0x8010);
    arm2_set_r15(&t->cpu, ARM_MODE_USR);
    CHECK_EQ(t->cpu.r[0], 5);                       /* R0 non toccato */
}

TEST(prefetch_abort)
{
    T *t = setup(ARM_MODE_USR);
    emit(t, DPI(MOV, 0, 0, 1, 0x200000));
    emit(t, DPR(MOV, 0, 0, PC, 1, LSL, 0));         /* MOV PC,R1 */
    run(t);
    CHECK_EQ(t->exception, ARM_VEC_PABORT);
    CHECK_EQ(t->cpu.r[14] & ARM_PC_MASK, 0x200004);
}

TEST(undefined_instruction)
{
    T *t = setup(ARM_MODE_USR);
    emit(t, 0xEE000000u);                           /* CDP senza coprocessore */
    run(t);
    CHECK_EQ(t->exception, ARM_VEC_UNDEF);
    CHECK_EQ(t->cpu.r[14] & ARM_PC_MASK, 0x8004);
}

TEST(shifter_edge_cases)
{
    T *t = setup(ARM_MODE_USR);
    emit(t, DPI(MOV, 0, 0, 0, 0x80000001u));
    emit(t, DPR(MOV, 1, 0, 1, 0, LSR, 0));          /* LSR #32 -> 0, C=1   */
    emit(t, DPR(MOV, 0, 0, 2, 0, ASR, 0));          /* ASR #32 -> -1       */
    emit(t, DPR(MOV, 1, 0, 3, 0, ROR, 0));          /* RRX con C=1         */
    emit(t, DPI(MOV, 0, 0, 9, 33));
    emit(t, DPRS(MOV, 1, 0, 4, 0, LSL, 9));         /* LSL R9=33 -> 0, C=0 */
    emit(t, DPI(MOV, 0, 0, 9, 0));
    emit(t, DPI(CMP, 1, 0, 0, 0));                  /* C=1 */
    emit(t, DPRS(MOV, 1, 0, 5, 0, LSR, 9));         /* shift 0: C invariato */
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->cpu.r[1], 0);
    CHECK_EQ(t->cpu.r[2], 0xFFFFFFFFu);
    CHECK_EQ(t->cpu.r[3], 0xC0000000u);
    CHECK_EQ(t->cpu.r[4], 0);
    CHECK_EQ(t->cpu.r[5], 0x80000001u);
    CHECK(t->cpu.r[15] & ARM_C);
}

TEST(pc_wraps_26bit)
{
    /* un salto oltre la fine dello spazio a 26 bit si avvolge */
    T *t = setup(ARM_MODE_USR);
    arm2_set_pc(&t->cpu, 0x3FFFFF8u);
    poke(t, 0x3FFFFF8u & (RAM_SIZE - 1), 0);
    t->cpu.r[15] = ARM_Z | 0x8000u;
    emit(t, 0xEAFFFFFFu);                           /* 8000 B &8004 */
    arm2_step(&t->cpu);
    CHECK_EQ(arm2_pc(&t->cpu), 0x8004);
    char buf[64];
    arm2_disasm(0xEAFFFFFCu, 0, buf, sizeof buf);   /* B -8 da 0      */
    CHECK(strcmp(buf, "B &3FFFFF8") == 0);
    arm2_disasm(0xEAFFFFFCu, 0, buf, sizeof buf);
    t->cpu.r[15] = 0;
    poke(t, 0, 0xEAFFFFFCu);
    arm2_step(&t->cpu);
    CHECK_EQ(t->cpu.r[15], 0x3FFFFF8u);             /* nessun bit sporcato nel PSR */
}

TEST(pipeline_self_modifying)
{
    /* STR sull'istruzione successiva: l'ARM2 l'ha gia' letta, esegue la vecchia
       (trucco usato dalle protezioni anticopia, es. Elite) */
    T *t = setup(ARM_MODE_USR);
    emit(t, DPI(MOV, 0, 0, 1, 0x8000));             /* 8000 */
    emit(t, DPI(ADD, 0, 1, 1, 0x10));               /* 8004 R1 = &8010 */
    emit(t, DPI(MOV, 0, 0, 2, 0));                  /* 8008 */
    emit(t, MEM(0, 0, 3, 1, 0, 1, 0));              /* 800C STR R3,[R1]: riscrive &8010 */
    emit(t, DPI(MOV, 0, 0, 5, 5));                  /* 8010 MOV R5,#5 (gia' nella pipeline) */
    emit(t, DPI(MOV, 0, 0, 6, 6));                  /* 8014 */
    emit(t, EXIT);
    t->cpu.r[3] = DPI(MOV, 0, 0, 5, 9);             /* la versione "nuova": MOV R5,#9 */
    run(t);
    CHECK_EQ(t->cpu.r[5], 5);
    CHECK_EQ(peek(t, 0x8010), DPI(MOV, 0, 0, 5, 9)); /* in memoria c'e' la nuova */

    /* dopo un salto la pipeline si svuota e si vede la versione nuova */
    t = setup(ARM_MODE_USR);
    emit(t, DPI(MOV, 0, 0, 1, 0x8000));             /* 8000 */
    emit(t, DPI(ADD, 0, 1, 1, 0x18));               /* 8004 R1 = &8018 */
    emit(t, MEM(0, 0, 3, 1, 0, 1, 0));              /* 8008 STR R3,[R1] */
    emit(t, B(0x800C, 0x8018, 0));                  /* 800C B &8018 */
    emit(t, 0);                                     /* 8010 */
    emit(t, 0);                                     /* 8014 */
    emit(t, DPI(MOV, 0, 0, 5, 5));                  /* 8018 (riscritta prima di arrivarci) */
    emit(t, EXIT);
    t->cpu.r[3] = DPI(MOV, 0, 0, 5, 9);
    run(t);
    CHECK_EQ(t->cpu.r[5], 9);
}

TEST(disassembler)
{
    struct { uint32_t i; uint32_t addr; const char *text; } cases[] = {
        { DPI(MOV, 1, 0, 0, 0),                0, "MOVS R0,#0" },
        { MOVS_PC_LR,                          0, "MOVS PC,R14" },
        { TEQP_IMM(3),                         0, "TEQP PC,#3" },
        { DPR(ADD, 0, 1, 2, 3, LSL, 2),        0, "ADD R2,R1,R3,LSL #2" },
        { DPRS(ORR, 1, 1, 2, 3, ROR, 4),       0, "ORRS R2,R1,R3,ROR R4" },
        { cond(NE, DPI(CMP, 1, 0, 0, 255)),    0, "CMPNE R0,#&FF" },
        { MEM(1, 0, 0, 1, 4, 1, 1),            0, "LDR R0,[R1,#4]!" },
        { MEM(0, 1, 2, 3, -1, 0, 0),           0, "STRB R2,[R3],#-1" },
        { MEM(1, 0, 0, PC, 8, 1, 0),           0x8000, "LDR R0,&8010" },
        { BLK(0, 1, 0, 0, 1, SP, 0x400F),      0, "STMFD R13!,{R0-R3,R14}" },
        { BLK(1, 0, 1, 1, 1, SP, 0x800F),      0, "LDMFD R13!,{R0-R3,PC}^" },
        { BLK(1, 0, 1, 0, 0, 2, 0x5),          0, "LDMIA R2,{R0,R2}" },
        { B(0x8000, 0x8100, 1),                0x8000, "BL &8100" },
        { SWI(0x20011),                        0, "SWI &20011" },
        { MLA(1, 2, 3, 4),                     0, "MLA R1,R2,R3,R4" },
        { SWP(1, 0, 1, 2),                     0, "SWPB R0,R1,[R2]" },
    };
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        char buf[64];
        arm2_disasm(cases[k].i, cases[k].addr, buf, sizeof buf);
        checks++;
        if (strcmp(buf, cases[k].text)) {
            failures++;
            fprintf(stderr, "FALLITO [disassembler] %08X: \"%s\", atteso \"%s\"\n", cases[k].i, buf, cases[k].text);
        }
    }
}

int main(void)
{
    RUN_TEST(flags_add_overflow);
    RUN_TEST(r15_as_rn_vs_rm);
    RUN_TEST(branch_and_link_keeps_psr);
    RUN_TEST(branch_backwards_and_conditions);
    RUN_TEST(user_mode_cannot_change_mode_or_irq);
    RUN_TEST(swi_exception_and_return);
    RUN_TEST(teqp_mode_switch_and_banking);
    RUN_TEST(irq_entry_and_return);
    RUN_TEST(irq_masked);
    RUN_TEST(ldr_unaligned_rotates);
    RUN_TEST(str_byte_and_indexing);
    RUN_TEST(str_pc_stores_plus12_with_psr);
    RUN_TEST(ldr_pc_keeps_flags);
    RUN_TEST(stm_ldm_stack_with_psr_restore);
    RUN_TEST(stm_user_bank);
    RUN_TEST(stm_base_in_list);
    RUN_TEST(ldm_modes);
    RUN_TEST(multiply);
    RUN_TEST(swap);
    RUN_TEST(address_exception);
    RUN_TEST(data_abort_unmapped);
    RUN_TEST(prefetch_abort);
    RUN_TEST(undefined_instruction);
    RUN_TEST(shifter_edge_cases);
    RUN_TEST(pc_wraps_26bit);
    RUN_TEST(pipeline_self_modifying);
    RUN_TEST(disassembler);

    printf("%d controlli, %d falliti\n", checks, failures);
    return failures ? 1 : 0;
}
