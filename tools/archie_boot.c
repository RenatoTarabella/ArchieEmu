/*
 * archie_boot.c - Avvio della macchina Archimedes da riga di comando, per
 * seguire il boot della ROM: esegue per un tempo emulato, riassume dove
 * e' stata la CPU e salva lo schermo in PNG.
 *
 *   archie_boot --rom ROM311 [--ms 3000] [--png schermo.png] [--floppy disco.adf] [--floppy2 disco.adf]
 *               [--ram 4] [--keys "testo"] [--trace-vectors] [--hist] [--hostfs cartella]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "archie/archie.h"
#include "cpu/arm2_disasm.h"
#include "frontend/png.h"

/* --- digitazione simulata: "{F12}", "{ENTER}" e caratteri normali --- */
typedef struct KeyStep { int code; int down; } KeyStep;
static KeyStep key_steps[4096];
static int key_count, key_next;

#define VK_SHIFT_   0x10
#define VK_RETURN_  0x0D
#define VK_F12_     0x7B

static void push_vk(int vk, int shift)
{
    int sc = kbd_code_from_vk(VK_SHIFT_, 0), c = kbd_code_from_vk(vk, 0);
    if (c < 0 || key_count > 4090) return;
    if (shift) key_steps[key_count++] = (KeyStep){ sc, 1 };
    key_steps[key_count++] = (KeyStep){ c, 1 };
    key_steps[key_count++] = (KeyStep){ c, 0 };
    if (shift) key_steps[key_count++] = (KeyStep){ sc, 0 };
}

/* tastiera UK: i simboli che servono ai comandi di RISC OS */
static void push_char(char ch)
{
    if (ch >= 'a' && ch <= 'z') { push_vk(ch - 32, 0); return; }
    if (ch >= 'A' && ch <= 'Z') { push_vk(ch, 1); return; }
    if (ch >= '0' && ch <= '9') { push_vk(ch, 0); return; }
    switch (ch) {
    case ' ': push_vk(0x20, 0); break;
    case '.': push_vk(0xBE, 0); break;
    case ',': push_vk(0xBC, 0); break;
    case '-': push_vk(0xBD, 0); break;
    case ':': push_vk(0xBA, 1); break;
    case ';': push_vk(0xBA, 0); break;
    case '$': push_vk('4', 1); break;
    case '!': push_vk('1', 1); break;
    case '*': push_vk('8', 1); break;
    case '"': push_vk('2', 1); break;
    case '&': push_vk('7', 1); break;
    case '_': push_vk(0xBD, 1); break;
    case '%': push_vk('5', 1); break;
    case '(': push_vk('9', 1); break;
    case ')': push_vk('0', 1); break;
    case '=': push_vk(0xBB, 0); break;
    case '+': push_vk(0xBB, 1); break;
    case '/': push_vk(0xBF, 0); break;
    case '<': push_vk(0xBC, 1); break;
    case '>': push_vk(0xBE, 1); break;
    default: break;
    }
}

static void parse_keys(const char *s)
{
    while (*s) {
        if (!strncmp(s, "{F12}", 5)) { push_vk(VK_F12_, 0); s += 5; }
        else if (!strncmp(s, "{RESET}", 7)) { key_steps[key_count++] = (KeyStep){ -2, 0 }; s += 7; }
        else if (!strncmp(s, "{WAIT}", 6)) {                   /* 5 s di pausa */
            for (int w = 0; w < 125; w++) key_steps[key_count++] = (KeyStep){ -1, 0 };
            s += 6;
        }
        else if (!strncmp(s, "{ENTER}", 7)) { push_vk(VK_RETURN_, 0); s += 7; }
        else if (!strncmp(s, "{ALT", 4)) {
            /* {ALTnnn}: Alt tenuto e codice decimale sul tastierino */
            int alt = kbd_code_from_vk(0x12, 0);
            key_steps[key_count++] = (KeyStep){ alt, 1 };
            for (s += 4; *s >= '0' && *s <= '9'; s++) {
                int c = kbd_code_from_vk(0x60 + (*s - '0'), 0);
                key_steps[key_count++] = (KeyStep){ c, 1 };
                key_steps[key_count++] = (KeyStep){ c, 0 };
            }
            key_steps[key_count++] = (KeyStep){ alt, 0 };
            if (*s == '}') s++;
        }
        else if (!strncmp(s, "{VK", 3) || !strncmp(s, "{SVK", 4)) {
            /* {VKxx} / {SVKxx}: tasto di Windows in esadecimale, con Shift */
            int shift = s[1] == 'S';
            const char *h = s + (shift ? 4 : 3);
            push_vk((int)strtol(h, NULL, 16), shift);
            while (*s && *s != '}') s++;
            if (*s) s++;
        }
        else push_char(*s++);
    }
}

static const char *vector_names[8] = {
    "reset", "undefined", "SWI", "prefetch abort", "data abort", "address exception", "IRQ", "FIQ"
};

static uint64_t vec_count[8];
static int trace_vectors;
static Archie a;

/* ultime istruzioni eseguite, per capire come si arriva a un abort */
#define RING 256
static struct { uint32_t pc, instr, r[16]; } ring[RING];
static unsigned ring_pos;
static int trace_abort, abort_dumped;

static void on_step(Arm2 *cpu, uint32_t pc, uint32_t instr)
{
    /* le lunghe scivolate su istruzioni nulle non interessano: si tiene la prima */
    static uint32_t last_instr = 1;
    if ((cpu->r[15] & 3) != ARM_MODE_USR) return;              /* solo il programma */
    if (instr == 0 && last_instr == 0) return;
    last_instr = instr;
    unsigned k = ring_pos++ % RING;
    ring[k].pc = pc;
    ring[k].instr = instr;
    memcpy(ring[k].r, cpu->r, sizeof ring[k].r);
}

static void dump_ring(void)
{
    for (unsigned n = 0; n < RING; n++) {
        unsigned k = (ring_pos + n) % RING;
        char text[80];
        arm2_disasm(ring[k].instr, ring[k].pc, text, sizeof text);
        fprintf(stderr, "%07X  %-30s R0=%08X R1=%08X R2=%08X R3=%08X R12=%08X R13=%08X R14=%08X\n",
                ring[k].pc, text, ring[k].r[0], ring[k].r[1], ring[k].r[2], ring[k].r[3],
                ring[k].r[12], ring[k].r[13], ring[k].r[14]);
    }
}

static void on_exception(Arm2 *cpu, uint32_t vector, uint32_t link, void *user)
{
    (void)cpu; (void)user;
    uint32_t v = (vector >> 2) & 7;
    vec_count[v]++;
    if (trace_abort && !abort_dumped && v == 3 && (link & 3) == ARM_MODE_USR) {
        abort_dumped = 1;
        fprintf(stderr, "prefetch abort in modo utente, R14=&%08X; ultime istruzioni:\n", link);
        dump_ring();
    }
    /* per gli abort dei dati l'istruzione colpevole e' a R14-8 */
    if (trace_vectors && v != 6 && v != 7 && vec_count[v] <= 12)
        fprintf(stderr, "[%.3f ms] %s, R14=&%08X\n", (double)archie_now(&a) / 24000.0, vector_names[v], link);
}

/* Rapporto del POST di RISC OS 3: impulsi sul bit 0 del latch A; un
   intervallo lungo (> 2 milioni di cicli a 8 MHz) vale 1. 32 bit dal
   piu' significativo. */
static void post_latch(void *user, uint8_t v, ArcTime now)
{
    static const char *names[32] = {
        "self-test dovuto all'accensione", "self-test da hardware di interfaccia", "self-test da link di test",
        "test lungo della memoria eseguito", "ARM3 presente", "test lungo della memoria disabilitato",
        "I/O stile PC rilevato", "VRAM rilevata",
        "GUASTO: checksum della CMOS", "GUASTO: checksum della ROM", "GUASTO: mappatura CAM del MEMC",
        "GUASTO: protezione del MEMC", "GUASTO: registri dell'IOC", "(bit 13)", "GUASTO: tempi di Virq (VIDC)",
        "GUASTO: tempi di Sirq (suono)", "GUASTO: CMOS illeggibile", "GUASTO: linee di controllo della RAM",
        "GUASTO: test lungo della RAM"
    };
    static ArcTime last;
    static int bitpos, prev;
    (void)user;
    int state = v & 1;
    if (state && !prev && bitpos <= 32) {
        int bit = now - last > ARC_US(250000);
        int n = 32 - bitpos;
        if (n <= 18 && names[n])
            printf("  POST %08X %-4s %s\n", 1u << n, bit ? (n < 8 ? "si" : "FAIL") : (n < 8 ? "no" : "ok"), names[n]);
        bitpos++;
    }
    if (state != prev) last = now;
    prev = state;
}

int main(int argc, char **argv)
{
    ArchieConfig cfg = { NULL, 4, NULL, { NULL, NULL }, 8, NULL };
    double ms = 3000;
    const char *png = NULL, *png2 = NULL;   /* png2: l'altro banco del doppio buffer */
    int hist = 0;
    const char *wav = NULL;
    FILE *wf = NULL;
    uint32_t wav_frames = 0;
    double peak = 0, energy = 0;
    double keys_at = 15000;                        /* ms: quando iniziare a digitare */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) cfg.rom_path = argv[++i];
        else if (!strcmp(argv[i], "--ms") && i + 1 < argc) ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) png = argv[++i];
        else if (!strcmp(argv[i], "--floppy") && i + 1 < argc) cfg.floppy[0] = argv[++i];
        else if (!strcmp(argv[i], "--floppy2") && i + 1 < argc) cfg.floppy[1] = argv[++i];
        else if (!strcmp(argv[i], "--ram") && i + 1 < argc) cfg.ram_mb = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cmos") && i + 1 < argc) cfg.cmos_path = argv[++i];
        else if (!strcmp(argv[i], "--hostfs") && i + 1 < argc) cfg.hostfs_dir = argv[++i];
        else if (!strcmp(argv[i], "--trace-vectors")) trace_vectors = 1;
        else if (!strcmp(argv[i], "--hist")) hist = 1;
        else if (!strcmp(argv[i], "--trace-abort")) trace_abort = 1;
        else if (!strcmp(argv[i], "--png2") && i + 1 < argc) png2 = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) parse_keys(argv[++i]);
        else if (!strcmp(argv[i], "--keys-at") && i + 1 < argc) keys_at = atof(argv[++i]);
        else { fprintf(stderr, "opzione sconosciuta: %s\n", argv[i]); return 1; }
    }
    if (!cfg.rom_path) { fprintf(stderr, "uso: archie_boot --rom file [--ms N] [--png file]\n"); return 1; }

    char err[256];
    if (!archie_create(&a, &cfg, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    a.cpu.exception_hook = on_exception;
    if (trace_abort) a.cpu.trace_hook = on_step;
    a.latch_a_hook = post_latch;
    if (wav) {
        wf = fopen(wav, "wb");
        if (wf) { static const uint8_t zero[44] = { 0 }; fwrite(zero, 1, 44, wf); }
    }

    /* esegue a fette da 1 ms con un istogramma del PC */
    static uint32_t pc_hist[4096];                 /* per blocchi da 4 KB della ROM */
    int slices = (int)ms;
    for (int s = 0; s < slices && !a.cpu.halted; s++) {
        ArcTime target = archie_now(&a) + ARC_MS(1);
        while (archie_now(&a) < target && !a.cpu.halted) {
            archie_run(&a, ARC_US(50));
            /* un evento di tasto ogni 40 ms dopo keys_at */
            if (key_next < key_count && archie_now(&a) >= ARC_MS(keys_at) + (ArcTime)key_next * ARC_MS(40)) {
                if (key_steps[key_next].code == -2) archie_reset(&a);
                else if (key_steps[key_next].code >= 0) kbd_key(&a.kbd, key_steps[key_next].code, key_steps[key_next].down);
                key_next++;
            }
            uint32_t pc = arm2_pc(&a.cpu);
            if (hist && pc >= 0x3800000u) pc_hist[((pc - 0x3800000u) >> 12) & 4095]++;
            int16_t buf[2 * 4096];
            uint32_t got;
            while ((got = archie_audio_read(&a, buf, 4096)) > 0) {
                for (uint32_t k = 0; k < 2 * got; k++) {
                    double v = buf[k] < 0 ? -buf[k] : buf[k];
                    if (v > peak) peak = v;
                    energy += (double)buf[k] * buf[k];
                }
                if (wf) fwrite(buf, 4, got, wf);
                wav_frames += got;
            }
        }
    }

    if (wf) {
        /* intestazione WAV: PCM 16 bit stereo */
        uint32_t data = wav_frames * 4, rate = ARCHIE_AUDIO_HZ;
        uint8_t h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 2,0 };
        uint32_t v32[] = { rate, rate * 4 };
        memcpy(h + 24, &v32[0], 4); memcpy(h + 28, &v32[1], 4);
        h[32] = 4; h[34] = 16;
        memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
        uint32_t riff = data + 36; memcpy(h + 4, &riff, 4);
        fseek(wf, 0, SEEK_SET); fwrite(h, 1, 44, wf); fclose(wf);
    }
    if (wav_frames)
        printf("audio: %u campioni (%.1f s), picco %.0f, RMS %.0f\n", wav_frames, wav_frames / (double)ARCHIE_AUDIO_HZ,
               peak, wav_frames ? sqrt(energy / (2.0 * wav_frames)) : 0.0);
    uint32_t pc = arm2_pc(&a.cpu);
    printf("tempo emulato %.1f ms, %llu istruzioni, %llu frame, PC=&%07X %s\n",
           (double)archie_now(&a) / 24000.0, (unsigned long long)a.cpu.instructions,
           (unsigned long long)a.frames, pc, arm2_mode_name(arm2_mode(&a.cpu)));
    for (int v = 0; v < 8; v++)
        if (vec_count[v]) printf("  vettore %-18s %llu\n", vector_names[v], (unsigned long long)vec_count[v]);
    printf("  abort del MEMC: %u\n", a.memc.aborts);
    printf("  IOC: IRQA=%02X/%02X IRQB=%02X/%02X FIQ=%02X/%02X timer0 latch=%u\n", a.ioc.irqa, a.ioc.irqa_mask,
           a.ioc.irqb, a.ioc.irqb_mask, a.ioc.fiq, a.ioc.fiq_mask, a.ioc.timer[0].latch);
    {
        static const unsigned rom_ns[4] = { 450, 325, 200, 200 };
        printf("  MEMC: controllo &%04X (ROM alta %u ns), DMA video %.1f%% della banda\n", a.memc.control,
               rom_ns[(a.memc.control >> 6) & 3], a.dma_fraction * 100);
    }
    printf("  MEMC: pagine %u KB, video DMA %d, vinit=&%X vstart=&%X vend=&%X\n",
           1u << (a.memc.page_shift - 10), a.memc.video_dma, a.memc.vinit, a.memc.vstart, a.memc.vend);
    VidcTiming t;
    vidc_timing(&a.vidc, &t);
    printf("  audio: SFR=%u (un byte ogni %u us), stereo %u%u%u%u%u%u%u%u\n", a.sound_sfr, a.sound_sfr + 2,
           a.vidc.stereo[0], a.vidc.stereo[1], a.vidc.stereo[2], a.vidc.stereo[3],
           a.vidc.stereo[4], a.vidc.stereo[5], a.vidc.stereo[6], a.vidc.stereo[7]);
    printf("  VIDC: %s %dx%d %d bpp\n", t.valid ? "valido" : "non programmato", t.width, t.height, 1 << t.log2bpp);
    for (int i = -4; i <= 4; i++) {
        uint32_t addr = pc + 4u * (uint32_t)i;
        int ab = 0;
        uint32_t w = memc_read32(&a.memc, addr, &ab);
        char text[80];
        arm2_disasm(w, addr, text, sizeof text);
        printf("  %c %07X  %08X  %s\n", i == 0 ? '>' : ' ', addr, w, text);
    }
    if (hist) {
        printf("blocchi della ROM piu' eseguiti (offset):\n");
        for (int k = 0; k < 8; k++) {
            int best = 0;
            for (int i = 1; i < 4096; i++) if (pc_hist[i] > pc_hist[best]) best = i;
            if (!pc_hist[best]) break;
            printf("  &%06X  %u\n", best << 12, pc_hist[best]);
            pc_hist[best] = 0;
        }
    }

    if (png && t.valid) {
        static uint32_t pix[1024 * 768];
        int w = 0, h = 0;
        archie_render(&a, pix, 1024, &w, &h);
        /* png_write vuole righe contigue: compatta */
        for (int y = 0; y < h; y++) memmove(pix + (size_t)y * w, pix + (size_t)y * 1024, (size_t)w * 4);
        int ys = h <= 300 ? 2 : 1;
        if (!png_write(png, pix, w, h, ys)) fprintf(stderr, "impossibile scrivere %s\n", png);
    }
    if (png2 && t.valid) {
        /* mostra il banco che parte a vinit +/- meta' del buffer */
        Memc *m = &a.memc;
        uint32_t half = (m->vend + 16 - m->vstart) / 2;
        uint32_t save = m->vinit;
        m->vinit = m->vinit >= m->vstart + half ? m->vinit - half : m->vinit + half;
        static uint32_t pix2[1024 * 768];
        int w = 0, h = 0;
        archie_render(&a, pix2, 1024, &w, &h);
        for (int y = 0; y < h; y++) memmove(pix2 + (size_t)y * w, pix2 + (size_t)y * 1024, (size_t)w * 4);
        png_write(png2, pix2, w, h, h <= 300 ? 2 : 1);
        m->vinit = save;
    }
    archie_destroy(&a);
    return 0;
}
