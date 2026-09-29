/*
 * modinfo.c - Mostra l'intestazione di un modulo RISC OS, lo espande se
 * compresso e opzionalmente lo salva o lo disassembla.
 *
 *   modinfo modulo [-o espanso] [-d]
 */
#include <stdio.h>
#include <string.h>
#include "riscos/module.h"
#include "cpu/arm2_disasm.h"

int main(int argc, char **argv)
{
    const char *in = NULL, *out = NULL;
    int dis = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-d")) dis = 1;
        else in = argv[i];
    }
    if (!in) { fprintf(stderr, "uso: modinfo modulo [-o espanso] [-d]\n"); return 1; }

    RiscosModule m;
    char err[256];
    if (!module_load(in, &m, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }

    printf("dimensione   %u byte%s\n", (unsigned)m.size, m.was_squeezed ? " (era compresso)" : "");
    static const char *names[] = { "start", "init", "final", "service", "title", "help",
                                   "commands", "swi base", "swi handler", "swi table",
                                   "swi decode", "messages", "flags" };
    for (int k = 0; k < 13; k++)
        printf("%-12s &%08X\n", names[k], module_word(&m, 4u * (uint32_t)k));
    printf("titolo       %s\n", module_string(&m, module_word(&m, MODULE_TITLE)));
    printf("help         %s\n", module_string(&m, module_word(&m, MODULE_HELP)));

    if (out) {
        FILE *f = fopen(out, "wb");
        if (!f || fwrite(m.data, 1, m.size, f) != m.size) { perror(out); return 1; }
        fclose(f);
        printf("salvato in %s\n", out);
    }
    if (dis) {
        for (uint32_t a = 0; a + 4 <= m.size; a += 4) {
            char text[80];
            uint32_t w = module_word(&m, a);
            arm2_disasm(w, a, text, sizeof text);
            printf("%06X  %08X  %s\n", a, w, text);
        }
    }
    module_free(&m);
    return 0;
}
