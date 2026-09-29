/*
 * romdis.c - Disassembla un'immagine binaria ARMv2 (es. una ROM).
 *
 *   romdis file base inizio [quante]      (numeri in esadecimale, "&" o "0x" facoltativi)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu/arm2_disasm.h"

static unsigned long hex(const char *s)
{
    if (*s == '&') s++;
    return strtoul(s, NULL, 16);
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "uso: romdis file base inizio [quante]\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    unsigned long base = hex(argv[2]), start = hex(argv[3]);
    long count = argc > 4 ? strtol(argv[4], NULL, 10) : 64;
    if (fseek(f, (long)(start - base), SEEK_SET) != 0) { fprintf(stderr, "fuori dal file\n"); return 1; }
    for (long i = 0; i < count; i++) {
        unsigned char b[4];
        if (fread(b, 1, 4, f) != 4) break;
        uint32_t w = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
        uint32_t addr = (uint32_t)(start + 4 * (unsigned long)i);
        char text[80];
        arm2_disasm(w, addr, text, sizeof text);
        char ascii[5];
        for (int k = 0; k < 4; k++) ascii[k] = (b[k] >= 32 && b[k] < 127) ? (char)b[k] : '.';
        ascii[4] = 0;
        printf("%07X  %08X  %s  %s\n", addr, w, ascii, text);
    }
    fclose(f);
    return 0;
}
