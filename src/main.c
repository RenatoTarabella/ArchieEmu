/*
 * main.c - Macchina di prova: RAM + CPU ARMv2 + SWI di RISC OS in HLE.
 *
 *   armemu [-t] [-s] [-a indirizzo] [-m MB] [-c cicli] programma.bin
 *
 * Il programma viene caricato (default &8000) ed eseguito in modo utente,
 * con R13 in cima alla RAM e R14 che punta a un OS_Exit: un "MOV PC,R14"
 * finale termina pulito.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cpu/arm2.h"
#include "cpu/arm2_disasm.h"
#include "core/bus.h"
#include "hle/riscos_swi.h"

#define EXIT_STUB_ADDR  0x100u
#define SWI_EXC_BASE    0xEE0000u   /* SWI private per segnalare le eccezioni */

typedef struct Machine {
    Bus       bus;
    Arm2      cpu;
    RiscosHle hle;
    uint8_t  *ram;
    uint32_t  ram_size;
} Machine;

static const char *vector_names[8] = {
    "reset", "istruzione non definita", "SWI", "prefetch abort",
    "data abort", "address exception", "IRQ", "FIQ"
};

static int machine_swi(Arm2 *cpu, uint32_t comment, void *user)
{
    Machine *m = (Machine *)user;
    if ((comment & 0xFFFF00u) == SWI_EXC_BASE) {
        uint32_t vec = comment & 0xFF;
        fflush(stdout);
        fprintf(stderr, "\nEccezione: %s (R14=&%08X)\n", vector_names[(vec >> 2) & 7], cpu->r[14]);
        m->hle.exit_code = 2;
        cpu->halted = 1;
        return 1;
    }
    return riscos_swi_hook(cpu, comment, &m->hle);
}

static void poke32(Machine *m, uint32_t addr, uint32_t v)
{
    int abort = 0;
    bus_write32(&m->bus, addr, v, &abort);
}

static void dump_regs(const Arm2 *c)
{
    for (int r = 0; r < 16; r++)
        fprintf(stderr, "R%-2d=%08X%s", r, c->r[r], (r & 3) == 3 ? "\n" : "  ");
    uint32_t f = c->r[15];
    fprintf(stderr, "PC=&%07X  %c%c%c%c %c%c  %s\n", f & ARM_PC_MASK,
            (f & ARM_N) ? 'N' : 'n', (f & ARM_Z) ? 'Z' : 'z', (f & ARM_C) ? 'C' : 'c',
            (f & ARM_V) ? 'V' : 'v', (f & ARM_I) ? 'I' : 'i', (f & ARM_F) ? 'F' : 'f',
            arm2_mode_name(arm2_mode(c)));
}

static void usage(void)
{
    fprintf(stderr,
        "uso: armemu [opzioni] programma.bin\n"
        "  -a ind   indirizzo di caricamento ed esecuzione (default &8000)\n"
        "  -m MB    RAM in megabyte (default 4)\n"
        "  -c N     ferma dopo N cicli (default illimitato)\n"
        "  -t       traccia ogni istruzione su stderr\n"
        "  -s       statistiche e registri alla fine\n");
}

int main(int argc, char **argv)
{
    uint32_t load = 0x8000, mb = 4;
    uint64_t max_cycles = 0;
    int trace = 0, stats = 0;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t")) trace = 1;
        else if (!strcmp(argv[i], "-s")) stats = 1;
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) load = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) mb = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) max_cycles = strtoull(argv[++i], NULL, 0);
        else if (argv[i][0] == '-') { usage(); return 1; }
        else path = argv[i];
    }
    if (!path || mb == 0 || mb > 32) { usage(); return 1; }

    static Machine m;
    m.ram_size = mb << 20;
    m.ram = calloc(1, m.ram_size);
    if (!m.ram) { fprintf(stderr, "memoria esaurita\n"); return 1; }

    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    size_t n = fread(m.ram + load, 1, m.ram_size - load, f);
    fclose(f);

    /* moduli: bus + RAM, CPU collegata al bus, HLE sulle SWI */
    bus_init(&m.bus);
    bus_map_memory(&m.bus, 0, m.ram_size, m.ram, 0);

    ArmBus cpubus;
    bus_attach_cpu(&m.bus, &cpubus);
    arm2_init(&m.cpu, &cpubus);
    m.cpu.swi_hook = machine_swi;
    m.cpu.swi_user = &m;

    m.hle.bus = &m.bus;
    m.hle.ram_limit = m.ram_size;
    m.hle.trace = trace;

    /* vettori: ognuno e' una SWI privata che riporta l'eccezione */
    for (uint32_t v = 0; v < 0x20; v += 4)
        poke32(&m, v, 0xEF000000u | SWI_EXC_BASE | v);
    poke32(&m, EXIT_STUB_ADDR, 0xEF000011u);              /* SWI OS_Exit */

    /* avvio in modo utente, interrupt abilitati */
    arm2_set_r15(&m.cpu, ARM_MODE_USR);
    arm2_set_pc(&m.cpu, load);
    m.cpu.r[13] = m.ram_size;
    m.cpu.r[14] = EXIT_STUB_ADDR;

    if (trace) fprintf(stderr, "caricati %u byte a &%X\n", (unsigned)n, load);

    clock_t t0 = clock();
    while (!m.cpu.halted) {
        if (max_cycles && m.cpu.cycles >= max_cycles) {
            fflush(stdout);
            fprintf(stderr, "\nlimite di %llu cicli raggiunto\n", (unsigned long long)max_cycles);
            break;
        }
        if (trace) {
            char text[80];
            uint32_t pc = arm2_pc(&m.cpu);
            int abort = 0;
            uint32_t instr = bus_read32(&m.bus, pc, &abort);
            arm2_disasm(instr, pc, text, sizeof text);
            fprintf(stderr, "%07X  %08X  %-32s %s\n", pc, instr, text, arm2_mode_name(arm2_mode(&m.cpu)));
        }
        arm2_step(&m.cpu);
    }
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    fflush(stdout);

    if (stats) {
        fprintf(stderr, "\n%llu istruzioni, %llu cicli (~%.3f s su un ARM2 a 8 MHz)\n",
                (unsigned long long)m.cpu.instructions, (unsigned long long)m.cpu.cycles,
                (double)m.cpu.cycles / 8e6);
        if (secs > 0.01)
            fprintf(stderr, "host: %.3f s, %.1f MIPS (%.1f volte un ARM2 a 8 MHz)\n", secs,
                    (double)m.cpu.instructions / secs / 1e6, (double)m.cpu.cycles / 8e6 / secs);
        dump_regs(&m.cpu);
    }
    free(m.ram);
    return m.hle.exit_code;
}
