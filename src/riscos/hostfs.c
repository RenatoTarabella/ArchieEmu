/*
 * hostfs.c - Il "disco" della macchina e' una cartella dell'host.
 *
 * Convenzione dei nomi (la stessa di RPCEmu/HostFS):
 *   - in RISC OS il separatore di cartelle e' '.', e '/' fa da estensione:
 *     "giochi.mandel/bas" <-> "giochi/mandel.bas" sull'host;
 *   - il tipo di file RISC OS sta in un suffisso ",xxx": un programma BASIC
 *     salvato con SAVE "mandel" diventa "mandel,ffb". Senza suffisso il
 *     file e' di tipo Text (&FFF).
 */
#include "kernel_priv.h"
#include "core/hostdir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define TYPE_TEXT  HOSTDIR_TYPE_TEXT
#define ERR_NOT_FOUND 0x108D6u
#define OBJ_NONE HOSTOBJ_NONE
#define OBJ_FILE HOSTOBJ_FILE
#define OBJ_DIR  HOSTOBJ_DIR

typedef HostObject Object;

/* ------------------------------------------------------------------ */
/* nomi                                                               */
/* ------------------------------------------------------------------ */

/* nome RISC OS -> percorso dell'host senza suffisso del tipo. 0 se non valido */
static int map_name(RiscosKernel *k, const char *ro, char *out, size_t size)
{
    const char *p = ro;
    while (*p == ' ') p++;
    const char *colon = strrchr(p, ':');           /* "HostFS::HD.$.x", "ADFS:x" */
    if (colon) p = colon + 1;
    const char *dollar = strstr(p, "$.");
    if (dollar) p = dollar + 2;
    else if (!strcmp(p, "$") || !strcmp(p, "@")) p = "";
    else if (!strncmp(p, "@.", 2)) p += 2;
    return hostdir_map(k->disc_dir, p, out, size);
}

static int find_object(RiscosKernel *k, const char *ro, Object *o)
{
    char base[600];
    memset(o, 0, sizeof *o);
    return map_name(k, ro, base, sizeof base) && hostdir_find(base, o);
}

#define stat_object hostdir_stat
#define load_exec   hostdir_load_exec
#define list_dir    hostdir_list
#define mkdir_host  hostdir_mkdir
#define rmdir(p)    (hostdir_rmdir(p) ? 0 : -1)

static void not_found(RiscosKernel *k, uint32_t swi, const char *name)
{
    char msg[300];
    snprintf(msg, sizeof msg, "File '%s' not found", name);
    kernel_error(k, swi, ERR_NOT_FOUND, msg);
}

/* percorso per un file nuovo di tipo 'type', togliendo le versioni con altri suffissi */
static void new_file_path(RiscosKernel *k, const char *ro, uint32_t type, char *out, size_t size)
{
    char base[600];
    map_name(k, ro, base, sizeof base);
    hostdir_new_file(base, type, out, size);
}

/* ------------------------------------------------------------------ */
/* OS_File                                                            */
/* ------------------------------------------------------------------ */

#define R(n) (k->cpu->r[n])

static int save_block(RiscosKernel *k, const char *path, uint32_t start, uint32_t end)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    for (uint32_t a = start; a < end; a++) fputc(kernel_rd8(k, a), f);
    fclose(f);
    return 1;
}

void hostfs_file(RiscosKernel *k, uint32_t swi)
{
    uint32_t reason = R(0) & 0xFF;
    char name[256];
    kernel_read_str(k, R(1), name, sizeof name);
    Object o;
    char path[640];

    switch (reason) {
    case 0:                                        /* salva con load/exec */
    case 10: {                                     /* salva con il tipo */
        uint32_t type = reason == 10 ? (R(2) & 0xFFF)
                      : (R(2) >> 20) == 0xFFF ? (R(2) >> 8) & 0xFFF : 0xFFDu;
        if (!map_name(k, name, path, sizeof path)) { kernel_error(k, swi, 0xCC, "Bad name"); return; }
        new_file_path(k, name, type, path, sizeof path);
        if (!save_block(k, path, R(4), R(5))) { kernel_error(k, swi, 0xC1, "Unable to create file"); return; }
        return;
    }
    case 1: case 2: case 3: case 4: case 9:       /* attributi e date: ignorati */
        return;
    case 18: {                                     /* cambia tipo */
        if (!find_object(k, name, &o) || o.kind != OBJ_FILE) { not_found(k, swi, name); return; }
        char base[600];
        map_name(k, name, base, sizeof base);
        char dest[640];
        if ((R(2) & 0xFFF) == TYPE_TEXT) snprintf(dest, sizeof dest, "%s", base);
        else snprintf(dest, sizeof dest, "%s,%03x", base, R(2) & 0xFFF);
        if (strcmp(dest, o.path)) rename(o.path, dest);
        return;
    }
    case 5: case 13: case 15: case 17: case 20: case 21: case 23: {   /* informazioni */
        if (!find_object(k, name, &o)) {
            R(0) = OBJ_NONE;
            if (reason == 20 || reason == 21) R(6) = 0;
            return;
        }
        uint32_t load, exec;
        load_exec(&o, &load, &exec);
        R(0) = (uint32_t)o.kind;
        R(2) = load;
        R(3) = exec;
        R(4) = o.kind == OBJ_FILE ? o.length : 0;
        R(5) = 0x33;                                /* WR/wr */
        if (reason == 20 || reason == 21 || reason == 23) R(6) = o.type;
        return;
    }
    case 6: {                                      /* cancella */
        if (!find_object(k, name, &o)) { R(0) = OBJ_NONE; return; }
        uint32_t load, exec;
        load_exec(&o, &load, &exec);
        if (o.kind == OBJ_DIR ? rmdir(o.path) != 0 : remove(o.path) != 0) {
            kernel_error(k, swi, 0xC3, "Locked");
            return;
        }
        R(0) = (uint32_t)o.kind; R(2) = load; R(3) = exec; R(4) = o.length; R(5) = 0x33;
        return;
    }
    case 7: case 11: {                             /* crea un file vuoto */
        uint32_t type = reason == 11 ? (R(2) & 0xFFF) : (R(2) >> 20) == 0xFFF ? (R(2) >> 8) & 0xFFF : 0xFFDu;
        if (!map_name(k, name, path, sizeof path)) { kernel_error(k, swi, 0xCC, "Bad name"); return; }
        new_file_path(k, name, type, path, sizeof path);
        FILE *f = fopen(path, "wb");
        if (!f) { kernel_error(k, swi, 0xC1, "Unable to create file"); return; }
        for (uint32_t i = R(4); i < R(5); i++) fputc(0, f);
        fclose(f);
        return;
    }
    case 8:                                        /* crea una cartella */
        if (!map_name(k, name, path, sizeof path)) { kernel_error(k, swi, 0xCC, "Bad name"); return; }
        mkdir_host(path);
        return;
    case 12: case 14: case 16: case 255: {         /* carica */
        if (!find_object(k, name, &o)) { not_found(k, swi, name); return; }
        if (o.kind == OBJ_DIR) {
            char msg[300];
            snprintf(msg, sizeof msg, "'%s' is a directory", name);
            kernel_error(k, swi, 0x108B4, msg);
            return;
        }
        uint32_t load, exec;
        load_exec(&o, &load, &exec);
        if ((R(3) & 0xFF) != 0) {                  /* all'indirizzo del file: non ha senso con i tipi */
            kernel_error(k, swi, 0xD7, "File has no load address");
            return;
        }
        uint32_t addr = R(2);
        FILE *f = fopen(o.path, "rb");
        if (!f) { not_found(k, swi, name); return; }
        int c;
        uint32_t n = 0;
        while ((c = fgetc(f)) != EOF) kernel_wr8(k, addr + n++, (uint8_t)c);
        fclose(f);
        R(2) = load; R(3) = exec; R(4) = n; R(5) = 0x33;
        R(0) = OBJ_FILE;
        return;
    }
    default:
        kernel_error(k, swi, 0x1E6, "OS_File reason not supported");
        return;
    }
}

/* ------------------------------------------------------------------ */
/* file aperti                                                        */
/* ------------------------------------------------------------------ */

int hostfs_open(RiscosKernel *k, uint32_t swi, uint32_t reason, const char *name, int *handle)
{
    Object o;
    int exists = find_object(k, name, &o);
    const char *mode;
    char path[640];
    if ((reason & 0xC0) == 0x40) {                 /* OPENIN */
        if (!exists || o.kind != OBJ_FILE) return 0;
        mode = "rb";
        snprintf(path, sizeof path, "%s", o.path);
    } else if ((reason & 0xC0) == 0x80) {          /* OPENOUT: crea o tronca */
        if (!map_name(k, name, path, sizeof path)) return 0;
        if (exists && o.kind == OBJ_FILE) snprintf(path, sizeof path, "%s", o.path);
        mode = "w+b";
    } else {                                       /* OPENUP */
        if (!exists || o.kind != OBJ_FILE) return 0;
        mode = "r+b";
        snprintf(path, sizeof path, "%s", o.path);
    }
    for (int h = 1; h < K_MAX_FILES; h++) {
        if (k->files[h].used) continue;
        FILE *f = fopen(path, mode);
        if (!f) return 0;
        memset(&k->files[h], 0, sizeof k->files[h]);
        k->files[h].used = 1;
        k->files[h].host = 1;
        k->files[h].fp = f;
        *handle = h;
        return 1;
    }
    kernel_error(k, swi, 0xC0, "Too many open files");
    *handle = -1;
    return 1;
}

void hostfs_close(RiscosKernel *k, int h)
{
    if (h <= 0 || h >= K_MAX_FILES || !k->files[h].used) return;
    if (k->files[h].host && k->files[h].fp) fclose((FILE *)k->files[h].fp);
    memset(&k->files[h], 0, sizeof k->files[h]);
}

static KernelFile *file_of(RiscosKernel *k, int h)
{
    return h > 0 && h < K_MAX_FILES && k->files[h].used ? &k->files[h] : NULL;
}

int hostfs_bget(RiscosKernel *k, int h)
{
    KernelFile *f = file_of(k, h);
    if (!f) return -2;
    if (!f->host) return f->ptr < f->size ? kernel_rd8(k, f->data + f->ptr++) : -1;
    int c = fgetc((FILE *)f->fp);
    return c == EOF ? -1 : c;
}

int hostfs_bput(RiscosKernel *k, int h, uint8_t b)
{
    KernelFile *f = file_of(k, h);
    if (!f || !f->host) return 0;
    return fputc(b, (FILE *)f->fp) != EOF;
}

static long file_length(FILE *fp)
{
    long pos = ftell(fp);
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, pos, SEEK_SET);
    return len;
}

int hostfs_eof(RiscosKernel *k, int h)
{
    KernelFile *f = file_of(k, h);
    if (!f) return 1;
    if (!f->host) return f->ptr >= f->size;
    return ftell((FILE *)f->fp) >= file_length((FILE *)f->fp);
}

void hostfs_gbpb(RiscosKernel *k, uint32_t swi)
{
    uint32_t reason = R(0);
    KernelFile *f = file_of(k, (int)R(1));
    if (reason < 1 || reason > 4) { kernel_error(k, swi, 0x1E6, "OS_GBPB reason not supported"); return; }
    if (!f) { kernel_error(k, swi, 0xDE, "Channel"); return; }
    if (f->host) {
        FILE *fp = (FILE *)f->fp;
        if (reason == 1 || reason == 3) fseek(fp, (long)R(4), SEEK_SET);
        uint32_t n = 0, want = R(3);
        if (reason <= 2) {
            for (; n < want; n++) fputc(kernel_rd8(k, R(2) + n), fp);
        } else {
            int c;
            for (; n < want && (c = fgetc(fp)) != EOF; n++) kernel_wr8(k, R(2) + n, (uint8_t)c);
        }
        R(2) += n;
        R(3) = want - n;
        R(4) = (uint32_t)ftell(fp);
        k->cpu->r[15] = R(3) ? (k->cpu->r[15] | ARM_C) : (k->cpu->r[15] & ~ARM_C);
        return;
    }
    if (reason <= 2) { kernel_error(k, swi, 0xC1, "Read only"); return; }
    if (reason == 3) f->ptr = R(4);
    uint32_t n = 0;
    for (; n < R(3) && f->ptr < f->size; n++) kernel_wr8(k, R(2) + n, kernel_rd8(k, f->data + f->ptr++));
    R(2) += n;
    R(3) -= n;
    R(4) = f->ptr;
    k->cpu->r[15] = R(3) ? (k->cpu->r[15] | ARM_C) : (k->cpu->r[15] & ~ARM_C);
}

void hostfs_args(RiscosKernel *k, uint32_t swi)
{
    uint32_t reason = R(0);
    if (R(1) == 0) {                               /* filing system corrente */
        if (reason == 0) R(0) = 0x99;
        return;
    }
    KernelFile *f = file_of(k, (int)R(1));
    if (!f) { kernel_error(k, swi, 0xDE, "Channel"); return; }
    FILE *fp = f->host ? (FILE *)f->fp : NULL;
    switch (reason) {
    case 0: R(2) = fp ? (uint32_t)ftell(fp) : f->ptr; break;                 /* PTR# */
    case 1: if (fp) fseek(fp, (long)R(2), SEEK_SET); else f->ptr = R(2); break;
    case 2: R(2) = fp ? (uint32_t)file_length(fp) : f->size; break;         /* EXT# */
    case 3:
        if (fp) {
            long len = file_length(fp), pos = ftell(fp);
            if ((long)R(2) > len) { fseek(fp, 0, SEEK_END); for (long i = len; i < (long)R(2); i++) fputc(0, fp); }
            fseek(fp, pos, SEEK_SET);
        }
        break;
    case 5: R(2) = hostfs_eof(k, (int)R(1)) ? 0xFFFFFFFFu : 0; break;       /* EOF# */
    case 255: if (fp) fflush(fp); break;
    default: break;
    }
}

/* ------------------------------------------------------------------ */
/* comandi *                                                          */
/* ------------------------------------------------------------------ */

static const char *type_name(uint32_t t)
{
    switch (t) {
    case 0xFFB: return "BASIC";
    case 0xFFF: return "Text";
    case 0xFFD: return "Data";
    case 0xFF8: return "Absolute";
    case 0xFFA: return "Module";
    case 0xFEB: return "Obey";
    case 0xFFE: return "Command";
    case 0xFF9: return "Sprite";
    case 0x1000: return "Directory";
    default: return NULL;
    }
}

typedef struct CatCtx { RiscosKernel *k; const char *dir; int verbose, col; } CatCtx;

static int cat_entry(const char *name, void *vctx)
{
    CatCtx *c = vctx;
    Object o;
    memset(&o, 0, sizeof o);
    snprintf(o.path, sizeof o.path, "%s/%s", c->dir, name);
    if (!stat_object(&o)) return 0;
    char ro[256];
    hostdir_ro_name(name, ro, sizeof ro);
    char line[160];
    if (c->verbose) {
        const char *tn = type_name(o.type);
        char tbuf[8];
        if (!tn) { snprintf(tbuf, sizeof tbuf, "&%03X", o.type); tn = tbuf; }
        time_t t = (time_t)(o.date_cs / 100 - 2208988800ull);
        struct tm *tm = localtime(&t);
        char when[32] = "";
        if (tm) strftime(when, sizeof when, "%H:%M:%S %d-%b-%Y", tm);
        if (o.kind == OBJ_DIR) snprintf(line, sizeof line, "%-20s D/   %s\n", ro, "Directory");
        else snprintf(line, sizeof line, "%-20s WR/  %-9s %s %8u byte\n", ro, tn, when, o.length);
        kernel_print(c->k, line);
    } else {
        snprintf(line, sizeof line, "%-18s%s", ro, o.kind == OBJ_DIR ? "D " : "WR");
        kernel_print(c->k, line);
        if (++c->col == 3) { kernel_print(c->k, "\n"); c->col = 0; }
        else kernel_print(c->k, "    ");
    }
    return 0;
}

static void catalogue(RiscosKernel *k, uint32_t swi, const char *args, int verbose)
{
    char dir[600];
    if (!map_name(k, *args ? args : "$", dir, sizeof dir)) { kernel_error(k, swi, 0xCC, "Bad name"); return; }
    struct stat st;
    if (stat(dir, &st) != 0 || !(st.st_mode & S_IFDIR)) { not_found(k, swi, args); return; }
    char head[700];
    snprintf(head, sizeof head, "HostFS: %s\n\n", dir);
    kernel_print(k, head);
    CatCtx c = { k, dir, verbose, 0 };
    list_dir(dir, cat_entry, &c);
    if (c.col) kernel_print(k, "\n");
}

static void first_word(const char *args, char *out, size_t size)
{
    size_t n = 0;
    while (*args == ' ') args++;
    if (*args == '"') {
        args++;
        while (*args && *args != '"' && n < size - 1) out[n++] = *args++;
    } else {
        while (*args && *args != ' ' && n < size - 1) out[n++] = *args++;
    }
    out[n] = 0;
}

int hostfs_command(RiscosKernel *k, uint32_t swi, const char *cmd, const char *args)
{
    char a1[256], a2[256];
    first_word(args, a1, sizeof a1);
    if (!strcmp(cmd, "CAT") || !strcmp(cmd, ".")) { catalogue(k, swi, a1, 0); return 1; }
    if (!strcmp(cmd, "EX") || !strcmp(cmd, "INFO") || !strcmp(cmd, "FILEINFO")) { catalogue(k, swi, a1, 1); return 1; }
    if (!strcmp(cmd, "DELETE") || !strcmp(cmd, "REMOVE") || !strcmp(cmd, "WIPE")) {
        Object o;
        if (!find_object(k, a1, &o)) { if (strcmp(cmd, "REMOVE")) not_found(k, swi, a1); return 1; }
        if (o.kind == OBJ_DIR ? rmdir(o.path) != 0 : remove(o.path) != 0) kernel_error(k, swi, 0xC3, "Locked");
        return 1;
    }
    if (!strcmp(cmd, "RENAME")) {
        const char *p = args;
        while (*p == ' ') p++;
        while (*p && *p != ' ') p++;
        first_word(p, a2, sizeof a2);
        Object o;
        if (!find_object(k, a1, &o)) { not_found(k, swi, a1); return 1; }
        char dest[640];
        if (!map_name(k, a2, dest, sizeof dest)) { kernel_error(k, swi, 0xCC, "Bad name"); return 1; }
        if (o.kind == OBJ_FILE && o.type != TYPE_TEXT) {
            size_t l = strlen(dest);
            snprintf(dest + l, sizeof dest - l, ",%03x", o.type);
        }
        if (rename(o.path, dest) != 0) kernel_error(k, swi, 0xC3, "Rename failed");
        return 1;
    }
    if (!strcmp(cmd, "CDIR")) {
        char path[640];
        if (!map_name(k, a1, path, sizeof path)) { kernel_error(k, swi, 0xCC, "Bad name"); return 1; }
        mkdir_host(path);
        return 1;
    }
    if (!strcmp(cmd, "TYPE") || !strcmp(cmd, "PRINT")) {
        Object o;
        if (!find_object(k, a1, &o) || o.kind != OBJ_FILE) { not_found(k, swi, a1); return 1; }
        FILE *f = fopen(o.path, "rb");
        if (!f) return 1;
        int c;
        while ((c = fgetc(f)) != EOF) {
            if (c == '\n') kernel_print(k, "\n");
            else if (c != '\r') { char s[2] = { (char)c, 0 }; kernel_print(k, s); }
        }
        fclose(f);
        return 1;
    }
    return 0;
}
