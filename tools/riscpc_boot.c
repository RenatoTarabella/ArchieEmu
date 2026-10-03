/*
 * riscpc_boot.c - Avvio della macchina Risc PC da riga di comando, per
 * seguire il boot della ROM: esegue per un tempo emulato, elenca gli
 * accessi a indirizzi che nessun dispositivo conosce, riassume dove e'
 * stata la CPU e salva lo schermo in PNG.
 *
 *   riscpc_boot --rom ROM350 [--ms 3000] [--png schermo.png] [--ram 16] [--vram 1]
 *               [--keys "testo{ENTER}"] [--keys-at ms]   (vedi riscpc_keys.h; da 10 s)
 *               [--arm710] [--trace-io] [--trace-vectors] [--hist] [--trace N] [--trace-at istr]
 *               [--trace-modes] [--watch-low] [--break pc] [--watch-io lo hi] [--ring N]   (le ultime N istruzioni prima del primo abort o undef)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "riscpc/riscpc.h"
#include "cpu/arm2_disasm.h"
#include "frontend/png.h"
#include "riscpc_keys.h"

/* --- accessi sconosciuti: uno per indirizzo e direzione --- */
typedef struct IoEntry {
    uint32_t addr, first_pc, first_value, last_value;
    int      write, size;
    uint64_t count, first_instr;
} IoEntry;
static IoEntry io_tab[4096];
static int io_count, trace_io;
static RiscPc *mach;

static void io_hook(void *user, int write, uint32_t addr, uint32_t value, int size)
{
    (void)user;
    for (int k = 0; k < io_count; k++) {
        if (io_tab[k].addr == addr && io_tab[k].write == write) {
            io_tab[k].count++;
            io_tab[k].last_value = value;
            return;
        }
    }
    uint32_t pc = mach->cpu.r[15] - 4;
    if (trace_io)
        fprintf(stderr, "[%10llu] %s%d %08X = %08X  pc %08X\n", (unsigned long long)mach->cpu.instructions,
                write ? "W" : "R", size, addr, value, pc);
    if (io_count < 4096)
        io_tab[io_count++] = (IoEntry){ addr, pc, value, value, write, size, 1, mach->cpu.instructions };
}

/* --- registro circolare delle ultime istruzioni --- */
typedef struct RingEntry { uint32_t pc, instr, mode; } RingEntry;
static RingEntry ring[4096];
static uint32_t ring_size, ring_pos;
static int ring_dumped;

static void ring_dump(void)
{
    fprintf(stderr, "--- ultime %u istruzioni ---\n", ring_size);
    for (uint32_t k = 0; k < ring_size; k++) {
        RingEntry *e = &ring[(ring_pos + k) % ring_size];
        char text[80];
        arm6_disasm(e->instr, e->pc, text, sizeof text);
        fprintf(stderr, "%08X %08X %-5s %s\n", e->pc, e->instr, arm6_mode_name((int)e->mode), text);
    }
    ring_dumped = 1;
}

/* --- --watch-io lo hi: tutti gli accessi I/O nell'intervallo --- */
static uint64_t watch_left = 2000000;

static void io_watch(void *user, int write, uint32_t addr, uint32_t value, int size)
{
    (void)user;
    if (!watch_left) return;
    watch_left--;
    fprintf(stderr, "[%10llu] %s%d %08X %s %08X  pc %08X\n", (unsigned long long)mach->cpu.instructions,
            write ? "W" : "R", size, addr, write ? "<-" : "->", value, mach->cpu.r[15] - 4);
}

/* --- eccezioni --- */
static int trace_vectors;
static uint64_t vec_count[8];

static void exc_hook(Arm6 *cpu, uint32_t vector, uint32_t link, void *user)
{
    (void)user;
    vec_count[(vector >> 2) & 7]++;
    if (ring_size && !ring_dumped && vector != ARM6_VEC_IRQ && vector != ARM6_VEC_SWI && vector != ARM6_VEC_FIQ)
        ring_dump();
    if (trace_vectors && vector != ARM6_VEC_IRQ && vector != ARM6_VEC_SWI) {
        static const char *names[8] = { "RESET", "UNDEF", "SWI", "PABORT", "DABORT", "ADDREX", "IRQ", "FIQ" };
        fprintf(stderr, "[%10llu] %s da %08X (%s) FSR %02X FAR %08X\n", (unsigned long long)cpu->instructions,
                names[(vector >> 2) & 7], link, arm6_mode_name(arm6_mode(cpu)), cpu->fsr, cpu->far);
    }
}

/* --- istogramma dei PC (blocchi da 256 byte) e traccia --- */
static int hist;
static uint64_t trace_left, trace_at;
static int trace_modes, last_mode = -1, watch_low;
static uint32_t prev_pc;
static uint32_t last_pc, last_instr;
static uint32_t break_pc[16];
static int break_count, break_hits[16];
typedef struct HistEntry { uint32_t block; uint64_t n; } HistEntry;
static HistEntry hist_tab[65536];

static void trace_hook(Arm6 *cpu, uint32_t pc, uint32_t instr)
{
    if (ring_size && instr) {                   /* le parole a zero (memoria vuota) no */
        ring[ring_pos] = (RingEntry){ pc, instr, (uint32_t)arm6_mode(cpu) };
        ring_pos = (ring_pos + 1) % ring_size;
    }
    /* --watch-low: dal codice alto (kernel, RAM) di nuovo alla ROM bassa */
    if (watch_low && ring_size && !ring_dumped && pc < 0x200000u && prev_pc >= 0x2000000u) {
        fprintf(stderr, "[%10llu] salto da %08X a %08X\n", (unsigned long long)cpu->instructions, prev_pc, pc);
        ring_dump();
    }
    prev_pc = pc;
    /* --break: registri (anche i banchi) ogni volta che il PC passa di li' */
    for (int k = 0; k < break_count; k++) {
        if (pc != break_pc[k] || break_hits[k] >= 4) continue;
        break_hits[k]++;
        fprintf(stderr, "[%10llu] break %08X %s CPSR %08X\n", (unsigned long long)cpu->instructions, pc,
                arm6_mode_name(arm6_mode(cpu)), cpu->cpsr);
        for (int r = 0; r < 15; r++) fprintf(stderr, " R%d=%08X%s", r, cpu->r[r], r % 5 == 4 ? "\n" : "");
        fprintf(stderr, " R8-12 fiq %08X %08X %08X %08X %08X  usr %08X %08X %08X %08X %08X\n",
                cpu->fiq_r8_12[0], cpu->fiq_r8_12[1], cpu->fiq_r8_12[2], cpu->fiq_r8_12[3], cpu->fiq_r8_12[4],
                cpu->usr_r8_12[0], cpu->usr_r8_12[1], cpu->usr_r8_12[2], cpu->usr_r8_12[3], cpu->usr_r8_12[4]);
    }
    if (trace_modes) {
        int mode = arm6_mode(cpu);
        if (mode != last_mode && last_mode >= 0) {
            char text[80];
            arm6_disasm(last_instr, last_pc, text, sizeof text);
            fprintf(stderr, "[%10llu] %s -> %s dopo %08X %s\n", (unsigned long long)cpu->instructions,
                    arm6_mode_name(last_mode), arm6_mode_name(mode), last_pc, text);
        }
        last_mode = mode;
        last_pc = pc;
        last_instr = instr;
    }
    if (trace_left && cpu->instructions >= trace_at) {
        char text[80];
        arm6_disasm(instr, pc, text, sizeof text);
        fprintf(stderr, "%08X %08X %-5s %s\n", pc, instr, arm6_mode_name(arm6_mode(cpu)), text);
        trace_left--;
    }
    if (hist) {
        uint32_t b = pc >> 8, h = (b * 2654435761u) >> 16;
        for (;;) {
            if (hist_tab[h].n == 0) { hist_tab[h].block = b; hist_tab[h].n = 1; break; }
            if (hist_tab[h].block == b) { hist_tab[h].n++; break; }
            h = (h + 1) & 0xFFFF;
        }
    }
}

static int cmp_hist(const void *a, const void *b)
{
    const HistEntry *x = a, *y = b;
    return x->n < y->n ? 1 : x->n > y->n ? -1 : 0;
}

int main(int argc, char **argv)
{
    RiscPcConfig cfg = { 0 };
    uint32_t m_watch_lo = 0, m_watch_hi = 0;
    const char *png = NULL;
    double ms = 3000, keys_at = 10000;
    cfg.rom_path = "roms/1. Major/ROM350";
    cfg.vram_mb = 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--rom") && v)        { cfg.rom_path = v; i++; }
        else if (!strcmp(a, "--ms") && v)    { ms = atof(v); i++; }
        else if (!strcmp(a, "--png") && v)   { png = v; i++; }
        else if (!strcmp(a, "--ram") && v)   { cfg.ram_mb = (uint32_t)atoi(v); i++; }
        else if (!strcmp(a, "--vram") && v)  { cfg.vram_mb = (uint32_t)atoi(v); i++; }
        else if (!strcmp(a, "--cmos") && v)  { cfg.cmos_path = v; i++; }
        else if (!strcmp(a, "--trace") && v) { trace_left = strtoull(v, NULL, 10); i++; }
        else if (!strcmp(a, "--trace-at") && v) { trace_at = strtoull(v, NULL, 10); i++; }
        else if (!strcmp(a, "--ring") && v) { ring_size = (uint32_t)atoi(v); if (ring_size > 4096) ring_size = 4096; i++; }
        else if (!strcmp(a, "--trace-modes")) trace_modes = 1;
        else if (!strcmp(a, "--keys") && v)  { parse_keys(v); i++; }
        else if (!strcmp(a, "--keys-at") && v) { keys_at = atof(v); i++; }
        else if (!strcmp(a, "--watch-low")) watch_low = 1;
        else if (!strcmp(a, "--watch-io") && i + 2 < argc) {
            m_watch_lo = (uint32_t)strtoul(argv[i + 1], NULL, 16);
            m_watch_hi = (uint32_t)strtoul(argv[i + 2], NULL, 16);
            i += 2;
        }
        else if (!strcmp(a, "--break") && v && break_count < 16) { break_pc[break_count++] = (uint32_t)strtoul(v, NULL, 16); i++; }
        else if (!strcmp(a, "--arm710"))     cfg.arm710 = 1;
        else if (!strcmp(a, "--trace-io"))   trace_io = 1;
        else if (!strcmp(a, "--trace-vectors")) trace_vectors = 1;
        else if (!strcmp(a, "--hist"))       hist = 1;
        else { fprintf(stderr, "opzione sconosciuta: %s\n", a); return 1; }
    }

    static RiscPc m;
    char err[256];
    if (!riscpc_create(&m, &cfg, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    mach = &m;
    m.io_hook = io_hook;
    if (m_watch_hi) { m.io_trace = io_watch; m.trace_lo = m_watch_lo; m.trace_hi = m_watch_hi; }
    m.cpu.exception_hook = exc_hook;
    if (hist || trace_left || ring_size || trace_modes || watch_low || break_count) m.cpu.trace_hook = trace_hook;

    if (key_count) run_with_keys(&m, (ArcTime)(ms * 24000.0), (ArcTime)(keys_at * 24000.0));
    else           riscpc_run(&m, (ArcTime)(ms * 24000.0));

    Arm6 *c = &m.cpu;
    printf("ROM %s, %u KB, CPU %s a %.0f MHz, %u MB di DRAM, %u KB di VRAM\n", cfg.rom_path, m.rom_size >> 10,
           cfg.arm710 ? "ARM710" : "ARM610", m.mhz, m.ram_size >> 20, m.vram_size >> 10);
    printf("%.0f ms emulati: %llu istruzioni, %llu frame\n", ms,
           (unsigned long long)c->instructions, (unsigned long long)m.frames);
    printf("PC %08X  modo %s  CPSR %08X  controllo %03X  TTB %08X  domini %08X\n",
           c->r[15], arm6_mode_name(arm6_mode(c)), c->cpsr, c->ctrl, c->ttb, c->dacr);
    for (int k = 0; k < 16; k++) printf("R%-2d %08X%s", k, c->r[k], k % 4 == 3 ? "\n" : "  ");
    printf("eccezioni: undef %llu  swi %llu  pabort %llu  dabort %llu  addrex %llu  irq %llu  fiq %llu\n",
           (unsigned long long)vec_count[1], (unsigned long long)vec_count[2], (unsigned long long)vec_count[3],
           (unsigned long long)vec_count[4], (unsigned long long)vec_count[5], (unsigned long long)vec_count[6],
           (unsigned long long)vec_count[7]);
    printf("IOMD: IRQA %02X/%02X  IRQB %02X/%02X  FIQ %02X/%02X  DMA %02X/%02X  timer0 %u  VIDINIT %08X VIDSTART %08X VIDEND %08X VIDCR %X\n",
           m.iomd.irqa, m.iomd.irqa_mask, m.iomd.irqb, m.iomd.irqb_mask, m.iomd.fiq, m.iomd.fiq_mask,
           m.iomd.dma_irq, m.iomd.dma_mask, m.iomd.timer[0].latch, m.iomd.vidinit, m.iomd.vidstart, m.iomd.vidend, m.iomd.vidcr);
    int w, h;
    vidc20_size(&m.vidc, &w, &h);
    printf("VIDC20: %dx%d, %d bpp, controllo %06X  HDSR %u VDSR %u  cursore HCSR %u VCSR %u VCER %u CURSINIT %08X\n",
           w, h, 1 << vidc20_log2bpp(&m.vidc), m.vidc.control, m.vidc.horiz[3], m.vidc.vert[3],
           m.vidc.horiz[6], m.vidc.vert[6], m.vidc.vert[7], m.iomd.cursinit);

    if (io_count) {
        printf("accessi sconosciuti (%d indirizzi):\n", io_count);
        for (int k = 0; k < io_count; k++) {
            IoEntry *e = &io_tab[k];
            printf("  %s%d %08X  x%-8llu primo pc %08X (istr. %llu) valore %08X ultimo %08X\n",
                   e->write ? "W" : "R", e->size, e->addr, (unsigned long long)e->count, e->first_pc,
                   (unsigned long long)e->first_instr, e->first_value, e->last_value);
        }
    }
    if (hist) {
        qsort(hist_tab, 65536, sizeof hist_tab[0], cmp_hist);
        printf("blocchi piu' eseguiti:\n");
        for (int k = 0; k < 16 && hist_tab[k].n; k++)
            printf("  %08X  %llu\n", hist_tab[k].block << 8, (unsigned long long)hist_tab[k].n);
    }

    if (png) {
        static uint32_t pix[2048 * 2048];
        riscpc_render(&m, pix, 2048, &w, &h);
        if (w && h) {
            for (int y = 1; y < h; y++) memmove(pix + (size_t)y * w, pix + (size_t)y * 2048, (size_t)w * 4);
            if (!png_write(png, pix, w, h, 1)) fprintf(stderr, "impossibile scrivere %s\n", png);
            else printf("schermo %dx%d in %s\n", w, h, png);
        } else {
            printf("schermo non programmato: niente PNG\n");
        }
    }
    riscpc_destroy(&m);
    return 0;
}
