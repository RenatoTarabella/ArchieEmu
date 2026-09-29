/*
 * cli_main.c - Frontend da console: la macchina con il BASIC, tastiera da
 * stdin (una riga alla volta, quando il programma aspetta input) e testo
 * sullo stdout. Utile per i test automatici; --png salva lo schermo.
 *
 *   armbasic [--rom modulo] [--mode n] [--ram MB] [--png file] [--trace-swi]
 *
 * Nelle righe di input, "\e" invia Escape.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "machine/machine.h"
#include "png.h"

static void echo(void *ctx, int ch)
{
    (void)ctx;
    if (ch >= 32 && ch < 127) putchar(ch);
    else if (ch == 10) putchar('\n');
}

static const char *find_rom(const char *argv0)
{
    static char path[1024];
    static const char *rel[] = { "third_party/riscos/BASIC", "../third_party/riscos/BASIC",
                                 "../../third_party/riscos/BASIC", "../../../third_party/riscos/BASIC" };
    /* relativo alla cartella corrente, poi a quella dell'eseguibile */
    for (size_t i = 0; i < sizeof rel / sizeof rel[0]; i++) {
        FILE *f = fopen(rel[i], "rb");
        if (f) { fclose(f); return rel[i]; }
    }
    const char *slash = strrchr(argv0, '\\');
    const char *slash2 = strrchr(argv0, '/');
    if (!slash || (slash2 && slash2 > slash)) slash = slash2;
    if (slash) {
        size_t dir = (size_t)(slash - argv0) + 1;
        for (size_t i = 0; i < sizeof rel / sizeof rel[0]; i++) {
            snprintf(path, sizeof path, "%.*s%s", (int)dir, argv0, rel[i]);
            FILE *f = fopen(path, "rb");
            if (f) { fclose(f); return path; }
        }
    }
    return rel[0];
}

int main(int argc, char **argv)
{
    MachineConfig cfg = { NULL, 4, -1, 0, NULL, 1 };
    char disc[1024];
    const char *png = NULL;
    int stats = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) cfg.rom_path = argv[++i];
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) cfg.mode = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ram") && i + 1 < argc) cfg.ram_mb = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) png = argv[++i];
        else if (!strcmp(argv[i], "--trace-swi")) cfg.trace_swi = 1;
        else if (!strcmp(argv[i], "--stats")) stats = 1;
        else if (!strcmp(argv[i], "--disc") && i + 1 < argc) cfg.disc_dir = argv[++i];
        else {
            fprintf(stderr, "uso: armbasic [--rom modulo] [--mode n] [--ram MB] [--png file] [--disc cartella] [--trace-swi] [--stats]\n");
            return 1;
        }
    }
    if (!cfg.rom_path) cfg.rom_path = find_rom(argv[0]);
    if (!cfg.disc_dir) { machine_default_disc(cfg.rom_path, disc, sizeof disc); cfg.disc_dir = disc; }

    static Machine m;
    char err[256];
    if (!machine_create(&m, &cfg, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    m.vdu.echo = echo;

    char line[1024];
    while (!m.cpu.halted) {
        machine_run(&m, 1000000000ull);
        if (m.cpu.halted) break;
        if (m.kernel.waiting && !kernel_keys_pending(&m.kernel) && !m.kernel.inkey_active) {
            fflush(stdout);
            if (!fgets(line, sizeof line, stdin)) break;
            for (char *p = line; *p && *p != '\n' && *p != '\r'; p++) {
                if (p[0] == '\\' && p[1] == 'e') { kernel_key(&m.kernel, 27); p++; continue; }
                kernel_key(&m.kernel, (uint8_t)*p);
            }
            if (!strstr(line, "\\e")) kernel_key(&m.kernel, 13);
        }
    }
    fflush(stdout);

    if (png) {
        uint32_t *rgb = malloc((size_t)m.vdu.width * m.vdu.height * 4);
        if (rgb) {
            vdu_render(&m.vdu, rgb, 0, 1);
            int ys = m.vdu.yeig > m.vdu.xeig ? 2 : 1;
            if (!png_write(png, rgb, m.vdu.width, m.vdu.height, ys)) fprintf(stderr, "impossibile scrivere %s\n", png);
            free(rgb);
        }
    }
    if (stats)
        fprintf(stderr, "\n%llu istruzioni, %llu cicli: %.3f s su un ARM2 a 8 MHz (DMA video %.0f%%)\n",
                (unsigned long long)m.cpu.instructions, (unsigned long long)m.cpu.cycles,
                (double)m.cpu.cycles / 8e6 / (1.0 - machine_dma_fraction(&m)), machine_dma_fraction(&m) * 100);
    int code = m.kernel.exit_code;
    machine_destroy(&m);
    return code;
}
