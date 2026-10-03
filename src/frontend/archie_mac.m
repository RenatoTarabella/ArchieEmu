/*
 * archie_mac.m - Finestra delle macchine Archimedes e Risc PC per macOS (Cocoa).
 *
 * Senza --rom si apre una finestra iniziale per scegliere macchina, ROM
 * (trovate nella cartella roms accanto all'app o in Documents/ArchieEmu/
 * roms), processore e memoria, ricordate nelle preferenze.
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
#include "riscpc/riscpc.h"
#include "riscpc/hdformat.h"
#include "romlist.h"
#include "mac_keys.h"

#define FRAME_HZ   50
#define TURBO_MHZ  64.0
#define BUF_W      2048
#define BUF_H      2048

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

static uint32_t machine_audio(int16_t *out, uint32_t max);

/* prende i campioni prodotti dalla macchina e li mette nell'anello */
static void audio_pump(void)
{
    int16_t tmp[ARCHIE_AUDIO_HZ / 50 * 2];
    uint32_t got;
    while ((got = machine_audio(tmp, ARCHIE_AUDIO_HZ / 50)) > 0) {
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
    RiscPc    r;
    int       rpc;                   /* la macchina e' il Risc PC */
    int       rpc_buttons;           /* tasti del mouse premuti (bit 0 Adjust, 1 Menu, 2 Select) */
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
    int       free_armed;            /* Ctrl+Option premuti da soli */
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

/* campioni della macchina (48 kHz stereo per tutte e due) */
static uint32_t machine_audio(int16_t *out, uint32_t max)
{
    return app.rpc ? riscpc_audio_read(&app.r, out, max) : archie_audio_read(&app.a, out, max);
}

/* le unita' floppy: WD1772 dell'Archimedes o 82077 del Risc PC */
static Fdc *floppies(void) { return app.rpc ? &app.r.sio.fdc.media : &app.a.fdc; }

/* code: tasto dell'Archimedes (&70 Select, &71 Menu, &72 Adjust) */
static void mouse_button(int code, int down)
{
    if (!app.rpc) { kbd_key(&app.a.kbd, code, down); return; }
    int bit = code == 0x70 ? 4 : code == 0x71 ? 2 : 1;
    app.rpc_buttons = down ? app.rpc_buttons | bit : app.rpc_buttons & ~bit;
    riscpc_mouse_buttons(&app.r, app.rpc_buttons);
}

static void mouse_move(int dx, int dy)
{
    if (app.rpc) riscpc_mouse_move(&app.r, dx, dy);
    else         kbd_mouse_move(&app.a.kbd, dx, dy);
}

static void rpc_key(void *ctx, int code, int down) { riscpc_key((RiscPc *)ctx, (uint32_t)code, down); }

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
    snprintf(t, sizeof t, "%s - %s%s", app.rpc ? "Risc PC" : "Archimedes", app.rom_name,
             app.captured ? "   (Ctrl+Option: free mouse)" : "");
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
    fdc_eject(floppies(), drive);
    if (fdc_insert(floppies(), drive, path)) {
        snprintf(app.floppy_name[drive], sizeof app.floppy_name[drive], "%s", base_name(path));
    } else {
        app.floppy_name[drive][0] = 0;
        show_message(@"Archimedes", [NSString stringWithFormat:@"Unrecognised image:\n%s\n\n"
                                     "An ADFS .adf image (800 KB or 640 KB) or an .hfe is needed; on the Risc PC also "
                                     "ADFS F (1.6 MB) and DOS (720 KB, 1.44 MB).", path]);
    }
    update_title();
}

/* dischetti cambiati da RISC OS (*HostFS_Insert): il titolo li segue */
static void sync_floppy_names(void)
{
    for (int d = 0; d < 2; d++) {
        const FdcDrive *fd = &floppies()->drive[d];
        const char *name = fd->image ? base_name(fd->path) : "";
        if (strcmp(name, app.floppy_name[d])) {
            snprintf(app.floppy_name[d], sizeof app.floppy_name[d], "%s", name);
            update_title();
        }
    }
}

static void eject_floppy(int drive)
{
    fdc_eject(floppies(), drive);
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

/* Disco fisso del Risc PC: un'immagine esistente (new_mb = 0) o una nuova
   di new_mb MB gia' formattata ADFS (hdformat.c); la macchina riparte. */
static void choose_hd(int new_mb)
{
    capture_mouse(0);
    NSString *path = nil;
    if (new_mb) {
        NSSavePanel *p = [NSSavePanel savePanel];
        p.title = @"New hard disc image";
        p.nameFieldStringValue = @"HardDisc4.hdf";
        if ([p runModal] != NSModalResponseOK) { release_modifiers(); return; }
        path = p.URL.path;
        if (!hdf_create(path.fileSystemRepresentation, (uint32_t)new_mb, "HardDisc4", 0)) {
            show_message(@"Risc PC", @"Could not create the image.");
            return;
        }
    } else {
        NSOpenPanel *p = [NSOpenPanel openPanel];
        p.title = @"Hard disc image for drive :4";
        if ([p runModal] != NSModalResponseOK) { release_modifiers(); return; }
        path = p.URL.path;
    }
    release_modifiers();
    [window makeKeyAndOrderFront:nil];
    if (!riscpc_attach_hd(&app.r, path.fileSystemRepresentation)) {
        show_message(@"Risc PC", @"Not a hard disc image.");
        return;
    }
    [NSUserDefaults.standardUserDefaults setObject:path forKey:@"HardDiscRiscPC"];
    riscpc_reset(&app.r);
}

static void toggle_turbo(void)
{
    app.turbo = !app.turbo;
    if (app.rpc) riscpc_set_mhz(&app.r, app.turbo ? 200.0 : app.mhz);
    else         archie_set_mhz(&app.a, app.turbo ? TURBO_MHZ : app.mhz);
    update_title();
}

static void mouse_moved(NSEvent *e)
{
    if (app.captured) {
        int dx = (int)e.deltaX, dy = (int)e.deltaY;
        if (dx || dy) mouse_move(dx, -dy);
        return;
    }
    NSPoint p = e.locationInWindow;
    if (app.have_last && app.img_w > 0) {
        /* unita' del mouse Archimedes: circa 2 unita' OS per passo; lo
           schermo e' largo 1280 unita' OS */
        double k = 1280.0 / 2.0 / app.img_w;
        /* Risc PC: RISC OS muove il puntatore di 1,5 pixel per passo */
        if (app.rpc) k = (app.disp_w ? app.disp_w : 640) / 1.5 / app.img_w;
        app.acc_x += (p.x - app.last.x) * k;
        app.acc_y += (p.y - app.last.y) * k;            /* in Cocoa la y cresce verso l'alto */
        int dx = (int)app.acc_x, dy = (int)app.acc_y;
        if (dx || dy) {
            mouse_move(dx, dy);
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
    uint32_t border;
    if (app.rpc) {
        riscpc_render(&app.r, app.pixels, BUF_W, &w, &h);
        border = riscpc_border_rgb(&app.r);
    } else {
        VidcTiming t;
        vidc_timing(&app.a.vidc, &t);
        if (t.valid) archie_render(&app.a, app.pixels, BUF_W, &w, &h);
        border = vidc_border_rgb(&app.a.vidc);
    }
    if (w <= 0 || h <= 0) { w = 640; h = app.rpc ? 480 : 256; memset(app.pixels, 0, (size_t)BUF_W * (size_t)h * 4); }
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
    app.free_armed = 0;
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
    /* Ctrl+Option premuti e rilasciati da soli liberano il mouse */
    int ctrl = (f & NSEventModifierFlagControl) != 0, opt = (f & NSEventModifierFlagOption) != 0;
    if (ctrl && opt) app.free_armed = 1;
    else if (app.free_armed && !(ctrl && opt)) { app.free_armed = 0; capture_mouse(0); }
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
    mouse_button(0x70, 1);
}
- (void)mouseUp:(NSEvent *)e { (void)e; mouse_button(0x70, 0); }
/* come sull'Archimedes: centrale = Menu, destro = Adjust; con "Right Button
   Is Menu" (trackpad, mouse a due tasti) il destro fa Menu */
- (void)rightMouseDown:(NSEvent *)e { (void)e; mouse_button(app.right_menu ? 0x71 : 0x72, 1); }
- (void)rightMouseUp:(NSEvent *)e { (void)e; mouse_button(app.right_menu ? 0x71 : 0x72, 0); }
- (void)otherMouseDown:(NSEvent *)e { (void)e; mouse_button(app.right_menu ? 0x72 : 0x71, 1); }
- (void)otherMouseUp:(NSEvent *)e { (void)e; mouse_button(app.right_menu ? 0x72 : 0x71, 0); }

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
- (void)resetMachine:(id)s
{
    (void)s;
    if (app.rpc) riscpc_reset(&app.r);
    else         archie_reset(&app.a);
}
- (void)chooseMachine:(id)s
{
    (void)s;
    /* un'istanza nuova con la finestra iniziale; questa si chiude */
    NSWorkspaceOpenConfiguration *c = [NSWorkspaceOpenConfiguration configuration];
    c.createsNewApplicationInstance = YES;
    c.arguments = @[ @"--choose" ];
    [NSWorkspace.sharedWorkspace openApplicationAtURL:NSBundle.mainBundle.bundleURL configuration:c
                                    completionHandler:^(NSRunningApplication *a, NSError *err) {
        (void)a;
        if (!err) dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
    }];
}
- (void)openHardDisc:(id)s { (void)s; choose_hd(0); }
- (void)newHardDisc:(NSMenuItem *)item { choose_hd((int)item.tag); }
- (void)removeHardDisc:(id)s
{
    (void)s;
    capture_mouse(0);
    NSAlert *al = [NSAlert new];
    al.messageText = @"Removing the hard disc restarts the machine.";
    [al addButtonWithTitle:@"Restart"];
    [al addButtonWithTitle:@"Cancel"];
    if ([al runModal] != NSAlertFirstButtonReturn) return;
    riscpc_detach_hd(&app.r);
    [NSUserDefaults.standardUserDefaults removeObjectForKey:@"HardDiscRiscPC"];
    riscpc_reset(&app.r);
}
- (void)setRam:(NSMenuItem *)item
{
    uint32_t mb = (uint32_t)item.tag;
    if (mb == app.a.ram_size >> 20) return;
    capture_mouse(0);
    NSAlert *al = [NSAlert new];
    al.messageText = @"Changing the RAM restarts the machine.";
    [al addButtonWithTitle:@"Restart"];
    [al addButtonWithTitle:@"Cancel"];
    if ([al runModal] != NSAlertFirstButtonReturn) return;
    archie_set_ram(&app.a, mb);
    for (NSMenuItem *i in item.menu.itemArray)
        if (i.action == @selector(setRam:))
            i.state = i.tag == (NSInteger)mb ? NSControlStateValueOn : NSControlStateValueOff;
}
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
    if (app.rpc) riscpc_destroy(&app.r);
    else         archie_destroy(&app.a);
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
    if (app.rpc) riscpc_run(&app.r, ARC_MS(1000 / FRAME_HZ));
    else         archie_run(&app.a, ARC_MS(1000 / FRAME_HZ));
    audio_pump();
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
    [app_menu addItemWithTitle:@"Quit ArchieEmu" action:@selector(terminate:) keyEquivalent:@"q"];
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
    if (app.rpc) {
        [m addItemWithTitle:@"Hard Disc Image (:4)..." action:@selector(openHardDisc:) keyEquivalent:@""];
        NSMenuItem *nh = [m addItemWithTitle:@"New Hard Disc Image" action:nil keyEquivalent:@""];
        NSMenu *sizes = [[NSMenu alloc] initWithTitle:@"New Hard Disc Image"];
        for (int mb = 64; mb <= 512; mb *= 2) {
            NSMenuItem *it = [sizes addItemWithTitle:[NSString stringWithFormat:@"%d MB...", mb]
                                              action:@selector(newHardDisc:) keyEquivalent:@""];
            it.tag = mb;
        }
        nh.submenu = sizes;
        [m addItemWithTitle:@"Remove Hard Disc" action:@selector(removeHardDisc:) keyEquivalent:@""];
    } else {
        for (int mb = 1; mb <= 4; mb *= 2) {
            NSString *t = mb == 1 ? @"RAM 1 MB (A3000)" : [NSString stringWithFormat:@"RAM %d MB", mb];
            NSMenuItem *r = [m addItemWithTitle:t action:@selector(setRam:) keyEquivalent:@""];
            r.tag = mb;
            r.state = (app.a.ram_size >> 20) == (uint32_t)mb ? NSControlStateValueOn : NSControlStateValueOff;
        }
    }
    [m addItem:NSMenuItem.separatorItem];
    NSMenuItem *rst = [m addItemWithTitle:@"Reset" action:@selector(resetMachine:) keyEquivalent:@"r"];
    rst.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [m addItemWithTitle:@"Choose Another Machine..." action:@selector(chooseMachine:) keyEquivalent:@""];
    m_item.submenu = m;
    NSApp.mainMenu = bar;
}

/* ------------------------------------------------------------------ */
/* finestra iniziale: quale macchina                                   */
/* ------------------------------------------------------------------ */

typedef struct MacRom { char path[PATH_MAX]; char label[160]; int version, riscpc; } MacRom;
static MacRom roms[128];
static int nroms;

static int cmp_rom(const void *a, const void *b)
{
    const MacRom *x = a, *y = b;
    return x->version != y->version ? x->version - y->version : strcmp(x->path, y->path);
}

static void scan_rom_dir(NSString *dir, NSString *sub)
{
    NSString *d = sub ? [dir stringByAppendingPathComponent:sub] : dir;
    for (NSString *f in [NSFileManager.defaultManager contentsOfDirectoryAtPath:d error:nil]) {
        NSString *full = [d stringByAppendingPathComponent:f];
        BOOL isdir = NO;
        if (![NSFileManager.defaultManager fileExistsAtPath:full isDirectory:&isdir] || isdir) continue;
        int rpc, v = romlist_version(f.UTF8String, &rpc);
        if (!v || nroms >= 128) continue;
        MacRom *r = &roms[nroms++];
        snprintf(r->path, sizeof r->path, "%s", full.fileSystemRepresentation);
        char name[64];
        romlist_name(v, name, sizeof name);
        const char *extra = f.UTF8String + 6;
        snprintf(r->label, sizeof r->label, "%s%s%s   (%s)", name, *extra ? " " : "", extra,
                 sub ? sub.UTF8String : "roms");
        r->version = v;
        r->riscpc = rpc;
    }
}

/* la cartella roms accanto all'app (o piu' su) e Documents/ArchieEmu/roms */
static void scan_roms(void)
{
    nroms = 0;
    NSMutableArray *dirs = [NSMutableArray new];
    for (NSString *r in @[ @"roms", @"../roms", @"../../roms", @"../../../roms" ])
        [dirs addObject:[app_folder() stringByAppendingPathComponent:r].stringByStandardizingPath];
    [dirs addObject:user_folder(@"roms")];
    NSMutableSet *seen = [NSMutableSet new];
    for (NSString *d in dirs) {
        BOOL isdir = NO;
        if (![NSFileManager.defaultManager fileExistsAtPath:d isDirectory:&isdir] || !isdir) continue;
        NSString *real = d.stringByResolvingSymlinksInPath;
        if ([seen containsObject:real]) continue;
        [seen addObject:real];
        scan_rom_dir(d, nil);
        for (NSString *sub in [NSFileManager.defaultManager contentsOfDirectoryAtPath:d error:nil]) {
            NSString *full = [d stringByAppendingPathComponent:sub];
            if (![NSFileManager.defaultManager fileExistsAtPath:full isDirectory:&isdir] || !isdir) continue;
            if ([sub rangeOfString:@"NCOS"].location != NSNotFound) continue;   /* Network Computer */
            scan_rom_dir(d, sub);
        }
    }
    qsort(roms, (size_t)nroms, sizeof roms[0], cmp_rom);
}

typedef struct MachineChoice {
    int  riscpc, cpu, ram_mb, vram_mb;         /* cpu: 0 ARM610, 1 ARM710, 2 StrongARM */
    char rom[PATH_MAX], rom_name[64];
} MachineChoice;

@interface SplashController : NSObject
@property NSButton *archie, *riscpc, *start;
@property NSPopUpButton *rom, *cpu, *ram, *vram;
@property NSTextField *vramLabel;
@property int rpc;
@property NSMutableArray<NSNumber *> *romIndex;
@end

@implementation SplashController

- (void)fill
{
    NSUserDefaults *u = NSUserDefaults.standardUserDefaults;
    int rpc = self.rpc;
    NSString *want = [u stringForKey:rpc ? @"RomRiscPC" : @"RomArchimedes"];
    [self.rom removeAllItems];
    self.romIndex = [NSMutableArray new];
    NSInteger sel = -1, fallback = -1;
    for (int i = 0; i < nroms; i++) {
        if (roms[i].riscpc != rpc) continue;
        /* direttamente nel menu: addItemWithTitle scarterebbe i titoli uguali */
        [self.rom.menu addItemWithTitle:[NSString stringWithUTF8String:roms[i].label] action:nil keyEquivalent:@""];
        if (want && !strcmp(want.fileSystemRepresentation, roms[i].path)) sel = (NSInteger)self.romIndex.count;
        if (roms[i].version == (rpc ? 350 : 311) && fallback < 0) fallback = (NSInteger)self.romIndex.count;
        [self.romIndex addObject:@(i)];
    }
    if (sel < 0) sel = fallback >= 0 ? fallback : (NSInteger)self.romIndex.count - 1;
    if (sel >= 0) [self.rom selectItemAtIndex:sel];

    [self.cpu removeAllItems];
    if (rpc) {
        [self.cpu addItemsWithTitles:@[ @"ARM610, 30 MHz (Risc PC 600)", @"ARM710, 40 MHz (Risc PC 700)",
                                        @"StrongARM SA-110 (RISC OS 3.7 only)" ]];
        NSInteger cpu = [u integerForKey:@"CPU"];
        [self.cpu selectItemAtIndex:cpu >= 0 && cpu <= 2 ? cpu : 0];
    } else {
        [self.cpu addItemWithTitle:@"ARM2, 8 MHz"];
    }
    self.cpu.enabled = rpc;

    [self.ram removeAllItems];
    NSArray *sizes = rpc ? @[ @4, @8, @16, @32, @64 ] : @[ @1, @2, @4 ];
    NSInteger want_ram = [u integerForKey:rpc ? @"RamRiscPC" : @"RamArchimedes"];
    if (!want_ram) want_ram = rpc ? 16 : 4;
    for (NSNumber *n in sizes) {
        [self.ram addItemWithTitle:[NSString stringWithFormat:@"%@ MB%@", n,
                                    !rpc && n.intValue == 1 ? @" (A3000)" : @""]];
        if (n.integerValue == want_ram) [self.ram selectItemAtIndex:self.ram.numberOfItems - 1];
    }

    [self.vram removeAllItems];
    [self.vram addItemsWithTitles:@[ @"None (screen in DRAM)", @"1 MB", @"2 MB" ]];
    NSInteger v = [u objectForKey:@"VRAM"] ? [u integerForKey:@"VRAM"] : 2;
    [self.vram selectItemAtIndex:v >= 0 && v <= 2 ? v : 2];
    self.vram.enabled = rpc;
    self.vramLabel.textColor = rpc ? NSColor.labelColor : NSColor.disabledControlTextColor;
    self.start.enabled = self.romIndex.count > 0;
}

- (void)machineChanged:(NSButton *)b
{
    self.rpc = b == self.riscpc;
    self.archie.state = self.rpc ? NSControlStateValueOff : NSControlStateValueOn;
    self.riscpc.state = self.rpc ? NSControlStateValueOn : NSControlStateValueOff;
    [self fill];
}

- (void)startPressed:(id)s { (void)s; [NSApp stopModalWithCode:NSModalResponseOK]; }
- (void)quitPressed:(id)s { (void)s; [NSApp stopModalWithCode:NSModalResponseCancel]; }

@end

static NSTextField *label(NSString *text, NSRect r, NSFont *font)
{
    NSTextField *t = [NSTextField labelWithString:text];
    t.frame = r;
    t.font = font;
    return t;
}

/* ritorna 0 se l'utente rinuncia (o non ci sono ROM: allora found = 0) */
static int run_splash(MachineChoice *c, int *found)
{
    scan_roms();
    *found = nroms > 0;
    if (!nroms) return 0;
    int have[2] = { 0, 0 };
    for (int i = 0; i < nroms; i++) have[roms[i].riscpc] = 1;

    NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 480, 340)
                                              styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    w.title = @"ArchieEmu";
    NSView *v = w.contentView;
    SplashController *sc = [SplashController new];
    NSFont *f = [NSFont systemFontOfSize:13];
    [v addSubview:label(@"ArchieEmu", NSMakeRect(20, 290, 300, 32), [NSFont systemFontOfSize:24 weight:NSFontWeightSemibold])];
    [v addSubview:label(@"Choose the machine to start", NSMakeRect(22, 266, 400, 20), f)];
    sc.archie = [NSButton radioButtonWithTitle:@"Acorn Archimedes   (ARM2, Arthur and RISC OS up to 3.11)"
                                        target:sc action:@selector(machineChanged:)];
    sc.archie.frame = NSMakeRect(20, 234, 440, 22);
    sc.riscpc = [NSButton radioButtonWithTitle:@"Acorn Risc PC   (ARM6/ARM7/StrongARM, RISC OS 3.5 to 3.7)"
                                        target:sc action:@selector(machineChanged:)];
    sc.riscpc.frame = NSMakeRect(20, 210, 440, 22);
    sc.archie.enabled = have[0];
    sc.riscpc.enabled = have[1];
    [v addSubview:sc.archie];
    [v addSubview:sc.riscpc];

    int y = 170;
    [v addSubview:label(@"RISC OS", NSMakeRect(20, y + 3, 95, 20), f)];
    sc.rom = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(118, y, 342, 26) pullsDown:NO];
    [v addSubview:sc.rom];
    y -= 34;
    [v addSubview:label(@"Processor", NSMakeRect(20, y + 3, 95, 20), f)];
    sc.cpu = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(118, y, 342, 26) pullsDown:NO];
    [v addSubview:sc.cpu];
    y -= 34;
    [v addSubview:label(@"RAM", NSMakeRect(20, y + 3, 95, 20), f)];
    sc.ram = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(118, y, 170, 26) pullsDown:NO];
    [v addSubview:sc.ram];
    y -= 34;
    sc.vramLabel = label(@"VRAM", NSMakeRect(20, y + 3, 95, 20), f);
    [v addSubview:sc.vramLabel];
    sc.vram = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(118, y, 170, 26) pullsDown:NO];
    [v addSubview:sc.vram];

    sc.start = [NSButton buttonWithTitle:@"Start" target:sc action:@selector(startPressed:)];
    sc.start.frame = NSMakeRect(270, 16, 96, 30);
    sc.start.keyEquivalent = @"\r";
    NSButton *quit = [NSButton buttonWithTitle:@"Quit" target:sc action:@selector(quitPressed:)];
    quit.frame = NSMakeRect(370, 16, 96, 30);
    quit.keyEquivalent = @"\e";
    [v addSubview:sc.start];
    [v addSubview:quit];

    int rpc = [NSUserDefaults.standardUserDefaults boolForKey:@"RiscPC"] ? 1 : 0;
    if (!have[rpc]) rpc = !rpc;
    [sc machineChanged:rpc ? sc.riscpc : sc.archie];
    [w center];
    NSModalResponse r = [NSApp runModalForWindow:w];
    [w orderOut:nil];
    if (r != NSModalResponseOK || sc.rom.indexOfSelectedItem < 0) return 0;
    /* lo StrongARM vuole RISC OS 3.7: con le ROM precedenti non parte (come sul vero) */
    while (sc.rpc && sc.cpu.indexOfSelectedItem == 2 &&
           roms[sc.romIndex[(NSUInteger)sc.rom.indexOfSelectedItem].intValue].version < 370) {
        NSAlert *al = [NSAlert new];
        al.messageText = @"The StrongARM needs RISC OS 3.7";
        al.informativeText = @"RISC OS 3.5 and 3.6 do not run on a StrongARM, as on the real Risc PC. "
                              "Choose RISC OS 3.70 or 3.71, or an ARM610/ARM710 processor.";
        [al runModal];
        [w makeKeyAndOrderFront:nil];
        r = [NSApp runModalForWindow:w];
        [w orderOut:nil];
        if (r != NSModalResponseOK || sc.rom.indexOfSelectedItem < 0) return 0;
    }

    const MacRom *rom = &roms[sc.romIndex[(NSUInteger)sc.rom.indexOfSelectedItem].intValue];
    c->riscpc = sc.rpc;
    snprintf(c->rom, sizeof c->rom, "%s", rom->path);
    romlist_name(rom->version, c->rom_name, sizeof c->rom_name);
    c->cpu = sc.rpc ? (int)sc.cpu.indexOfSelectedItem : 0;
    c->ram_mb = [sc.ram.titleOfSelectedItem intValue];
    c->vram_mb = sc.rpc ? (int)sc.vram.indexOfSelectedItem : 0;

    NSUserDefaults *u = NSUserDefaults.standardUserDefaults;
    [u setBool:c->riscpc forKey:@"RiscPC"];
    [u setObject:[NSString stringWithUTF8String:c->rom] forKey:c->riscpc ? @"RomRiscPC" : @"RomArchimedes"];
    [u setInteger:c->ram_mb forKey:c->riscpc ? @"RamRiscPC" : @"RamArchimedes"];
    if (c->riscpc) { [u setInteger:c->cpu forKey:@"CPU"]; [u setInteger:c->vram_mb forKey:@"VRAM"]; }
    return 1;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        ArchieConfig cfg = { NULL, 4, NULL, { NULL, NULL }, 8, NULL };
        int force_rpc = 0, arm710 = 0, strongarm = 0, choose = 0, vram = -1;
        uint32_t ram = 0;
        double mhz = 0;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--rom") && i + 1 < argc) cfg.rom_path = argv[++i];
            else if (!strcmp(argv[i], "--floppy") && i + 1 < argc) cfg.floppy[0] = argv[++i];
            else if (!strcmp(argv[i], "--floppy2") && i + 1 < argc) cfg.floppy[1] = argv[++i];
            else if (!strcmp(argv[i], "--ram") && i + 1 < argc) ram = (uint32_t)atoi(argv[++i]);
            else if (!strcmp(argv[i], "--vram") && i + 1 < argc) vram = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--mhz") && i + 1 < argc) mhz = atof(argv[++i]);
            else if (!strcmp(argv[i], "--cmos") && i + 1 < argc) cfg.cmos_path = argv[++i];
            else if (!strcmp(argv[i], "--hostfs") && i + 1 < argc) cfg.hostfs_dir = argv[++i];
            else if (!strcmp(argv[i], "--riscpc")) force_rpc = 1;
            else if (!strcmp(argv[i], "--arm710")) arm710 = 1;
            else if (!strcmp(argv[i], "--strongarm")) strongarm = 1;
            else if (!strcmp(argv[i], "--choose")) choose = 1;
            else if (argv[i][0] != '-' && !cfg.floppy[0]) cfg.floppy[0] = argv[i];
        }
        app.right_menu = [NSUserDefaults.standardUserDefaults boolForKey:@"RightButtonIsMenu"];

        [NSApplication sharedApplication];
        NSApp.activationPolicy = NSApplicationActivationPolicyRegular;
        [NSApp activateIgnoringOtherApps:YES];

        /* quale macchina: la finestra iniziale, oppure --rom (la macchina dal nome della ROM) */
        static char rom_buf[PATH_MAX], cmos_buf[PATH_MAX];
        MachineChoice mc;
        memset(&mc, 0, sizeof mc);
        if (!cfg.rom_path && (choose || !cfg.floppy[0])) {
            int found = 0;
            if (run_splash(&mc, &found)) {
                snprintf(rom_buf, sizeof rom_buf, "%s", mc.rom);
                cfg.rom_path = rom_buf;
                app.rpc = mc.riscpc;
                if (!ram) ram = (uint32_t)mc.ram_mb;
                if (vram < 0) vram = mc.vram_mb;
                arm710 = mc.cpu == 1;
                strongarm = mc.cpu == 2;
                snprintf(app.rom_name, sizeof app.rom_name, "%s", mc.rom_name);
            } else if (found) {
                return 0;                                       /* Quit */
            }
        }
        if (!cfg.rom_path) {
            /* nessuna ROM nelle cartelle: la si chiede (RISC OS 3.11 per l'Archimedes) */
            if (!(cfg.rom_path = find_rom(rom_buf, sizeof rom_buf))) return 1;
        }
        if (!app.rom_name[0]) {
            int rpc = 0, v = romlist_version(cfg.rom_path, &rpc);
            app.rpc = rpc || force_rpc;
            if (v) romlist_name(v, app.rom_name, sizeof app.rom_name);
            else   snprintf(app.rom_name, sizeof app.rom_name, "%s", base_name(cfg.rom_path));
        }
        if (!cfg.cmos_path) {
            /* la CMOS si salva accanto alla ROM, una per ogni versione */
            snprintf(cmos_buf, sizeof cmos_buf, "%s.cmos", cfg.rom_path);
            cfg.cmos_path = cmos_buf;
        }
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
            if (![NSFileManager.defaultManager fileExistsAtPath:hfs isDirectory:&isdir] || !isdir) {
                NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
                BOOL fresh = ![NSFileManager.defaultManager fileExistsAtPath:
                               [docs stringByAppendingPathComponent:@"ArchieEmu/HostFS"]];
                hfs = user_folder(@"HostFS");
                /* cartella nuova: !Boot e l'MDF del Risc PC dalle risorse dell'app */
                NSString *seed = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"HostFS"];
                if (fresh)
                    for (NSString *f in [NSFileManager.defaultManager contentsOfDirectoryAtPath:seed error:nil])
                        [NSFileManager.defaultManager copyItemAtPath:[seed stringByAppendingPathComponent:f]
                                                              toPath:[hfs stringByAppendingPathComponent:f] error:nil];
            }
            snprintf(hostfs_buf, sizeof hostfs_buf, "%s", hfs.fileSystemRepresentation);
            cfg.hostfs_dir = hostfs_buf;
        }

        char err[300];
        if (app.rpc) {
            RiscPcConfig rcfg = { 0 };
            rcfg.rom_path = cfg.rom_path;
            rcfg.cmos_path = cfg.cmos_path;
            rcfg.ram_mb = ram ? ram : 16;
            rcfg.vram_mb = vram >= 0 ? (uint32_t)vram : 2;
            rcfg.arm710 = arm710;
            rcfg.strongarm = strongarm;
            rcfg.mhz = mhz > 0 ? mhz : strongarm ? 100 : arm710 ? 40 : 30;
            rcfg.hostfs_dir = cfg.hostfs_dir;
            app.mhz = rcfg.mhz;
            if (!riscpc_create(&app.r, &rcfg, err, sizeof err)) {
                show_message(@"Risc PC", [NSString stringWithUTF8String:err]);
                return 1;
            }
            for (int d = 0; d < 2; d++)
                if (cfg.floppy[d]) riscpc_insert_floppy(&app.r, d, cfg.floppy[d]);
            NSString *hd = [NSUserDefaults.standardUserDefaults stringForKey:@"HardDiscRiscPC"];
            if (hd.length) riscpc_attach_hd(&app.r, hd.fileSystemRepresentation);
        } else {
            cfg.ram_mb = ram ? ram : 4;
            app.mhz = mhz > 0 ? mhz : 8;
            cfg.mhz = app.mhz;
            if (!archie_create(&app.a, &cfg, err, sizeof err)) {
                show_message(@"Archimedes", [NSString stringWithUTF8String:err]);
                return 1;
            }
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

        if (app.rpc) keys_init_ps2(&keys, rpc_key, &app.r);
        else         keys_init(&keys, &app.a.kbd);
        audio_open();
        app.next = now_s();
        NSTimer *t = [NSTimer timerWithTimeInterval:0.004 target:delegate selector:@selector(tick:)
                                           userInfo:nil repeats:YES];
        [NSRunLoop.currentRunLoop addTimer:t forMode:NSRunLoopCommonModes];
        [NSApp run];
    }
    return 0;
}
