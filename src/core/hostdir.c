/*
 * hostdir.c - File RISC OS in una cartella dell'host (vedi hostdir.h)
 */
#include "hostdir.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#include <sys/utime.h>
#define strncasecmp _strnicmp
#define strcasecmp _stricmp
#define utimbuf _utimbuf
#define utime _utime
#else
#include <dirent.h>
#include <strings.h>
#include <unistd.h>
#include <utime.h>
#endif

#define EPOCH_1900 2208988800ull           /* secondi fra il 1900 e il 1970 */
#define HARD_SPACE ((char)(unsigned char)0xA0u)

int hostdir_map(const char *root, const char *rel, char *out, size_t size)
{
    char buf[512];
    size_t n = 0;
    for (const char *p = rel; (unsigned char)*p > ' ' && n < sizeof buf - 2; p++) {
        char c = *p;
        if (c == '^' || c == '\\' || c == '"' || c == '*' || c == '?' || c == '<' || c == '>' || c == '|' || c == ':')
            return 0;
        c = c == '.' ? '/' : c == '/' ? '.' : c == HARD_SPACE ? ' ' : c;
#ifndef _WIN32
        /* RISC OS scrive in Latin-1, i nomi dell'host sono UTF-8 */
        unsigned char u = (unsigned char)c;
        if (u >= 0x80) { buf[n++] = (char)(0xC0 | u >> 6); buf[n++] = (char)(0x80 | (u & 0x3F)); continue; }
#endif
        buf[n++] = c;
    }
    buf[n] = 0;
    if (strstr(buf, "..") || !root || !root[0]) return 0;
    snprintf(out, size, n ? "%s/%s" : "%s", root, buf);
    return 1;
}

static int hex_digits(const char *s, int n, uint32_t *v)
{
    uint32_t r = 0;
    for (int i = 0; i < n; i++) {
        int c = (unsigned char)s[i];
        if (!isxdigit(c)) return 0;
        r = r * 16 + (uint32_t)(isdigit(c) ? c - '0' : toupper(c) - 'A' + 10);
    }
    *v = r;
    return 1;
}

/* suffisso dopo l'ultima virgola: 1 = ",ttt", 2 = ",llllllll-eeeeeeee", 0 = niente */
static int parse_suffix(const char *name, uint32_t *a, uint32_t *b, const char **at)
{
    const char *c = strrchr(name, ',');
    if (!c) return 0;
    size_t n = strlen(c + 1);
    if (n == 3 && hex_digits(c + 1, 3, a)) { *at = c; return 1; }
    if (n == 17 && c[9] == '-' && hex_digits(c + 1, 8, a) && hex_digits(c + 10, 8, b)) { *at = c; return 2; }
    return 0;
}

int hostdir_suffix_type(const char *name)
{
    uint32_t a, b;
    const char *at;
    return parse_suffix(name, &a, &b, &at) == 1 ? (int)a : -1;
}

void hostdir_ro_name(const char *leaf, char *out, size_t size)
{
    uint32_t a, b;
    const char *at = NULL;
    if (!parse_suffix(leaf, &a, &b, &at)) at = NULL;
    size_t n = 0;
    for (const char *p = leaf; *p && p != at && n < size - 1; p++) {
        char c = *p;
#ifndef _WIN32
        /* UTF-8 -> Latin-1; quello che il Latin-1 non ha diventa '_' */
        unsigned char u = (unsigned char)c;
        if (u >= 0xC0 && (p[1] & 0xC0) == 0x80) {
            uint32_t cp = u < 0xE0 ? (uint32_t)(u & 0x1F) : u < 0xF0 ? (uint32_t)(u & 0x0F) : (uint32_t)(u & 0x07);
            while ((p[1] & 0xC0) == 0x80 && p + 1 != at) cp = cp << 6 | (uint32_t)(*++p & 0x3F);
            c = cp <= 0xFF ? (char)cp : '_';
        }
#endif
        out[n++] = c == '.' ? '/' : c == ' ' ? HARD_SPACE : c;
    }
    out[n] = 0;
}

void hostdir_list(const char *dir, HostDirCallback cb, void *ctx)
{
#ifdef _WIN32
    char pattern[700];
    snprintf(pattern, sizeof pattern, "%s/*", dir);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pattern, &fd);
    if (h == -1) return;
    do {
        if (strcmp(fd.name, ".") && strcmp(fd.name, "..") && !(fd.attrib & (_A_HIDDEN | _A_SYSTEM)) && cb(fd.name, ctx)) break;
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.' && cb(e->d_name, ctx)) break;       /* niente file nascosti */
    closedir(d);
#endif
}

static const char *leaf_of(const char *path)
{
    const char *s = strrchr(path, '/'), *b = strrchr(path, '\\');
    if (b && (!s || b > s)) s = b;
    return s ? s + 1 : path;
}

int hostdir_stat(HostObject *o)
{
    struct stat st;
    if (stat(o->path, &st) != 0) return 0;
    o->kind = (st.st_mode & S_IFDIR) ? HOSTOBJ_DIR : HOSTOBJ_FILE;
    o->length = o->kind == HOSTOBJ_DIR ? 0 : (uint32_t)st.st_size;
    o->date_cs = ((uint64_t)st.st_mtime + EPOCH_1900) * 100;
    o->untyped = 0;
    uint32_t a = 0, b = 0;
    const char *at;
    int kind = o->kind == HOSTOBJ_DIR ? 0 : parse_suffix(leaf_of(o->path), &a, &b, &at);
    if (kind == 2) {
        o->untyped = 1;
        o->load = a;
        o->exec = b;
        o->type = (a >> 20) == 0xFFF ? (a >> 8) & 0xFFF : 0;
    } else {
        o->type = o->kind == HOSTOBJ_DIR ? HOSTDIR_TYPE_DIR : kind == 1 ? a : HOSTDIR_TYPE_TEXT;
        /* immagini di dischetti senza suffisso: tipo &FCE, il doppio clic le inserisce */
        const char *dot = strrchr(leaf_of(o->path), '.');
        if (o->kind == HOSTOBJ_FILE && !kind && dot &&
            (!strcasecmp(dot, ".adf") || !strcasecmp(dot, ".adl") || !strcasecmp(dot, ".hfe")))
            o->type = 0xFCE;
    }
    return 1;
}

typedef struct FindCtx { const char *leaf; int any_case; char found[256]; } FindCtx;

/* "nome" o "nome,suffisso"; con any_case senza distinguere le maiuscole, come RISC OS */
static int find_match(const char *name, void *vctx)
{
    FindCtx *f = vctx;
    size_t n = strlen(f->leaf);
    if (f->any_case ? strncasecmp(name, f->leaf, n) : strncmp(name, f->leaf, n)) return 0;
    uint32_t a, b;
    const char *at;
    if ((name[n] == 0 && f->any_case) || (name[n] == ',' && parse_suffix(name, &a, &b, &at) && at == name + n)) {
        snprintf(f->found, sizeof f->found, "%s", name);
        return 1;
    }
    return 0;
}

int hostdir_find(const char *base, HostObject *o)
{
    memset(o, 0, sizeof *o);
    snprintf(o->path, sizeof o->path, "%s", base);
    if (hostdir_stat(o)) return 1;

    char dir[600];
    snprintf(dir, sizeof dir, "%s", base);
    char *slash = strrchr(dir, '/');
    if (!slash) return 0;
    *slash = 0;
    FindCtx f = { slash + 1, 0, "" };
    hostdir_list(dir, find_match, &f);
    if (!f.found[0]) { f.any_case = 1; hostdir_list(dir, find_match, &f); }
    if (!f.found[0]) return 0;
    snprintf(o->path, sizeof o->path, "%s/%s", dir, f.found);
    return hostdir_stat(o);
}

void hostdir_load_exec(const HostObject *o, uint32_t *load, uint32_t *exec)
{
    if (o->untyped) { *load = o->load; *exec = o->exec; return; }
    *load = 0xFFF00000u | (o->type & 0xFFF) << 8 | (uint32_t)(o->date_cs >> 32);
    *exec = (uint32_t)o->date_cs;
}

static void suffixed(const char *base, uint32_t load, uint32_t exec, char *out, size_t size)
{
    if ((load >> 20) != 0xFFF) snprintf(out, size, "%s,%08x-%08x", base, load, exec);
    else if (((load >> 8) & 0xFFF) == HOSTDIR_TYPE_TEXT) snprintf(out, size, "%s", base);
    else snprintf(out, size, "%s,%03x", base, (load >> 8) & 0xFFF);
}

void hostdir_new_file(const char *base, uint32_t type, char *out, size_t size)
{
    hostdir_new_file_le(base, 0xFFF00000u | (type & 0xFFF) << 8, 0, out, size);
}

void hostdir_new_file_le(const char *base, uint32_t load, uint32_t exec, char *out, size_t size)
{
    HostObject old;
    if (hostdir_find(base, &old) && old.kind == HOSTOBJ_FILE) {
        /* si riusa il nome che c'e' (maiuscole comprese), cambiando solo il suffisso */
        hostdir_retype_path(old.path, load, exec, out, size);
        if (strcmp(out, old.path)) remove(old.path);
        return;
    }
    suffixed(base, load, exec, out, size);
}

void hostdir_retype_path(const char *path, uint32_t load, uint32_t exec, char *out, size_t size)
{
    char base[600];
    snprintf(base, sizeof base, "%s", path);
    uint32_t a, b;
    const char *at;
    if (parse_suffix(leaf_of(base), &a, &b, &at)) base[at - base] = 0;
    suffixed(base, load, exec, out, size);
}

int hostdir_set_date(const char *path, uint64_t date_cs)
{
    if (date_cs < EPOCH_1900 * 100) return 0;
    struct utimbuf t;
    t.actime = t.modtime = (time_t)(date_cs / 100 - EPOCH_1900);
    return utime(path, &t) == 0;
}

int hostdir_mkdir(const char *path)
{
#ifdef _WIN32
    return _mkdir(path) == 0;
#else
    return mkdir(path, 0777) == 0;
#endif
}

int hostdir_rmdir(const char *path)
{
#ifdef _WIN32
    return _rmdir(path) == 0;
#else
    return rmdir(path) == 0;
#endif
}

int hostdir_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFDIR);
}
