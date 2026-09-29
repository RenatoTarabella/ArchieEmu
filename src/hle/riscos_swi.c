/*
 * riscos_swi.c - SWI di RISC OS emulate dall'host
 */
#include "riscos_swi.h"
#include <stdio.h>

#define SWI_X_BIT 0x20000u   /* forma "X": errori ritornati con V invece che generati */

static uint8_t peek8(RiscosHle *h, uint32_t addr)
{
    int abort = 0;
    return bus_read8(h->bus, addr, &abort);
}

static void write_c(uint8_t ch)
{
    if (ch == 13) return;          /* il CR di RISC OS: il terminale usa solo LF */
    putchar(ch);
}

int riscos_swi_hook(Arm2 *cpu, uint32_t comment, void *user)
{
    RiscosHle *h = (RiscosHle *)user;
    uint32_t swi = comment & ~SWI_X_BIT;

    if (h->trace) fprintf(stderr, "[SWI &%X at &%X]\n", comment, arm2_pc(cpu) - 4);

    switch (swi) {
    case 0x00:                                      /* OS_WriteC */
        write_c((uint8_t)cpu->r[0]);
        return 1;
    case 0x01: {                                    /* OS_WriteS: stringa dopo la SWI */
        uint32_t a = arm2_pc(cpu);
        uint8_t ch;
        while ((ch = peek8(h, a++)) != 0) write_c(ch);
        arm2_set_pc(cpu, (a + 3) & ~3u);
        return 1;
    }
    case 0x02: {                                    /* OS_Write0 */
        uint32_t a = cpu->r[0];
        uint8_t ch;
        while ((ch = peek8(h, a++)) != 0) write_c(ch);
        cpu->r[0] = a;
        return 1;
    }
    case 0x03:                                      /* OS_NewLine */
        putchar('\n');
        return 1;
    case 0x04: {                                    /* OS_ReadC */
        int ch = getchar();
        if (ch == '\n') ch = 13;
        cpu->r[0] = ch == EOF ? 27u : (uint32_t)ch;
        cpu->r[15] &= ~ARM_C;
        if (ch == EOF) cpu->r[15] |= ARM_C;         /* C=1: escape */
        return 1;
    }
    case 0x10:                                      /* OS_GetEnv */
        cpu->r[0] = 0;
        cpu->r[1] = h->ram_limit;
        cpu->r[2] = 0;
        return 1;
    case 0x11:                                      /* OS_Exit */
        h->exit_code = (cpu->r[1] == 0x58454241u) ? (int)cpu->r[2] : 0;   /* "ABEX" */
        cpu->halted = 1;
        return 1;
    default:
        fflush(stdout);
        fprintf(stderr, "\nSWI &%X non implementata (PC=&%X)\n", comment, arm2_pc(cpu) - 4);
        h->exit_code = 1;
        cpu->halted = 1;
        return 1;
    }
}
