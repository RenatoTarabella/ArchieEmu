/*
 * kernel.c - Kernel RISC OS in HLE (vedi kernel.h)
 */
#include "kernel.h"
#include "kernel_priv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#define X_BIT 0x20000u

/* ------------------------------------------------------------------ */
/* memoria emulata                                                    */
/* ------------------------------------------------------------------ */

static uint8_t rd8(RiscosKernel *k, uint32_t a)  { int ab = 0; return bus_read8(k->bus, a, &ab); }
static uint32_t rd32(RiscosKernel *k, uint32_t a) { int ab = 0; return bus_read32(k->bus, a, &ab); }
static void wr8(RiscosKernel *k, uint32_t a, uint8_t v)   { int ab = 0; bus_write8(k->bus, a, v, &ab); }
static void wr32(RiscosKernel *k, uint32_t a, uint32_t v) { int ab = 0; bus_write32(k->bus, a, v, &ab); }

/* legge una stringa terminata da un carattere di controllo (<32) */
static int read_str(RiscosKernel *k, uint32_t a, char *out, int max)
{
    int n = 0;
    for (; n < max - 1; n++) {
        uint8_t c = rd8(k, a + (uint32_t)n);
        if (c < 32) break;
        out[n] = (char)c;
    }
    out[n] = 0;
    return n;
}

static uint32_t write_str(RiscosKernel *k, uint32_t a, const char *s)
{
    while (*s) wr8(k, a++, (uint8_t)*s++);
    wr8(k, a, 0);
    return a;
}

/* ------------------------------------------------------------------ */
/* registri e flag                                                    */
/* ------------------------------------------------------------------ */

#define R(n) (k->cpu->r[n])

static void set_flag(RiscosKernel *k, uint32_t flag, int on)
{
    if (on) k->cpu->r[15] |= flag; else k->cpu->r[15] &= ~flag;
}

/* ------------------------------------------------------------------ */
/* errori                                                             */
/* ------------------------------------------------------------------ */

/* Consegna l'errore in 'block' (indirizzo emulato): con la forma X torna
   con V alzato, altrimenti salta al gestore di errori dell'applicazione. */
static void raise_error(RiscosKernel *k, uint32_t swi, uint32_t block)
{
    if (swi & X_BIT) {
        R(0) = block;
        set_flag(k, ARM_V, 1);
        return;
    }
    uint32_t handler = k->handlers[6][0], ws = k->handlers[6][1], buf = k->handlers[6][2];
    if (!handler || !buf) {
        char msg[256];
        read_str(k, block + 4, msg, sizeof msg);
        fflush(stdout);
        fprintf(stderr, "\nErrore non gestito &%X: %s\n", rd32(k, block), msg);
        k->exit_code = 1;
        k->cpu->halted = 1;
        return;
    }
    wr32(k, buf, arm2_pc(k->cpu));
    wr32(k, buf + 4, rd32(k, block));
    uint32_t i = 0;
    uint8_t c;
    do {
        c = i < 251 ? rd8(k, block + 4 + i) : 0;
        wr8(k, buf + 8 + i, c);
        i++;
    } while (c);
    /* modo utente, interrupt abilitati, R0 = workspace del gestore */
    arm2_set_r15(k->cpu, (k->cpu->r[15] & 0xF0000000u) | (handler & ARM_PC_MASK));
    R(0) = ws;
}

static void error_msg(RiscosKernel *k, uint32_t swi, uint32_t num, const char *msg)
{
    wr32(k, K_ERRBLOCK, num);
    write_str(k, K_ERRBLOCK + 4, msg);
    raise_error(k, swi, K_ERRBLOCK);
}

/* ------------------------------------------------------------------ */
/* chiamate host -> ARM                                               */
/* ------------------------------------------------------------------ */

int kernel_call(RiscosKernel *k, uint32_t addr, int mode, uint32_t *regs)
{
    Arm2 *c = k->cpu;
    Arm2 saved = *c;

    arm2_set_r15(c, (c->r[15] & 0xF0000000u) | ARM_I | (uint32_t)mode);
    if (regs) for (int i = 0; i < 13; i++) c->r[i] = regs[i];
    if (mode == ARM_MODE_SVC) c->r[13] = K_SVC_STACK;
    c->r[14] = K_TRAMPOLINE | (c->r[15] & ARM_PSR_MASK);
    arm2_set_pc(c, addr);

    k->call_done = 0;
    for (long n = 0; n < 50000000L && !k->call_done && !c->halted; n++) arm2_step(c);
    int ok = k->call_done;

    uint32_t out[13];
    memcpy(out, c->r, sizeof out);
    uint64_t cycles = c->cycles, instr = c->instructions;
    int halted = c->halted;
    *c = saved;
    c->cycles = cycles;
    c->instructions = instr;
    c->halted = halted;
    if (regs) memcpy(regs, out, sizeof out);
    return ok;
}

/* ------------------------------------------------------------------ */
/* tempo                                                              */
/* ------------------------------------------------------------------ */

uint64_t kernel_time_cs(RiscosKernel *k)
{
    return k->now_cs ? k->now_cs(k->now_ctx) : (uint64_t)clock() * 100 / CLOCKS_PER_SEC;
}

static uint64_t monotonic(RiscosKernel *k) { return kernel_time_cs(k) - k->start_cs; }

/* ------------------------------------------------------------------ */
/* tastiera ed Escape                                                 */
/* ------------------------------------------------------------------ */

static void call_escape_handler(RiscosKernel *k, int set)
{
    if (!k->handlers[9][0]) return;
    uint32_t regs[13] = { 0 };
    regs[11] = set ? 0x40 : 0;
    regs[12] = k->handlers[9][1];
    kernel_call(k, k->handlers[9][0], ARM_MODE_SVC, regs);
}

void kernel_key(RiscosKernel *k, uint8_t code)
{
    if (code == 27 && k->escape_enabled) {
        k->key_head = k->key_tail = 0;         /* Escape svuota il buffer */
        if (!k->escape_pending) {
            k->escape_pending = 1;
            call_escape_handler(k, 1);
        }
        return;
    }
    int next = (k->key_tail + 1) % K_KEYBUF;
    if (next == k->key_head) return;           /* buffer pieno */
    k->keys[k->key_tail] = code;
    k->key_tail = next;
}

int kernel_keys_pending(const RiscosKernel *k)
{
    return (k->key_tail - k->key_head + K_KEYBUF) % K_KEYBUF;
}

static int key_get(RiscosKernel *k)
{
    if (k->key_head == k->key_tail) return -1;
    int c = k->keys[k->key_head];
    k->key_head = (k->key_head + 1) % K_KEYBUF;
    return c;
}

/* la SWI corrente verra' ripetuta al prossimo frame */
static void wait_and_retry(RiscosKernel *k)
{
    arm2_set_pc(k->cpu, arm2_pc(k->cpu) - 4);
    k->waiting = 1;
}

static void clear_escape(RiscosKernel *k)
{
    if (k->escape_pending) {
        k->escape_pending = 0;
        call_escape_handler(k, 0);
    }
}

/* ------------------------------------------------------------------ */
/* ResourceFS e file                                                  */
/* ------------------------------------------------------------------ */

/* cerca "Resources.BASIC.BASICMsgs" nei blocchi registrati */
static int resource_find(RiscosKernel *k, const char *path, uint32_t *data, uint32_t *size)
{
    const char *p = path;
    if (!strncmp(p, "Resources:", 10)) p += 10;
    if (!strncmp(p, "$.", 2)) p += 2;
    for (int b = 0; b < k->nresblocks; b++) {
        uint32_t e = k->resblocks[b];
        for (int guard = 0; guard < 4096; guard++) {
            uint32_t next = rd32(k, e);
            if (!next) break;
            char name[128];
            int n = 0;
            for (; n < 127; n++) {
                uint8_t c = rd8(k, e + 20 + (uint32_t)n);
                if (!c) break;
                name[n] = (char)c;
            }
            name[n] = 0;
#ifdef _MSC_VER
            if (!_stricmp(name, p)) {
#else
            if (!strcasecmp(name, p)) {
#endif
                uint32_t d = (e + 20 + (uint32_t)n + 4) & ~3u;
                *size = rd32(k, e + 12);
                *data = d + 4;
                return 1;
            }
            e += next;
        }
    }
    return 0;
}

static void swi_find(RiscosKernel *k, uint32_t swi)
{
    uint32_t reason = R(0) & 0xFF;
    if ((reason & 0xC0) == 0) {                     /* chiusura */
        uint32_t h = R(1);
        if (h == 0) { for (int i = 1; i < K_MAX_FILES; i++) hostfs_close(k, i); return; }
        hostfs_close(k, (int)h);
        return;
    }
    char name[256];
    read_str(k, R(1), name, sizeof name);
    uint32_t data, size;
    if ((reason & 0xC0) == 0x40 && resource_find(k, name, &data, &size)) {
        for (int h = 1; h < K_MAX_FILES; h++) {
            if (!k->files[h].used) {
                memset(&k->files[h], 0, sizeof k->files[h]);
                k->files[h].used = 1;
                k->files[h].data = data;
                k->files[h].size = size;
                R(0) = (uint32_t)h;
                return;
            }
        }
        error_msg(k, swi, 0xC0, "Too many open files");
        return;
    }
    int handle = 0;
    if (hostfs_open(k, swi, reason, name, &handle)) {
        if (handle > 0) R(0) = (uint32_t)handle;
        return;
    }
    if (reason & 0x08) {
        char msg[300];
        snprintf(msg, sizeof msg, "File '%s' not found", name);
        error_msg(k, swi, 0x108D6, msg);
        return;
    }
    R(0) = 0;
}

static void swi_fscontrol(RiscosKernel *k, uint32_t swi)
{
    if (R(0) == 21) {                               /* handle -> handle interno */
        uint32_t h = R(1);
        if (h < K_MAX_FILES && k->files[h].used) {
            R(1) = k->files[h].data;                /* per ResourceFS: l'indirizzo */
            R(2) = 46;                              /* numero del filing system */
            return;
        }
        error_msg(k, swi, 0xDE, "Channel");
        return;
    }
    error_msg(k, swi, 0xB0, "Filing system operation not supported");
}

/* ------------------------------------------------------------------ */
/* MessageTrans                                                       */
/* ------------------------------------------------------------------ */

#define MT_MAGIC 0x4D534754u      /* "MSGT" */

/* messaggi globali minimi (descrittore 0) */
static const char *global_messages =
    "NoStore:No room\n"
    "BadCommand:Bad command\n"
    "Escape:Escape\n";

/* Cerca 'token' nel testo dei messaggi. Ritorna l'offset del messaggio e la
   sua lunghezza (fino a LF), o -1. */
static long find_token(const char *text, size_t len, const char *token)
{
    size_t i = 0;
    while (i < len) {
        size_t line = i;
        size_t eol = i;
        while (eol < len && text[eol] != '\n') eol++;
        if (text[line] != '#') {
            /* elenco di token separati da '/', poi ':' */
            size_t p = line;
            while (p < eol) {
                const char *t = token;
                size_t q = p;
                while (q < eol && text[q] != ':' && text[q] != '/' && *t &&
                       (text[q] == '?' || text[q] == *t)) { q++; t++; }
                if (!*t && q < eol && (text[q] == ':' || text[q] == '/')) {
                    while (q < eol && text[q] != ':') q++;
                    return q < eol ? (long)(q + 1) : -1;
                }
                while (p < eol && text[p] != ':' && text[p] != '/') p++;
                if (p < eol && text[p] == ':') break;
                p++;
            }
        }
        i = eol + 1;
    }
    return -1;
}

/* Legge il file messaggi in un buffer host */
static char *load_messages(RiscosKernel *k, uint32_t desc, size_t *len)
{
    if (desc == 0) {
        *len = strlen(global_messages);
        char *t = malloc(*len + 1);
        if (t) memcpy(t, global_messages, *len + 1);
        return t;
    }
    if (rd32(k, desc) != MT_MAGIC) return NULL;
    uint32_t data = rd32(k, desc + 4), size = rd32(k, desc + 8);
    char *t = malloc(size + 1);
    if (!t) return NULL;
    for (uint32_t i = 0; i < size; i++) t[i] = (char)rd8(k, data + i);
    t[size] = 0;
    *len = size;
    return t;
}

/* Traduzione di un token con i parametri %0-%3 (stringhe in R4-R7).
   Ritorna 1 se trovato; *msg_addr = indirizzo nel file (se desc != 0). */
static int lookup_message(RiscosKernel *k, uint32_t desc, const char *token_in,
                          const uint32_t params[4], char *out, size_t outsize, uint32_t *msg_addr)
{
    char token[64];
    strncpy(token, token_in, sizeof token - 1);
    token[sizeof token - 1] = 0;
    char *deflt = strchr(token, ':');
    if (deflt) *deflt++ = 0;

    size_t len;
    char *text = load_messages(k, desc, &len);
    long off = text ? find_token(text, len, token) : -1;
    const char *src;
    size_t srclen;
    if (off >= 0) {
        src = text + off;
        srclen = 0;
        while ((size_t)off + srclen < len && src[srclen] != '\n' && src[srclen] != 0) srclen++;
        if (msg_addr) *msg_addr = desc ? rd32(k, desc + 4) + (uint32_t)off : 0;
    } else if (deflt) {
        src = deflt;
        srclen = strlen(deflt);
        if (msg_addr) *msg_addr = 0;
    } else {
        free(text);
        return 0;
    }

    size_t o = 0;
    for (size_t i = 0; i < srclen && o + 1 < outsize; i++) {
        if (src[i] == '%' && i + 1 < srclen && src[i + 1] >= '0' && src[i + 1] <= '3') {
            uint32_t pa = params[src[i + 1] - '0'];
            i++;
            if (pa) {
                char tmp[256];
                read_str(k, pa, tmp, sizeof tmp);
                for (char *s = tmp; *s && o + 1 < outsize; s++) out[o++] = *s;
            }
        } else if (src[i] == '%' && i + 1 < srclen && src[i + 1] == '%') {
            out[o++] = '%';
            i++;
        } else {
            out[o++] = src[i];
        }
    }
    out[o] = 0;
    free(text);
    return 1;
}

static void swi_messagetrans(RiscosKernel *k, uint32_t swi, uint32_t n)
{
    char name[256], token[64], text[1024];
    switch (n) {
    case 0: {                                           /* FileInfo */
        uint32_t data, size;
        read_str(k, R(1), name, sizeof name);
        if (!resource_find(k, name, &data, &size)) { error_msg(k, swi, 0xAC2, "Message file not found"); return; }
        R(0) = 1;                                       /* in memoria: niente buffer */
        R(2) = size;
        return;
    }
    case 1: {                                           /* OpenFile */
        uint32_t data, size;
        read_str(k, R(1), name, sizeof name);
        if (!resource_find(k, name, &data, &size)) {
            char msg[300];
            snprintf(msg, sizeof msg, "File '%s' not found", name);
            error_msg(k, swi, 0x108D6, msg);
            return;
        }
        wr32(k, R(0), MT_MAGIC);
        wr32(k, R(0) + 4, data);
        wr32(k, R(0) + 8, size);
        wr32(k, R(0) + 12, 0);
        return;
    }
    case 2: {                                           /* Lookup */
        uint32_t params[4] = { R(4), R(5), R(6), R(7) };
        uint32_t addr = 0;
        read_str(k, R(1), token, sizeof token);
        if (!lookup_message(k, R(0), token, params, text, sizeof text, &addr)) {
            char msg[128];
            snprintf(msg, sizeof msg, "Message token %s not found", token);
            error_msg(k, swi, 0xAC2, msg);
            return;
        }
        size_t len = strlen(text);
        if (R(2) == 0) {
            /* puntatore diretto al messaggio (senza sostituzioni) */
            if (!addr) { write_str(k, K_MSGBUF, text); addr = K_MSGBUF; }
            R(2) = addr;
            R(3) = (uint32_t)len;
            return;
        }
        uint32_t max = R(3) ? R(3) - 1 : 0;
        if (len > max) len = max;
        for (uint32_t i = 0; i < len; i++) wr8(k, R(2) + i, (uint8_t)text[i]);
        wr8(k, R(2) + (uint32_t)len, 0);
        R(3) = (uint32_t)len;
        return;
    }
    case 4:                                             /* CloseFile */
        return;
    case 6:                                             /* ErrorLookup */
    case 8: {                                           /* CopyError */
        uint32_t params[4] = { R(4), R(5), R(6), R(7) };
        uint32_t num = rd32(k, R(0));
        read_str(k, R(0) + 4, token, sizeof token);
        if (n == 8 || !lookup_message(k, R(1), token, params, text, sizeof text, NULL))
            strcpy(text, token);
        uint32_t buf = (n == 6 && R(2)) ? R(2) : K_MSGBUF;
        wr32(k, buf, num);
        write_str(k, buf + 4, text);
        raise_error(k, swi | (n == 8 ? X_BIT : 0), buf);
        if (n == 8) set_flag(k, ARM_V, 0);
        return;
    }
    default:
        error_msg(k, swi, 0x1E6, "MessageTrans SWI not supported");
        return;
    }
}

/* ------------------------------------------------------------------ */
/* conversioni numeriche                                              */
/* ------------------------------------------------------------------ */

static void convert_out(RiscosKernel *k, uint32_t swi, const char *s)
{
    uint32_t len = (uint32_t)strlen(s);
    if (len + 1 > R(2)) { error_msg(k, swi, 0x1E4, "Buffer overflow"); return; }
    uint32_t end = write_str(k, R(1), s);
    R(0) = R(1);
    R(2) -= len;
    R(1) = end;
}

static void swi_convert(RiscosKernel *k, uint32_t swi, uint32_t n)
{
    char s[40];
    uint32_t v = R(0);
    if (n >= 0xD0 && n <= 0xD4) {                    /* Hex1/2/4/6/8 */
        static const int digits[] = { 1, 2, 4, 6, 8 };
        int d = digits[n - 0xD0];
        uint32_t mask = d == 8 ? 0xFFFFFFFFu : (1u << (4 * d)) - 1;
        snprintf(s, sizeof s, "%0*X", d, v & mask);
    } else if (n >= 0xD5 && n <= 0xD8) {             /* Cardinal1-4 */
        int bits = 8 * (int)(n - 0xD4);
        snprintf(s, sizeof s, "%u", bits == 32 ? v : v & ((1u << bits) - 1));
    } else if (n >= 0xD9 && n <= 0xDC) {             /* Integer1-4 */
        int bits = 8 * (int)(n - 0xD8);
        int32_t x = bits == 32 ? (int32_t)v : (int32_t)(v << (32 - bits)) >> (32 - bits);
        snprintf(s, sizeof s, "%d", x);
    } else if (n >= 0xDD && n <= 0xE0) {             /* Binary1-4 */
        int bits = 8 * (int)(n - 0xDC);
        for (int i = 0; i < bits; i++) s[i] = (v >> (bits - 1 - i)) & 1 ? '1' : '0';
        s[bits] = 0;
    } else {
        snprintf(s, sizeof s, "%u", v);
    }
    convert_out(k, swi, s);
}

static void swi_read_unsigned(RiscosKernel *k, uint32_t swi)
{
    uint32_t flags = R(0), p = R(1);
    uint32_t base = flags & 0x7F;
    if (base < 2 || base > 36) base = 10;
    uint8_t c = rd8(k, p);
    if (c == '&') { base = 16; p++; }
    else {
        /* forma "base_numero" */
        uint32_t q = p, b = 0;
        while ((c = rd8(k, q)) >= '0' && c <= '9') { b = b * 10 + (c - '0'); q++; }
        if (c == '_' && q > p && b >= 2 && b <= 36) { base = b; p = q + 1; }
    }
    uint64_t val = 0;
    int any = 0;
    for (;; p++) {
        c = rd8(k, p);
        uint32_t d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (isalpha(c)) d = (uint32_t)(toupper(c) - 'A' + 10);
        else break;
        if (d >= base) break;
        val = val * base + d;
        any = 1;
        if (val > 0xFFFFFFFFu) { error_msg(k, swi, 0x16B, "Number too big"); return; }
    }
    if (!any) { error_msg(k, swi, 0x16A, "Bad number"); return; }
    if ((flags & 0x20000000u) && val > 255) { error_msg(k, swi, 0x16B, "Number too big"); return; }
    if ((flags & 0x10000000u) && val > R(2)) { error_msg(k, swi, 0x16B, "Number too big"); return; }
    R(1) = p;
    R(2) = (uint32_t)val;
}

/* ------------------------------------------------------------------ */
/* OS_ReadLine, OS_Byte, OS_Word                                      */
/* ------------------------------------------------------------------ */

static void vdu(RiscosKernel *k, uint8_t c) { vdu_write(k->vdu, c); }

static void swi_readline(RiscosKernel *k)
{
    uint32_t buf = R(0) & 0x3FFFFFFFu, max = R(1);
    uint8_t lo = (uint8_t)R(2), hi = (uint8_t)R(3);
    if (!k->readline_active) { k->readline_active = 1; k->readline_len = 0; }
    for (;;) {
        if (k->escape_pending) {
            k->readline_active = 0;
            R(1) = k->readline_len;
            set_flag(k, ARM_C, 1);
            return;
        }
        int c = key_get(k);
        if (c < 0) { wait_and_retry(k); return; }
        if (c == 13 || c == 10) {
            wr8(k, buf + k->readline_len, 13);
            vdu(k, 13); vdu(k, 10);
            k->readline_active = 0;
            R(1) = k->readline_len;
            set_flag(k, ARM_C, 0);
            return;
        }
        if (c == 127 || c == 8) {
            if (k->readline_len) { k->readline_len--; vdu(k, 127); }
        } else if (c == 21) {
            while (k->readline_len) { k->readline_len--; vdu(k, 127); }
        } else if (c >= lo && c <= hi && c >= 32) {
            if (k->readline_len < max) {
                wr8(k, buf + k->readline_len++, (uint8_t)c);
                vdu(k, (uint8_t)c);
            } else {
                vdu(k, 7);
            }
        }
    }
}

static void swi_readc(RiscosKernel *k)
{
    if (k->escape_pending) { R(0) = 27; set_flag(k, ARM_C, 1); return; }
    int c = key_get(k);
    if (c < 0) { wait_and_retry(k); return; }
    R(0) = (uint32_t)c;
    set_flag(k, ARM_C, 0);
}

static void swi_byte(RiscosKernel *k)
{
    uint32_t a = R(0) & 0xFF, x = R(1), y = R(2);
    switch (a) {
    case 0:                                           /* versione del sistema */
        R(1) = 6;
        break;
    case 19:                                          /* attesa del vsync */
        k->waiting = 1;
        break;
    case 124: clear_escape(k); break;
    case 125:
        if (!k->escape_pending) { k->escape_pending = 1; call_escape_handler(k, 1); }
        break;
    case 126:                                         /* conferma Escape */
        R(1) = k->escape_pending ? 0xFF : 0;
        k->key_head = k->key_tail;
        clear_escape(k);
        break;
    case 127:                                         /* EOF# */
        R(1) = hostfs_eof(k, (int)x) ? 0xFF : 0;
        break;
    case 128:                                         /* ADVAL */
        if ((x & 0xFF) == 0xFF) { uint32_t n = (uint32_t)kernel_keys_pending(k); R(1) = n & 0xFF; R(2) = n >> 8; }
        else { R(1) = 0; R(2) = 0; }
        break;
    case 129: {                                       /* INKEY */
        if ((y & 0xFF) == 0xFF && (x & 0xFF) == 0) {  /* INKEY(-256): versione */
            R(1) = 0xA1;
            R(2) = 0;
            break;
        }
        if ((y & 0xFF) == 0xFF) {                     /* scansione di un tasto */
            R(1) = 0; R(2) = 0;
            break;
        }
        if (k->escape_pending) { k->inkey_active = 0; R(2) = 27; set_flag(k, ARM_C, 1); break; }
        int c = key_get(k);
        if (c >= 0) { k->inkey_active = 0; R(1) = (uint32_t)c; R(2) = 0; set_flag(k, ARM_C, 0); break; }
        uint32_t cs = (x & 0xFF) | (y & 0x7F) << 8;
        if (!k->inkey_active) { k->inkey_active = 1; k->inkey_deadline = monotonic(k) + cs; }
        if (cs == 0 || monotonic(k) >= k->inkey_deadline) {
            k->inkey_active = 0;
            R(2) = 0xFF;
            set_flag(k, ARM_C, 1);
            break;
        }
        wait_and_retry(k);
        break;
    }
    case 134:                                         /* POS e VPOS */
        R(1) = (uint32_t)k->vdu->tx;
        R(2) = (uint32_t)k->vdu->ty;
        break;
    case 135:                                         /* carattere al cursore e modo */
        R(1) = (uint32_t)vdu_char_at_cursor(k->vdu);
        R(2) = (uint32_t)(k->vdu->mode < 0 ? 0 : k->vdu->mode);
        break;
    case 218:                                         /* byte nella coda VDU */
        R(1) = (uint32_t)(k->vdu->queue_need ? k->vdu->queue_need - k->vdu->queue_have : 0);
        if ((x & 0xFF) == 0 && (y & 0xFF) == 0) k->vdu->queue_need = 0;
        break;
    case 229:                                         /* abilita/disabilita Escape */
        R(1) = k->escape_enabled ? 0 : 1;
        k->escape_enabled = ((x & y) ^ (uint32_t)(!k->escape_enabled)) == 0;
        break;
    default:
        break;
    }
}

static void swi_word(RiscosKernel *k)
{
    uint32_t b = R(1);
    switch (R(0) & 0xFF) {
    case 1: {                                         /* legge TIME */
        uint64_t t = (uint64_t)((int64_t)monotonic(k) + k->time_offset);
        for (int i = 0; i < 5; i++) wr8(k, b + (uint32_t)i, (uint8_t)(t >> (8 * i)));
        break;
    }
    case 2: {                                         /* scrive TIME */
        uint64_t t = 0;
        for (int i = 0; i < 5; i++) t |= (uint64_t)rd8(k, b + (uint32_t)i) << (8 * i);
        k->time_offset = (int64_t)t - (int64_t)monotonic(k);
        break;
    }
    case 10: {                                        /* definizione di un carattere */
        uint8_t ch = rd8(k, b);
        for (int i = 0; i < 8; i++) wr8(k, b + 1 + (uint32_t)i, k->vdu->font[ch][i]);
        break;
    }
    case 14: {                                        /* orologio */
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        uint8_t reason = rd8(k, b);
        if (reason == 0 && tm) {
            char s[32];
            strftime(s, sizeof s, "%a,%d %b %Y.%H:%M:%S", tm);
            uint32_t end = write_str(k, b, s);
            wr8(k, end, 13);
        } else if (reason == 1 && tm) {
            int v[7] = { tm->tm_year % 100, tm->tm_mon + 1, tm->tm_mday, tm->tm_wday + 1,
                         tm->tm_hour, tm->tm_min, tm->tm_sec };
            for (int i = 0; i < 7; i++) wr8(k, b + (uint32_t)i, (uint8_t)((v[i] / 10) << 4 | v[i] % 10));
        } else if (reason == 3) {
            /* centesimi dal 1900 */
            uint64_t cs = ((uint64_t)now + 2208988800ull) * 100;
            for (int i = 0; i < 5; i++) wr8(k, b + (uint32_t)i, (uint8_t)(cs >> (8 * i)));
        }
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* OS_CLI                                                             */
/* ------------------------------------------------------------------ */

static void swi_cli(RiscosKernel *k, uint32_t swi)
{
    char line[256];
    read_str(k, R(0), line, sizeof line);
    char *p = line;
    while (*p == ' ' || *p == '*') p++;
    if (!*p || *p == '|') return;
    char cmd[32];
    int n = 0;
    if (*p == '.') cmd[n++] = *p++;                  /* "*." = *CAT */
    else while (*p && *p != ' ' && *p != '.' && n < 31) cmd[n++] = (char)toupper((unsigned char)*p++);
    cmd[n] = 0;
    if (*p == '.' && n > 1) p++;                     /* abbreviazioni come "*CAT." */
    while (*p == ' ') p++;

    if (!strcmp(cmd, "FX")) {
        uint32_t a = 0, x = 0, y = 0;
        sscanf(p, "%u%*[ ,]%u%*[ ,]%u", &a, &x, &y);
        uint32_t save[3] = { R(0), R(1), R(2) };
        R(0) = a; R(1) = x; R(2) = y;
        swi_byte(k);
        R(0) = save[0]; R(1) = save[1]; R(2) = save[2];
        return;
    }
    if (!strcmp(cmd, "ECHO")) {
        for (; *p; p++) vdu(k, (uint8_t)*p);
        vdu(k, 13); vdu(k, 10);
        return;
    }
    if (!strcmp(cmd, "QUIT")) { k->cpu->halted = 1; return; }
    if (!strcmp(cmd, "BASIC")) return;
    if (hostfs_command(k, swi, cmd, p)) return;
    char msg[300];
    snprintf(msg, sizeof msg, "File '%s' not found", cmd);
    error_msg(k, swi, 0x214, msg);
}

/* ------------------------------------------------------------------ */
/* OS_ScreenMode e modi                                               */
/* ------------------------------------------------------------------ */

static int select_mode(RiscosKernel *k, uint32_t spec)
{
    if (spec < 256) return vdu_set_mode(k->vdu, (int)spec);
    if ((rd32(k, spec) & 1) == 0) return 0;          /* selettore di modo */
    uint32_t w = rd32(k, spec + 4), h = rd32(k, spec + 8), l2 = rd32(k, spec + 12);
    return vdu_set_mode_spec(k->vdu, (int)w, (int)h, (int)l2);
}

/* "X640 Y480 C16M" -> modo */
static int select_mode_string(RiscosKernel *k, const char *s)
{
    int w = 0, h = 0, l2 = 2;
    while (*s) {
        while (*s == ' ' || *s == ',') s++;
        char c = (char)toupper((unsigned char)*s);
        if (!c) break;
        s++;
        long v = strtol(s, (char **)&s, 10);
        if (c == 'X') w = (int)v;
        else if (c == 'Y') h = (int)v;
        else if (c == 'C' || c == 'G') {
            char u = (char)toupper((unsigned char)*s);
            if (u == 'K') { s++; v *= 1024; }
            else if (u == 'M') { s++; v *= 1024 * 1024; }
            l2 = v <= 2 ? 0 : v <= 4 ? 1 : v <= 16 ? 2 : v <= 256 ? 3 : v <= 65536 ? 4 : 5;
        } else if (c == 'E' || c == 'F') {
            while (*s && *s != ' ' && *s != ',') s++;
        } else {
            return 0;
        }
    }
    return w && h && vdu_set_mode_spec(k->vdu, w, h, l2);
}

static void swi_screenmode(RiscosKernel *k, uint32_t swi)
{
    switch (R(0)) {
    case 0:
        if (!select_mode(k, R(1))) error_msg(k, swi, 0x19C, "Screen mode not available");
        return;
    case 1:
        if (k->vdu->mode >= 0) { R(1) = (uint32_t)k->vdu->mode; return; }
        wr32(k, K_SCRATCH, 1);
        wr32(k, K_SCRATCH + 4, (uint32_t)k->vdu->width);
        wr32(k, K_SCRATCH + 8, (uint32_t)k->vdu->height);
        wr32(k, K_SCRATCH + 12, (uint32_t)k->vdu->log2bpp);
        wr32(k, K_SCRATCH + 16, 60);
        wr32(k, K_SCRATCH + 20, 0xFFFFFFFFu);
        R(1) = K_SCRATCH;
        return;
    case 15: {
        char s[128];
        read_str(k, R(1), s, sizeof s);
        if (!select_mode_string(k, s)) error_msg(k, swi, 0x19C, "Screen mode not available");
        return;
    }
    default:
        error_msg(k, swi, 0x19C, "Screen mode not available");
        return;
    }
}

/* ------------------------------------------------------------------ */
/* nomi delle SWI (SYS "nome")                                        */
/* ------------------------------------------------------------------ */

typedef struct SwiName { uint32_t num; const char *name; } SwiName;
static const SwiName swi_names[] = {
    { 0x00, "OS_WriteC" }, { 0x01, "OS_WriteS" }, { 0x02, "OS_Write0" }, { 0x03, "OS_NewLine" },
    { 0x04, "OS_ReadC" }, { 0x05, "OS_CLI" }, { 0x06, "OS_Byte" }, { 0x07, "OS_Word" },
    { 0x08, "OS_File" }, { 0x09, "OS_Args" }, { 0x0A, "OS_BGet" }, { 0x0B, "OS_BPut" },
    { 0x0C, "OS_GBPB" }, { 0x0D, "OS_Find" }, { 0x0E, "OS_ReadLine" }, { 0x10, "OS_GetEnv" }, { 0x11, "OS_Exit" },
    { 0x16, "OS_EnterOS" }, { 0x1C, "OS_Mouse" }, { 0x21, "OS_ReadUnsigned" },
    { 0x23, "OS_ReadVarVal" }, { 0x29, "OS_FSControl" }, { 0x2B, "OS_GenerateError" },
    { 0x2C, "OS_ReadEscapeState" }, { 0x2F, "OS_ReadPalette" }, { 0x31, "OS_ReadVduVariables" },
    { 0x32, "OS_ReadPoint" }, { 0x35, "OS_ReadModeVariable" }, { 0x40, "OS_ChangeEnvironment" },
    { 0x42, "OS_ReadMonotonicTime" }, { 0x44, "OS_PrettyPrint" }, { 0x45, "OS_Plot" },
    { 0x46, "OS_WriteN" }, { 0x61, "OS_SetColour" }, { 0x65, "OS_ScreenMode" },
    { 0x6E, "OS_SynchroniseCodeAreas" },
    { 0xD0, "OS_ConvertHex1" }, { 0xD1, "OS_ConvertHex2" }, { 0xD2, "OS_ConvertHex4" },
    { 0xD3, "OS_ConvertHex6" }, { 0xD4, "OS_ConvertHex8" }, { 0xD5, "OS_ConvertCardinal1" },
    { 0xD6, "OS_ConvertCardinal2" }, { 0xD7, "OS_ConvertCardinal3" }, { 0xD8, "OS_ConvertCardinal4" },
    { 0xD9, "OS_ConvertInteger1" }, { 0xDA, "OS_ConvertInteger2" }, { 0xDB, "OS_ConvertInteger3" },
    { 0xDC, "OS_ConvertInteger4" },
    { 0x40743, "ColourTrans_SetGCOL" }, { 0x40761, "ColourTrans_SetTextColour" },
    { 0x41500, "MessageTrans_FileInfo" }, { 0x41501, "MessageTrans_OpenFile" },
    { 0x41502, "MessageTrans_Lookup" }, { 0x41504, "MessageTrans_CloseFile" },
    { 0x41506, "MessageTrans_ErrorLookup" },
    { 0x41B40, "ResourceFS_RegisterFiles" }, { 0x41B41, "ResourceFS_DeregisterFiles" },
};

static const char *swi_name(uint32_t n)
{
    n &= ~X_BIT;
    if (n >= 0x100 && n < 0x200) return "OS_WriteI";
    for (size_t i = 0; i < sizeof swi_names / sizeof swi_names[0]; i++)
        if (swi_names[i].num == n) return swi_names[i].name;
    return NULL;
}

static void swi_number_from_string(RiscosKernel *k, uint32_t swi)
{
    char s[64];
    read_str(k, R(1), s, sizeof s);
    const char *name = s;
    uint32_t x = 0;
    if (s[0] == 'X') { x = X_BIT; name = s + 1; }
    for (size_t i = 0; i < sizeof swi_names / sizeof swi_names[0]; i++)
        if (!strcmp(swi_names[i].name, name)) { R(0) = swi_names[i].num | x; return; }
    if (!strncmp(name, "OS_WriteI", 9)) { R(0) = 0x100 | x; return; }
    error_msg(k, swi, 0x1E6, "SWI name not known");
}

static void swi_number_to_string(RiscosKernel *k)
{
    const char *n = swi_name(R(0));
    char s[64];
    if (n) snprintf(s, sizeof s, "%s%s", (R(0) & X_BIT) ? "X" : "", n);
    else   snprintf(s, sizeof s, "&%X", R(0));
    uint32_t len = (uint32_t)strlen(s);
    if (R(2) > len) write_str(k, R(1), s);
    R(2) = len + 1;
}

/* ------------------------------------------------------------------ */
/* OS_PrettyPrint                                                     */
/* ------------------------------------------------------------------ */

static void pretty_emit(RiscosKernel *k, uint32_t s, uint32_t dict, uint32_t special, int depth)
{
    for (;; s++) {
        uint8_t c = rd8(k, s);
        if (c == 0) return;
        if (c == 27) {
            uint8_t n = rd8(k, ++s);
            if (depth > 4) continue;
            if (n == 0) { if (special) pretty_emit(k, special, dict, 0, depth + 1); continue; }
            if (!dict) continue;
            uint32_t e = dict;
            for (int i = 1; i < n; i++) {
                uint8_t len = rd8(k, e);
                if (!len) { e = 0; break; }
                e += len;
            }
            if (e && rd8(k, e)) pretty_emit(k, e + 1, dict, special, depth + 1);
            continue;
        }
        if (c == 13) { vdu(k, 13); vdu(k, 10); continue; }
        if (c == 31) { vdu(k, ' '); continue; }
        if (c == 9) { do vdu(k, ' '); while (k->vdu->tx % 8); continue; }
        if (c == ' ') {
            /* a capo se la parola seguente non entra nella riga */
            int len = 0;
            while (len < 80) {
                uint8_t d = rd8(k, s + 1 + (uint32_t)len);
                if (d <= ' ' || d == 27) break;
                len++;
            }
            int width = k->vdu->twr - k->vdu->twl + 1;
            if (k->vdu->tx + 1 + len > width) { vdu(k, 13); vdu(k, 10); continue; }
        }
        vdu(k, c);
    }
}

/* ------------------------------------------------------------------ */
/* colori                                                             */
/* ------------------------------------------------------------------ */

static uint32_t bbggrr_to_rgb(uint32_t pal)
{
    return ((pal >> 8) & 255) << 16 | ((pal >> 16) & 255) << 8 | ((pal >> 24) & 255);
}

/* ------------------------------------------------------------------ */
/* smistamento                                                        */
/* ------------------------------------------------------------------ */

static int kernel_swi_body(RiscosKernel *k, Arm2 *cpu, uint32_t comment, uint32_t swi, uint32_t n);

int kernel_swi(Arm2 *cpu, uint32_t comment, void *user)
{
    RiscosKernel *k = (RiscosKernel *)user;
    uint32_t swi = comment;
    uint32_t n = comment & ~X_BIT;

    if (comment == K_SWI_RETURN) { k->call_done = 1; return 1; }
    if ((comment & 0xFFFF00u) == K_SWI_EXCEPTION) {
        static const char *names[8] = { "reset", "istruzione non definita", "SWI", "prefetch abort",
                                        "data abort", "address exception", "IRQ", "FIQ" };
        fflush(stdout);
        fprintf(stderr, "\nEccezione: %s (R14=&%08X)\n", names[(comment >> 2) & 7], cpu->r[14]);
        k->exit_code = 2;
        cpu->halted = 1;
        return 1;
    }

    if (k->trace_swi && n != 0 && (n < 0x100 || n >= 0x200)) {
        const char *nm = swi_name(comment);
        fprintf(stderr, "[SWI %s%s &%X R0=%08X R1=%08X R2=%08X]\n", (swi & X_BIT) ? "X" : "",
                nm ? nm : "?", comment, cpu->r[0], cpu->r[1], cpu->r[2]);
    }

    set_flag(k, ARM_V, 0);
    int handled = kernel_swi_body(k, cpu, comment, swi, n);
    /* tempo che la SWI avrebbe richiesto al sistema vero: ingresso e uscita
       dal kernel, piu' il lavoro del driver VDU */
    if (k->faithful) {
        cpu->extra_cycles += 120 + k->vdu->cost;
        if (n == 0x40743 || n == 0x40761) cpu->extra_cycles += 8000;   /* ColourTrans */
    }
    k->vdu->cost = 0;
    return handled;
}

static int kernel_swi_body(RiscosKernel *k, Arm2 *cpu, uint32_t comment, uint32_t swi, uint32_t n)
{
    if (n >= 0x100 && n < 0x200) { vdu(k, (uint8_t)n); return 1; }   /* OS_WriteI */

    switch (n) {
    case 0x00: vdu(k, (uint8_t)R(0)); return 1;
    case 0x01: {                                                     /* OS_WriteS */
        uint32_t a = arm2_pc(cpu);
        uint8_t c;
        while ((c = rd8(k, a++)) != 0) vdu(k, c);
        arm2_set_pc(cpu, (a + 3) & ~3u);
        return 1;
    }
    case 0x02: {                                                     /* OS_Write0 */
        uint32_t a = R(0);
        uint8_t c;
        while ((c = rd8(k, a++)) != 0) vdu(k, c);
        R(0) = a;
        return 1;
    }
    case 0x03: vdu(k, 13); vdu(k, 10); return 1;                    /* OS_NewLine */
    case 0x04: swi_readc(k); return 1;
    case 0x05: swi_cli(k, swi); return 1;
    case 0x06: swi_byte(k); return 1;
    case 0x07: swi_word(k); return 1;
    case 0x08: hostfs_file(k, swi); return 1;                       /* OS_File */
    case 0x09: hostfs_args(k, swi); return 1;                       /* OS_Args */
    case 0x0A: {                                                     /* OS_BGet */
        int c = hostfs_bget(k, (int)R(1));
        if (c == -2) { error_msg(k, swi, 0xDE, "Channel"); return 1; }
        R(0) = c < 0 ? 0xFFFFFFFFu : (uint32_t)c;
        set_flag(k, ARM_C, c < 0);
        return 1;
    }
    case 0x0B:                                                       /* OS_BPut */
        if (!hostfs_bput(k, (int)R(1), (uint8_t)R(0))) error_msg(k, swi, 0xDE, "Channel");
        return 1;
    case 0x0C: hostfs_gbpb(k, swi); return 1;                       /* OS_GBPB */
    case 0x0D: swi_find(k, swi); return 1;
    case 0x0E: swi_readline(k); return 1;
    case 0x10:                                                       /* OS_GetEnv */
        R(0) = K_ENVSTRING;
        R(1) = k->app_limit;
        R(2) = K_STARTTIME;
        return 1;
    case 0x11:                                                       /* OS_Exit */
        k->exit_code = (R(1) == 0x58454241u) ? (int)R(2) : 0;
        cpu->halted = 1;
        return 1;
    case 0x16:                                                       /* OS_EnterOS */
        arm2_set_r15(cpu, cpu->r[15] | ARM_MODE_SVC);
        return 1;
    case 0x1C: R(0) = R(1) = R(2) = 0; R(3) = (uint32_t)monotonic(k); return 1;   /* OS_Mouse */
    case 0x21: swi_read_unsigned(k, swi); return 1;
    case 0x23: {                                                     /* OS_ReadVarVal */
        char name[64];
        read_str(k, R(0), name, sizeof name);
        R(2) = 0;
        char msg[128];
        snprintf(msg, sizeof msg, "System variable '%s' not found", name);
        error_msg(k, swi, 0x124, msg);
        return 1;
    }
    case 0x29: swi_fscontrol(k, swi); return 1;
    case 0x2B: raise_error(k, swi, R(0)); return 1;                 /* OS_GenerateError */
    case 0x2C: set_flag(k, ARM_C, k->escape_pending); return 1;     /* OS_ReadEscapeState */
    case 0x2F: {                                                     /* OS_ReadPalette */
        uint32_t l = R(0) & 255;
        const VduPalette *p = &k->vdu->palette[l];
        uint32_t f = p->first, s = p->second;
        R(2) = (f & 0xFF) << 24 | ((f >> 8) & 0xFF) << 16 | ((f >> 16) & 0xFF) << 8;
        R(3) = (s & 0xFF) << 24 | ((s >> 8) & 0xFF) << 16 | ((s >> 16) & 0xFF) << 8;
        return 1;
    }
    case 0x30: return 1;                                             /* OS_ServiceCall */
    case 0x31: {                                                     /* OS_ReadVduVariables */
        uint32_t in = R(0), out = R(1);
        for (int i = 0; i < 64; i++) {
            uint32_t var = rd32(k, in + 4u * (uint32_t)i);
            if (var == 0xFFFFFFFFu) break;
            wr32(k, out + 4u * (uint32_t)i, (uint32_t)vdu_read_variable(k->vdu, (int)var));
        }
        return 1;
    }
    case 0x32: {                                                     /* OS_ReadPoint */
        uint32_t v;
        int ok = vdu_point(k->vdu, (int32_t)R(0), (int32_t)R(1), &v);
        R(2) = ok ? v : 0;
        R(3) = 0;
        R(4) = ok ? 0 : 0xFFFFFFFFu;
        return 1;
    }
    case 0x35:                                                       /* OS_ReadModeVariable */
        R(2) = (uint32_t)vdu_read_variable(k->vdu, (int)R(1));
        set_flag(k, ARM_C, 0);
        return 1;
    case 0x40: {                                                     /* OS_ChangeEnvironment */
        uint32_t h = R(0);
        if (h > 16) { error_msg(k, swi, 0x1E5, "Bad environment number"); return 1; }
        if (h == 0 || h == 14) {                                     /* limiti di memoria */
            uint32_t old = k->app_limit;
            if (R(1)) k->app_limit = R(1);
            R(1) = old;
            return 1;
        }
        uint32_t old[3] = { k->handlers[h][0], k->handlers[h][1], k->handlers[h][2] };
        for (int i = 0; i < 3; i++) if (R(1 + i)) k->handlers[h][i] = R(1 + i);
        R(1) = old[0]; R(2) = old[1]; R(3) = old[2];
        return 1;
    }
    case 0x42: R(0) = (uint32_t)monotonic(k); return 1;             /* OS_ReadMonotonicTime */
    case 0x44: pretty_emit(k, R(0), R(1), R(2), 0); return 1;       /* OS_PrettyPrint */
    case 0x45: vdu_plot(k->vdu, (int)R(0), (int32_t)R(1), (int32_t)R(2)); return 1;
    case 0x46:                                                       /* OS_WriteN */
        for (uint32_t i = 0; i < R(1); i++) vdu(k, rd8(k, R(0) + i));
        return 1;
    case 0x61: {                                                     /* OS_SetColour */
        uint32_t f = R(0);
        Vdu *v = k->vdu;
        if (f & 0x40) { if (f & 0x10) v->tbg = R(1); else v->tfg = R(1); }
        else if (f & 0x10) { v->gbg = R(1); v->gbg_action = (int)(f & 7); }
        else { v->gfg = R(1); v->gfg_action = (int)(f & 7); }
        return 1;
    }
    case 0x65: swi_screenmode(k, swi); return 1;
    case 0x6E: return 1;                                             /* OS_SynchroniseCodeAreas */
    case 0x38: swi_number_to_string(k); return 1;
    case 0x39: swi_number_from_string(k, swi); return 1;
    case 0x400EC:                                                    /* Wimp_SlotSize */
        R(0) = k->app_limit - K_APP_BASE;
        R(1) = 0xFFFFFFFFu;
        R(2) = 0;
        return 1;
    case 0x40743: {                                                  /* ColourTrans_SetGCOL */
        vdu_set_gcol_rgb(k->vdu, bbggrr_to_rgb(R(0)), (R(3) & 0x80) != 0, (int)(R(4) & 7));
        R(0) = 0;
        return 1;
    }
    case 0x40761:                                                    /* ColourTrans_SetTextColour */
        vdu_set_text_rgb(k->vdu, bbggrr_to_rgb(R(0)), (R(3) & 0x80) != 0);
        R(0) = 0;
        return 1;
    case 0x41B40:                                                    /* ResourceFS_RegisterFiles */
        if (k->nresblocks < K_MAX_RESBLOCKS) k->resblocks[k->nresblocks++] = R(0);
        return 1;
    case 0x41B41:                                                    /* ResourceFS_DeregisterFiles */
        for (int i = 0; i < k->nresblocks; i++)
            if (k->resblocks[i] == R(0)) k->resblocks[i] = k->resblocks[--k->nresblocks];
        return 1;
    default:
        break;
    }
    if (n >= 0x41500 && n <= 0x4153F) { swi_messagetrans(k, swi, n - 0x41500); return 1; }
    if (n >= 0xD0 && n <= 0xE8) { swi_convert(k, swi, n); return 1; }
    if (n >= 0x40140 && n <= 0x401BF) return 1;                     /* Sound: silenzio */

    if (n != k->last_unknown_swi) {
        fprintf(stderr, "[SWI &%X non implementata a &%X]\n", comment, arm2_pc(cpu) - 4);
        k->last_unknown_swi = n;
    }
    char msg[64];
    snprintf(msg, sizeof msg, "SWI &%X not known", n);
    error_msg(k, swi, 0x1E6, msg);
    return 1;
}

/* ------------------------------------------------------------------ */
/* servizi per i sottomoduli (kernel_priv.h)                          */
/* ------------------------------------------------------------------ */

uint8_t  kernel_rd8(RiscosKernel *k, uint32_t a)              { return rd8(k, a); }
uint32_t kernel_rd32(RiscosKernel *k, uint32_t a)             { return rd32(k, a); }
void     kernel_wr8(RiscosKernel *k, uint32_t a, uint8_t v)   { wr8(k, a, v); }
void     kernel_wr32(RiscosKernel *k, uint32_t a, uint32_t v) { wr32(k, a, v); }
int      kernel_read_str(RiscosKernel *k, uint32_t a, char *out, int max) { return read_str(k, a, out, max); }
void     kernel_error(RiscosKernel *k, uint32_t swi, uint32_t num, const char *msg) { error_msg(k, swi, num, msg); }

void kernel_print(RiscosKernel *k, const char *s)
{
    for (; *s; s++) {
        if (*s == '\n') { vdu(k, 13); vdu(k, 10); }
        else vdu(k, (uint8_t)*s);
    }
}

/* ------------------------------------------------------------------ */
/* inizializzazione                                                   */
/* ------------------------------------------------------------------ */

void kernel_init(RiscosKernel *k, Arm2 *cpu, Bus *bus, Vdu *vdu, uint32_t app_limit)
{
    memset(k, 0, sizeof *k);
    k->cpu = cpu;
    k->bus = bus;
    k->vdu = vdu;
    k->app_limit = app_limit;
    k->escape_enabled = 1;
    k->start_cs = kernel_time_cs(k);

    for (uint32_t v = 0; v < 0x20; v += 4) wr32(k, K_VECTORS + v, 0xEF000000u | K_SWI_EXCEPTION | v);
    wr32(k, K_TRAMPOLINE, 0xEF000000u | K_SWI_RETURN);
    wr32(k, K_EXIT_STUB, 0xEF000011u);
    uint64_t t = ((uint64_t)time(NULL) + 2208988800ull) * 100;
    for (int i = 0; i < 5; i++) wr8(k, K_STARTTIME + (uint32_t)i, (uint8_t)(t >> (8 * i)));
    write_str(k, K_ENVSTRING, "BASIC");

    cpu->swi_hook = kernel_swi;
    cpu->swi_user = k;
}
