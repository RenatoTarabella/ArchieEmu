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

void romlist_name(int v, char *out, size_t size)
{
    if (v < 200) snprintf(out, size, "Arthur %d.%02d", v / 100, v % 100);
    else         snprintf(out, size, "RISC OS %d.%02d", v / 100, v % 100);
}
