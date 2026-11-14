// CoreFoundation bridge (guest handles <-> host CF objects) and native replacements (HLE)
// for the Cocoa-based Apple_* platform functions.
#include "rt.h"
#include "tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <mach/mach_time.h>
#include <CoreFoundation/CoreFoundation.h>
#include <ApplicationServices/ApplicationServices.h>

#define S(n) ((char *)G2H(n))
#define RET(v) (c->eax = (uint32_t)(v))

// The original game's preference domain, so settings are shared 1:1 with it.
static CFStringRef app_id(void) { return CFSTR("com.telltalegames.JurassicPark100"); }

// ---------- handles ----------
// guest handle = 16-byte guest block {magic 'CFOB', index}; index into host table
#define NCF 65536
static CFTypeRef cfobj[NCF];
static uint32_t cfhandle[NCF];
static int ncf = 1;
static pthread_mutex_t cflock = PTHREAD_MUTEX_INITIALIZER;
extern const uint32_t gcfstr_lo, gcfstr_hi;

static uint32_t handle_for(CFTypeRef o) {   // takes ownership of one reference
    if (!o) return 0;
    pthread_mutex_lock(&cflock);
    for (int i = 1; i < ncf; i++) if (cfobj[i] == o) { uint32_t h = cfhandle[i]; pthread_mutex_unlock(&cflock); CFRelease(o); return h; }
    int i = ncf++;
    uint32_t g = gmalloc(16); wr32(g, 0x43464F42); wr32(g + 4, (uint32_t)i);
    cfobj[i] = o; cfhandle[i] = g;
    pthread_mutex_unlock(&cflock);
    return g;
}
static CFTypeRef cf(uint32_t g) {
    if (!g) return NULL;
    if (g >= gcfstr_lo && g < gcfstr_hi) {   // compile-time CFSTR("...") constant in the guest image
        static CFStringRef consts[64]; static uint32_t keys[64];
        int slot = (int)((g - gcfstr_lo) / 16);
        if (slot < 64 && keys[slot] == g) return consts[slot];
        CFStringRef s = CFStringCreateWithBytes(NULL, M + rd32(g + 8), rd32(g + 12), kCFStringEncodingUTF8, false);
        if (slot < 64) { consts[slot] = s; keys[slot] = g; }
        return s;
    }
    if (rd32(g) == 0x43464F42) return cfobj[rd32(g + 4)];
    fprintf(stderr, "[cf] unknown CF handle 0x%x\n", g);
    return NULL;
}

// ---------- strings ----------
void I__CFStringCreateMutable(CPU *c) { RET(handle_for(CFStringCreateMutable(NULL, (CFIndex)ARG(1)))); }
void I__CFStringAppendCString(CPU *c) { CFStringAppendCString((CFMutableStringRef)cf(ARG(0)), S(ARG(1)), (CFStringEncoding)ARG(2)); }
void I__CFStringGetCString(CPU *c) {
    CFTypeRef o = cf(ARG(0));
    if (!o || CFGetTypeID(o) != CFStringGetTypeID()) { RET(0); return; }
    RET(CFStringGetCString((CFStringRef)o, S(ARG(1)), (CFIndex)ARG(2), (CFStringEncoding)ARG(3)));
}
void I__CFRelease(CPU *c) { (void)c; /* handles live for the process; host objects stay retained */ }
void I__CFShow(CPU *c) { CFShow(cf(ARG(0))); }

// ---------- numbers / booleans / data ----------
void I__CFNumberGetValue(CPU *c) {
    CFNumberType t = (CFNumberType)ARG(1);
    if (t == kCFNumberLongType || t == kCFNumberSInt32Type || t == kCFNumberIntType) {
        int32_t v = 0; RET(CFNumberGetValue((CFNumberRef)cf(ARG(0)), kCFNumberSInt32Type, &v)); wr32(ARG(2), (uint32_t)v);
    } else if (t == kCFNumberSInt64Type || t == kCFNumberLongLongType) {
        int64_t v = 0; RET(CFNumberGetValue((CFNumberRef)cf(ARG(0)), kCFNumberSInt64Type, &v)); wr64(ARG(2), (uint64_t)v);
    } else if (t == kCFNumberFloatType || t == kCFNumberFloat32Type) {
        float v = 0; RET(CFNumberGetValue((CFNumberRef)cf(ARG(0)), kCFNumberFloat32Type, &v)); wrf(ARG(2), v);
    } else if (t == kCFNumberDoubleType || t == kCFNumberFloat64Type) {
        double v = 0; RET(CFNumberGetValue((CFNumberRef)cf(ARG(0)), kCFNumberFloat64Type, &v)); wrd(ARG(2), v);
    } else { int32_t v = 0; RET(CFNumberGetValue((CFNumberRef)cf(ARG(0)), kCFNumberSInt32Type, &v)); wr32(ARG(2), (uint32_t)v); }
}
void I__CFBooleanGetValue(CPU *c) { RET(CFBooleanGetValue((CFBooleanRef)cf(ARG(0)))); }
void I__CFDataGetBytes(CPU *c) {   // (data, CFRange{loc,len} by value, buf)
    CFDataGetBytes((CFDataRef)cf(ARG(0)), CFRangeMake((CFIndex)(int32_t)ARG(1), (CFIndex)(int32_t)ARG(2)), (UInt8 *)S(ARG(3)));
}

// ---------- arrays / dictionaries (values are guest handles) ----------
void I__CFArrayCreateMutable(CPU *c) { RET(handle_for(CFArrayCreateMutable(NULL, (CFIndex)ARG(1), &kCFTypeArrayCallBacks))); }
void I__CFArrayAppendValue(CPU *c) { CFArrayAppendValue((CFMutableArrayRef)cf(ARG(0)), cf(ARG(1))); }
void I__CFArrayGetCount(CPU *c) { RET(CFArrayGetCount((CFArrayRef)cf(ARG(0)))); }
void I__CFArrayGetValueAtIndex(CPU *c) {
    CFTypeRef v = CFArrayGetValueAtIndex((CFArrayRef)cf(ARG(0)), (CFIndex)ARG(1));
    RET(v ? handle_for(CFRetain(v)) : 0);
}
void I__CFDictionaryCreateMutable(CPU *c) {
    RET(handle_for(CFDictionaryCreateMutable(NULL, (CFIndex)ARG(1), &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)));
}
void I__CFDictionarySetValue(CPU *c) { CFDictionarySetValue((CFMutableDictionaryRef)cf(ARG(0)), cf(ARG(1)), cf(ARG(2))); }
void I__CFDictionaryGetValue(CPU *c) {
    CFTypeRef v = CFDictionaryGetValue((CFDictionaryRef)cf(ARG(0)), cf(ARG(1)));
    RET(v ? handle_for(CFRetain(v)) : 0);
}

// ---------- preferences ----------
void I__CFPreferencesCopyAppValue(CPU *c) { RET(handle_for(CFPreferencesCopyAppValue((CFStringRef)cf(ARG(0)), app_id()))); }
void I__CFPreferencesSetAppValue(CPU *c) { CFPreferencesSetAppValue((CFStringRef)cf(ARG(0)), cf(ARG(1)), app_id()); }
void I__CFPreferencesAppSynchronize(CPU *c) { RET(CFPreferencesAppSynchronize(app_id())); }

// ---------- UUID ----------
void I__CFUUIDCreate(CPU *c) { RET(handle_for(CFUUIDCreate(NULL))); }
void I__CFUUIDCreateString(CPU *c) { RET(handle_for(CFUUIDCreateString(NULL, (CFUUIDRef)cf(ARG(1))))); }

// ---------- CoreGraphics bits that work natively ----------
void I__CGEventSourceKeyState(CPU *c) { RET(CGEventSourceKeyState((CGEventSourceStateID)ARG(0), (CGKeyCode)ARG(1))); }
void I__CGMainDisplayID(CPU *c) { RET(CGMainDisplayID()); }
void I__CGDisplayCurrentMode(CPU *c);
void I__CGDisplayPixelsHigh(CPU *c) { RET(CGDisplayPixelsHigh((CGDirectDisplayID)ARG(0))); }
void I__SetSystemUIMode(CPU *c) { RET(0); }

// ---------- HLE: Apple_* platform layer ----------
static char resources_dir[1024];
void platform_set_resources(const char *dir) { snprintf(resources_dir, sizeof resources_dir, "%s", dir); }
static uint32_t static_str(uint32_t *slot, const char *s) {
    if (!*slot) *slot = gmalloc(2048);
    snprintf(S(*slot), 2048, "%s", s);
    return *slot;
}
void H_Apple_GetBundleResourceDirectory(CPU *c) { static uint32_t g; RET(static_str(&g, resources_dir)); }
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
__attribute__((weak)) void host_show_cursor(int show) { (void)show; }   // overridden by the Cocoa host
void H_Apple_ShowSystemCursor(CPU *c) { host_show_cursor(ARG(0) & 0xFF); }
void H_Apple_OpenURL(CPU *c) { fprintf(stderr, "[game] open URL: %s\n", S(ARG(0))); }
void H_Apple_Terminate(CPU *c) { fprintf(stderr, "[game] Apple_Terminate\n"); exit(0); }

// ---------- legacy CGDirectDisplay mode dictionaries (10.6 API) on top of CGDisplayMode ----------
static CFDictionaryRef mode_dict(CGDisplayModeRef m) {
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    long w = (long)CGDisplayModeGetWidth(m), h = (long)CGDisplayModeGetHeight(m), bpp = 32, io = (long)CGDisplayModeGetIOFlags(m);
    long mode = (long)CGDisplayModeGetIODisplayModeID(m);
    double hz = CGDisplayModeGetRefreshRate(m); if (hz <= 0) hz = 60;
    CFNumberRef nw = CFNumberCreate(NULL, kCFNumberLongType, &w), nh = CFNumberCreate(NULL, kCFNumberLongType, &h);
    CFNumberRef nb = CFNumberCreate(NULL, kCFNumberLongType, &bpp), nr = CFNumberCreate(NULL, kCFNumberDoubleType, &hz);
    CFNumberRef ni = CFNumberCreate(NULL, kCFNumberLongType, &io), nm = CFNumberCreate(NULL, kCFNumberLongType, &mode);
    CFDictionarySetValue(d, CFSTR("Width"), nw); CFDictionarySetValue(d, CFSTR("Height"), nh);
    CFDictionarySetValue(d, CFSTR("BitsPerPixel"), nb); CFDictionarySetValue(d, CFSTR("RefreshRate"), nr);
    CFDictionarySetValue(d, CFSTR("IOFlags"), ni); CFDictionarySetValue(d, CFSTR("Mode"), nm);
    CFDictionarySetValue(d, CFSTR("UsableForDesktopGUI"), CGDisplayModeIsUsableForDesktopGUI(m) ? kCFBooleanTrue : kCFBooleanFalse);
    CFDictionarySetValue(d, CFSTR("kCGDisplayModeIsSafeForHardware"), kCFBooleanTrue);
    CFRelease(nw); CFRelease(nh); CFRelease(nb); CFRelease(nr); CFRelease(ni); CFRelease(nm);
    return d;
}
void I__CGDisplayAvailableModes(CPU *c) {
    CFArrayRef modes = CGDisplayCopyAllDisplayModes((CGDirectDisplayID)ARG(0), NULL);
    CFMutableArrayRef out = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    for (CFIndex i = 0; modes && i < CFArrayGetCount(modes); i++) {
        CFDictionaryRef d = mode_dict((CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i));
        CFArrayAppendValue(out, d); CFRelease(d);
    }
    if (modes) CFRelease(modes);
    RET(handle_for(out));
}
void I__CGDisplayCurrentMode(CPU *c) {
    CGDisplayModeRef m = CGDisplayCopyDisplayMode((CGDirectDisplayID)ARG(0));
    if (!m) { RET(0); return; }
    CFDictionaryRef d = mode_dict(m); CGDisplayModeRelease(m);
    RET(handle_for(d));
}
void I__CGDisplayBestModeForParameters(CPU *c) {   // (display, bpp, width, height, boolean_t *exactMatch)
    if (ARG(4)) wr32(ARG(4), 0);
    I__CGDisplayCurrentMode(c);
}
void I__CGDisplaySwitchToMode(CPU *c) { RET(0); }   // the port never changes the desktop resolution
void I__CGDisplayIDToOpenGLDisplayMask(CPU *c) { RET(CGDisplayIDToOpenGLDisplayMask((CGDirectDisplayID)ARG(0))); }
void I__CGCaptureAllDisplays(CPU *c) { RET(0); }
void I__CGReleaseAllDisplays(CPU *c) { RET(0); }
void I__CGDisplayMoveCursorToPoint(CPU *c) { RET(0); }

// CFArrayReplaceValues(array, CFRange{loc,len}, const void **newValues, CFIndex newCount)
void I__CFArrayReplaceValues(CPU *c) {
    CFIndex n = (CFIndex)(int32_t)ARG(4);
    const void **vals = n > 0 ? calloc((size_t)n, sizeof *vals) : NULL;
    for (CFIndex i = 0; i < n; i++) vals[i] = cf(rd32(ARG(3) + 4 * (uint32_t)i));
    CFArrayReplaceValues((CFMutableArrayRef)cf(ARG(0)), CFRangeMake((CFIndex)(int32_t)ARG(1), (CFIndex)(int32_t)ARG(2)), vals, n);
    free(vals);
}
// CFArraySortValues(array, CFRange, CFComparatorFunction (guest), void *context)
typedef struct { CPU *c; uint32_t fn, ctx; } SortCtx;
static CFComparisonResult guest_cmp(const void *a, const void *b, void *p) {
    SortCtx *s = p;
    uint32_t args[3] = { handle_for(CFRetain(a)), handle_for(CFRetain(b)), s->ctx };
    return (CFComparisonResult)(int32_t)guest_call(s->c, s->fn, 3, args);
}
void I__CFArraySortValues(CPU *c) {
    SortCtx s = { c, ARG(3), ARG(4) };
    CFArraySortValues((CFMutableArrayRef)cf(ARG(0)), CFRangeMake((CFIndex)(int32_t)ARG(1), (CFIndex)(int32_t)ARG(2)), guest_cmp, &s);
}

// ---------- Bink: Jurassic Park ships no Bink movies; fail cleanly if one is ever requested ----------
void I__BinkOpen(CPU *c) { fprintf(stderr, "[bink] BinkOpen(%s): no Bink support (game ships no .bik files)\n", ARG(0) ? (char *)G2H(ARG(0)) : "?"); RET(0); }
void I__BinkClose(CPU *c) { RET(0); }
void I__BinkSetMemory(CPU *c) { RET(0); }
void I__BinkSetVolume(CPU *c) { RET(0); }
void I__BinkPause(CPU *c) { RET(0); }
void I__BinkWait(CPU *c) { RET(0); }
void I__BinkShouldSkip(CPU *c) { RET(0); }
void I__BinkDoFrame(CPU *c) { RET(0); }
void I__BinkNextFrame(CPU *c) { RET(0); }
void I__BinkGetFrameBuffersInfo(CPU *c) { RET(0); }
void I__BinkRegisterFrameBuffers(CPU *c) { RET(0); }
