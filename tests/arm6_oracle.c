/*
 * arm6_oracle.c - Esegue istruzioni singole del core ARMv3 lette da stdin e
 * stampa lo stato risultante, per il confronto con Unicorn
 * (tests/diff_unicorn_arm6.py). Configurazione a 32 bit (P e D), MMU spenta.
 *
 * Ogni riga in ingresso:  istr indirizzo cpsr spsr r0 ... r14       (esadecimale)
 * Ogni riga in uscita:    cpsr spsr pc r0 ... r14 [indirizzo=parola ...]
 *
 * La memoria dei dati (DATA, 4 KB) riparte a ogni caso dallo stesso schema;
 * in uscita si elencano le parole cambiate.
 */
#include <stdio.h>
#include <string.h>
#include "cpu/arm6.h"

#define DATA      0x20000u
#define DATA_SIZE 0x1000u

static uint8_t  data[DATA_SIZE];
static uint32_t code_addr, code_word;

static uint32_t pattern(uint32_t k) { return k * 0x9E3779B1u + 0x12345678u; }

static uint32_t r32(void *ctx, uint32_t a, int *ab)
{
    (void)ctx;
    a &= ~3u;
    if (a == code_addr) return code_word;
    if (a - DATA < DATA_SIZE) {
        const uint8_t *p = &data[a - DATA];
        return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
    }
    if (a - code_addr < 16) return 0;          /* la pipeline legge avanti */
    *ab = 1;
    return 0;
}
static uint8_t r8(void *ctx, uint32_t a, int *ab)
{
    (void)ctx;
    if (a - DATA < DATA_SIZE) return data[a - DATA];
    *ab = 1;
    return 0;
}
static void w32(void *ctx, uint32_t a, uint32_t v, int *ab)
{
    (void)ctx;
    a &= ~3u;
    if (a - DATA >= DATA_SIZE) { *ab = 1; return; }
    uint8_t *p = &data[a - DATA];
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void w8(void *ctx, uint32_t a, uint8_t v, int *ab)
{
    (void)ctx;
    if (a - DATA >= DATA_SIZE) { *ab = 1; return; }
    data[a - DATA] = v;
}

static void reset_data(void)
{
    for (uint32_t k = 0; k < DATA_SIZE / 4; k++) {
        uint32_t v = pattern(k);
        memcpy(&data[4 * k], &v, 4);
    }
}

int main(int argc, char **argv)
{
    static Arm6 cpu;
    ArmBus ab = { NULL, r32, r8, w32, w8, NULL, NULL };
    /* --v4: StrongARM (mezze parole e moltiplicazioni lunghe) */
    int v4 = argc > 1 && !strcmp(argv[1], "--v4");
    arm6_init(&cpu, &ab, v4 ? ARM6_ID_SA110 : ARM6_ID_ARM610);
    cpu.ctrl = ARM6_CTRL_P | ARM6_CTRL_D;

    unsigned instr, addr, cpsr, spsr, r[15];
    while (scanf("%x %x %x %x", &instr, &addr, &cpsr, &spsr) == 4) {
        for (int k = 0; k < 15; k++) if (scanf("%x", &r[k]) != 1) return 1;
        reset_data();
        code_addr = addr;
        code_word = instr;
        /* tutti i banchi partono dagli stessi valori, come in Unicorn */
        arm6_set_cpsr(&cpu, cpsr);
        for (int k = 0; k < 15; k++) cpu.r[k] = r[k];
        for (int k = 0; k < 5; k++) cpu.usr_r8_12[k] = cpu.fiq_r8_12[k] = r[8 + k];
        for (int b = 0; b < 6; b++) {
            cpu.r13_14[b][0] = r[13];
            cpu.r13_14[b][1] = r[14];
            cpu.spsr[b] = spsr;
        }
        arm6_set_pc(&cpu, addr);
        arm6_step(&cpu);

        int b = 0;
        switch (cpu.cpsr & 0x1F) {
        case 0x11: b = 1; break; case 0x12: b = 2; break; case 0x13: b = 3; break;
        case 0x17: b = 4; break; case 0x1B: b = 5; break;
        }
        printf("%x %x %x", cpu.cpsr, b ? cpu.spsr[b] : 0, cpu.r[15]);
        for (int k = 0; k < 15; k++) printf(" %x", cpu.r[k]);
        for (uint32_t k = 0; k < DATA_SIZE / 4; k++) {
            uint32_t v;
            memcpy(&v, &data[4 * k], 4);
            if (v != pattern(k)) printf(" %x=%x", DATA + 4 * k, v);
        }
        printf("\n");
    }
    return 0;
}
