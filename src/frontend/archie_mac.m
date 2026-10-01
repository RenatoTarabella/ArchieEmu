/*
 * archie_mac.m - Finestra della macchina Archimedes per macOS (Cocoa).
 *
 *   ArchieEmu Archimedes [--rom file] [--floppy disco.adf] [--floppy2 disco.adf]
 *                        [--ram MB] [--mhz N] [--cmos file]
 *
 * Come archie_win32.c. La tastiera passa per la stessa traduzione
 * (archie_keys.c): i tasti del Mac diventano i codici di Windows nella
 * stessa posizione (mac_keys.c), Option fa da AltGr. Comandi solo dal menu
 * Machine (Cmd+O dischetto, Cmd+E espelle, Cmd+T turbo, Cmd+Esc libera il
 * mouse...): i tasti Ctrl+F restano a RISC OS (Ctrl+F12 task window...).
 * Mouse: sinistro Select, centrale Menu, destro Adjust, come sull'Archimedes;
 * "Right Button Is Menu" (ricordato fra un avvio e l'altro) per il trackpad.
 *
 * La ROM si cerca accanto all'applicazione (roms/1. Major/ROM311) e in
 * ~/Documents/ArchieEmu/roms/ROM311; se manca la si sceglie con una finestra
 * e viene copiata li'. La CMOS si salva accanto alla ROM. La cartella
 * ~/Documents/ArchieEmu/HostFS (o --hostfs) e' il disco HostFS di RISC OS.
 */
#import <Cocoa/Cocoa.h>
#import <AudioToolbox/AudioToolbox.h>
#include <mach-o/dyld.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "archie/archie.h"
#include "archie_keys.h"
#include "mac_keys.h"

#define FRAME_HZ   50
#define TURBO_MHZ  64.0
#define BUF_W      1024
#define BUF_H      768

/* ------------------------------------------------------------------ */
/* audio: AudioQueue alimentata da un anello                          */
/* ------------------------------------------------------------------ */

#define RING_FRAMES   (ARCHIE_AUDIO_HZ / 4)          /* 250 ms */
#define AUDIO_AHEAD   (ARCHIE_AUDIO_HZ / 12)         /* oltre ~80 ms in coda si scarta */
#define QBUF_FRAMES   (ARCHIE_AUDIO_HZ / 100)        /* buffer della coda da 10 ms */
#define QBUFS         3

typedef struct Audio {
    AudioQueueRef queue;
    int16_t       ring[RING_FRAMES * 2];
    _Atomic uint32_t rd, wr;                          /* in frame, contatori liberi */
    int           ok, muted;
} Audio;

static Audio audio;

static void audio_callback(void *user, AudioQueueRef q, AudioQueueBufferRef buf)
{
    (void)user;
    int16_t *out = buf->mAudioData;
    uint32_t rd = atomic_load(&audio.rd), wr = atomic_load(&audio.wr);
    uint32_t avail = wr - rd, n = avail < QBUF_FRAMES ? avail : QBUF_FRAMES;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = (rd + i) % RING_FRAMES;
        out[i * 2] = audio.ring[k * 2];
        out[i * 2 + 1] = audio.ring[k * 2 + 1];
    }
    memset(out + n * 2, 0, (QBUF_FRAMES - n) * 4);       /* in ritardo: silenzio */
    atomic_store(&audio.rd, rd + n);
    buf->mAudioDataByteSize = QBUF_FRAMES * 4;
    AudioQueueEnqueueBuffer(q, buf, 0, NULL);
}

static void audio_open(void)
{
    AudioStreamBasicDescription f;
    memset(&f, 0, sizeof f);
    f.mSampleRate = ARCHIE_AUDIO_HZ;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 16;
    f.mBytesPerFrame = 4;
    f.mFramesPerPacket = 1;
    f.mBytesPerPacket = 4;
    if (AudioQueueNewOutput(&f, audio_callback, NULL, NULL, NULL, 0, &audio.queue) != noErr) return;
    for (int i = 0; i < QBUFS; i++) {
        AudioQueueBufferRef b;
        if (AudioQueueAllocateBuffer(audio.queue, QBUF_FRAMES * 4, &b) != noErr) return;
        audio_callback(NULL, audio.queue, b);
    }
    if (AudioQueueStart(audio.queue, NULL) != noErr) return;
    audio.ok = 1;
}

static void audio_close(void)
{
    if (!audio.queue) return;
    AudioQueueStop(audio.queue, true);
    AudioQueueDispose(audio.queue, true);
    audio.queue = NULL;
    audio.ok = 0;
}

/* prende i campioni prodotti dalla macchina e li mette nell'anello */
static void audio_pump(Archie *a)
{
    int16_t tmp[ARCHIE_AUDIO_HZ / 50 * 2];
    uint32_t got;
    while ((got = archie_audio_read(a, tmp, ARCHIE_AUDIO_HZ / 50)) > 0) {
        if (!audio.ok) continue;
        uint32_t wr = atomic_load(&audio.wr), queued = wr - atomic_load(&audio.rd);
        if (queued + got > AUDIO_AHEAD) continue;      /* troppo avanti: si scarta */
        for (uint32_t i = 0; i < got; i++) {
            uint32_t k = (wr + i) % RING_FRAMES;
            audio.ring[k * 2] = audio.muted ? 0 : tmp[i * 2];
            audio.ring[k * 2 + 1] = audio.muted ? 0 : tmp[i * 2 + 1];
        }
        atomic_store(&audio.wr, wr + got);
    }
}

/* ------------------------------------------------------------------ */

typedef struct App {
    Archie    a;
    uint32_t  pixels[BUF_W * BUF_H];
    int       disp_w, disp_h;        /* ultima immagine */
    double    mhz;
    int       turbo;
    char      rom_name[64];
    char      floppy_name[2][64];
    double    next;
    /* mouse */
    int       have_last;
    NSPoint   last;
    double    acc_x, acc_y;
    double    img_w;
    int       captured;              /* mouse catturato: movimento relativo */
    int       right_menu;            /* tasto destro = Menu invece di Adjust */
    /* tastiera */
    uint32_t  dead;
    double    caps_release;          /* Caps Lock: rilascio ritardato */
    unsigned long flags;             /* ultimi modificatori visti */
} App;

static App app;
static ArchieKeys keys;
static NSWindow *window;
static NSView *view;
static NSCursor *blank_cursor;
static char adf_dir[PATH_MAX];

static double now_s(void)
{
    return [NSDate timeIntervalSinceReferenceDate];
}

static uint32_t now_ms(void)
{
    return (uint32_t)(uint64_t)(now_s() * 1000.0);
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void update_title(void)
{
    char t[256];
    snprintf(t, sizeof t, "Archimedes - %s   [%s, Cmd+T %s]%s%s%s%s",
             app.rom_name, app.turbo ? "TURBO" : "ARM2 8 MHz", app.turbo ? "for 8 MHz" : "for turbo",
             app.captured ? "   [mouse captured: Cmd+Esc to release]" : "   [click to capture the mouse]",
             app.floppy_name[0][0] ? "   :0 " : "", app.floppy_name[0], app.floppy_name[1][0] ? "  :1 " : "");
    if (app.floppy_name[1][0]) strncat(t, app.floppy_name[1], sizeof t - strlen(t) - 1);
    NSString *s = [NSString stringWithUTF8String:t];
    if (![window.title isEqualToString:s]) window.title = s;
}

/* dimensione a schermo: i modi da 256 righe hanno pixel alti il doppio */
static void display_size(int w, int h, int *dw, int *dh)
{
    *dw = w < 640 ? 640 : w;
    *dh = h <= 300 ? h * 2 : h;
}

static void fit_window(void)
{
    int dw, dh;
    display_size(app.disp_w ? app.disp_w : 640, app.disp_h ? app.disp_h : 256, &dw, &dh);
    NSRect work = (window.screen ?: NSScreen.mainScreen).visibleFrame;
    int scale = 2;
    while (scale > 1 && (dw * scale > work.size.width * 9 / 10 || dh * scale > work.size.height * 9 / 10)) scale--;
    NSRect frame = [window frameRectForContentRect:NSMakeRect(0, 0, dw * scale, dh * scale)];
    NSRect old = window.frame;
    frame.origin.x = old.origin.x;
    frame.origin.y = old.origin.y + old.size.height - frame.size.height;
    [window setFrame:frame display:YES];
}

static void show_message(NSString *title, NSString *text)
{
    NSAlert *a = [NSAlert new];
    a.messageText = title;
    a.informativeText = text;
    [a runModal];
}

static void insert_floppy(int drive, const char *path)
{
    fdc_eject(&app.a.fdc, drive);
    if (fdc_insert(&app.a.fdc, drive, path)) {
        snprintf(app.floppy_name[drive], sizeof app.floppy_name[drive], "%s", base_name(path));
    } else {
        app.floppy_name[drive][0] = 0;
        show_message(@"Archimedes", [NSString stringWithFormat:@"Unrecognised image:\n%s\n\n"
                                     "An ADFS .adf image (800 KB or 640 KB) or an .hfe is needed.", path]);
    }
    update_title();
}

/* dischetti cambiati da RISC OS (*HostFS_Insert): il titolo li segue */
static void sync_floppy_names(void)
{
    for (int d = 0; d < 2; d++) {
        const FdcDrive *fd = &app.a.fdc.drive[d];
        const char *name = fd->image ? base_name(fd->path) : "";
        if (strcmp(name, app.floppy_name[d])) {
            snprintf(app.floppy_name[d], sizeof app.floppy_name[d], "%s", name);
            update_title();
        }
    }
}

static void eject_floppy(int drive)
{
    fdc_eject(&app.a.fdc, drive);
    app.floppy_name[drive][0] = 0;
    update_title();
}

static void capture_mouse(int on)
{
    if (on == app.captured) return;
    app.captured = on;
    CGAssociateMouseAndMouseCursorPosition(!on);
    if (on) {
        /* il cursore (invisibile) si porta al centro della vista */
        NSRect r = [view convertRect:view.bounds toView:nil];
        r = [window convertRectToScreen:r];
        CGFloat top = NSMaxY(NSScreen.screens.firstObject.frame);
        CGWarpMouseCursorPosition(CGPointMake(NSMidX(r), top - NSMidY(r)));
    }
    app.have_last = 0;
    update_title();
}

/* i tasti modificatori premuti finiscono nella finestra di dialogo: si rilasciano */
static void release_modifiers(void)
{
    uint32_t t = now_ms();
    keys_key(&keys, 0x10, 0, 0, 0, t);
    keys_key(&keys, 0x11, 0, 0, 0, t);
    keys_key(&keys, 0x11, 1, 0, 0, t);
    keys_key(&keys, 0x12, 1, 0, 0, t);
    app.flags = 0;
}

static void choose_floppy(int drive)
{
    capture_mouse(0);
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.title = [NSString stringWithFormat:@"Floppy disc for drive %d", drive];
    p.canChooseDirectories = NO;
    p.allowsMultipleSelection = NO;
    if (adf_dir[0]) p.directoryURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:adf_dir]];
    NSModalResponse r = [p runModal];
    release_modifiers();
    [window makeKeyAndOrderFront:nil];
    if (r != NSModalResponseOK) return;
    NSString *path = p.URL.path;
    insert_floppy(drive, path.fileSystemRepresentation);
    snprintf(adf_dir, sizeof adf_dir, "%s", path.stringByDeletingLastPathComponent.fileSystemRepresentation);
}

static void toggle_turbo(void)
{
    app.turbo = !app.turbo;
    archie_set_mhz(&app.a, app.turbo ? TURBO_MHZ : app.mhz);
    update_title();
}

static void mouse_moved(NSEvent *e)
{
    if (app.captured) {
        int dx = (int)e.deltaX, dy = (int)e.deltaY;
        if (dx || dy) kbd_mouse_move(&app.a.kbd, dx, -dy);
        return;
    }
    NSPoint p = e.locationInWindow;
    if (app.have_last && app.img_w > 0) {
        /* unita' del mouse Archimedes: circa 2 unita' OS per passo; lo
           schermo e' largo 1280 unita' OS */
        double k = 1280.0 / 2.0 / app.img_w;
        app.acc_x += (p.x - app.last.x) * k;
        app.acc_y += (p.y - app.last.y) * k;            /* in Cocoa la y cresce verso l'alto */
        int dx = (int)app.acc_x, dy = (int)app.acc_y;
        if (dx || dy) {
            kbd_mouse_move(&app.a.kbd, dx, dy);
            app.acc_x -= dx;
            app.acc_y -= dy;
        }
    }
    app.last = p;
    app.have_last = 1;
}

/* ------------------------------------------------------------------ */
/* vista                                                              */
/* ------------------------------------------------------------------ */

@interface EmuView : NSView <NSDraggingDestination>
@end

@implementation EmuView

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }

- (void)resetCursorRects
{
    [self addCursorRect:self.bounds cursor:blank_cursor];   /* si vede il puntatore di RISC OS */
}

- (void)updateTrackingAreas
{
    for (NSTrackingArea *t in self.trackingAreas) [self removeTrackingArea:t];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect
        options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow |
                NSTrackingInVisibleRect owner:self userInfo:nil]];
    [super updateTrackingAreas];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    int w = 0, h = 0;
    VidcTiming t;
    vidc_timing(&app.a.vidc, &t);
    if (t.valid) archie_render(&app.a, app.pixels, BUF_W, &w, &h);
    if (w <= 0 || h <= 0) { w = 640; h = 256; memset(app.pixels, 0, sizeof app.pixels); }
    if (w != app.disp_w || h != app.disp_h) {
        app.disp_w = w;
        app.disp_h = h;
        fit_window();
    }

    NSRect b = self.bounds;
    double cw = b.size.width, ch = b.size.height;
    int dw, dh;
    display_size(w, h, &dw, &dh);
    double sw = cw, sh = cw * dh / dw;
    if (sh > ch) { sh = ch; sw = ch * dw / dh; }
    app.img_w = sw;

    CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;
    uint32_t border = vidc_border_rgb(&app.a.vidc);
    CGContextSetRGBFillColor(ctx, ((border >> 16) & 255) / 255.0, ((border >> 8) & 255) / 255.0,
                             (border & 255) / 255.0, 1.0);
    CGContextFillRect(ctx, NSRectToCGRect(b));

    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, app.pixels, (size_t)BUF_W * h * 4, NULL);
    CGImageRef img = CGImageCreate(w, h, 8, 32, BUF_W * 4, rgb,
                                   kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst, dp, NULL, false,
                                   kCGRenderingIntentDefault);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
    CGContextDrawImage(ctx, CGRectMake((cw - sw) / 2, (ch - sh) / 2, sw, sh), img);
    CGImageRelease(img);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(rgb);
}

- (void)keyDown:(NSEvent *)e
{
    if (e.modifierFlags & NSEventModifierFlagCommand) return;   /* i comandi li gestisce il menu */
    if (e.isARepeat) return;                                     /* la ripetizione la fa la macchina */
    int ext, vk = mac_key_to_vk(e.keyCode, &ext);
    if (vk < 0) return;
    keys_key(&keys, vk, ext, 1, 0, now_ms());
    uint16_t buf[8];
    int n = mac_key_chars(e.keyCode, e.modifierFlags, &app.dead, buf, 8);
    if (n == 0 && app.dead) keys_deadchar(&keys);
    for (int i = 0; i < n; i++) keys_char(&keys, buf[i]);
}

- (void)keyUp:(NSEvent *)e
{
    int ext, vk = mac_key_to_vk(e.keyCode, &ext);
    if (vk >= 0) keys_key(&keys, vk, ext, 0, 0, now_ms());
}

- (void)flagsChanged:(NSEvent *)e
{
    unsigned short kc = e.keyCode;
    unsigned long f = e.modifierFlags;
    app.flags = f;
    int ext, vk = mac_key_to_vk(kc, &ext);
    if (vk < 0) return;                                          /* Command, fn */
    if (vk == 0x14) {
        /* Caps Lock: il Mac segnala solo il cambio di stato; l'Archimedes
           vuole una pressione, rilasciata dopo che RISC OS l'ha vista */
        keys_key(&keys, vk, 0, 1, 0, now_ms());
        app.caps_release = now_s() + 0.06;
        return;
    }
    keys_key(&keys, vk, ext, mac_modifier_down(kc, f), 0, now_ms());
}

- (void)mouseMoved:(NSEvent *)e { mouse_moved(e); }
- (void)mouseDragged:(NSEvent *)e { mouse_moved(e); }
- (void)rightMouseDragged:(NSEvent *)e { mouse_moved(e); }
- (void)otherMouseDragged:(NSEvent *)e { mouse_moved(e); }
- (void)mouseExited:(NSEvent *)e { (void)e; app.have_last = 0; }

- (void)mouseDown:(NSEvent *)e
{
    (void)e;
    if (!app.captured) { capture_mouse(1); return; }   /* il primo clic cattura soltanto */
    kbd_key(&app.a.kbd, 0x70, 1);
}
- (void)mouseUp:(NSEvent *)e { (void)e; kbd_key(&app.a.kbd, 0x70, 0); }
/* come sull'Archimedes: centrale = Menu, destro = Adjust; con "Right Button
   Is Menu" (trackpad, mouse a due tasti) il destro fa Menu */
- (void)rightMouseDown:(NSEvent *)e { (void)e; kbd_key(&app.a.kbd, app.right_menu ? 0x71 : 0x72, 1); }
- (void)rightMouseUp:(NSEvent *)e { (void)e; kbd_key(&app.a.kbd, app.right_menu ? 0x71 : 0x72, 0); }
- (void)otherMouseDown:(NSEvent *)e { (void)e; kbd_key(&app.a.kbd, app.right_menu ? 0x72 : 0x71, 1); }
- (void)otherMouseUp:(NSEvent *)e { (void)e; kbd_key(&app.a.kbd, app.right_menu ? 0x72 : 0x71, 0); }

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)s { (void)s; return NSDragOperationCopy; }

- (BOOL)performDragOperation:(id<NSDraggingInfo>)s
{
    NSArray<NSURL *> *urls = [s.draggingPasteboard readObjectsForClasses:@[ NSURL.class ]
                                                                 options:@{ NSPasteboardURLReadingFileURLsOnlyKey: @YES }];
    if (!urls.count) return NO;
    int drive = (NSEvent.modifierFlags & NSEventModifierFlagShift) ? 1 : 0;
    insert_floppy(drive, urls.firstObject.path.fileSystemRepresentation);
    return YES;
}

/* menu Machine */
- (void)insert0:(id)s { (void)s; choose_floppy(0); }
- (void)insert1:(id)s { (void)s; choose_floppy(1); }
- (void)eject0:(id)s { (void)s; eject_floppy(0); }
- (void)eject1:(id)s { (void)s; eject_floppy(1); }
- (void)toggleTurbo:(id)s { (void)s; toggle_turbo(); }
- (void)toggleSound:(id)s { (void)s; audio.muted = !audio.muted; }
- (void)releaseMouse:(id)s { (void)s; capture_mouse(0); }
- (void)resetMachine:(id)s { (void)s; archie_reset(&app.a); }
- (void)toggleRightMenu:(NSMenuItem *)item
{
    app.right_menu = !app.right_menu;
    item.state = app.right_menu ? NSControlStateValueOn : NSControlStateValueOff;
    [NSUserDefaults.standardUserDefaults setBool:app.right_menu forKey:@"RightButtonIsMenu"];
}

@end

/* ------------------------------------------------------------------ */
/* ciclo della macchina                                               */
/* ------------------------------------------------------------------ */

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation AppDelegate

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { (void)a; return YES; }

/* [NSApp run] non ritorna: la CMOS si salva qui */
- (void)applicationWillTerminate:(NSNotification *)n
{
    (void)n;
    capture_mouse(0);
    audio_close();
    archie_destroy(&app.a);
}

- (void)windowDidResignKey:(NSNotification *)n { (void)n; capture_mouse(0); release_modifiers(); }

- (BOOL)application:(NSApplication *)a openFile:(NSString *)f
{
    (void)a;
    insert_floppy(0, f.fileSystemRepresentation);              /* .adf aperto con l'applicazione */
    return YES;
}

- (void)tick:(NSTimer *)timer
{
    (void)timer;
    double t = now_s();
    if (t < app.next) return;
    if (app.caps_release && t >= app.caps_release) {
        app.caps_release = 0;
        keys_key(&keys, 0x14, 0, 0, 0, now_ms());
    }
    keys_tick(&keys, now_ms());
    archie_run(&app.a, ARC_MS(1000 / FRAME_HZ));
    audio_pump(&app.a);
    sync_floppy_names();
    view.needsDisplay = YES;
    app.next += 1.0 / FRAME_HZ;
    if (app.next < t - 0.1) app.next = t;                     /* recupera dopo una pausa */
}

@end

/* ------------------------------------------------------------------ */
/* ROM e cartelle                                                     */
/* ------------------------------------------------------------------ */

static int exists(const char *p) { return access(p, R_OK) == 0; }

/* cartella che contiene l'applicazione (o l'eseguibile, fuori dal bundle) */
static NSString *app_folder(void)
{
    NSString *b = NSBundle.mainBundle.bundlePath;
    if ([b.pathExtension isEqualToString:@"app"]) return b.stringByDeletingLastPathComponent;
    char exe[PATH_MAX];
    uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size) == 0)
        return [NSString stringWithUTF8String:exe].stringByDeletingLastPathComponent;
    return @".";
}

static NSString *user_folder(NSString *sub)
{
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString *d = [[docs stringByAppendingPathComponent:@"ArchieEmu"] stringByAppendingPathComponent:sub];
    [NSFileManager.defaultManager createDirectoryAtPath:d withIntermediateDirectories:YES attributes:nil error:nil];
    return d;
}

static const char *find_rom(char *buf, size_t size)
{
    NSString *base = app_folder();
    NSArray *rel = @[ @"roms/1. Major/ROM311", @"../roms/1. Major/ROM311", @"../../roms/1. Major/ROM311",
                      @"../../../roms/1. Major/ROM311" ];
    for (NSString *r in rel) {
        NSString *p = [base stringByAppendingPathComponent:r].stringByStandardizingPath;
        if (exists(p.fileSystemRepresentation)) { snprintf(buf, size, "%s", p.fileSystemRepresentation); return buf; }
    }
    NSString *mine = [user_folder(@"roms") stringByAppendingPathComponent:@"ROM311"];
    if (exists(mine.fileSystemRepresentation)) { snprintf(buf, size, "%s", mine.fileSystemRepresentation); return buf; }

    /* non c'e': la si chiede all'utente e la si copia in ~/Documents/ArchieEmu/roms */
    NSAlert *a = [NSAlert new];
    a.messageText = @"RISC OS ROM not found";
    a.informativeText = @"ArchieEmu needs a RISC OS 3.11 ROM image (a single 2 MB file), which is not included.\n\n"
                         "Choose it now: it will be copied to Documents/ArchieEmu/roms/ROM311.";
    [a addButtonWithTitle:@"Choose ROM..."];
    [a addButtonWithTitle:@"Quit"];
    if ([a runModal] != NSAlertFirstButtonReturn) return NULL;
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.title = @"RISC OS 3.11 ROM image";
    if ([p runModal] != NSModalResponseOK) return NULL;
    NSError *err = nil;
    if (![NSFileManager.defaultManager copyItemAtURL:p.URL toURL:[NSURL fileURLWithPath:mine] error:&err]) {
        show_message(@"Archimedes", err.localizedDescription);
        return NULL;
    }
    snprintf(buf, size, "%s", mine.fileSystemRepresentation);
    return buf;
}

static void build_menu(void)
{
    NSMenu *bar = [NSMenu new];
    NSMenuItem *app_item = [NSMenuItem new];
    [bar addItem:app_item];
    NSMenu *app_menu = [NSMenu new];
    [app_menu addItemWithTitle:@"Quit Archimedes" action:@selector(terminate:) keyEquivalent:@"q"];
    app_item.submenu = app_menu;

    NSMenuItem *m_item = [NSMenuItem new];
    [bar addItem:m_item];
    NSMenu *m = [[NSMenu alloc] initWithTitle:@"Machine"];
    [m addItemWithTitle:@"Insert Floppy in :0..." action:@selector(insert0:) keyEquivalent:@"o"];
    NSMenuItem *i1 = [m addItemWithTitle:@"Insert Floppy in :1..." action:@selector(insert1:) keyEquivalent:@"o"];
    i1.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [m addItemWithTitle:@"Eject :0" action:@selector(eject0:) keyEquivalent:@"e"];
    NSMenuItem *e1 = [m addItemWithTitle:@"Eject :1" action:@selector(eject1:) keyEquivalent:@"e"];
    e1.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [m addItem:NSMenuItem.separatorItem];
    [m addItemWithTitle:@"Turbo" action:@selector(toggleTurbo:) keyEquivalent:@"t"];
    NSMenuItem *snd = [m addItemWithTitle:@"Sound On/Off" action:@selector(toggleSound:) keyEquivalent:@"m"];
    snd.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [m addItemWithTitle:@"Release Mouse" action:@selector(releaseMouse:) keyEquivalent:@"\e"];
    NSMenuItem *rm = [m addItemWithTitle:@"Right Button Is Menu" action:@selector(toggleRightMenu:) keyEquivalent:@""];
    rm.state = app.right_menu ? NSControlStateValueOn : NSControlStateValueOff;
    [m addItem:NSMenuItem.separatorItem];
    NSMenuItem *rst = [m addItemWithTitle:@"Reset" action:@selector(resetMachine:) keyEquivalent:@"r"];
    rst.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    m_item.submenu = m;
    NSApp.mainMenu = bar;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        ArchieConfig cfg = { NULL, 4, NULL, { NULL, NULL }, 8, NULL };
        app.mhz = 8;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--rom") && i + 1 < argc) cfg.rom_path = argv[++i];
            else if (!strcmp(argv[i], "--floppy") && i + 1 < argc) cfg.floppy[0] = argv[++i];
            else if (!strcmp(argv[i], "--floppy2") && i + 1 < argc) cfg.floppy[1] = argv[++i];
            else if (!strcmp(argv[i], "--ram") && i + 1 < argc) cfg.ram_mb = (uint32_t)atoi(argv[++i]);
            else if (!strcmp(argv[i], "--mhz") && i + 1 < argc) app.mhz = atof(argv[++i]);
            else if (!strcmp(argv[i], "--cmos") && i + 1 < argc) cfg.cmos_path = argv[++i];
            else if (!strcmp(argv[i], "--hostfs") && i + 1 < argc) cfg.hostfs_dir = argv[++i];
            else if (argv[i][0] != '-' && !cfg.floppy[0]) cfg.floppy[0] = argv[i];
        }
        if (app.mhz <= 0) app.mhz = 8;
        cfg.mhz = app.mhz;
        app.right_menu = [NSUserDefaults.standardUserDefaults boolForKey:@"RightButtonIsMenu"];

        [NSApplication sharedApplication];
        NSApp.activationPolicy = NSApplicationActivationPolicyRegular;
        [NSApp activateIgnoringOtherApps:YES];

        static char rom_buf[PATH_MAX], cmos_buf[PATH_MAX];
        if (!cfg.rom_path && !(cfg.rom_path = find_rom(rom_buf, sizeof rom_buf))) return 1;
        if (!cfg.cmos_path) {
            /* la CMOS si salva accanto alla ROM, una per ogni versione */
            snprintf(cmos_buf, sizeof cmos_buf, "%s.cmos", cfg.rom_path);
            cfg.cmos_path = cmos_buf;
        }
        snprintf(app.rom_name, sizeof app.rom_name, "%s", base_name(cfg.rom_path));
        if (!strcmp(app.rom_name, "ROM311")) snprintf(app.rom_name, sizeof app.rom_name, "RISC OS 3.11");
        for (int d = 0; d < 2; d++)
            if (cfg.floppy[d]) snprintf(app.floppy_name[d], sizeof app.floppy_name[d], "%s", base_name(cfg.floppy[d]));

        /* cartella iniziale per i dischetti: ADF accanto all'app, altrimenti in Documents */
        NSString *adf = [app_folder() stringByAppendingPathComponent:@"ADF"];
        BOOL isdir = NO;
        if (![NSFileManager.defaultManager fileExistsAtPath:adf isDirectory:&isdir] || !isdir) adf = user_folder(@"ADF");
        snprintf(adf_dir, sizeof adf_dir, "%s", adf.fileSystemRepresentation);

        /* il disco HostFS: accanto all'app se c'e' una cartella HostFS, altrimenti in Documents */
        static char hostfs_buf[PATH_MAX];
        if (!cfg.hostfs_dir) {
            NSString *hfs = [app_folder() stringByAppendingPathComponent:@"HostFS"];
            if (![NSFileManager.defaultManager fileExistsAtPath:hfs isDirectory:&isdir] || !isdir) hfs = user_folder(@"HostFS");
            snprintf(hostfs_buf, sizeof hostfs_buf, "%s", hfs.fileSystemRepresentation);
            cfg.hostfs_dir = hostfs_buf;
        }

        char err[300];
        if (!archie_create(&app.a, &cfg, err, sizeof err)) {
            show_message(@"Archimedes", [NSString stringWithUTF8String:err]);
            return 1;
        }

        AppDelegate *delegate = [AppDelegate new];
        NSApp.delegate = delegate;
        build_menu();

        NSImage *blank = [[NSImage alloc] initWithSize:NSMakeSize(1, 1)];
        blank_cursor = [[NSCursor alloc] initWithImage:blank hotSpot:NSZeroPoint];

        window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1280, 1024)
                                             styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                       NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                               backing:NSBackingStoreBuffered defer:NO];
        window.delegate = delegate;
        view = [[EmuView alloc] initWithFrame:NSMakeRect(0, 0, 1280, 1024)];
        [view registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
        window.contentView = view;
        window.acceptsMouseMovedEvents = YES;
        [window makeFirstResponder:view];
        fit_window();
        [window center];
        update_title();
        [window makeKeyAndOrderFront:nil];

        keys_init(&keys, &app.a.kbd);
        audio_open();
        app.next = now_s();
        NSTimer *t = [NSTimer timerWithTimeInterval:0.004 target:delegate selector:@selector(tick:)
                                           userInfo:nil repeats:YES];
        [NSRunLoop.currentRunLoop addTimer:t forMode:NSRunLoopCommonModes];
        [NSApp run];
    }
    return 0;
}
