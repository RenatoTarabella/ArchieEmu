/*
 * machine.c - Montaggio e avvio della macchina (vedi machine.h)
 */
#include "machine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir(p, 0777)
#endif

void machine_default_disc(const char *rom_path, char *out, size_t size)
{
    static const char *tail = "third_party/riscos/";
    size_t n = strlen(rom_path), t = strlen(tail);
    /* cerca "third_party?riscos?" con qualunque separatore */
    for (size_t i = 0; i + t <= n; i++) {
        size_t j = 0;
        for (; j < t; j++) {
            char a = rom_path[i + j], b = tail[j];
            if (b == '/' ? (a != '/' && a != '\\') : a != b) break;
        }
        if (j == t) {
            snprintf(out, size, "%.*sdisc", (int)i, rom_path);
            return;
        }
    }
    snprintf(out, size, "disc");
}

static uint64_t host_now_cs(void *ctx)
{
    (void)ctx;
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 100 + (uint64_t)ts.tv_nsec / 10000000;
}

/* orologio dai cicli: un ARM2 a 8 MHz, meno la banda del DMA video */
static uint64_t emulated_now_cs(void *ctx)
{
    const Machine *m = ctx;
    return (uint64_t)((double)m->cpu.cycles / 80000.0 / (1.0 - machine_dma_fraction(m)));
}

int machine_create(Machine *m, const MachineConfig *cfg, char *err, size_t errsize)
{
    memset(m, 0, sizeof *m);
    uint32_t mb = cfg->ram_mb ? cfg->ram_mb : 4;
    if (mb > 16) mb = 16;
    m->ram_size = mb << 20;
    m->ram = calloc(1, m->ram_size);
    m->screen = calloc(1, VDU_MAX_BYTES);
    if (!m->ram || !m->screen) { snprintf(err, errsize, "memoria esaurita"); return 0; }

    if (!module_load(cfg->rom_path, &m->module, err, errsize)) return 0;
    uint32_t start = module_word(&m->module, MODULE_START);
    if (!start || start >= m->module.size) {
        snprintf(err, errsize, "%s: il modulo non ha un punto d'ingresso (start)", cfg->rom_path);
        return 0;
    }
    m->rom_size = ((uint32_t)m->module.size + BUS_PAGE_SIZE - 1) & ~(BUS_PAGE_SIZE - 1);
    m->rom = calloc(1, m->rom_size);
    if (!m->rom) { snprintf(err, errsize, "memoria esaurita"); return 0; }
    memcpy(m->rom, m->module.data, m->module.size);

    /* bus: RAM, video, ROM in sola lettura */
    bus_init(&m->bus);
    bus_map_memory(&m->bus, 0, m->ram_size, m->ram, 0);
    bus_map_memory(&m->bus, MACHINE_SCREEN_ADDR, VDU_MAX_BYTES, m->screen, 0);
    bus_map_memory(&m->bus, MACHINE_ROM_ADDR, m->rom_size, m->rom, 1);

    ArmBus cpubus;
    bus_attach_cpu(&m->bus, &cpubus);
    arm2_init(&m->cpu, &cpubus);
    /* tempi di un ARM2 sul MEMC, come nella macchina Archimedes: cicli N
       doppi, e il modulo linguaggio sta in ROM (325 ns, come la programma
       RISC OS 3.11: 3 tick per ogni fetch) */
    m->cpu.s_ticks = 1;
    m->cpu.n_ticks = 2;
    m->cpu.slow_base = MACHINE_ROM_ADDR;
    m->cpu.slow_fetch_extra = 2;

    vdu_init(&m->vdu, m->screen, MACHINE_SCREEN_ADDR);
    if (cfg->mode >= 0 && !vdu_set_mode(&m->vdu, cfg->mode)) {
        snprintf(err, errsize, "modo %d non disponibile", cfg->mode);
        return 0;
    }

    kernel_init(&m->kernel, &m->cpu, &m->bus, &m->vdu, m->ram_size);
    if (cfg->emulated_clock) { m->kernel.now_cs = emulated_now_cs; m->kernel.now_ctx = m; }
    else m->kernel.now_cs = host_now_cs;
    m->kernel.start_cs = m->kernel.now_cs(m->kernel.now_ctx);
    m->kernel.trace_swi = cfg->trace_swi;
    m->kernel.faithful = 1;
    if (cfg->disc_dir && cfg->disc_dir[0]) {
        snprintf(m->kernel.disc_dir, sizeof m->kernel.disc_dir, "%s", cfg->disc_dir);
        struct stat st;
        if (stat(cfg->disc_dir, &st) != 0) make_dir(cfg->disc_dir);
    }

    /* inizializzazione del modulo (registra le sue risorse) */
    uint32_t init = module_word(&m->module, MODULE_INIT);
    if (init && init < m->module.size) {
        uint32_t regs[13] = { 0 };
        regs[12] = K_PRIVWORD;
        if (!kernel_call(&m->kernel, MACHINE_ROM_ADDR + init, ARM_MODE_SVC, regs)) {
            snprintf(err, errsize, "l'inizializzazione del modulo non e' terminata");
            return 0;
        }
    }

    /* il nome del modulo diventa la riga di comando (come "*BASIC") */
    const char *title = module_string(&m->module, module_word(&m->module, MODULE_TITLE));
    char cmd[64];
    snprintf(cmd, sizeof cmd, "%s", *title ? title : "BASIC");
    for (size_t i = 0; cmd[i]; i++) m->ram[K_ENVSTRING + i] = (uint8_t)cmd[i];
    m->ram[K_ENVSTRING + strlen(cmd)] = 0;

    /* avvio in modo utente, come OS_Module Enter */
    arm2_set_r15(&m->cpu, ARM_MODE_USR);
    arm2_set_pc(&m->cpu, MACHINE_ROM_ADDR + start);
    m->cpu.r[12] = K_PRIVWORD;
    m->cpu.r[13] = m->ram_size;
    m->cpu.r[14] = K_EXIT_STUB;
    return 1;
}

void machine_destroy(Machine *m)
{
    module_free(&m->module);
    free(m->ram);
    free(m->screen);
    free(m->rom);
    memset(m, 0, sizeof *m);
}

double machine_dma_fraction(const Machine *m)
{
    const Vdu *v = &m->vdu;
    if (v->log2bpp > 3) return 0;
    double bytes = (double)v->line_bytes * v->height;
    double hz = v->height <= 300 ? 50 : 60;
    /* il MEMC legge 16 byte per volta: 1 ciclo N + 3 S = 5 tick a 8 MHz */
    double f = bytes * hz / 16.0 * 5.0 / 8e6;
    return f > 0.5 ? 0.5 : f;
}

uint64_t machine_run(Machine *m, uint64_t cycles)
{
    uint64_t start = m->cpu.cycles;
    m->kernel.waiting = 0;
    while (!m->cpu.halted && !m->kernel.waiting && m->cpu.cycles - start < cycles)
        arm2_step(&m->cpu);
    return m->cpu.cycles - start;
}
