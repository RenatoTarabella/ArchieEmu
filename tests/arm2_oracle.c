/*
 * arm2_oracle.c - Esegue istruzioni singole lette da stdin e stampa lo
 * stato risultante, per il confronto con Unicorn (tests/diff_unicorn.py).
 *
 * Ogni riga in ingresso:  istr nzcv r0 r1 ... r14      (esadecimale)
 * Ogni riga in uscita:    nzcv r0 r1 ... r14
 */
#include <stdio.h>
#include <string.h>
#include "cpu/arm2.h"
#include "core/bus.h"

static uint8_t ram[0x10000];

int main(void)
{
    Bus bus;
    bus_init(&bus);
    bus_map_memory(&bus, 0, sizeof ram, ram, 0);
    ArmBus ab;
    bus_attach_cpu(&bus, &ab);
    static Arm2 cpu;
    arm2_init(&cpu, &ab);

    unsigned instr, nzcv, r[15];
    while (scanf("%x %x", &instr, &nzcv) == 2) {
        for (int k = 0; k < 15; k++) if (scanf("%x", &r[k]) != 1) return 1;
        arm2_set_r15(&cpu, ARM_MODE_USR);
        for (int k = 0; k < 15; k++) cpu.r[k] = r[k];
        cpu.r[15] = ((uint32_t)nzcv << 28) | 0x8000u;
        int abort = 0;
        bus_write32(&bus, 0x8000, instr, &abort);
        arm2_step(&cpu);
        printf("%x", cpu.r[15] >> 28);
        for (int k = 0; k < 15; k++) printf(" %x", cpu.r[k]);
        printf("\n");
    }
    return 0;
}
