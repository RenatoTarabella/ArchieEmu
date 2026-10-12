/*
 * romlist.c - Riconoscimento delle ROM (vedi romlist.h)
 */
#include "romlist.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int same_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

int romlist_version(const char *path, int *riscpc)
{
    const char *s = strrchr(path, '\\'), *t = strrchr(path, '/');
    if (t && (!s || t > s)) s = t;
    s = s ? s + 1 : path;
    *riscpc = 0;
    if (tolower((unsigned char)s[0]) != 'r' || tolower((unsigned char)s[1]) != 'o' ||
        tolower((unsigned char)s[2]) != 'm' || !isdigit((unsigned char)s[3]) ||
        !isdigit((unsigned char)s[4]) || !isdigit((unsigned char)s[5]))
        return 0;
    int v = (s[3] - '0') * 100 + (s[4] - '0') * 10 + (s[5] - '0');
    const char *rest = s + 6;
    if (strstr(rest, ".cmos") || strstr(rest, ".CMOS")) return 0;
    if (v >= 350) {
        /* 3.50-3.80 per ARM6/ARM7; la versione StrongARM e RISC OS 4+ non ancora */
        if (v > 380 || same_ci(rest, ".SA")) return 0;
        *riscpc = 1;
    }
    return v;
}

/* 3.50-3.80 sono del Risc PC; dopo la 3.80 nessuna (StrongARM e RISC OS 4+) */
static int classify(int v, int *riscpc)
{
    *riscpc = 0;
    if (v >= 350) {
        if (v > 380) return 0;
        *riscpc = 1;
    }
    return v;
}

int romlist_identify(const char *path, int *riscpc)
{
    *riscpc = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long size = ftell(f);
    /* dimensioni di una ROM: da 512 KB (Arthur, RISC OS 2) a 8 MB */
    if (size < 512 * 1024 || size > 8 * 1024 * 1024 + 16) { fclose(f); return 0; }
    rewind(f);
    static unsigned char buf[512 * 1024];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    static const char *const tags[] = { "RISC OS ", "Arthur " };
    for (size_t i = 0; i + 16 < n; i++) {
        if (buf[i] != 'R' && buf[i] != 'A') continue;
        for (int t = 0; t < 2; t++) {
            size_t len = strlen(tags[t]);
            if (memcmp(buf + i, tags[t], len)) continue;
            const unsigned char *p = buf + i + len;
            while (*p == ' ') p++;
            if (!isdigit(p[0]) || p[1] != '.' || !isdigit(p[2]) || !isdigit(p[3])) continue;
            /* dopo il numero la data tra parentesi: "RISC OS 3.10 or later" non conta */
            const unsigned char *q = p + 4;
            while (q < buf + n && q < p + 16 && *q != '(' && *q >= ' ' && *q < 127) q++;
            if (q >= buf + n || *q != '(') continue;
            return classify((p[0] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0'), riscpc);
        }
    }
    return 0;
}

int romlist_detect(const char *path, int *riscpc)
{
    int named = romlist_version(path, riscpc);
    const char *s = strrchr(path, '\\'), *t = strrchr(path, '/');
    if (t && (!s || t > s)) s = t;
    s = s ? s + 1 : path;
    /* ROM380.SA e simili: il nome la esclude, il contenuto non lo sa */
    if (!named && strlen(s) > 6 && same_ci(s + 6, ".SA")) return 0;
    int v = romlist_identify(path, riscpc);
    return v ? v : romlist_version(path, riscpc);
}

void romlist_name(int v, char *out, size_t size)
{
    if (v < 200) snprintf(out, size, "Arthur %d.%02d", v / 100, v % 100);
    else         snprintf(out, size, "RISC OS %d.%02d", v / 100, v % 100);
}
