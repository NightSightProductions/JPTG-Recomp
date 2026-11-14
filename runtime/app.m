// Native macOS host: replaces the game's MainController/MyOpenGLView Cocoa glue and drives the
// recompiled engine through its C API in the same order the original did.
// usage: JurassicPark [ORIG_BINARY RESOURCES_DIR]
#define GL_SILENCE_DEPRECATION
#import <Cocoa/Cocoa.h>
#import <OpenGL/gl.h>
#import <OpenGL/glext.h>
#include "rt.h"
#include "tables.h"

void platform_set_resources(const char *dir);
void steam_set_app_id(uint32_t id);

static CPU *gcpu;
static uint32_t g_args;   // guest scratch: [0]=w [4]=h [8]=windowed [12]=empty cmdline
static uint32_t fn(const char *s) { return guest_func(s); }
static uint32_t call0(const char *s) { return guest_call(gcpu, fn(s), 0, NULL); }
static uint32_t call1(const char *s, uint32_t a) { uint32_t v[1] = { a }; return guest_call(gcpu, fn(s), 1, v); }

void host_present(void) { [[NSOpenGLContext currentContext] flushBuffer]; }

static BOOL engine_start(void) {
    wr8(g_args + 8, 1);
    uint32_t a[4] = { g_args + 12, g_args, g_args + 4, g_args + 8 };
    return (guest_call(gcpu, fn("_GameEngine_Start"), 4, a) & 0xFF) != 0;
}

// ---- input: same protocol as the original MyOpenGLView ----
// GameEngine_Event(type, a, b, c): 1 key down, 2 key up, 3 mouse down, 4 mouse up, 6 wheel, 7 double click
// state bytes read by Apple_*Down(): shift 61, control 62, option 63, left 64, right 65, middle 66
#define ST_SHIFT 0x575d61u
#define ST_CTRL  0x575d62u
#define ST_ALT   0x575d63u
#define ST_LEFT  0x575d64u
#define ST_RIGHT 0x575d65u
#define ST_MID   0x575d66u
static void engine_event(uint32_t type, uint32_t a, uint32_t b, uint32_t c3) {
    uint32_t v[4] = { type, a, b, c3 };
    guest_call(gcpu, fn("_GameEngine_Event"), 4, v);
}
static void set_cursor(float x, float y) {
    uint32_t v[3]; memcpy(&v[0], &x, 4); memcpy(&v[1], &y, 4); v[2] = 0;
    guest_call(gcpu, fn("__ZN8MacInput17SetCursorPositionEffb"), 3, v);
}
static BOOL game_wants_system_cursor;   // set by the game through Apple_ShowSystemCursor
static NSView *game_view;
static NSCursor *blank_cursor(void) {
    static NSCursor *c;
    if (!c) {
        NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(16, 16)];
        [img lockFocus]; [[NSColor clearColor] set]; NSRectFill(NSMakeRect(0, 0, 16, 16)); [img unlockFocus];
        c = [[NSCursor alloc] initWithImage:img hotSpot:NSZeroPoint];
    }
    return c;
}
static BOOL scripted(void) { return getenv("JP_INPUT") != NULL; }   // automated runs ignore the real mouse/keyboard
void host_show_cursor(int show) {
    if (game_wants_system_cursor == (BOOL)show) return;
    game_wants_system_cursor = (BOOL)show;
    dispatch_async(dispatch_get_main_queue(), ^{ [game_view.window invalidateCursorRectsForView:game_view]; });
}

@interface JPView : NSOpenGLView
@property (strong) NSTimer *timer;
@end

@implementation JPView
- (BOOL)acceptsFirstResponder { return YES; }

- (void)prepareOpenGL {
    [super prepareOpenGL];
    GLint swap = 1;
    [[self openGLContext] setValues:&swap forParameter:NSOpenGLContextParameterSwapInterval];
    [[self openGLContext] makeCurrentContext];
    fprintf(stderr, "[host] GL %s / %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    if (!engine_start()) { fprintf(stderr, "[host] GameEngine_Start failed\n"); [NSApp terminate:nil]; return; }
    call1("__Z37RenderDeviceGL_ObjC_SetCurrentContextP15NSOpenGLContext", 1);
    float w = rdf(g_args), h = rdf(g_args + 4);
    fprintf(stderr, "[host] engine started, window %gx%g\n", w, h);
    if (w > 0 && h > 0) [self fitWindowToEngineWidth:w height:h];
    [[self window] center];
    call1("__Z37RenderDeviceGL_ObjC_SetCurrentContextP15NSOpenGLContext", 1);
    call0("_GameEngine_ReInitializeDevice");
    self.timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60.0 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
    [[NSRunLoop currentRunLoop] addTimer:self.timer forMode:NSEventTrackingRunLoopMode];
}

- (void)tick:(NSTimer *)t { [self setNeedsDisplay:YES]; }

// Map the engine's resolution onto the real drawable (uniform scale, centered) and clear the bars.
- (void)updateSurfaceMapping {
    extern float gl_win_scale, gl_win_off_x, gl_win_off_y;
    float ew = rdf(g_args), eh = rdf(g_args + 4);
    if (ew < 1 || eh < 1) return;
    NSSize d = [self convertSizeToBacking:self.bounds.size];
    float s = (float)MIN(d.width / ew, d.height / eh);
    gl_win_scale = s;
    gl_win_off_x = (float)((d.width - ew * s) / 2); gl_win_off_y = (float)((d.height - eh * s) / 2);
    // clear the bars without disturbing GL state the engine has cached
    GLint fb = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &fb);
    GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
    GLfloat cc[4]; glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    GLboolean sc = glIsEnabled(GL_SCISSOR_TEST), cm[4]; glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    if (fb) glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    glDisable(GL_SCISSOR_TEST); glColorMask(1, 1, 1, 1);
    glViewport(0, 0, (GLsizei)d.width, (GLsizei)d.height);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(cc[0], cc[1], cc[2], cc[3]); glColorMask(cm[0], cm[1], cm[2], cm[3]);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    if (sc) glEnable(GL_SCISSOR_TEST);
    if (fb) glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, (GLuint)fb);
}

// Follow the game's own fullscreen setting (only when it changes, so the green button keeps working).
- (void)followEngineFullscreen {
    static int last = -1;
    int windowed = (int)(call0("_GameEngine_GetWindowed") & 0xFF);
    if (windowed == last) return;
    BOOL isFull = (self.window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    if (last != -1 && windowed == isFull) [self.window toggleFullScreen:nil];
    else if (last == -1 && !windowed && !isFull) [self.window toggleFullScreen:nil];
    last = windowed;
}

// The engine's resolution is in backing pixels: on a Retina display the window is half that in points.
// If it still does not fit the screen, scale it down (keeping aspect) - the GL surface is then smaller
// than the engine's backbuffer and the engine's viewport is stretched to fit by the host.
- (void)fitWindowToEngineWidth:(float)w height:(float)h {
    NSWindow *win = [self window];
    win.contentAspectRatio = NSMakeSize(w, h);
    if (win.styleMask & NSWindowStyleMaskFullScreen) return;   // fullscreen: the mapping handles it
    CGFloat scale = win.backingScaleFactor > 0 ? win.backingScaleFactor : 1;
    NSSize pts = NSMakeSize(w / scale, h / scale);
    NSRect vis = (win.screen ?: [NSScreen mainScreen]).visibleFrame;
    NSRect frame = [win frameRectForContentRect:NSMakeRect(0, 0, pts.width, pts.height)];
    CGFloat chrome = frame.size.height - pts.height;
    CGFloat k = MIN(1.0, MIN(vis.size.width / pts.width, (vis.size.height - chrome) / pts.height));
    pts.width *= k; pts.height *= k;
    [win setContentSize:pts];
    if (k < 1) fprintf(stderr, "[host] %gx%g does not fit the screen; window scaled by %.2f\n", w, h, k);
    [win center];
    [[self openGLContext] update];
    [self updateTrackingAreas];
    [win invalidateCursorRectsForView:self];
}

// ---- mouse ----
- (void)moveTo:(NSEvent *)e {
    if (scripted()) return;
    NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    NSPoint px = [self convertPointToBacking:p];   // engine coordinates are backing pixels
    NSSize sz = [self convertSizeToBacking:self.bounds.size];
    extern float gl_win_scale, gl_win_off_x, gl_win_off_y;
    set_cursor((float)((px.x - gl_win_off_x) / gl_win_scale), (float)((sz.height - px.y - gl_win_off_y) / gl_win_scale));
}
- (void)mouseMoved:(NSEvent *)e { [self moveTo:e]; }
- (void)mouseDragged:(NSEvent *)e { [self moveTo:e]; }
- (void)rightMouseDragged:(NSEvent *)e { [self moveTo:e]; }
- (void)otherMouseDragged:(NSEvent *)e { [self moveTo:e]; }
- (void)button:(int)b down:(BOOL)down event:(NSEvent *)e {
    if (scripted()) return;
    static const uint32_t st[3] = { ST_LEFT, ST_RIGHT, ST_MID };
    [self moveTo:e];
    wr8(st[b], down ? 1 : 0);
    if (down && b == 0 && [e clickCount] == 2) engine_event(7, 0, 0, 0);
    else engine_event(down ? 3 : 4, (uint32_t)b, 0, 0);
}
- (void)mouseDown:(NSEvent *)e { [self button:0 down:YES event:e]; }
- (void)mouseUp:(NSEvent *)e { [self button:0 down:NO event:e]; }
- (void)rightMouseDown:(NSEvent *)e { [self button:1 down:YES event:e]; }
- (void)rightMouseUp:(NSEvent *)e { [self button:1 down:NO event:e]; }
- (void)otherMouseDown:(NSEvent *)e { [self button:2 down:YES event:e]; }
- (void)otherMouseUp:(NSEvent *)e { [self button:2 down:NO event:e]; }
- (void)scrollWheel:(NSEvent *)e {
    if (scripted()) return;
    float d = (float)[e deltaY];
    if ((int)d) engine_event(6, (uint32_t)(int32_t)d, 0, 0);
}
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea *t in self.trackingAreas) [self removeTrackingArea:t];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:self.bounds
        options:NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
        owner:self userInfo:nil]];
}
// The game draws its own cursor: over the game view the macOS pointer is invisible,
// unless the game asked for the system cursor. macOS restores the pointer outside the view.
- (void)resetCursorRects {
    [self addCursorRect:self.bounds cursor:game_wants_system_cursor ? [NSCursor arrowCursor] : blank_cursor()];
}

// ---- keyboard ----
- (uint32_t)keyChar:(NSEvent *)e {
    NSString *s = [e charactersIgnoringModifiers];
    return s.length ? (uint32_t)[s characterAtIndex:0] : 0;
}
- (void)keyDown:(NSEvent *)e {
    if (scripted()) return;
    if ([e isARepeat]) return;
    engine_event(1, [self keyChar:e], (uint32_t)[e modifierFlags], 0);
}
- (void)keyUp:(NSEvent *)e { if (scripted()) return; engine_event(2, [self keyChar:e], (uint32_t)[e modifierFlags], 0); }
- (void)flagsChanged:(NSEvent *)e {
    if (scripted()) return;
    NSEventModifierFlags f = [e modifierFlags];
    struct { uint32_t addr, code; NSEventModifierFlags mask; } mods[3] = {
        { ST_SHIFT, 0x10, NSEventModifierFlagShift }, { ST_CTRL, 0x11, NSEventModifierFlagControl }, { ST_ALT, 0x12, NSEventModifierFlagOption } };
    for (int i = 0; i < 3; i++) {
        BOOL now = (f & mods[i].mask) != 0, was = rd8(mods[i].addr) != 0;
        if (now != was) { engine_event(now ? 1 : 2, mods[i].code, 0, 0); wr8(mods[i].addr, now); }
    }
}

- (void)drawRect:(NSRect)r {
    [[self openGLContext] makeCurrentContext];
    call1("__Z37RenderDeviceGL_ObjC_SetCurrentContextP15NSOpenGLContext", 1);
    if (call0("_GameEngine_RequestedQuit") & 0xFF) { [NSApp terminate:nil]; return; }
    uint32_t a[2] = { g_args, g_args + 4 };
    guest_call(gcpu, fn("_GameEngine_GetWindowSize"), 2, a);
    {   // like the original SetGLWinSize: follow resolution changes made in the game's settings
        float w = rdf(g_args), h = rdf(g_args + 4);
        static float lw, lh;
        if (w >= 320 && h >= 200 && (w != lw || h != lh)) {
            lw = w; lh = h;
            fprintf(stderr, "[host] engine resolution -> %gx%g\n", w, h);
            [self fitWindowToEngineWidth:w height:h];
        }
    }
    [self updateSurfaceMapping];
    [self followEngineFullscreen];
    call1("__Z35RenderDeviceGL_ObjC_SetContextReadyb", 1);
    // EngineUpdate
    call0("_GameEngine_Loop");
    if (call0("_GameEngine_GetReset") & 0xFF) {
        uint32_t code = call0("_GameEngine_GetResetCode");
        call0("_GameEngine_Stop");
        call1("_GameEngine_SetResetCode", code);
        engine_start();
        call1("_GameEngine_SetReset", 0);
    }
    if (call0("_GameEngine_RequestedQuit") & 0xFF) [NSApp terminate:nil];
    [self maybeScreenshot];
}

// JP_SHOT=frame[,frame...] writes shot_<frame>.png; JP_EXIT_AFTER=n quits after n frames
// JP_INPUT="frame:move:x,y;frame:click;frame:key:c" - scripted input for automated tests
- (void)scriptedInput:(int)frame {
    const char *spec = getenv("JP_INPUT");
    if (!spec) return;
    char buf[1024]; snprintf(buf, sizeof buf, "%s", spec);
    for (char *save = NULL, *t = strtok_r(buf, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
        int f = atoi(t); char *act = strchr(t, ':');
        if (f != frame || !act) continue;
        act++;
        if (!strncmp(act, "move:", 5)) { float x = 0, y = 0; sscanf(act + 5, "%f,%f", &x, &y); set_cursor(x, y); }
        else if (!strcmp(act, "down")) { wr8(ST_LEFT, 1); engine_event(3, 0, 0, 0); }
        else if (!strcmp(act, "up")) { wr8(ST_LEFT, 0); engine_event(4, 0, 0, 0); }
        else if (!strcmp(act, "click")) { wr8(ST_LEFT, 1); engine_event(3, 0, 0, 0); wr8(ST_LEFT, 0); engine_event(4, 0, 0, 0); }
        else if (!strncmp(act, "key:", 4)) { uint32_t k = (uint32_t)(act[4] == '#' ? strtoul(act + 5, NULL, 0) : (uint8_t)act[4]); engine_event(1, k, 0, 0); engine_event(2, k, 0, 0); }
        fprintf(stderr, "[host] frame %d: input %s\n", frame, act);
    }
}

- (void)maybeScreenshot {
    static int frame = 0; frame++;
    [self scriptedInput:frame];
    if (getenv("JP_TRACE_GL")) fprintf(stderr, "[host] ---- end of frame %d ----\n", frame);
    const char *shots = getenv("JP_SHOT");
    if (shots) {
        char key[32]; snprintf(key, sizeof key, ",%d,", frame);
        char list[512]; snprintf(list, sizeof list, ",%s,", shots);
        if (strstr(list, key)) {
            GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
            NSSize sz = [self convertSizeToBacking:self.bounds.size];
            int w = (int)sz.width, h = (int)sz.height;
            unsigned char *px = malloc((size_t)w * h * 4);
            glReadBuffer(GL_FRONT);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
            NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:w pixelsHigh:h
                bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:w * 4 bitsPerPixel:32];
            for (int y = 0; y < h; y++) {   // flip and force opaque
                unsigned char *dst = rep.bitmapData + (size_t)y * w * 4, *src = px + (size_t)(h - 1 - y) * w * 4;
                memcpy(dst, src, (size_t)w * 4);
                for (int x = 0; x < w; x++) dst[4 * x + 3] = 255;
            }
            NSString *path = [NSString stringWithFormat:@"shot_%d.png", frame];
            NSString *dir = getenv("JP_SHOT_DIR") ? @(getenv("JP_SHOT_DIR")) : @".";
            [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:[dir stringByAppendingPathComponent:path] atomically:YES];
            fprintf(stderr, "[host] wrote %s (%dx%d)\n", path.UTF8String, w, h);
            free(px);
        }
    }
    const char *ex = getenv("JP_EXIT_AFTER");
    if (ex && frame >= atoi(ex)) { fprintf(stderr, "[host] exiting after %d frames\n", frame); exit(0); }
}
@end

@interface JPAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property (strong) NSWindow *window;
@end
@implementation JPAppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)n {
    NSOpenGLPixelFormatAttribute attrs[] = {
        NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersionLegacy,
        NSOpenGLPFADoubleBuffer, NSOpenGLPFAAccelerated,
        NSOpenGLPFAColorSize, 24, NSOpenGLPFAAlphaSize, 8,
        NSOpenGLPFADepthSize, 24, NSOpenGLPFAStencilSize, 8, 0 };
    NSOpenGLPixelFormat *pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
    NSRect frame = NSMakeRect(0, 0, 1024, 768);
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                                                backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"Jurassic Park: The Game";
    self.window.delegate = self;
    self.window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    self.window.styleMask |= NSWindowStyleMaskResizable;
    JPView *v = [[JPView alloc] initWithFrame:frame pixelFormat:pf];
    v.wantsBestResolutionOpenGLSurface = YES;  // full-resolution GL surface; engine sizes are backing pixels
    self.window.contentView = v;
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:v];
    game_view = v;
    self.window.acceptsMouseMovedEvents = YES;
    [NSApp activateIgnoringOtherApps:YES];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { return YES; }
@end

// The original game (Steam: Jurassic Park: The Game, Mac). Remembered once chosen.
static BOOL valid_game(NSString *p) {
    return [[NSFileManager defaultManager] fileExistsAtPath:[p stringByAppendingPathComponent:@"Contents/MacOS/JurassicPark100"]];
}
static NSString *find_game(void) {
    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    NSString *saved = [ud stringForKey:@"GamePath"];
    if (saved && valid_game(saved)) return saved;
    NSString *steam = [NSHomeDirectory() stringByAppendingPathComponent:
        @"Library/Application Support/Steam/steamapps/common/JurassicPark/JurassicPark100.app"];
    if (valid_game(steam)) { [ud setObject:steam forKey:@"GamePath"]; return steam; }
    [NSApplication sharedApplication];
    NSAlert *a = [NSAlert new];
    a.messageText = @"Locate Jurassic Park: The Game";
    a.informativeText = @"Choose JurassicPark100.app from your Steam installation of Jurassic Park: The Game.";
    [a runModal];
    NSOpenPanel *op = [NSOpenPanel openPanel];
    op.canChooseDirectories = NO; op.canChooseFiles = YES; op.treatsFilePackagesAsDirectories = NO;
    op.allowedFileTypes = @[ @"app" ];
    if ([op runModal] != NSModalResponseOK) return nil;
    NSString *p = op.URL.path;
    if (!valid_game(p)) {
        NSAlert *e = [NSAlert new]; e.messageText = @"That is not the Mac version of Jurassic Park: The Game.";
        [e runModal]; return nil;
    }
    [ud setObject:p forKey:@"GamePath"];
    return p;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        NSString *exe = [[NSBundle mainBundle] executablePath];
        const char *bin = argc > 2 && argv[1][0] != '-' ? argv[1] : NULL;
        const char *res = argc > 2 && argv[1][0] != '-' ? argv[2] : NULL;
        static char binbuf[2048], resbuf[2048];
        if (!bin || !res) {
            // Launched as an app: log to ~/Library/Logs and locate the user's copy of the game.
            if (!isatty(2)) {
                NSString *log = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/jp-recomp.log"];
                freopen(log.UTF8String, "w", stderr); freopen(log.UTF8String, "a", stdout);
                setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);   // a freeze must not lose the last lines
            }
            NSString *game = find_game();
            if (!game) return 1;
            snprintf(binbuf, sizeof binbuf, "%s/Contents/MacOS/JurassicPark100", game.UTF8String);
            snprintf(resbuf, sizeof resbuf, "%s/Contents/Resources", game.UTF8String);
            bin = binbuf; res = resbuf;
        }
        (void)exe;
        gcpu = rt_init(bin);
        if (chdir(res)) { perror(res); return 1; }
        platform_set_resources(res);
        char p[2048]; snprintf(p, sizeof p, "%s/../MacOS/steam_appid.txt", res);
        FILE *f = fopen(p, "r"); unsigned id = 0;
        if (f) { if (fscanf(f, "%u", &id) != 1) id = 0; fclose(f); }
        steam_set_app_id(id);
        for (int i = 0; i < ngmodinit; i++) { uint32_t a[4] = { 0, 0, 0, 0 }; guest_call(gcpu, gmodinit[i], 4, a); }
        g_args = gmalloc(16); memset(M + g_args, 0, 16);

        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        JPAppDelegate *d = [JPAppDelegate new];
        app.delegate = d;
        [app run];
    }
    return 0;
}
