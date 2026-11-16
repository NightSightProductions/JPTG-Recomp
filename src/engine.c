// Jurassic Park: The Game (Telltale Tool) on Recomp-Framework mac32: the host hooks (the engine's GameEngine_* C
// API, called the way the original MainController/MyOpenGLView did) and the engine's native replacements (HLE: the
// Apple_* platform layer and the renderer's Objective-C bridge). Settings and the allocator are in game.c.
#include "game32.h"
#include "tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach_time.h>
#include <CoreFoundation/CoreFoundation.h>

#define RET(v) (c->eax = (uint32_t)(v))
#define S(n) ((char *)G2H(n))


// ---- engine C API ----
static uint32_t g_args;   // guest scratch: [0]=w [4]=h [8]=windowed [12]=empty cmdline
static uint32_t call0(CPU *c, const char *s) { return guest_call(c, guest_func(s), 0, NULL); }
static uint32_t call1(CPU *c, const char *s, uint32_t a) { uint32_t v[1] = { a }; return guest_call(c, guest_func(s), 1, v); }
static void set_context(CPU *c) { call1(c, "__Z37RenderDeviceGL_ObjC_SetCurrentContextP15NSOpenGLContext", 1); }
static int engine_start(CPU *c) {
    if (!g_args) { g_args = gmalloc(16); memset(M + g_args, 0, 16); }
    wr8(g_args + 8, 1);
    uint32_t a[4] = { g_args + 12, g_args, g_args + 4, g_args + 8 };
    return (guest_call(c, guest_func("_GameEngine_Start"), 4, a) & 0xFF) != 0;
}
int game32_start(CPU *c) {
    if (!engine_start(c)) return 0;
    set_context(c);
    call0(c, "_GameEngine_ReInitializeDevice");
    return 1;
}
void game32_window_size(CPU *c, float *w, float *h) {
    if (!g_args) { *w = *h = 0; return; }
    uint32_t a[2] = { g_args, g_args + 4 };
    guest_call(c, guest_func("_GameEngine_GetWindowSize"), 2, a);
    *w = rdf(g_args); *h = rdf(g_args + 4);
}
int game32_windowed(CPU *c) { return (int)(call0(c, "_GameEngine_GetWindowed") & 0xFF); }
int game32_quit_requested(CPU *c) { return (call0(c, "_GameEngine_RequestedQuit") & 0xFF) != 0; }
void game32_frame(CPU *c) {
    set_context(c);
    call1(c, "__Z35RenderDeviceGL_ObjC_SetContextReadyb", 1);
    call0(c, "_GameEngine_Loop");
    if (call0(c, "_GameEngine_GetReset") & 0xFF) {   // the engine asked to restart itself
        uint32_t code = call0(c, "_GameEngine_GetResetCode");
        call0(c, "_GameEngine_Stop");
        call1(c, "_GameEngine_SetResetCode", code);
        engine_start(c);
        call1(c, "_GameEngine_SetReset", 0);
    }
}

// ---- input: the original MyOpenGLView protocol ----
// GameEngine_Event(type, a, b, c): 1 key down, 2 key up, 3 mouse down, 4 mouse up, 6 wheel, 7 double click
// state bytes read by Apple_*Down(): shift 61, control 62, option 63, left 64, right 65, middle 66
#define ST_SHIFT 0x575d61u
#define ST_LEFT  0x575d64u
static void engine_event(CPU *c, uint32_t type, uint32_t a, uint32_t b, uint32_t c3) {
    uint32_t v[4] = { type, a, b, c3 };
    guest_call(c, guest_func("_GameEngine_Event"), 4, v);
}
void game32_mouse_move(CPU *c, float x, float y) {
    uint32_t v[3]; memcpy(&v[0], &x, 4); memcpy(&v[1], &y, 4); v[2] = 0;
    guest_call(c, guest_func("__ZN8MacInput17SetCursorPositionEffb"), 3, v);
}
void game32_mouse_button(CPU *c, int b, int down, int clicks) {
    wr8(ST_LEFT + (uint32_t)b, down ? 1 : 0);
    if (down && b == 0 && clicks == 2) engine_event(c, 7, 0, 0, 0);
    else engine_event(c, down ? 3 : 4, (uint32_t)b, 0, 0);
}
void game32_wheel(CPU *c, int delta) { engine_event(c, 6, (uint32_t)(int32_t)delta, 0, 0); }
void game32_key(CPU *c, int down, uint32_t ch, uint32_t mods) { engine_event(c, down ? 1 : 2, ch, mods, 0); }
void game32_modifier(CPU *c, int which, int down) {
    static const uint32_t code[3] = { 0x10, 0x11, 0x12 };
    engine_event(c, down ? 1 : 2, code[which], 0, 0);
    wr8(ST_SHIFT + (uint32_t)which, down ? 1 : 0);
}

// ---- HLE: the engine's Apple_* platform layer ----
static uint32_t static_str(uint32_t *slot, const char *s) {
    if (!*slot) *slot = gmalloc(2048);
    snprintf(S(*slot), 2048, "%s", s);
    return *slot;
}
void H_Apple_GetBundleResourceDirectory(CPU *c) { static uint32_t g; RET(static_str(&g, host_resources_dir())); }
void H_Apple_GetBundleDocumentsDirectory(CPU *c) {
    static uint32_t g; char p[1024];
    snprintf(p, sizeof p, "%s/Documents", getenv("HOME") ? getenv("HOME") : ".");
    RET(static_str(&g, p));
}
void H_Apple_GetSystemLanguage(CPU *c) {
    char lang[16] = "en";
    CFArrayRef langs = CFLocaleCopyPreferredLanguages();
    if (langs && CFArrayGetCount(langs) > 0) {
        CFStringGetCString(CFArrayGetValueAtIndex(langs, 0), lang, sizeof lang, kCFStringEncodingUTF8);
        char *dash = strchr(lang, '-'); if (dash) *dash = 0;
    }
    if (langs) CFRelease(langs);
    strcpy(S(ARG(0)), lang);
}
void H_Apple_GetMilliseconds(CPU *c) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    double ms = (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
    FPUSH(ms);
}
void H_Apple_GetOSString(CPU *c) { static uint32_t g; RET(static_str(&g, "Mac OS X 10.6.8")); }
void H_Apple_Alert(CPU *c) { fprintf(stderr, "[game alert] %s: %s\n", ARG(0) ? S(ARG(0)) : "", ARG(1) ? S(ARG(1)) : ""); }
void H_Apple_SetCursorPosition(CPU *c) { (void)c; }
void H_Apple_ShowSystemCursor(CPU *c) { host_show_cursor(ARG(0) & 0xFF); }
void H_Apple_OpenURL(CPU *c) { fprintf(stderr, "[game] open URL: %s\n", S(ARG(0))); }
void H_Apple_Terminate(CPU *c) { fprintf(stderr, "[game] Apple_Terminate\n"); exit(0); }

// ---- HLE: renderer Objective-C bridge ----
uint32_t gl_total_vram(void);
void H_RenderDeviceGL_ObjC_Present(CPU *c) { (void)c; host_present(); }
void H_RenderDeviceGL_ObjC_GetTotalVram(CPU *c) { RET(gl_total_vram()); }
