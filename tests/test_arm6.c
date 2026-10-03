/*
 * test_arm6.c - Test del core ARMv3 (ARM610/710): modi a 26 e a 32 bit,
 * CPSR/SPSR, eccezioni nelle due configurazioni, CP15 e MMU. La semantica
 * ALU e di load/store nei modi a 32 bit e' confrontata a parte con Unicorn
 * (tests/diff_unicorn_arm6.py).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu/arm6.h"
#include "cpu/arm2_disasm.h"

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

static uint32_t DPI(int op, int s, int rn, int rd, uint32_t v)
{
    return 0xE2000000u | (uint32_t)op << 21 | (uint32_t)s << 20 | (uint32_t)rn << 16 | (uint32_t)rd << 12 | enc_imm(v);
}
static uint32_t DPR(int op, int s, int rn, int rd, int rm, int sh, int amt)
{
    return 0xE0000000u | (uint32_t)op << 21 | (uint32_t)s << 20 | (uint32_t)rn << 16 | (uint32_t)rd << 12
         | (uint32_t)amt << 7 | (uint32_t)sh << 5 | (uint32_t)rm;
}
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
static uint32_t MRS(int rd, int spsr) { return 0xE10F0000u | (uint32_t)spsr << 22 | (uint32_t)rd << 12; }
static uint32_t MSR(int spsr, int fields, int rm)
{
    return 0xE120F000u | (uint32_t)spsr << 22 | (uint32_t)fields << 16 | (uint32_t)rm;
}
static uint32_t MSRI(int spsr, int fields, uint32_t v)
{
    return 0xE320F000u | (uint32_t)spsr << 22 | (uint32_t)fields << 16 | enc_imm(v);
}
static uint32_t MCR15(int crn, int rd) { return 0xEE000F10u | (uint32_t)crn << 16 | (uint32_t)rd << 12; }
static uint32_t MRC15(int crn, int rd) { return MCR15(crn, rd) | 1u << 20; }
#define TEQP_IMM(v) (DPI(TEQ, 1, PC, PC, (v)))
#define EXIT        SWI(0x11)
#define MOVS_PC_LR  DPR(MOV, 1, 0, PC, LR, LSL, 0)
#define F_C 1   /* campi di MSR */
#define F_F 8

/* ------------------------------------------------------------------ */
/* macchina di prova: 1 MB di RAM a 0 e 1 MB a &10000000              */
/* ------------------------------------------------------------------ */

#define RAM_SIZE 0x100000u
#define HIGH     0x10000000u
#define CODE     0x8000u
#define VEC_SWI_BASE 0xEE0000u

typedef struct T {
    Arm6     cpu;
    uint8_t  ram[RAM_SIZE];
    uint8_t  high[RAM_SIZE];
    int      exception;
    uint32_t pos;
} T;

static uint8_t *phys(T *t, uint32_t a)
{
    if (a < RAM_SIZE) return &t->ram[a];
    if (a - HIGH < RAM_SIZE) return &t->high[a - HIGH];
    return NULL;
}
static uint32_t b_r32(void *ctx, uint32_t a, int *ab)
{
    uint8_t *p = phys(ctx, a & ~3u);
    if (!p) { *ab = 1; return 0; }
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint8_t b_r8(void *ctx, uint32_t a, int *ab)
{
    uint8_t *p = phys(ctx, a);
    if (!p) { *ab = 1; return 0; }
    return *p;
}
static void b_w32(void *ctx, uint32_t a, uint32_t v, int *ab)
{
    uint8_t *p = phys(ctx, a & ~3u);
    if (!p) { *ab = 1; return; }
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void b_w8(void *ctx, uint32_t a, uint8_t v, int *ab)
{
    uint8_t *p = phys(ctx, a);
    if (!p) { *ab = 1; return; }
    *p = v;
}

static int hook(Arm6 *cpu, uint32_t n, void *user)
{
    T *t = (T *)user;
    if (n == 0x11) { cpu->halted = 1; return 1; }
    if ((n & 0xFFFF00u) == VEC_SWI_BASE) { t->exception = (int)(n & 0xFF); cpu->halted = 1; return 1; }
    return 0;
}

static void poke(T *t, uint32_t a, uint32_t v) { int ab = 0; b_w32(t, a, v, &ab); }
static uint32_t peek(T *t, uint32_t a) { int ab = 0; return b_r32(t, a, &ab); }

/* mode: un modo qualsiasi; con un modo a 32 bit si accende anche P */
static T *setup(int mode)
{
    static T t;
    memset(&t, 0, sizeof t);
    ArmBus ab = { &t, b_r32, b_r8, b_w32, b_w8, NULL, NULL };
    arm6_init(&t.cpu, &ab, ARM6_ID_ARM610);
    t.cpu.swi_hook = hook;
    t.cpu.swi_user = &t;
    t.exception = -1;
    for (uint32_t v = 0; v < 0x20; v += 4) poke(&t, v, SWI(VEC_SWI_BASE | v));
    if (mode & 0x10) t.cpu.ctrl |= ARM6_CTRL_P | ARM6_CTRL_D;
    arm6_set_cpsr(&t.cpu, (uint32_t)mode);
    arm6_set_pc(&t.cpu, CODE);
    t.cpu.r[13] = 0x10000;
    t.pos = CODE;
    return &t;
}

static void emit(T *t, uint32_t i) { poke(t, t->pos, i); t->pos += 4; }

static void run(T *t)
{
    for (int n = 0; n < 100000 && !t->cpu.halted; n++) arm6_step(&t->cpu);
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
/* modi a 26 bit (configurazione dopo il reset)                       */
/* ------------------------------------------------------------------ */

TEST(reset_state)
{
    T *t = setup(ARM6_SVC26);
    arm6_reset(&t->cpu);
    CHECK_EQ(t->cpu.cpsr, ARM6_I | ARM6_F | ARM6_SVC26);
    CHECK_EQ(t->cpu.r[15], 0);
    CHECK_EQ(t->cpu.ctrl, 0);
    CHECK_EQ(arm6_r15_26(&t->cpu), 0x0C000003u);
}

TEST(r15_26bit_as_rn_vs_rm)
{
    T *t = setup(ARM6_USR26);
    emit(t, DPI(MOV, 1, 0, 1, 0));                  /* 8000 MOVS R1,#0 -> Z */
    emit(t, DPI(ADD, 0, PC, 0, 0));                 /* 8004 ADD R0,PC,#0    */
    emit(t, DPR(MOV, 0, 0, 2, PC, LSL, 0));         /* 8008 MOV R2,PC       */
    emit(t, B(0x800C, 0x8014, 1));                  /* 800C BL              */
    emit(t, EXIT);
    emit(t, EXIT);                                  /* 8014                 */
    run(t);
    CHECK_EQ(t->cpu.r[0], 0x800C);
    CHECK_EQ(t->cpu.r[2], 0x8010u | ARM6_Z);
    CHECK_EQ(t->cpu.r[14], 0x8010u | ARM6_Z);
}

TEST(swi_26bit_config)
{
    T *t = setup(ARM6_USR26);
    poke(t, 0x08, B(0x08, 0x200, 0));
    poke(t, 0x200, DPR(MOV, 0, 0, 6, LR, LSL, 0));  /* MOV R6,R14   */
    poke(t, 0x204, MRS(7, 0));                      /* MRS R7,CPSR  */
    poke(t, 0x208, MOVS_PC_LR);
    emit(t, DPI(CMP, 1, 0, 0, 0));                  /* 8000 Z,C */
    emit(t, SWI(0x42));                             /* 8004     */
    emit(t, EXIT);                                  /* 8008     */
    run(t);
    CHECK_EQ(t->cpu.r[6], 0x8008u | 0x60000000u);  /* R14 = ritorno + PSR */
    CHECK_EQ(t->cpu.r[7], 0x60000000u | ARM6_I | ARM6_SVC26);
    CHECK_EQ(t->cpu.spsr[3], 0x60000000u | ARM6_USR26);
    CHECK_EQ(t->cpu.cpsr, 0x60000000u | ARM6_USR26);
}

TEST(teqp_26bit_and_banking)
{
    T *t = setup(ARM6_SVC26);
    emit(t, DPI(MOV, 0, 0, SP, 1));
    emit(t, DPI(MOV, 0, 0, 8, 8));
    emit(t, TEQP_IMM(0x08000001u));                 /* -> FIQ26, I=1 */
    emit(t, DPI(MOV, 0, 0, 8, 9));
    emit(t, MRS(0, 0));
    emit(t, TEQP_IMM(0x20000000u));                 /* -> USR26, C   */
    emit(t, DPR(MOV, 0, 0, 1, 8, LSL, 0));
    emit(t, TEQP_IMM(0x00000003u));                 /* utente: solo i flag */
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->cpu.r[0], ARM6_I | ARM6_FIQ26);
    CHECK_EQ(t->cpu.r[1], 8);
    CHECK_EQ(t->cpu.fiq_r8_12[0], 9);
    CHECK_EQ(t->cpu.r13_14[3][0], 1);
    CHECK_EQ(t->cpu.cpsr, ARM6_USR26);
}

TEST(address_exception_only_with_26bit_data)
{
    T *t = setup(ARM6_USR26);
    emit(t, DPI(MOV, 0, 0, 1, HIGH));
    emit(t, MEM(1, 0, 0, 1, 0, 1, 0));              /* 8004 LDR R0,[R1] */
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->exception, ARM6_VEC_ADDRESS);
    CHECK_EQ(arm6_mode(&t->cpu), ARM6_SVC26);
    CHECK_EQ(t->cpu.r[14] & 0x03FFFFFCu, 0x800C);

    t = setup(ARM6_USR26);
    t->cpu.ctrl = ARM6_CTRL_D;                      /* dati a 32 bit */
    t->high[0] = 0x5A;
    emit(t, DPI(MOV, 0, 0, 1, HIGH));
    emit(t, MEM(1, 0, 0, 1, 0, 1, 0));
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->exception, -1);
    CHECK_EQ(t->cpu.r[0], 0x5A);
}

TEST(undefined_26bit_config_enters_svc26)
{
    T *t = setup(ARM6_USR26);
    emit(t, 0xEE000000u);                           /* CDP */
    run(t);
    CHECK_EQ(t->exception, ARM6_VEC_UNDEF);
    CHECK_EQ(arm6_mode(&t->cpu), ARM6_SVC26);
    CHECK_EQ(t->cpu.r[14], 0x8004);
}

TEST(msr_32bit_mode_needs_p)
{
    T *t = setup(ARM6_SVC26);
    emit(t, MSRI(0, F_C, ARM6_SVC32));              /* P = 0: ignorato */
    emit(t, MRS(0, 0));
    emit(t, DPI(MOV, 0, 0, 1, ARM6_CTRL_P));
    emit(t, MCR15(1, 1));                           /* P = 1 */
    emit(t, MSRI(0, F_C, ARM6_SVC32));
    emit(t, MRS(2, 0));
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->cpu.r[0], ARM6_SVC26);
    CHECK_EQ(t->cpu.r[2], ARM6_SVC32);
}

TEST(clearing_p_falls_back_to_26bit_mode)
{
    /* il POST di RISC OS 3.5: MCR c1 con P = 0 mentre e' in SVC32, poi TEQP */
    T *t = setup(ARM6_SVC32);
    emit(t, DPI(MOV, 0, 0, 0, ARM6_CTRL_D));
    emit(t, MCR15(1, 0));
    emit(t, MRS(1, 0));
    emit(t, TEQP_IMM(0x0C000001u));                 /* -> FIQ26 */
    emit(t, MRS(2, 0));
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->cpu.r[1], ARM6_SVC26);
    CHECK_EQ(t->cpu.r[2], ARM6_I | ARM6_F | ARM6_FIQ26);
}

/* ------------------------------------------------------------------ */
/* modi a 32 bit                                                      */
/* ------------------------------------------------------------------ */

TEST(r15_32bit_and_high_pc)
{
    T *t = setup(ARM6_USR32);
    emit(t, DPI(MOV, 1, 0, 1, 0));                  /* 8000 Z        */
    emit(t, DPI(ADD, 0, PC, 0, 0));                 /* 8004          */
    emit(t, DPR(MOV, 0, 0, 2, PC, LSL, 0));         /* 8008          */
    emit(t, DPI(MOV, 0, 0, 3, HIGH));               /* 800C          */
    emit(t, MEM(0, 0, PC, 3, 0, 1, 0));             /* 8010 STR PC   */
    emit(t, DPR(MOV, 0, 0, PC, 3, LSL, 0));         /* 8014 MOV PC,R3 */
    int ab = 0;
    b_w32(t, HIGH + 4, B(HIGH + 4, HIGH + 12, 1), &ab);
    b_w32(t, HIGH + 12, EXIT, &ab);
    run(t);
    CHECK_EQ(t->cpu.r[0], 0x800C);
    CHECK_EQ(t->cpu.r[2], 0x8010);                  /* nessun PSR in R15  */
    CHECK_EQ(b_r32(t, HIGH, &ab), 0x801C);          /* STR PC: +12 */
    CHECK_EQ(t->cpu.r[14], HIGH + 8);               /* BL: solo l'indirizzo */
    CHECK_EQ(t->cpu.r[15], HIGH + 16);
}

TEST(mode_banks_32bit)
{
    T *t = setup(ARM6_SVC32);
    uint32_t modes[] = { ARM6_FIQ32, ARM6_IRQ32, ARM6_ABT32, ARM6_UND32, ARM6_SVC32 };
    for (int k = 0; k < 5; k++) {
        emit(t, MSRI(0, F_C, modes[k] | ARM6_I | ARM6_F));
        emit(t, DPI(MOV, 0, 0, SP, 0x100u * (uint32_t)(k + 1)));
        emit(t, DPI(MOV, 0, 0, 8, (uint32_t)(k + 1)));
        emit(t, DPI(MOV, 0, 0, LR, (uint32_t)(k + 1)));
    }
    emit(t, MSRI(0, F_C, ARM6_SVC32));
    emit(t, DPR(MOV, 0, 0, 0, SP, LSL, 0));
    emit(t, MSRI(0, F_C, ARM6_ABT32));
    emit(t, DPR(MOV, 0, 0, 1, SP, LSL, 0));
    emit(t, MSRI(0, F_C, ARM6_SVC26));              /* i modi a 26 bit condividono i banchi */
    emit(t, DPR(MOV, 0, 0, 2, SP, LSL, 0));
    emit(t, DPR(MOV, 0, 0, 3, 8, LSL, 0));
    emit(t, MSRI(0, F_C, ARM6_FIQ26));
    emit(t, DPR(MOV, 0, 0, 4, 8, LSL, 0));
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->cpu.r[0], 0x500);
    CHECK_EQ(t->cpu.r[1], 0x300);
    CHECK_EQ(t->cpu.r[2], 0x500);
    CHECK_EQ(t->cpu.r[3], 5);                       /* R8 condiviso fuori da FIQ */
    CHECK_EQ(t->cpu.r[4], 1);                       /* R8_fiq */
    CHECK_EQ(t->cpu.r13_14[5][1], 4);               /* R14_und */
}

TEST(swi_32bit_spsr_and_return)
{
    T *t = setup(ARM6_USR32);
    poke(t, 0x08, B(0x08, 0x200, 0));
    poke(t, 0x200, DPR(MOV, 0, 0, 6, LR, LSL, 0));
    poke(t, 0x204, MRS(7, 1));                      /* MRS R7,SPSR */
    poke(t, 0x208, MRS(8, 0));
    poke(t, 0x20C, MOVS_PC_LR);
    emit(t, DPI(CMP, 1, 0, 0, 0));                  /* 8000 Z,C */
    emit(t, SWI(0x42));                             /* 8004 */
    emit(t, MRS(9, 0));                             /* 8008 */
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->cpu.r[6], 0x8008);
    CHECK_EQ(t->cpu.r[7], 0x60000000u | ARM6_USR32);
    CHECK_EQ(t->cpu.r[8], 0x60000000u | ARM6_I | ARM6_SVC32);
    CHECK_EQ(t->cpu.r[9], 0x60000000u | ARM6_USR32);
}

TEST(exception_from_26bit_mode_with_p)
{
    /* RISC OS 3.5: programmi a 32 bit (P=1) ma codice nei modi a 26 bit */
    T *t = setup(ARM6_USR26);
    t->cpu.ctrl = ARM6_CTRL_P | ARM6_CTRL_D;
    poke(t, 0x08, B(0x08, 0x200, 0));
    poke(t, 0x200, DPR(MOV, 0, 0, 6, LR, LSL, 0));
    poke(t, 0x204, MRS(7, 1));
    poke(t, 0x208, MOVS_PC_LR);
    emit(t, DPI(MOV, 1, 0, 0, 0));                  /* 8000 Z */
    emit(t, SWI(0x42));                             /* 8004   */
    emit(t, DPR(MOV, 0, 0, 1, PC, LSL, 0));         /* 8008   */
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->cpu.r[6], 0x8008);                  /* niente PSR in R14 */
    CHECK_EQ(t->cpu.r[7], ARM6_Z | ARM6_USR26);
    CHECK_EQ(t->cpu.r[1], 0x8010u | ARM6_Z);        /* di nuovo a 26 bit */
}

TEST(undefined_and_aborts_32bit_modes)
{
    T *t = setup(ARM6_USR32);
    emit(t, 0xEE000000u);
    run(t);
    CHECK_EQ(t->exception, ARM6_VEC_UNDEF);
    CHECK_EQ(arm6_mode(&t->cpu), ARM6_UND32);
    CHECK_EQ(t->cpu.r[14], 0x8004);

    t = setup(ARM6_USR32);
    emit(t, DPI(MOV, 0, 0, 1, 0x20000000u));
    emit(t, MEM(1, 0, 0, 1, 0, 1, 0));              /* 8004 */
    run(t);
    CHECK_EQ(t->exception, ARM6_VEC_DABORT);
    CHECK_EQ(arm6_mode(&t->cpu), ARM6_ABT32);
    CHECK_EQ(t->cpu.r[14], 0x800C);
    CHECK_EQ(t->cpu.spsr[4], ARM6_USR32);

    t = setup(ARM6_USR32);
    emit(t, DPI(MOV, 0, 0, 1, 0x20000000u));
    emit(t, DPR(MOV, 0, 0, PC, 1, LSL, 0));
    run(t);
    CHECK_EQ(t->exception, ARM6_VEC_PABORT);
    CHECK_EQ(arm6_mode(&t->cpu), ARM6_ABT32);
    CHECK_EQ(t->cpu.r[14], 0x20000004u);

    /* moltiplicazioni lunghe e LDRH non esistono sull'ARM610 */
    t = setup(ARM6_USR32);
    emit(t, 0xE0810392u);                           /* UMULL */
    run(t);
    CHECK_EQ(t->exception, ARM6_VEC_UNDEF);
    t = setup(ARM6_USR32);
    emit(t, 0xE1D100B0u);                           /* LDRH R0,[R1] */
    run(t);
    CHECK_EQ(t->exception, ARM6_VEC_UNDEF);
}

TEST(irq_32bit_entry_and_return)
{
    T *t = setup(ARM6_USR32);
    poke(t, 0x18, B(0x18, 0x300, 0));
    poke(t, 0x300, DPI(ADD, 0, 6, 6, 1));
    poke(t, 0x304, DPR(MOV, 0, 0, 7, LR, LSL, 0));
    poke(t, 0x308, DPI(SUB, 1, LR, PC, 4));          /* SUBS PC,R14,#4 */
    for (int k = 0; k < 8; k++) emit(t, DPI(ADD, 0, 0, 0, 1));
    emit(t, EXIT);
    for (int k = 0; k < 3; k++) arm6_step(&t->cpu);
    t->cpu.irq_line = 1;
    arm6_step(&t->cpu);
    CHECK_EQ(arm6_mode(&t->cpu), ARM6_IRQ32);
    CHECK(t->cpu.cpsr & ARM6_I);
    t->cpu.irq_line = 0;
    run(t);
    CHECK_EQ(t->cpu.r[0], 8);
    CHECK_EQ(t->cpu.r[6], 1);
    CHECK_EQ(t->cpu.r[7], 0x8010);
    CHECK_EQ(t->cpu.cpsr, ARM6_USR32);
}

TEST(ldm_with_s_restores_spsr)
{
    T *t = setup(ARM6_SVC32);
    t->cpu.spsr[3] = ARM6_C | ARM6_USR32;
    poke(t, 0x1000, 7);
    poke(t, 0x1004, 0x8010);
    emit(t, DPI(MOV, 0, 0, 1, 0x1000));             /* 8000 */
    emit(t, BLK(1, 0, 1, 1, 0, 1, 0x8001));         /* 8004 LDMIA R1,{R0,PC}^ */
    emit(t, 0); emit(t, 0);
    emit(t, EXIT);                                  /* 8010 */
    run(t);
    CHECK_EQ(t->cpu.r[0], 7);
    CHECK_EQ(t->cpu.cpsr, ARM6_C | ARM6_USR32);
}

TEST(msr_user_and_spsr)
{
    T *t = setup(ARM6_USR32);
    emit(t, DPI(MVN, 0, 0, 0, 0));                  /* R0 = &FFFFFFFF */
    emit(t, MSR(0, F_F | F_C, 0));                  /* utente: solo i flag */
    emit(t, MRS(1, 0));
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->cpu.r[1], 0xF0000000u | ARM6_USR32);

    t = setup(ARM6_SVC32);
    emit(t, MSRI(1, F_F, 0xA0000000u));             /* SPSR_flg */
    emit(t, MSRI(1, F_C, ARM6_IRQ26));
    emit(t, MRS(2, 1));
    emit(t, MSRI(0, F_C, 0x1F));                    /* modo inesistente: resta */
    emit(t, MRS(3, 0));
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->cpu.r[2], 0xA0000000u | ARM6_IRQ26);
    CHECK_EQ(t->cpu.r[3], ARM6_SVC32);              /* I e F azzerati */
}

TEST(cp15_id_and_user_access)
{
    T *t = setup(ARM6_SVC26);
    emit(t, MRC15(0, 0));
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->cpu.r[0], ARM6_ID_ARM610);

    t = setup(ARM6_USR26);
    emit(t, MRC15(0, 0));
    run(t);
    CHECK_EQ(t->exception, ARM6_VEC_UNDEF);
}

/* ------------------------------------------------------------------ */
/* MMU                                                                */
/* ------------------------------------------------------------------ */

#define L1 0x4000u     /* tabella di primo livello (16 KB allineata) */
#define L2 0x8C00u     /* una tabella di secondo livello (1 KB)      */

static uint32_t section(uint32_t pa, int ap, int dom) { return (pa & 0xFFF00000u) | (uint32_t)ap << 10 | (uint32_t)dom << 5 | 0x12; }

/* VA 0 -> PA 0 (1 MB, sezione, dominio 0), MMU accesa da 'mode' */
static T *setup_mmu(int mode)
{
    T *t = setup(mode);
    for (uint32_t k = 0; k < 4096; k++) poke(t, L1 + 4 * k, 0);
    poke(t, L1, section(0, 3, 0));
    t->cpu.ttb = L1;
    t->cpu.dacr = 0x55555555u;                      /* tutti client */
    t->cpu.ctrl |= ARM6_CTRL_M;
    arm6_tlb_flush(&t->cpu);
    return t;
}

TEST(mmu_section_mapping)
{
    T *t = setup_mmu(ARM6_SVC32);
    poke(t, L1 + 4 * 0x020, section(HIGH, 3, 0));   /* VA &2000000 -> &10000000 */
    int ab = 0;
    b_w32(t, HIGH + 0x4800, 0xCAFEF00Du, &ab);
    emit(t, DPI(MOV, 0, 0, 1, 0x02000000u));
    emit(t, DPI(ADD, 0, 1, 1, 0x4800));
    emit(t, MEM(1, 0, 0, 1, 0, 1, 0));
    emit(t, DPI(MOV, 0, 0, 2, 0x99));
    emit(t, MEM(0, 1, 2, 1, 1, 1, 0));              /* STRB R2,[R1,#1] */
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->cpu.r[0], 0xCAFEF00Du);
    CHECK_EQ(b_r32(t, HIGH + 0x4800, &ab), 0xCAFE99 << 8 | 0x0D);
    uint32_t pa = 0;
    CHECK_EQ(arm6_translate(&t->cpu, 0x02012345u, 0, 0, &pa), 0);
    CHECK_EQ(pa, HIGH + 0x12345);
}

TEST(mmu_translation_and_domain_faults)
{
    T *t = setup_mmu(ARM6_SVC32);
    emit(t, DPI(MOV, 0, 0, 1, 0x03000000u));        /* nessuna voce */
    emit(t, MEM(1, 0, 0, 1, 0, 1, 0));
    run(t);
    CHECK_EQ(t->exception, ARM6_VEC_DABORT);
    CHECK_EQ(t->cpu.fsr, 0x05);
    CHECK_EQ(t->cpu.far_addr, 0x03000000u);

    t = setup_mmu(ARM6_SVC32);
    poke(t, L1 + 4 * 0x030, section(HIGH, 3, 7));
    t->cpu.dacr = 0x55555555u & ~(3u << 14);         /* dominio 7: nessun accesso */
    emit(t, DPI(MOV, 0, 0, 1, 0x03000000u));
    emit(t, DPI(ORR, 0, 1, 1, 0x40));
    emit(t, MEM(0, 0, 0, 1, 0, 1, 0));
    run(t);
    CHECK_EQ(t->exception, ARM6_VEC_DABORT);
    CHECK_EQ(t->cpu.fsr, 0x79);
    CHECK_EQ(t->cpu.far_addr, 0x03000040u);

    /* manager: nessun controllo dei permessi */
    t = setup_mmu(ARM6_USR32);
    poke(t, L1, section(0, 0, 2));
    t->cpu.dacr = 3u << 4;
    emit(t, DPI(MOV, 0, 0, 1, 0x1000));
    emit(t, MEM(0, 0, 1, 1, 0, 1, 0));
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->exception, -1);
    CHECK_EQ(peek(t, 0x1000), 0x1000);
}

TEST(mmu_small_pages_and_subpage_permissions)
{
    T *t = setup_mmu(ARM6_USR32);
    /* VA &100000 (1 MB): tabella delle pagine nel dominio 1 */
    poke(t, L1 + 4 * 1, L2 | 1u << 5 | 0x11);
    for (uint32_t k = 0; k < 256; k++) poke(t, L2 + 4 * k, 0);
    /* pagina 0 -> PA &10003000; sottopagine: AP 3, 2, 1, 3 */
    poke(t, L2, (HIGH + 0x3000) | 3u << 4 | 2u << 6 | 1u << 8 | 3u << 10 | 2);
    int ab = 0;
    b_w32(t, HIGH + 0x3400, 0x11111111u, &ab);
    emit(t, DPI(MOV, 0, 0, 1, 0x100000));
    emit(t, DPI(MOV, 0, 0, 2, 0x22));
    emit(t, MEM(0, 0, 2, 1, 0, 1, 0));              /* 8008 sottopagina 0: rw     */
    emit(t, DPI(ADD, 0, 1, 1, 0x400));              /* 800C                       */
    emit(t, MEM(1, 0, 3, 1, 0, 1, 0));              /* 8010 sottopagina 1: lettura */
    emit(t, MEM(0, 0, 2, 1, 0, 1, 0));              /* 8014 scrittura: fault      */
    run(t);
    CHECK_EQ(b_r32(t, HIGH + 0x3000, &ab), 0x22);
    CHECK_EQ(t->cpu.r[3], 0x11111111u);
    CHECK_EQ(t->exception, ARM6_VEC_DABORT);
    CHECK_EQ(t->cpu.fsr, 0x1F);                     /* permesso, pagina, dominio 1 */
    CHECK_EQ(t->cpu.far_addr, 0x100400u);
    CHECK_EQ(t->cpu.r[14], 0x801C);

    /* sottopagina 2 (AP 1): solo privilegiato; LDRT dal SVC usa i permessi utente */
    t = setup_mmu(ARM6_SVC32);
    poke(t, L1 + 4 * 1, L2 | 1u << 5 | 0x11);
    poke(t, L2, (HIGH + 0x3000) | 3u << 4 | 2u << 6 | 1u << 8 | 3u << 10 | 2);
    b_w32(t, HIGH + 0x3800, 0x5555u, &ab);
    emit(t, DPI(MOV, 0, 0, 1, 0x100000));
    emit(t, DPI(ORR, 0, 1, 1, 0x800));
    emit(t, MEM(1, 0, 0, 1, 0, 1, 0));              /* LDR: ok        */
    emit(t, MEM(1, 0, 2, 1, 0, 0, 1));              /* LDRT: fault    */
    run(t);
    CHECK_EQ(t->cpu.r[0], 0x5555);
    CHECK_EQ(t->exception, ARM6_VEC_DABORT);
    CHECK_EQ(t->cpu.fsr, 0x1F);
    CHECK_EQ(t->cpu.r[1], 0x100800);                /* ARM610: la base non si aggiorna */

    /* pagina mancante: fault di traduzione della pagina, col dominio */
    t = setup_mmu(ARM6_SVC32);
    poke(t, L1 + 4 * 1, L2 | 1u << 5 | 0x11);
    for (uint32_t k = 0; k < 256; k++) poke(t, L2 + 4 * k, 0);
    emit(t, DPI(MOV, 0, 0, 1, 0x100000));
    emit(t, DPI(ORR, 0, 1, 1, 0x5000));
    emit(t, MEM(1, 0, 0, 1, 0, 1, 0));
    run(t);
    CHECK_EQ(t->cpu.fsr, 0x17);
    CHECK_EQ(t->cpu.far_addr, 0x105000);
}

TEST(mmu_large_page_and_ap0)
{
    T *t = setup_mmu(ARM6_SVC32);
    poke(t, L1 + 4 * 2, L2 | 0x11);                 /* VA &200000, dominio 0 */
    for (uint32_t k = 0; k < 256; k++) poke(t, L2 + 4 * k, 0);
    /* pagina grande (64 KB) ripetuta 16 volte; sottopagine da 16 KB: AP 0,3,3,3 */
    uint32_t large = HIGH | 0u << 4 | 3u << 6 | 3u << 8 | 3u << 10 | 1;
    for (uint32_t k = 0; k < 16; k++) poke(t, L2 + 4 * k, large);
    int ab = 0;
    b_w32(t, HIGH + 0x4000, 0xABCD, &ab);
    b_w32(t, HIGH + 0x0100, 0x1234, &ab);
    t->cpu.ctrl |= ARM6_CTRL_S;                     /* AP 0 + S: SVC in sola lettura */
    emit(t, DPI(MOV, 0, 0, 1, 0x200000));
    emit(t, DPI(ADD, 0, 1, 2, 0x4000));
    emit(t, MEM(1, 0, 0, 2, 0, 1, 0));              /* sottopagina 1 */
    emit(t, MEM(1, 0, 3, 1, 0x100, 1, 0));          /* sottopagina 0: lettura ok */
    emit(t, MEM(0, 0, 3, 1, 0x100, 1, 0));          /* scrittura: fault */
    run(t);
    CHECK_EQ(t->cpu.r[0], 0xABCD);
    CHECK_EQ(t->cpu.r[3], 0x1234);
    CHECK_EQ(t->exception, ARM6_VEC_DABORT);
    CHECK_EQ(t->cpu.fsr, 0x0F);
}

TEST(mmu_prefetch_abort_keeps_fsr)
{
    T *t = setup_mmu(ARM6_SVC32);
    t->cpu.fsr = 0xAA;
    emit(t, DPI(MOV, 0, 0, 1, 0x00400000u));
    emit(t, DPR(MOV, 0, 0, PC, 1, LSL, 0));
    run(t);
    CHECK_EQ(t->exception, ARM6_VEC_PABORT);
    CHECK_EQ(t->cpu.fsr, 0xAA);
    CHECK_EQ(t->cpu.r[14], 0x00400004u);
}

TEST(mmu_cp15_programming)
{
    /* il codice accende la MMU da se': TTB, domini, controllo, poi flush */
    T *t = setup(ARM6_SVC26);
    for (uint32_t k = 0; k < 4096; k++) poke(t, L1 + 4 * k, 0);
    poke(t, L1, section(0, 1, 0));                  /* VA 0: solo privilegiato */
    poke(t, L1 + 4 * 0x020, section(HIGH, 1, 0));
    int ab = 0;
    b_w32(t, HIGH, 0x600DF00Du, &ab);
    emit(t, DPI(MOV, 0, 0, 0, L1));
    emit(t, MCR15(2, 0));
    emit(t, DPI(MOV, 0, 0, 0, 1));
    emit(t, MCR15(3, 0));
    emit(t, DPI(MOV, 0, 0, 0, ARM6_CTRL_M | ARM6_CTRL_C | ARM6_CTRL_P | ARM6_CTRL_D));
    emit(t, MCR15(1, 0));
    emit(t, MCR15(5, 0));                           /* svuota il TLB */
    emit(t, MCR15(7, 0));                           /* svuota la cache */
    emit(t, DPI(MOV, 0, 0, 1, 0x02000000u));
    emit(t, MEM(1, 0, 2, 1, 0, 1, 0));
    emit(t, MRC15(2, 3));
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->cpu.r[2], 0x600DF00Du);
    CHECK_EQ(t->cpu.r[3], L1);
    CHECK_EQ(t->exception, -1);
}

TEST(alignment_fault)
{
    T *t = setup(ARM6_SVC32);
    t->cpu.ctrl |= ARM6_CTRL_A;
    emit(t, DPI(MOV, 0, 0, 1, 0x1000));
    emit(t, DPI(ORR, 0, 1, 1, 1));
    emit(t, MEM(1, 1, 0, 1, 0, 1, 0));              /* LDRB: ok */
    emit(t, MEM(1, 0, 0, 1, 0, 1, 0));              /* LDR: fault */
    run(t);
    CHECK_EQ(t->exception, ARM6_VEC_DABORT);
    CHECK_EQ(t->cpu.fsr, 0x01);
    CHECK_EQ(t->cpu.far_addr, 0x1001);
}

TEST(pipeline_self_modifying)
{
    T *t = setup(ARM6_USR32);
    emit(t, DPI(MOV, 0, 0, 1, 0x8000));
    emit(t, DPI(ADD, 0, 1, 1, 0x10));
    emit(t, DPI(MOV, 0, 0, 2, 0));
    emit(t, MEM(0, 0, 3, 1, 0, 1, 0));              /* 800C riscrive &8010 */
    emit(t, DPI(MOV, 0, 0, 5, 5));                  /* 8010 gia' letta */
    emit(t, EXIT);
    t->cpu.r[3] = DPI(MOV, 0, 0, 5, 9);
    run(t);
    CHECK_EQ(t->cpu.r[5], 5);
}

TEST(strongarm_v4)
{
    /* SA-110: ID, P e D sempre accesi, mezze parole e moltiplicazioni lunghe */
    T *t = setup(ARM6_SVC26);
    t->cpu.cp15_id = ARM6_ID_SA110;
    t->cpu.v4 = 1;
    poke(t, 0x1000, 0x8001FF80u);
    emit(t, MRC15(0, 0));
    emit(t, DPI(MOV, 0, 0, 1, 0));
    emit(t, MCR15(1, 1));                           /* controllo = 0 */
    emit(t, MRC15(1, 2));
    emit(t, DPI(MOV, 0, 0, 3, 0x1000));
    emit(t, 0xE1D340F0u);                           /* LDRSH R4,[R3,#0]  -> &FFFFFF80 */
    emit(t, 0xE1D350F2u);                           /* LDRSH R5,[R3,#2]  -> &FFFF8001 */
    emit(t, 0xE1D360D1u);                           /* LDRSB R6,[R3,#1]  -> &FFFFFFFF */
    emit(t, 0xE1C341B4u);                           /* STRH R4,[R3,#20] */
    emit(t, 0xE0C87594u);                           /* SMULL R7,R8,R4,R5 */
    emit(t, EXIT);
    run(t);
    CHECK_EQ(t->cpu.r[0], ARM6_ID_SA110);
    CHECK_EQ(t->cpu.r[2], ARM6_CTRL_P | ARM6_CTRL_D);
    CHECK_EQ(t->cpu.r[4], 0xFFFFFF80u);
    CHECK_EQ(t->cpu.r[5], 0xFFFF8001u);
    CHECK_EQ(t->cpu.r[6], 0xFFFFFFFFu);
    CHECK_EQ(peek(t, 0x1014), 0xFF80);
    /* -128 * -32767 = 4194176 */
    CHECK_EQ(t->cpu.r[7], 4194176u);
    CHECK_EQ(t->cpu.r[8], 0);

    /* sull'ARM610 le stesse istruzioni non esistono */
    t = setup(ARM6_SVC26);
    emit(t, 0xE1D340F0u);
    run(t);
    CHECK_EQ(t->exception, ARM6_VEC_UNDEF);
}

TEST(disassembler_v3)
{
    struct { uint32_t i; uint32_t addr; const char *text; } cases[] = {
        { MRS(3, 0),                     0, "MRS R3,CPSR" },
        { MRS(3, 1),                     0, "MRS R3,SPSR" },
        { MSR(0, 9, 2),                  0, "MSR CPSR_all,R2" },
        { MSR(1, 8, 2),                  0, "MSR SPSR_flg,R2" },
        { MSRI(0, 1, 0x13),              0, "MSR CPSR_ctl,#&13" },
        { MCR15(1, 0),                   0, "MCR P15,0,R0,C1,C0" },
        { MRC15(0, 4),                   0, "MRC P15,0,R4,C0,C0" },
        { B(0x10000000u, 0x10000100u, 1), 0x10000000u, "BL &10000100" },
        { 0xE1000090u,                   0, "SWP R0,R0,[R0]" },
    };
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        char buf[64];
        arm6_disasm(cases[k].i, cases[k].addr, buf, sizeof buf);
        checks++;
        if (strcmp(buf, cases[k].text)) {
            failures++;
            fprintf(stderr, "FALLITO [disassembler_v3] %08X: \"%s\", atteso \"%s\"\n", cases[k].i, buf, cases[k].text);
        }
    }
}

int main(void)
{
    RUN_TEST(reset_state);
    RUN_TEST(r15_26bit_as_rn_vs_rm);
    RUN_TEST(swi_26bit_config);
    RUN_TEST(teqp_26bit_and_banking);
    RUN_TEST(address_exception_only_with_26bit_data);
    RUN_TEST(undefined_26bit_config_enters_svc26);
    RUN_TEST(msr_32bit_mode_needs_p);
    RUN_TEST(clearing_p_falls_back_to_26bit_mode);
    RUN_TEST(r15_32bit_and_high_pc);
    RUN_TEST(mode_banks_32bit);
    RUN_TEST(swi_32bit_spsr_and_return);
    RUN_TEST(exception_from_26bit_mode_with_p);
    RUN_TEST(undefined_and_aborts_32bit_modes);
    RUN_TEST(irq_32bit_entry_and_return);
    RUN_TEST(ldm_with_s_restores_spsr);
    RUN_TEST(msr_user_and_spsr);
    RUN_TEST(cp15_id_and_user_access);
    RUN_TEST(mmu_section_mapping);
    RUN_TEST(mmu_translation_and_domain_faults);
    RUN_TEST(mmu_small_pages_and_subpage_permissions);
    RUN_TEST(mmu_large_page_and_ap0);
    RUN_TEST(mmu_prefetch_abort_keeps_fsr);
    RUN_TEST(mmu_cp15_programming);
    RUN_TEST(alignment_fault);
    RUN_TEST(pipeline_self_modifying);
    RUN_TEST(strongarm_v4);
    RUN_TEST(disassembler_v3);

    printf("%d controlli, %d falliti\n", checks, failures);
    return failures ? 1 : 0;
}
