/*
 * armwin_mac.m - Frontend a finestra per macOS (Cocoa, nessuna dipendenza).
 *
 *   ArchieEmu BASIC [--rom modulo] [--mode n] [--ram MB] [--mhz N] [--turbo] [--disc dir]
 *
 * Come win32_main.c: clock limitato a 8 MHz (F12 o Cmd+T per il turbo),
 * selezione del testo col mouse, Cmd+C copia, Cmd+V incolla. Dentro il
 * bundle il disco e' ~/Documents/ArchieEmu/disc, creato al primo avvio con
 * i programmi di esempio.
 */
#import <Cocoa/Cocoa.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "machine/machine.h"
#include "mac_keys.h"

#define FRAME_HZ     50
#define FLASH_CS     25            /* periodo del lampeggio (come *FX 9/10) */

typedef struct App {
    Machine   m;
    uint32_t *pixels;
    int       pix_w, pix_h;
    double    mhz;
    int       turbo;
    char     *paste;               /* testo da incollare ancora da inviare */
    size_t    paste_pos, paste_len;
    uint64_t  stat_c0;
    double    stat_t0, stat_mhz;
    char      notice[64];          /* messaggio temporaneo nel titolo */
    double    notice_until;
    double    next;
    int       last_w, last_h;
    uint32_t  dead;                /* stato dei tasti morti */

    /* posizione dell'immagine nella vista (letterbox), coordinate dall'alto */
    double    img_x, img_y, img_w, img_h;

    int       sel_valid, sel_dragging;
    int       sel_c0, sel_r0, sel_c1, sel_r1;
} App;

static App app;
static NSWindow *window;
static NSView *view;

static double now_s(void)
{
    return [NSDate timeIntervalSinceReferenceDate];
}

static void update_title(void)
{
    char t[128];
    if (app.notice[0] && now_s() < app.notice_until)
        snprintf(t, sizeof t, "BBC BASIC V   [%s]", app.notice);
    else if (app.turbo)
        snprintf(t, sizeof t, "BBC BASIC V   [TURBO: %.0f MHz, Cmd+T for %.0f MHz]", app.stat_mhz, app.mhz);
    else
        snprintf(t, sizeof t, "BBC BASIC V   [ARM2 %.0f MHz, Cmd+T for turbo]", app.mhz);
    NSString *s = [NSString stringWithUTF8String:t];
    if (![window.title isEqualToString:s]) window.title = s;
}

static void set_notice(const char *msg)
{
    snprintf(app.notice, sizeof app.notice, "%s", msg);
    app.notice_until = now_s() + 2.0;
    update_title();
}

/* dimensione a schermo del modo corrente: pixel alti raddoppiati */
static void display_size(int *w, int *h)
{
    const Vdu *v = &app.m.vdu;
    *w = v->width << (v->xeig - 1);
    *h = v->height << (v->yeig - 1);
    if (*w < 640) { *h = *h * 640 / *w; *w = 640; }
}

static void fit_window(void)
{
    int w, h;
    display_size(&w, &h);
    NSRect work = (window.screen ?: NSScreen.mainScreen).visibleFrame;
    int scale = 2;
    while (scale > 1 && (w * scale > work.size.width * 9 / 10 || h * scale > work.size.height * 9 / 10)) scale--;
    NSRect frame = [window frameRectForContentRect:NSMakeRect(0, 0, w * scale, h * scale)];
    NSRect old = window.frame;
    frame.origin.x = old.origin.x;
    frame.origin.y = old.origin.y + old.size.height - frame.size.height;   /* resta fermo l'angolo in alto */
    [window setFrame:frame display:YES];
}

/* ------------------------------------------------------------------ */
/* selezione e appunti                                                */
/* ------------------------------------------------------------------ */

static void sel_range(int *c0, int *r0, int *c1, int *r1)
{
    int a = app.sel_r0 * 4096 + app.sel_c0, b = app.sel_r1 * 4096 + app.sel_c1;
    if (a > b) { int t = a; a = b; b = t; }
    *r0 = a / 4096; *c0 = a % 4096; *r1 = b / 4096; *c1 = b % 4096;
}

static int in_selection(int col, int row)
{
    int c0, r0, c1, r1;
    sel_range(&c0, &r0, &c1, &r1);
    int k = row * 4096 + col;
    return k >= r0 * 4096 + c0 && k <= r1 * 4096 + c1;
}

static void highlight_selection(void)
{
    const Vdu *v = &app.m.vdu;
    for (int row = 0; row < v->rows; row++)
        for (int col = 0; col < v->cols; col++) {
            if (!in_selection(col, row)) continue;
            for (int y = 0; y < 8; y++) {
                uint32_t *p = app.pixels + (size_t)(row * 8 + y) * v->width + col * 8;
                for (int x = 0; x < 8; x++) p[x] ^= 0xFFFFFF;
            }
        }
}

static void mouse_to_cell(NSPoint p, int *col, int *row)
{
    const Vdu *v = &app.m.vdu;
    int px = app.img_w > 0 ? (int)((p.x - app.img_x) * v->width / app.img_w) : 0;
    int py = app.img_h > 0 ? (int)((p.y - app.img_y) * v->height / app.img_h) : 0;
    *col = px < 0 ? 0 : px / 8 >= v->cols ? v->cols - 1 : px / 8;
    *row = py < 0 ? 0 : py / 8 >= v->rows ? v->rows - 1 : py / 8;
}

static void copy_selection(void)
{
    const Vdu *v = &app.m.vdu;
    int c0, r0, c1, r1;
    sel_range(&c0, &r0, &c1, &r1);
    size_t cap = (size_t)(r1 - r0 + 1) * (v->cols + 1) + 1, n = 0, chars = 0;
    char *text = malloc(cap);
    if (!text) return;
    for (int row = r0; row <= r1; row++) {
        int from = row == r0 ? c0 : 0, to = row == r1 ? c1 : v->cols - 1;
        size_t line_start = n;
        for (int col = from; col <= to; col++) {
            int ch = vdu_char_at(v, col, row);
            text[n++] = (char)(ch ? ch : ' ');
        }
        while (n > line_start && text[n - 1] == ' ') n--;     /* spazi finali */
        chars += n - line_start;
        if (row < r1) text[n++] = '\n';
    }
    NSString *s = [[NSString alloc] initWithBytes:text length:n encoding:NSISOLatin1StringEncoding];
    free(text);
    NSPasteboard *pb = NSPasteboard.generalPasteboard;
    [pb clearContents];
    [pb setString:s forType:NSPasteboardTypeString];
    char msg[64];
    snprintf(msg, sizeof msg, "copied %u characters", (unsigned)chars);
    set_notice(msg);
}

static void select_word(int col, int row)
{
    const Vdu *v = &app.m.vdu;
    int ch = vdu_char_at(v, col, row);
    if (!ch || ch == ' ') return;
    int a = col, b = col;
    while (a > 0) { int c = vdu_char_at(v, a - 1, row); if (!c || c == ' ') break; a--; }
    while (b < v->cols - 1) { int c = vdu_char_at(v, b + 1, row); if (!c || c == ' ') break; b++; }
    app.sel_c0 = a; app.sel_r0 = row; app.sel_c1 = b; app.sel_r1 = row;
    app.sel_valid = 1;
}

static void start_paste(void)
{
    NSString *str = [NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString];
    if (!str) return;
    NSUInteger n = str.length;
    char *s = malloc(n * 3 + 1);          /* "..." puo' triplicare */
    if (!s) return;
    size_t o = 0;
    for (NSUInteger i = 0; i < n; i++) {
        unichar c = [str characterAtIndex:i];
        if (c == '\r') { s[o++] = 13; if (i + 1 < n && [str characterAtIndex:i + 1] == '\n') i++; }
        else if (c == '\n') s[o++] = 13;
        else if (c == '\t') s[o++] = ' ';
        else if (c == 0xA0) s[o++] = ' ';                           /* spazio non separabile */
        else if (c < 256) s[o++] = (char)c;
        /* tipografia di chat e pagine web -> ASCII */
        else if (c == 0x201C || c == 0x201D || c == 0x201E || c == 0x2033) s[o++] = '"';
        else if (c == 0x2018 || c == 0x2019 || c == 0x201A || c == 0x2032) s[o++] = '\'';
        else if (c == 0x2013 || c == 0x2014 || c == 0x2212) s[o++] = '-';
        else if (c == 0x2026) { s[o++] = '.'; s[o++] = '.'; s[o++] = '.'; }
        else if (c >= 0x2000 && c <= 0x200B) s[o++] = ' ';
        else if (c == 0xFEFF) continue;                              /* BOM */
        else s[o++] = '?';
    }
    free(app.paste);
    app.paste = s;
    app.paste_len = o;
    app.paste_pos = 0;
}

static void feed_paste(void)
{
    while (app.paste && app.paste_pos < app.paste_len && kernel_keys_pending(&app.m.kernel) < 200)
        kernel_key(&app.m.kernel, (uint8_t)app.paste[app.paste_pos++]);
    if (app.paste && app.paste_pos >= app.paste_len) { free(app.paste); app.paste = NULL; }
}

static void toggle_turbo(void)
{
    app.turbo = !app.turbo;
    update_title();
}

/* ------------------------------------------------------------------ */
/* vista                                                              */
/* ------------------------------------------------------------------ */

@interface EmuView : NSView
@end

@implementation EmuView

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isOpaque { return YES; }

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    Vdu *v = &app.m.vdu;
    if (v->width != app.pix_w || v->height != app.pix_h) {
        free(app.pixels);
        app.pixels = malloc((size_t)v->width * v->height * 4);
        app.pix_w = v->width;
        app.pix_h = v->height;
        if (!app.pixels) return;
    }
    uint64_t cs = kernel_time_cs(&app.m.kernel);
    int phase = (int)((cs / FLASH_CS) & 1);
    vdu_render(v, app.pixels, phase, !phase && !app.sel_valid);

    NSRect b = self.bounds;
    double cw = b.size.width, ch = b.size.height;
    int dw, dh;
    display_size(&dw, &dh);
    /* scala mantenendo le proporzioni, bordo nel colore del border */
    double sw = cw, sh = cw * dh / dw;
    if (sh > ch) { sh = ch; sw = ch * dw / dh; }
    app.img_x = (cw - sw) / 2; app.img_y = (ch - sh) / 2; app.img_w = sw; app.img_h = sh;
    if (app.sel_valid) highlight_selection();

    CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;
    CGContextSetRGBFillColor(ctx, ((v->border >> 16) & 255) / 255.0, ((v->border >> 8) & 255) / 255.0,
                             (v->border & 255) / 255.0, 1.0);
    CGContextFillRect(ctx, NSRectToCGRect(b));

    CGColorSpaceRef cs_rgb = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, app.pixels, (size_t)v->width * v->height * 4, NULL);
    CGImageRef img = CGImageCreate(v->width, v->height, 8, 32, (size_t)v->width * 4, cs_rgb,
                                   kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst, dp, NULL, false,
                                   kCGRenderingIntentDefault);
    CGContextSaveGState(ctx);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
    /* la vista e' capovolta: l'immagine va raddrizzata */
    CGContextTranslateCTM(ctx, app.img_x, app.img_y + sh);
    CGContextScaleCTM(ctx, 1, -1);
    CGContextDrawImage(ctx, CGRectMake(0, 0, sw, sh), img);
    CGContextRestoreGState(ctx);
    CGImageRelease(img);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs_rgb);
}

- (void)keyDown:(NSEvent *)e
{
    if (e.modifierFlags & NSEventModifierFlagCommand) return;   /* i comandi li gestisce il menu */
    unsigned short kc = e.keyCode;
    Machine *m = &app.m;
    switch (kc) {
    case 0x35:                                                   /* Escape */
        if (app.sel_valid) { app.sel_valid = 0; return; }        /* prima annulla la selezione */
        free(app.paste); app.paste = NULL;
        kernel_key(&m->kernel, 27);
        return;
    case 0x6F: toggle_turbo(); return;                           /* F12 */
    case 0x7B: kernel_key(&m->kernel, 0x8C); return;             /* frecce */
    case 0x7C: kernel_key(&m->kernel, 0x8D); return;
    case 0x7D: kernel_key(&m->kernel, 0x8E); return;
    case 0x7E: kernel_key(&m->kernel, 0x8F); return;
    case 0x75: kernel_key(&m->kernel, 127); return;              /* Canc */
    case 0x73: kernel_key(&m->kernel, 30); return;               /* Home */
    case 0x33: app.sel_valid = 0; kernel_key(&m->kernel, 8); return;    /* Backspace */
    case 0x4C: app.sel_valid = 0; kernel_key(&m->kernel, 13); return;   /* Invio del tastierino */
    default: break;
    }
    uint16_t buf[8];
    int n = mac_key_chars(kc, e.modifierFlags, &app.dead, buf, 8);
    for (int i = 0; i < n; i++) {
        if (buf[i] >= 0xF700) continue;                          /* tasti funzione */
        app.sel_valid = 0;
        if (buf[i] < 256) kernel_key(&m->kernel, (uint8_t)buf[i]);
    }
}

- (void)mouseDown:(NSEvent *)e
{
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    int col, row;
    mouse_to_cell(p, &col, &row);
    if (e.clickCount == 2) { select_word(col, row); return; }
    app.sel_c0 = app.sel_c1 = col;
    app.sel_r0 = app.sel_r1 = row;
    app.sel_dragging = 1;
    app.sel_valid = 0;
}

- (void)mouseDragged:(NSEvent *)e
{
    if (!app.sel_dragging) return;
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    int col, row;
    mouse_to_cell(p, &col, &row);
    if (col != app.sel_c0 || row != app.sel_r0) app.sel_valid = 1;
    app.sel_c1 = col;
    app.sel_r1 = row;
}

- (void)mouseUp:(NSEvent *)e { (void)e; app.sel_dragging = 0; }

- (void)rightMouseUp:(NSEvent *)e
{
    (void)e;
    if (app.sel_valid) { copy_selection(); app.sel_valid = 0; }
    else start_paste();
}

- (void)copy:(id)sender
{
    (void)sender;
    if (app.sel_valid) { copy_selection(); app.sel_valid = 0; }
}

- (void)paste:(id)sender { (void)sender; start_paste(); }
- (void)toggleTurbo:(id)sender { (void)sender; toggle_turbo(); }

@end

/* ------------------------------------------------------------------ */
/* ciclo della macchina                                               */
/* ------------------------------------------------------------------ */

@interface AppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation AppDelegate

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { (void)a; return YES; }

/* [NSApp run] non ritorna: si chiude qui */
- (void)applicationWillTerminate:(NSNotification *)n { (void)n; machine_destroy(&app.m); }

- (void)tick:(NSTimer *)timer
{
    (void)timer;
    double frame = 1.0 / FRAME_HZ, t = now_s();
    if (t < app.next) return;
    feed_paste();
    if (app.turbo) {
        /* esegue finche' resta tempo nel frame, poi disegna */
        double until = t + frame * 0.8;
        while (!app.m.cpu.halted && now_s() < until) {
            machine_run(&app.m, 200000);
            if (app.m.kernel.waiting) break;
        }
    } else {
        machine_run(&app.m, (uint64_t)(app.mhz * 1e6 / FRAME_HZ * (1.0 - machine_dma_fraction(&app.m))));
    }
    if (app.m.cpu.halted) { [NSApp terminate:nil]; return; }

    if (app.m.vdu.width != app.last_w || app.m.vdu.height != app.last_h) {
        app.last_w = app.m.vdu.width;
        app.last_h = app.m.vdu.height;
        app.sel_valid = 0;
        fit_window();
    }
    view.needsDisplay = YES;

    t = now_s();
    if (t - app.stat_t0 >= 1.0) {
        app.stat_mhz = (double)(app.m.cpu.cycles - app.stat_c0) / (t - app.stat_t0) / 1e6;
        app.stat_t0 = t;
        app.stat_c0 = app.m.cpu.cycles;
        update_title();
    }
    if (app.notice[0] && t >= app.notice_until) { app.notice[0] = 0; update_title(); }
    app.next += frame;
    if (app.next < t - 0.1) app.next = t;                     /* recupera dopo una pausa */
}

@end

/* ------------------------------------------------------------------ */
/* file                                                               */
/* ------------------------------------------------------------------ */

static int exists(const char *p) { return access(p, R_OK) == 0; }

static const char *find_rom(void)
{
    static char path[PATH_MAX];
    NSString *res = [NSBundle.mainBundle pathForResource:@"BASIC" ofType:nil];
    if (res) { snprintf(path, sizeof path, "%s", res.fileSystemRepresentation); return path; }
    /* fuori dal bundle: albero dei sorgenti, rispetto alla cartella corrente o all'eseguibile */
    static const char *rel[] = { "third_party/riscos/BASIC", "../third_party/riscos/BASIC",
                                 "../../third_party/riscos/BASIC", "../../../third_party/riscos/BASIC" };
    for (size_t i = 0; i < sizeof rel / sizeof rel[0]; i++)
        if (exists(rel[i])) return rel[i];
    char exe[PATH_MAX];
    uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size) == 0) {
        char *slash = strrchr(exe, '/');
        if (slash) {
            slash[1] = 0;
            for (size_t i = 0; i < sizeof rel / sizeof rel[0]; i++) {
                snprintf(path, sizeof path, "%s%s", exe, rel[i]);
                if (exists(path)) return path;
            }
        }
    }
    return rel[0];
}

/* dentro il bundle: ~/Documents/ArchieEmu/disc, con gli esempi al primo avvio */
static int bundle_disc(char *out, size_t size)
{
    NSString *demos = [NSBundle.mainBundle pathForResource:@"disc" ofType:nil];
    if (!demos) return 0;
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString *disc = [[docs stringByAppendingPathComponent:@"ArchieEmu"] stringByAppendingPathComponent:@"disc"];
    if (![fm fileExistsAtPath:disc]) {
        [fm createDirectoryAtPath:disc withIntermediateDirectories:YES attributes:nil error:nil];
        for (NSString *f in [fm contentsOfDirectoryAtPath:demos error:nil])
            [fm copyItemAtPath:[demos stringByAppendingPathComponent:f]
                        toPath:[disc stringByAppendingPathComponent:f] error:nil];
    }
    snprintf(out, size, "%s", disc.fileSystemRepresentation);
    return 1;
}

static void build_menu(void)
{
    NSMenu *bar = [NSMenu new];
    NSMenuItem *app_item = [NSMenuItem new];
    [bar addItem:app_item];
    NSMenu *app_menu = [NSMenu new];
    [app_menu addItemWithTitle:@"Quit BBC BASIC" action:@selector(terminate:) keyEquivalent:@"q"];
    app_item.submenu = app_menu;

    NSMenuItem *edit_item = [NSMenuItem new];
    [bar addItem:edit_item];
    NSMenu *edit = [[NSMenu alloc] initWithTitle:@"Edit"];
    [edit addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [edit addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    edit_item.submenu = edit;

    NSMenuItem *m_item = [NSMenuItem new];
    [bar addItem:m_item];
    NSMenu *mach = [[NSMenu alloc] initWithTitle:@"Machine"];
    [mach addItemWithTitle:@"Turbo" action:@selector(toggleTurbo:) keyEquivalent:@"t"];
    m_item.submenu = mach;
    NSApp.mainMenu = bar;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        MachineConfig cfg = { NULL, 4, -1, 0, NULL, 0 };
        static char disc[PATH_MAX];
        app.mhz = 8;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--rom") && i + 1 < argc) cfg.rom_path = argv[++i];
            else if (!strcmp(argv[i], "--mode") && i + 1 < argc) cfg.mode = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--ram") && i + 1 < argc) cfg.ram_mb = (uint32_t)atoi(argv[++i]);
            else if (!strcmp(argv[i], "--mhz") && i + 1 < argc) app.mhz = atof(argv[++i]);
            else if (!strcmp(argv[i], "--turbo")) app.turbo = 1;
            else if (!strcmp(argv[i], "--disc") && i + 1 < argc) cfg.disc_dir = argv[++i];
        }
        if (app.mhz <= 0) app.mhz = 8;
        if (!cfg.rom_path) cfg.rom_path = find_rom();
        if (!cfg.disc_dir) {
            if (!bundle_disc(disc, sizeof disc)) machine_default_disc(cfg.rom_path, disc, sizeof disc);
            cfg.disc_dir = disc;
        }

        [NSApplication sharedApplication];
        NSApp.activationPolicy = NSApplicationActivationPolicyRegular;

        char err[256];
        if (!machine_create(&app.m, &cfg, err, sizeof err)) {
            NSAlert *a = [NSAlert new];
            a.messageText = @"BBC BASIC";
            a.informativeText = [NSString stringWithUTF8String:err];
            [a runModal];
            return 1;
        }

        AppDelegate *delegate = [AppDelegate new];
        NSApp.delegate = delegate;
        build_menu();

        window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1280, 960)
                                             styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                       NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                               backing:NSBackingStoreBuffered defer:NO];
        view = [[EmuView alloc] initWithFrame:NSMakeRect(0, 0, 1280, 960)];
        window.contentView = view;
        [window makeFirstResponder:view];
        app.last_w = app.m.vdu.width;
        app.last_h = app.m.vdu.height;
        fit_window();
        [window center];
        update_title();
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];

        app.next = app.stat_t0 = now_s();
        app.stat_c0 = app.m.cpu.cycles;
        NSTimer *t = [NSTimer timerWithTimeInterval:0.004 target:delegate selector:@selector(tick:)
                                           userInfo:nil repeats:YES];
        [NSRunLoop.currentRunLoop addTimer:t forMode:NSRunLoopCommonModes];
        [NSApp run];
    }
    return 0;
}
