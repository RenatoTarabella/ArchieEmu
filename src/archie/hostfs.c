/*
 * hostfs.c - HostFS dell'Archimedes (vedi hostfs.h)
 *
 * Il protocollo e' quello di FileSwitch 2.08, ricostruito dalla ROM 3.11
 * (ResourceFS e PipeFS come esempi, piu' una traccia delle chiamate):
 *   - i nomi arrivano assoluti: "$" o "$.dir.file", senza prefisso del FS;
 *   - i file aperti sono "bufferizzati": FileSwitch tiene puntatore ed
 *     estensione e chiede blocchi da 1 KB a un offset (GetBytes/PutBytes);
 *   - *Cat, *Ex, *Info li fa FileSwitch con Func 14/15 (elenco della cartella);
 *   - "non trovato": Open con R1 = 0, File 5 con R0 = 0;
 *   - errori: V acceso, R0 -> blocco (numero, messaggio).
 */
#include "hostfs.h"
#include "memc.h"
#include "core/hostdir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define strcasecmp _stricmp
#define truncate_file(fp, n) _chsize_s(_fileno(fp), (long long)(n))
#else
#include <strings.h>
#include <unistd.h>
#define truncate_file(fp, n) ftruncate(fileno(fp), (off_t)(n))
#endif

#define R(n)        (cpu->r[n])
#define BUF_SIZE    0x400u            /* blocchi chiesti da FileSwitch */
#define ATTR_FILE   0x13u             /* WR/R */
#define ERR(code)   (0x10000u | 0x9900u | (code))

static void arc_hostfs_dispatch(ArcHostFS *h, HostFsRegs *cpu, int entry);

/* ------------------------------------------------------------------ */
/* memoria e registri                                                 */
/* ------------------------------------------------------------------ */

static uint8_t rd8(ArcHostFS *h, uint32_t a) { return h->rd8(h->mem_ctx, a); }
static void wr8(ArcHostFS *h, uint32_t a, uint8_t v) { h->wr8(h->mem_ctx, a, v); }

static void wr32(ArcHostFS *h, uint32_t a, uint32_t v)
{
    for (int i = 0; i < 4; i++) wr8(h, a + (uint32_t)i, (uint8_t)(v >> (8 * i)));
}

static void read_str(ArcHostFS *h, uint32_t a, char *out, size_t size)
{
    size_t n = 0;
    uint8_t c;
    while (n < size - 1 && (c = rd8(h, a + (uint32_t)n)) >= 32) out[n++] = (char)c;
    out[n] = 0;
}

static uint32_t write_str(ArcHostFS *h, uint32_t a, const char *s)
{
    uint32_t n = 0;
    do wr8(h, a + n, (uint8_t)s[n]); while (s[n++]);
    return n;
}

static void ok(HostFsRegs *cpu) { cpu->v = 0; }

static void error(ArcHostFS *h, HostFsRegs *cpu, uint32_t num, const char *msg)
{
    uint32_t blk = R(12) + ARC_HOSTFS_WS_ERROR;
    wr32(h, blk, num);
    char s[240];
    snprintf(s, sizeof s, "%s", msg);
    write_str(h, blk + 4, s);
    R(0) = blk;
    cpu->v = 1;
}

/* ------------------------------------------------------------------ */
/* nomi                                                               */
/* ------------------------------------------------------------------ */

/* "$.dir.file" -> percorso base sull'host */
static int host_base(ArcHostFS *h, HostFsRegs *cpu, uint32_t name_addr, char *out, size_t size)
{
    char ro[256];
    read_str(h, name_addr, ro, sizeof ro);
    const char *p = ro;
    if (p[0] == '$' && p[1] == '.') p += 2;
    else if (p[0] == '$' && p[1] == 0) p++;
    if (!hostdir_map(h->root, p, out, size)) { error(h, cpu, ERR(0xCC), "Bad name"); return 0; }
    return 1;
}

/* Un nome che l'host non puo' rappresentare (jolly compresi: *Copy chiede
   "$.dir.*") non esiste: base resta vuota e si da' errore solo creandolo */
static int find(ArcHostFS *h, HostFsRegs *cpu, uint32_t name_addr, char *base, size_t bsize, HostObject *o)
{
    char ro[256];
    read_str(h, name_addr, ro, sizeof ro);
    const char *p = ro;
    if (p[0] == '$' && p[1] == '.') p += 2;
    else if (p[0] == '$' && p[1] == 0) p++;
    (void)cpu;
    memset(o, 0, sizeof *o);
    if (!hostdir_map(h->root, p, base, bsize)) { base[0] = 0; return 0; }
    return hostdir_find(base, o);
}

static int bad_name(ArcHostFS *h, HostFsRegs *cpu, const char *base)
{
    if (base[0]) return 0;
    error(h, cpu, ERR(0xCC), "Bad name");
    return 1;
}

static uint32_t leaf_name(ArcHostFS *h, HostFsRegs *cpu, const char *path)
{
    const char *s = strrchr(path, '/');
    char ro[256];
    hostdir_ro_name(s ? s + 1 : path, ro, sizeof ro);
    uint32_t a = R(12) + ARC_HOSTFS_WS_NAME;
    write_str(h, a, ro);
    return a;
}

static void info_regs(HostFsRegs *cpu, const HostObject *o)
{
    uint32_t load, exec;
    hostdir_load_exec(o, &load, &exec);
    R(0) = (uint32_t)o->kind;
    R(2) = load;
    R(3) = exec;
    R(4) = o->length;
    R(5) = o->kind == HOSTOBJ_FILE ? ATTR_FILE : 0;
}

/* nuovi load/exec: nome con il suffisso giusto e, se c'e', la data */
static void apply_load_exec(const char *path, uint32_t load, uint32_t exec)
{
    char dest[640];
    hostdir_retype_path(path, load, exec, dest, sizeof dest);
    if (strcmp(dest, path) && rename(path, dest) == 0) path = dest;
    if ((load >> 20) == 0xFFF) hostdir_set_date(path, ((uint64_t)(load & 0xFF) << 32) | exec);
}

/* ------------------------------------------------------------------ */
/* file aperti                                                        */
/* ------------------------------------------------------------------ */

static FILE *handle_fp(ArcHostFS *h, uint32_t handle)
{
    return handle >= 1 && handle <= ARC_HOSTFS_FILES ? (FILE *)h->fp[handle - 1] : NULL;
}

static long file_len(FILE *fp)
{
    long pos = ftell(fp);
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, pos, SEEK_SET);
    return n;
}

static void fs_open(ArcHostFS *h, HostFsRegs *cpu)
{
    uint32_t reason = R(0);
    char base[600];
    HostObject o;
    int found = find(h, cpu, R(1), base, sizeof base, &o);
    if (found < 0) return;
    if (!found && reason == 1) {                       /* crea: file di dati vuoto */
        if (bad_name(h, cpu, base)) return;
        char path[640];
        hostdir_new_file_le(base, 0xFFFFFD00u, 0, path, sizeof path);
        FILE *f = fopen(path, "wb");
        if (!f) { error(h, cpu, ERR(0xC1), "Unable to create file"); return; }
        fclose(f);
        found = hostdir_find(base, &o);
    }
    if (!found || o.kind != HOSTOBJ_FILE) { R(1) = 0; ok(cpu); return; }   /* non trovato */
    int slot = -1;
    for (int i = 0; i < ARC_HOSTFS_FILES; i++) if (!h->fp[i]) { slot = i; break; }
    if (slot < 0) { error(h, cpu, ERR(0xC0), "Too many open files on HostFS"); return; }
    FILE *f = fopen(o.path, reason == 0 ? "rb" : "r+b");
    if (!f && reason == 0) { R(1) = 0; ok(cpu); return; }
    if (!f) { error(h, cpu, ERR(0xC3), "Access violation"); return; }
    h->fp[slot] = f;
    snprintf(h->path[slot], sizeof h->path[slot], "%s", o.path);
    R(0) = reason == 0 ? 0x40000000u : 0xC0000000u;   /* lettura (e scrittura) */
    R(1) = (uint32_t)slot + 1;
    R(2) = BUF_SIZE;
    R(3) = o.length;
    R(4) = (o.length + BUF_SIZE - 1) & ~(BUF_SIZE - 1);
    ok(cpu);
}

static void fs_getbytes(ArcHostFS *h, HostFsRegs *cpu)
{
    FILE *f = handle_fp(h, R(1));
    if (!f) { error(h, cpu, ERR(0xDE), "Channel"); return; }
    uint8_t buf[BUF_SIZE];
    uint32_t addr = R(2), left = R(3);
    fseek(f, (long)R(4), SEEK_SET);
    while (left) {
        uint32_t n = left < BUF_SIZE ? left : BUF_SIZE;
        size_t got = fread(buf, 1, n, f);
        memset(buf + got, 0, n - got);                 /* oltre la fine: zeri */
        for (uint32_t i = 0; i < n; i++) wr8(h, addr + i, buf[i]);
        addr += n;
        left -= n;
    }
    ok(cpu);
}

static void fs_putbytes(ArcHostFS *h, HostFsRegs *cpu)
{
    FILE *f = handle_fp(h, R(1));
    if (!f) { error(h, cpu, ERR(0xDE), "Channel"); return; }
    uint8_t buf[BUF_SIZE];
    uint32_t addr = R(2), left = R(3);
    fseek(f, (long)R(4), SEEK_SET);
    while (left) {
        uint32_t n = left < BUF_SIZE ? left : BUF_SIZE;
        for (uint32_t i = 0; i < n; i++) buf[i] = rd8(h, addr + i);
        if (fwrite(buf, 1, n, f) != n) { error(h, cpu, ERR(0xC6), "HostFS: disc full"); return; }
        addr += n;
        left -= n;
    }
    ok(cpu);
}

static void fs_args(ArcHostFS *h, HostFsRegs *cpu)
{
    FILE *f = handle_fp(h, R(1));
    if (!f) { error(h, cpu, ERR(0xDE), "Channel"); return; }
    switch (R(0)) {
    case 3:                                            /* nuova estensione */
        fflush(f);
        if (truncate_file(f, R(2)) != 0) { error(h, cpu, ERR(0xC6), "HostFS: cannot set the file length"); return; }
        break;
    case 4:                                            /* spazio allocato */
        R(2) = ((uint32_t)file_len(f) + BUF_SIZE - 1) & ~(BUF_SIZE - 1);
        break;
    case 6:                                            /* svuota */
        fflush(f);
        break;
    case 7:                                            /* assicura la dimensione: lo spazio c'e' */
        break;
    case 8: {                                          /* scrive R3 zeri da R2 */
        static const uint8_t zero[256] = { 0 };
        fseek(f, (long)R(2), SEEK_SET);
        for (uint32_t left = R(3); left; ) {
            uint32_t n = left < sizeof zero ? left : (uint32_t)sizeof zero;
            fwrite(zero, 1, n, f);
            left -= n;
        }
        break;
    }
    case 9: {                                          /* load/exec */
        HostObject o;
        memset(&o, 0, sizeof o);
        snprintf(o.path, sizeof o.path, "%s", h->path[R(1) - 1]);
        hostdir_stat(&o);
        hostdir_load_exec(&o, &R(2), &R(3));
        break;
    }
    default:                                           /* gli altri solo per file non bufferizzati */
        break;
    }
    ok(cpu);
}

static void fs_close(ArcHostFS *h, HostFsRegs *cpu)
{
    FILE *f = handle_fp(h, R(1));
    if (!f) { error(h, cpu, ERR(0xDE), "Channel"); return; }
    fclose(f);
    h->fp[R(1) - 1] = NULL;
    if (R(2) || R(3)) apply_load_exec(h->path[R(1) - 1], R(2), R(3));
    ok(cpu);
}

/* ------------------------------------------------------------------ */
/* FSEntry_File                                                       */
/* ------------------------------------------------------------------ */

static int write_block(ArcHostFS *h, const char *path, uint32_t start, uint32_t end, int zeros)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    uint8_t buf[BUF_SIZE];
    for (uint32_t a = start; a < end; ) {
        uint32_t n = end - a < BUF_SIZE ? end - a : BUF_SIZE;
        for (uint32_t i = 0; i < n; i++) buf[i] = zeros ? 0 : rd8(h, a + i);
        if (fwrite(buf, 1, n, f) != n) { fclose(f); return 0; }
        a += n;
    }
    fclose(f);
    return 1;
}

static void fs_file(ArcHostFS *h, HostFsRegs *cpu)
{
    uint32_t reason = R(0);
    char base[600], path[640];
    HostObject o;
    int found = find(h, cpu, R(1), base, sizeof base, &o);
    if (found < 0) return;

    switch (reason) {
    case 0:                                            /* salva */
    case 7:                                            /* crea */
        if (found && o.kind == HOSTOBJ_DIR) { error(h, cpu, ERR(0xBD), "HostFS: a directory has that name"); return; }
        if (bad_name(h, cpu, base)) return;
        hostdir_new_file_le(base, R(2), R(3), path, sizeof path);
        if (!write_block(h, path, R(4), R(5), reason == 7)) { error(h, cpu, ERR(0xC1), "HostFS: unable to create file"); return; }
        if ((R(2) >> 20) == 0xFFF) hostdir_set_date(path, ((uint64_t)(R(2) & 0xFF) << 32) | R(3));
        if (reason == 0) R(6) = leaf_name(h, cpu, path);
        break;
    case 1: case 2: case 3: {                          /* scrive load/exec */
        if (!found) { error(h, cpu, ERR(0xD6), "File not found"); return; }
        if (o.kind == HOSTOBJ_DIR) break;
        uint32_t load, exec;
        hostdir_load_exec(&o, &load, &exec);
        if (reason != 3) load = R(2);
        if (reason != 2) exec = R(3);
        apply_load_exec(o.path, load, exec);
        break;
    }
    case 4:                                            /* attributi: l'host non li ha */
        break;
    case 5: case 9:                                    /* informazioni */
        if (!found) { R(0) = 0; R(2) = R(3) = R(4) = R(5) = 0; break; }
        info_regs(cpu, &o);
        break;
    case 6:                                            /* cancella */
        if (!found) { R(0) = 0; break; }
        if (o.kind == HOSTOBJ_DIR ? !hostdir_rmdir(o.path) : remove(o.path) != 0) {
            error(h, cpu, ERR(0xB4), o.kind == HOSTOBJ_DIR ? "Directory not empty" : "HostFS: file is in use");
            return;
        }
        info_regs(cpu, &o);
        break;
    case 8:                                            /* crea una cartella */
        if (found && o.kind == HOSTOBJ_DIR) break;
        if (bad_name(h, cpu, base)) return;
        if (found || !hostdir_mkdir(base)) { error(h, cpu, ERR(0xC1), "HostFS: unable to create directory"); return; }
        break;
    case 255: {                                        /* carica */
        if (!found || o.kind != HOSTOBJ_FILE) { error(h, cpu, ERR(0xD6), "File not found"); return; }
        FILE *f = fopen(o.path, "rb");
        if (!f) { error(h, cpu, ERR(0xC3), "Access violation"); return; }
        uint8_t buf[BUF_SIZE];
        size_t n;
        uint32_t addr = R(2);
        while ((n = fread(buf, 1, sizeof buf, f)) > 0)
            for (size_t i = 0; i < n; i++) wr8(h, addr++, buf[i]);
        fclose(f);
        info_regs(cpu, &o);
        R(6) = leaf_name(h, cpu, o.path);
        break;
    }
    default:
        error(h, cpu, ERR(0xA5), "Bad operation on HostFS");
        return;
    }
    ok(cpu);
}

/* ------------------------------------------------------------------ */
/* FSEntry_Func: elenchi e rinomina                                   */
/* ------------------------------------------------------------------ */

typedef struct Listing { char (*names)[256]; int n, cap; } Listing;

/* i nomi che RISC OS non puo' rappresentare si saltano */
static int add_name(const char *name, void *ctx)
{
    Listing *l = ctx;
    if (strpbrk(name, "$&@^%\\:*#\"|")) return 0;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 64;
        void *p = realloc(l->names, (size_t)cap * sizeof *l->names);
        if (!p) return 1;
        l->names = p;
        l->cap = cap;
    }
    snprintf(l->names[l->n++], 256, "%s", name);
    return 0;
}

static int cmp_names(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

static void read_dir(ArcHostFS *h, HostFsRegs *cpu, int reason)
{
    char dir[600];
    HostObject d;
    int found = find(h, cpu, R(1), dir, sizeof dir, &d);
    if (found < 0) return;
    if (!found || d.kind != HOSTOBJ_DIR) { error(h, cpu, ERR(0xD6), "Directory not found"); return; }
    Listing l = { NULL, 0, 0 };
    hostdir_list(d.path, add_name, &l);
    if (l.n) qsort(l.names, (size_t)l.n, sizeof *l.names, cmp_names);

    uint32_t buf = R(2), want = R(3), room = R(5), pos = 0, count = 0;
    int i = (int)R(4);
    for (; i < l.n && count < want; i++) {
        HostObject o;
        memset(&o, 0, sizeof o);
        snprintf(o.path, sizeof o.path, "%s/%s", d.path, l.names[i]);
        if (!hostdir_stat(&o)) continue;
        char ro[256];
        hostdir_ro_name(l.names[i], ro, sizeof ro);
        uint32_t len = (uint32_t)strlen(ro) + 1;
        uint32_t head = reason == 14 ? 0 : reason == 15 ? 20 : 29;
        uint32_t size = reason == 14 ? len : (head + len + 3) & ~3u;
        if (pos + size > room) break;
        if (reason != 14) {
            uint32_t load, exec;
            hostdir_load_exec(&o, &load, &exec);
            wr32(h, buf + pos, load);
            wr32(h, buf + pos + 4, exec);
            wr32(h, buf + pos + 8, o.length);
            wr32(h, buf + pos + 12, o.kind == HOSTOBJ_FILE ? ATTR_FILE : 0);
            wr32(h, buf + pos + 16, (uint32_t)o.kind);
            if (reason == 19) {                        /* SIN e data in 5 byte */
                wr32(h, buf + pos + 20, 0);
                for (int b = 0; b < 5; b++) wr8(h, buf + pos + 24 + (uint32_t)b, (uint8_t)(o.date_cs >> (8 * b)));
            }
        }
        write_str(h, buf + pos + head, ro);
        pos += size;
        count++;
    }
    /* come ResourceFS e PipeFS: -1 solo quando non c'e' piu' niente */
    R(3) = count;
    R(4) = count == 0 && i >= l.n ? 0xFFFFFFFFu : (uint32_t)i;
    free(l.names);
    ok(cpu);
}

static void fs_func(ArcHostFS *h, HostFsRegs *cpu)
{
    switch (R(0)) {
    case 0: case 1: case 7: case 17:                   /* *Dir, *Lib, *Opt, banner */
        break;
    case 8: {                                          /* rinomina */
        char base[600], dest_base[600], dest[640];
        HostObject o;
        int found = find(h, cpu, R(1), base, sizeof base, &o);
        if (found < 0) return;
        if (!found || !host_base(h, cpu, R(2), dest_base, sizeof dest_base)) { R(1) = 1; break; }
        if (o.kind == HOSTOBJ_FILE) {
            uint32_t load, exec;
            hostdir_load_exec(&o, &load, &exec);
            hostdir_retype_path(dest_base, load, exec, dest, sizeof dest);
        } else {
            snprintf(dest, sizeof dest, "%s", dest_base);
        }
        R(1) = rename(o.path, dest) == 0 ? 0 : 1;
        break;
    }
    case 11:                                           /* nome del disco: nessuno */
        wr8(h, R(2), 0);
        wr8(h, R(2) + 1, 0);
        break;
    case 14: case 15: case 19:
        read_dir(h, cpu, (int)R(0));
        return;
    case 16:                                           /* spegnimento */
        arc_hostfs_close_all(h);
        break;
    default:
        error(h, cpu, ERR(0xA5), "Bad operation on HostFS");
        return;
    }
    ok(cpu);
}

/* ------------------------------------------------------------------ */

void arc_hostfs_init_mem(ArcHostFS *h, const char *root, HostFsRead8 rd, HostFsWrite8 wr, void *ctx)
{
    memset(h, 0, sizeof *h);
    snprintf(h->root, sizeof h->root, "%s", root);
    size_t n = strlen(h->root);
    while (n > 1 && (h->root[n - 1] == '/' || h->root[n - 1] == '\\')) h->root[--n] = 0;
    h->rd8 = rd;
    h->wr8 = wr;
    h->mem_ctx = ctx;
}

/* Archimedes: la memoria logica passa dal MEMC */
static uint8_t memc_rd8(void *ctx, uint32_t a) { int ab = 0; return memc_read8((Memc *)ctx, a, &ab); }
static void memc_wr8(void *ctx, uint32_t a, uint8_t v) { int ab = 0; memc_write8((Memc *)ctx, a, v, &ab); }

void arc_hostfs_init(ArcHostFS *h, const char *root, struct Memc *memc)
{
    arc_hostfs_init_mem(h, root, memc_rd8, memc_wr8, memc);
}

void arc_hostfs_close_all(ArcHostFS *h)
{
    for (int i = 0; i < ARC_HOSTFS_FILES; i++)
        if (h->fp[i]) { fclose((FILE *)h->fp[i]); h->fp[i] = NULL; }
}

void arc_hostfs_entry(ArcHostFS *h, Arm2 *arm, int entry)
{
    /* i registri dell'ARM2; V torna nel PSR dentro R15 */
    HostFsRegs regs;
    memcpy(regs.r, arm->r, sizeof regs.r);
    regs.v = (arm->r[15] & ARM_V) != 0;
    arc_hostfs_call(h, &regs, entry);
    memcpy(arm->r, regs.r, 15 * sizeof(uint32_t));
    arm->r[15] = regs.v ? arm->r[15] | ARM_V : arm->r[15] & ~ARM_V;
}

void arc_hostfs_call(ArcHostFS *h, HostFsRegs *cpu, int entry)
{
    /* ARCHIE_HOSTFS_TRACE=1: ogni chiamata di FileSwitch su stderr (debug) */
    static int trace = -1;
    if (trace < 0) trace = getenv("ARCHIE_HOSTFS_TRACE") != NULL;
    if (trace) {
        char name[64] = "";
        if (entry == 0 || entry == 5 || (entry == 6 && R(0) != 11)) read_str(h, R(1), name, sizeof name);
        fprintf(stderr, "hostfs %d  in  R0=%08X R1=%08X R2=%08X R3=%08X R4=%08X R5=%08X %s\n",
                entry, R(0), R(1), R(2), R(3), R(4), R(5), name);
    }
    arc_hostfs_dispatch(h, cpu, entry);
    if (trace)
        fprintf(stderr, "hostfs %d  out R0=%08X R1=%08X R2=%08X R3=%08X R4=%08X R5=%08X%s\n",
                entry, R(0), R(1), R(2), R(3), R(4), R(5), cpu->v ? "  ERRORE" : "");
}

/* *HostFS_Insert: il nome arriva come lo da' il Filer ("HostFS:$.dir.Gioco/hfe",
   anche "HostFS::HostFS.$..."), si cerca il file e lo si mette nell'unita' 0 */
static void insert_disc(ArcHostFS *h, HostFsRegs *cpu)
{
    char ro[256], base[600];
    read_str(h, R(0), ro, sizeof ro);
    const char *p = strchr(ro, ' ') ? NULL : ro;
    if (p) {
        const char *colon = strrchr(p, ':');
        if (colon) p = colon + 1;
        const char *dollar = strstr(p, "$.");
        p = dollar ? dollar + 2 : p;
    }
    HostObject o;
    if (!p || !hostdir_map(h->root, p, base, sizeof base) || !hostdir_find(base, &o) || o.kind != HOSTOBJ_FILE) {
        error(h, cpu, ERR(0xD6), "HostFS_Insert: the disc image must be a file on HostFS");
        return;
    }
    if (!h->insert || !h->insert(h->insert_ctx, 0, o.path)) {
        error(h, cpu, ERR(0xC7), "Not a floppy image (.adf, .adl or .hfe)");
        return;
    }
    ok(cpu);
}

static void arc_hostfs_dispatch(ArcHostFS *h, HostFsRegs *cpu, int entry)
{
    switch (entry) {
    case 7: insert_disc(h, cpu); break;
    case 0: fs_open(h, cpu); break;
    case 1: fs_getbytes(h, cpu); break;
    case 2: fs_putbytes(h, cpu); break;
    case 3: fs_args(h, cpu); break;
    case 4: fs_close(h, cpu); break;
    case 5: fs_file(h, cpu); break;
    case 6: fs_func(h, cpu); break;
    default: error(h, cpu, ERR(0xA5), "Bad operation on HostFS"); break;
    }
}
