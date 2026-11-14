// Core runtime: guest address space, image loader, dispatch, heap, setjmp.
#include "rt.h"
#include "tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>

_Thread_local CPU *rt_cur;
// ---- JP_MEMWATCH: catch guest writes to an address range (debug) ----
static uint32_t mw_addr, mw_len = 8, mw_armed, mw_hits;
static uint8_t *mw_page; static size_t mw_pagesz;
uint8_t rt_watch[0x500000 / 8];
static void mw_protect(int on) { mprotect(mw_page, mw_pagesz, on ? PROT_READ : PROT_READ | PROT_WRITE); }
static int mw_reprotect_pending;
void rt_memwatch_tick(void) { if (mw_reprotect_pending) { mw_reprotect_pending = 0; mw_protect(1); } }
static void crash_handler(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    uintptr_t fa = (uintptr_t)si->si_addr;
    if (mw_armed && (uint8_t *)fa >= mw_page && (uint8_t *)fa < mw_page + mw_pagesz) {
        uint32_t g = (uint32_t)(fa - (uintptr_t)M);
        if (g >= mw_addr && g < mw_addr + mw_len) {
            CPU *c = rt_cur;
            fprintf(stderr, "[memwatch] write to 0x%x by %s (0x%x), caller %s\n", g, c ? rt_func_name(c->fn) : "?", c ? c->fn : 0,
                    c ? rt_func_name(rd32(c->esp)) : "?");
            if (++mw_hits >= 12 || getenv("JP_MEMWATCH_ONESHOT")) {
                fprintf(stderr, "[memwatch] guest esp=%x ebp=%x eax=%x ecx=%x edx=%x esi=%x edi=%x; stack:", c->esp, c->ebp, c->eax, c->ecx, c->edx, c->esi, c->edi);
                for (uint32_t f = c->ebp, k = 0; f && k < 12; f = rd32(f), k++) fprintf(stderr, "\n    %s", rt_func_name(rd32(f + 4)));
                fprintf(stderr, "\n");
                signal(sig, SIG_DFL); raise(SIGABRT);
            }
        }
        mw_protect(0); mw_reprotect_pending = 1;   // let this write through; re-arm at the next function entry
        return;
    }
    CPU *c = rt_cur;
    fprintf(stderr, "\n[rt] signal %d at host %p", sig, si->si_addr);
    if (fa >= (uintptr_t)M && fa < (uintptr_t)M + (1ull << 32)) fprintf(stderr, " (guest 0x%x)", (uint32_t)(fa - (uintptr_t)M));
    if (c) fprintf(stderr, "\n[rt] in guest %s (0x%x) eax=%x ecx=%x edx=%x ebx=%x esp=%x ebp=%x esi=%x edi=%x\n",
                   rt_func_name(c->fn), c->fn, c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
    signal(sig, SIG_DFL);
    raise(sig);
}

uint8_t *M;

#define STACK_TOP  0xF0000000u
#define HEAP_BASE  0x10000000u
#define HEAP_END   0xE0000000u

void rt_shims_init(void);

int32_t cvtt_i32(double v) {
    if (!(v > -2147483649.0 && v < 2147483648.0)) return INT32_MIN;  // x86 "integer indefinite"
    return (int32_t)v;
}

const char *rt_func_name(uint32_t a);
static const char *func_name(uint32_t a) { return rt_func_name(a); }
const char *rt_func_name(uint32_t a) {
    int lo = 0, hi = ngfuncs - 1, best = -1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (gfuncs[m].addr <= a) { best = m; lo = m + 1; } else hi = m - 1; }
    return best >= 0 ? gfuncs[best].name : "?";
}

static int lax_mode = -1;
void unimpl_import(CPU *c, const char *name) {
    if (lax_mode < 0) lax_mode = getenv("JP_STRICT") == NULL;   // players: log and continue; JP_STRICT=1: abort
    if (lax_mode) {
        static const char *seen[4096]; static int nseen;
        for (int i = 0; i < nseen; i++) if (seen[i] == name) { c->eax = 0; return; }
        if (nseen < 4096) seen[nseen++] = name;
        fprintf(stderr, "[lax] %s (from %s) -> 0\n", name, func_name(rd32(C.esp)));
        c->eax = 0;
        return;
    }
    fprintf(stderr, "[rt] unimplemented import %s (called from guest, ret=0x%x in %s)\n", name, rd32(C.esp), func_name(rd32(C.esp)));
    abort();
}
void missing_func(CPU *c, uint32_t addr, uint32_t from) {
    fprintf(stderr, "[rt] call to untranslated function 0x%x from 0x%x (in %s)\n", addr, from, func_name(from));
    abort();
}
void bad_jump(CPU *c, uint32_t t, uint32_t from) {
    fprintf(stderr, "[rt] bad indirect jump to 0x%x at 0x%x (%s)\n", t, from, func_name(from));
    abort();
}
void bad_insn(CPU *c, uint32_t a) {
    fprintf(stderr, "[rt] unsupported/ud2 instruction at 0x%x (%s)\n", a, func_name(a));
    abort();
}

// ---- native functions callable from guest code through synthetic addresses ----
#define HOSTFN_BASE 0xFF000000u
#define NHOSTFN 4096
static gfn hostfns[NHOSTFN];
static void (*hostfns_ctx[NHOSTFN])(CPU *, uint32_t);
static uint32_t hostctx[NHOSTFN];
static int nhostfn;
uint32_t rt_host_fn(gfn f) {
    int i = __atomic_fetch_add(&nhostfn, 1, __ATOMIC_SEQ_CST);
    hostfns[i] = f;
    return HOSTFN_BASE + 16u * (uint32_t)i;
}
uint32_t rt_host_fn_ctx(void (*f)(CPU *, uint32_t), uint32_t ctx) {
    int i = __atomic_fetch_add(&nhostfn, 1, __ATOMIC_SEQ_CST);
    hostfns_ctx[i] = f; hostctx[i] = ctx;
    return HOSTFN_BASE + 16u * (uint32_t)i;
}

void call_ind(CPU *c, uint32_t t) {
    if (t >= HOSTFN_BASE && t < HOSTFN_BASE + 16u * NHOSTFN) {
        uint32_t i = (t - HOSTFN_BASE) / 16;
        if (hostfns[i]) hostfns[i](c); else hostfns_ctx[i](c, hostctx[i]);
        C.esp += 4; return;
    }
    int lo = 0, hi = ngfuncs - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (gfuncs[m].addr == t) { gfuncs[m].fn(c); return; }
        if (gfuncs[m].addr < t) lo = m + 1; else hi = m - 1;
    }
    lo = 0; hi = ngstubs - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (gstubs[m].addr == t) { gstubs[m].fn(c); C.esp += 4; return; }
        if (gstubs[m].addr < t) lo = m + 1; else hi = m - 1;
    }
    fprintf(stderr, "[rt] indirect call to unknown 0x%x (ret=0x%x)\n", t, rd32(C.esp));
    abort();
}

uint32_t guest_func(const char *sym) {
    for (int i = 0; i < ngfuncs; i++) if (!strcmp(gfuncs[i].name, sym)) return gfuncs[i].addr;
    fprintf(stderr, "[rt] no guest function %s\n", sym); abort();
}

uint32_t guest_call(CPU *c, uint32_t addr, int nargs, const uint32_t *args) {
    CPU *saved = rt_cur; rt_cur = c;
    for (int i = nargs - 1; i >= 0; i--) PUSH(args[i]);
    PUSH(0xDEAD0000u);
    call_ind(c, addr);
    C.esp += 4 * nargs;
    rt_cur = saved;
    return C.eax;
}

// ---- MEMCHECK builds: report guest writes into [JP_MC_LO, JP_MC_HI) once armed ----
uint32_t rt_mc_lo = 0, rt_mc_hi = 0;
static uint32_t mc_lo_cfg, mc_hi_cfg, mc_arm_fn;
void rt_mc_hit(uint32_t a, uint32_t n) {
    CPU *c = rt_cur;
    fprintf(stderr, "[memcheck] %u-byte write to 0x%x in %s\n", n, a, c ? rt_func_name(c->fn) : "?");
    if (c) {
        fprintf(stderr, "    called from:");
        for (uint32_t f = c->ebp, k = 0; f > 0x1000 && k < 10; f = rd32(f), k++) fprintf(stderr, "\n      %s", rt_func_name(rd32(f + 4)));
        fprintf(stderr, "\n");
    }
    static int hits; if (++hits >= (getenv("JP_MC_MAX") ? atoi(getenv("JP_MC_MAX")) : 1)) abort();
}

// ---- JP_WATCH: log entry into selected guest functions ----
#include <stdio.h>
extern char *__cxa_demangle(const char *, char *, size_t *, int *);
static uint32_t mw_arm_fn;
void rt_on_watch(CPU *c, uint32_t a) {
    if (mc_arm_fn && a == mc_arm_fn) {   // arm MEMCHECK range on entry of the arm function
        static int n; int want = getenv("JP_MC_ARM_N") ? atoi(getenv("JP_MC_ARM_N")) : 1;
        if (++n == want) { rt_mc_lo = mc_lo_cfg; rt_mc_hi = mc_hi_cfg; fprintf(stderr, "[memcheck] armed 0x%x-0x%x\n", rt_mc_lo, rt_mc_hi); }
    }
    if (mw_reprotect_pending) rt_memwatch_tick();
    if (mw_arm_fn && a == mw_arm_fn && !mw_armed) {
        /* arm on the first call */
        mw_armed = 1;
        if (!getenv("JP_MEMWATCH_ONESHOT")) memset(rt_watch, 0xFF, sizeof rt_watch);   // every function entry re-arms protection
        mw_pagesz = (size_t)getpagesize(); mw_page = (uint8_t *)(((uintptr_t)(M + mw_addr)) & ~(mw_pagesz - 1));
        mw_protect(1);
        fprintf(stderr, "[memwatch] armed on 0x%x\n", mw_addr);
        return;
    }
    if (mw_armed) return;

    int st; char *d = __cxa_demangle(rt_func_name(a) + 1, NULL, NULL, &st);
    fprintf(stderr, "[watch] %s  <- %s  args %08x %08x %08x %08x\n", d ? d : rt_func_name(a),
            rt_func_name(rd32(c->esp)), rd32(c->esp + 4), rd32(c->esp + 8), rd32(c->esp + 12), rd32(c->esp + 16));
    free(d);
    if (strstr(rt_func_name(a), "St4list") && strstr(rt_func_name(a), "6remove")) {
        uint32_t head = rd32(c->esp + 4), n = rd32(head);
        uint32_t tb = head - 4 + 0x1030;
        fprintf(stderr, "          TextBuffer@%08x:", tb);
        for (int k = 0; k < 8; k++) fprintf(stderr, " %08x", rd32(tb + 4 * k));
        fprintf(stderr, "\n");
        fprintf(stderr, "          around: \"");
        for (uint32_t q = head - 0x6000; q < head - 0x4800; q++) { uint8_t ch = rd8(q); if ((q & 63) == 0) fprintf(stderr, "\n            %08x ", q); fputc(ch >= 32 && ch < 127 ? ch : '.', stderr); }
        fprintf(stderr, "\"\n");
        fprintf(stderr, "          list %08x next %08x prev %08x:", head, rd32(head), rd32(head + 4));
        for (int i = 0; i < 12 && n != head; i++) { fprintf(stderr, " [%08x n=%08x p=%08x]", n, rd32(n), rd32(n + 4)); n = rd32(n); }
        fprintf(stderr, "\n");
    }
    if (strstr(rt_func_name(a), "IntrusiveSet") && strstr(rt_func_name(a), "4find")) {
        // boost::intrusive rbtree: header {parent|color, left, right}; node key (Symbol) at +0x10
        uint32_t set = rd32(c->esp + 8), key = rd32(c->esp + 12);
        uint64_t want = rd64(key);
        uint32_t stack[256]; int sp = 0, n = 0, found = 0, bad = 0;
        uint32_t cur = rd32(set) & ~1u;
        while ((cur || sp) && n < 100000) {
            while (cur && sp < 256) { stack[sp++] = cur; cur = rd32(cur + 4); }
            if (!sp) break;
            cur = stack[--sp]; n++;
            if (rd64(cur + 0x10) == want) found = 1;
            if (cur < 0x1000) { bad = 1; break; }
            cur = rd32(cur + 8);
        }
        fprintf(stderr, "          set %08x: %d keys, key %016llx %s%s\n", set, n, (unsigned long long)want, found ? "PRESENT" : "absent", bad ? " (corrupt)" : "");
    }
    for (int i = 0; i < 6; i++) {   // show arguments that point at printable strings
        uint32_t p = rd32(c->esp + 4 + 4 * i);
        if (p < 0x1000 || p >= 0xF0000000u) continue;
        const uint8_t *s = M + p; int n = 0;
        while (n < 200 && s[n] >= 0x20 && s[n] < 0x7f) n++;
        if (n >= 4 && (s[n] == 0 || s[n] == '\n')) fprintf(stderr, "          arg%d: \"%.*s\"\n", i, n, s);
    }
}
static void watch_init(void) {
    if (getenv("JP_MC_LO") && getenv("JP_MC_ARM")) {
        mc_lo_cfg = (uint32_t)strtoul(getenv("JP_MC_LO"), NULL, 16);
        mc_hi_cfg = getenv("JP_MC_HI") ? (uint32_t)strtoul(getenv("JP_MC_HI"), NULL, 16) : mc_lo_cfg + 4;
        for (int i = 0; i < ngfuncs; i++) if (strstr(gfuncs[i].name, getenv("JP_MC_ARM"))) {
            mc_arm_fn = gfuncs[i].addr; rt_watch[mc_arm_fn >> 3] |= (uint8_t)(1u << (mc_arm_fn & 7)); break;
        }
    }
    if (getenv("JP_MEMWATCH") && getenv("JP_MEMWATCH_ARM")) {
        mw_addr = (uint32_t)strtoul(getenv("JP_MEMWATCH"), NULL, 16);
        for (int i = 0; i < ngfuncs; i++) if (strstr(gfuncs[i].name, getenv("JP_MEMWATCH_ARM"))) {
            mw_arm_fn = gfuncs[i].addr; rt_watch[mw_arm_fn >> 3] |= (uint8_t)(1u << (mw_arm_fn & 7)); break;
        }
    }
    const char *w = getenv("JP_WATCH");
    if (!w) return;
    char buf[4096]; snprintf(buf, sizeof buf, "%s", w);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ","))
        for (int i = 0; i < ngfuncs; i++) if (strstr(gfuncs[i].name, tok)) {
            uint32_t a = gfuncs[i].addr; rt_watch[a >> 3] |= (uint8_t)(1u << (a & 7));
        }
}

// ---- setjmp slots ----
#define NJMP 256
static GJmp jmps[NJMP];
GJmp *gjmp_slot(uint32_t buf) {
    for (int i = 0; i < NJMP; i++) if (jmps[i].guest_buf == buf) return &jmps[i];
    for (int i = 0; i < NJMP; i++) if (!jmps[i].guest_buf) { jmps[i].guest_buf = buf; return &jmps[i]; }
    fprintf(stderr, "[rt] out of jmp slots\n"); abort();
}

// ---- guest heap: power-of-two size classes, 16-byte header keeps 16-byte alignment ----
static uint32_t heap_top = HEAP_BASE;
static uint32_t freelist[40];
// Header (16 bytes): word0 = size class (<= 20) or 0xFF for large; word1 = free-list link;
// word2 = total block size for large blocks; word3 = state (ALOC / FREE) to catch double/bad frees.
#define BLK_ALLOC 0x434F4C41u
#define BLK_FREE  0x45455246u
#include <pthread.h>
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;
#define LARGE 0xFFu
static uint32_t large_free;   // singly linked, first fit
static int size_class(uint32_t n) { int c = 4; while ((1u << c) < n + 16) c++; return c; }
static uint32_t bump(uint32_t sz) {
    uint32_t b = heap_top; heap_top += sz;
    if (heap_top > HEAP_END || heap_top < b) { fprintf(stderr, "[rt] guest heap exhausted (%u bytes)\n", sz); abort(); }
    return b;
}
uint32_t gmalloc(uint32_t n) {
    uint32_t b;
    pthread_mutex_lock(&heap_lock);
    if (n + 16 <= (1u << 20)) {
        int c = size_class(n);
        if (freelist[c]) { b = freelist[c]; freelist[c] = rd32(b + 4); }
        else b = bump(1u << c);
        wr32(b, (uint32_t)c);
    } else {
        uint32_t sz = (n + 16 + 0xFFFu) & ~0xFFFu, *prev = &large_free;
        b = 0;
        for (uint32_t cur = large_free; cur; prev = (uint32_t *)(M + cur + 4), cur = rd32(cur + 4))
            if (rd32(cur + 8) >= sz) { b = cur; *prev = rd32(cur + 4); break; }
        if (!b) { b = bump(sz); wr32(b + 8, sz); }
        wr32(b, LARGE);
    }
    wr32(b + 12, BLK_ALLOC);
    pthread_mutex_unlock(&heap_lock);
    return b + 16;
}
static uint32_t gsize(uint32_t p) {
    uint32_t c = rd32(p - 16);
    return c == LARGE ? rd32(p - 16 + 8) - 16 : (1u << c) - 16;
}
uint32_t gmalloc_usable(uint32_t p) { return p ? gsize(p) : 0; }
void gfree(uint32_t p) {
    if (!p) return;
    uint32_t b = p - 16, c = rd32(b);
    pthread_mutex_lock(&heap_lock);
    uint32_t st = (p >= HEAP_BASE + 16 && p < heap_top) ? rd32(b + 12) : 0;
    if (st != BLK_ALLOC) {
        // macOS would report "pointer being freed was not allocated"; ignoring keeps the heap intact
        static int n; CPU *cur = rt_cur;
        if (n++ < 50) fprintf(stderr, "[rt] %s free of 0x%x ignored (in %s, called from %s)\n",
                              st == BLK_FREE ? "double" : "invalid", p, cur ? rt_func_name(cur->fn) : "?",
                              cur ? rt_func_name(rd32(cur->esp)) : "?");
        pthread_mutex_unlock(&heap_lock);
        return;
    }
    wr32(b + 12, BLK_FREE);
    if (c == LARGE) { wr32(b + 4, large_free); large_free = b; }
    else { wr32(b + 4, freelist[c]); freelist[c] = b; }
    pthread_mutex_unlock(&heap_lock);
}
uint32_t grealloc(uint32_t p, uint32_t n) {
    if (!p) return gmalloc(n);
    if (!n) { gfree(p); return 0; }
    uint32_t have = gsize(p);
    if (have >= n) return p;
    uint32_t q = gmalloc(n);
    memcpy(M + q, M + p, have);
    gfree(p);
    return q;
}
uint32_t gstrdup(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1, p = gmalloc(n);
    memcpy(M + p, s, n);
    return p;
}

// ---- CPUs (one per guest thread), each with its own guest stack ----
static uint32_t stack_next = STACK_TOP;
CPU *cpu_new(uint32_t stack_size) {
    CPU *c = calloc(1, sizeof(CPU));
    stack_size = (stack_size + 0xFFFFu) & ~0xFFFFu;
    c->stack_hi = __atomic_fetch_sub(&stack_next, stack_size + 0x10000u, __ATOMIC_SEQ_CST);
    c->stack_lo = c->stack_hi - stack_size;
    c->esp = c->stack_hi - 64;
    c->fcw = 0x37F;
    return c;
}

void cpu_cpuid(CPU *c) { c->eax = 1; c->ebx = c->ecx = c->edx = 0; }

double fpu_round(CPU *c, double v) {
    switch ((c->fcw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

CPU *rt_init(const char *binpath) {
    M = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (M == MAP_FAILED) { perror("mmap guest space"); exit(1); }
    FILE *f = fopen(binpath, "rb");
    if (!f) { perror(binpath); exit(1); }
    for (int i = 0; i < ngsegs; i++) {
        const GSeg *s = &gsegs[i];
        fseek(f, s->fo, SEEK_SET);
        if (fread(M + s->va, 1, s->fs, f) != s->fs) { fprintf(stderr, "short read %s\n", s->name); exit(1); }
    }
    fclose(f);
    // Imported function pointers point at their stubs (call_ind resolves them);
    // pure data imports get a zeroed placeholder block until a shim provides the real object.
    for (int i = 0; i < ngptrs; i++) wr32(gptrs[i].addr, rt_import_addr(gptrs[i].name));
    // External relocations: slot += address of symbol (addend is stored in the slot).
    for (int i = 0; i < ngextrel; i++) wr32(gextrel[i].addr, rd32(gextrel[i].addr) + rt_import_addr(gextrel[i].name));
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGFPE, &sa, NULL);
    rt_shims_init();
    watch_init();
    return cpu_new(8u << 20);
}

// ---- import name -> guest address ----
#define NIMPDATA 512
static struct { const char *name; uint32_t addr; } impdata[NIMPDATA];
static int nimpdata;
uint32_t rt_import_addr(const char *name) {
    for (int k = 0; k < ngstubs; k++) if (!strcmp(gstubs[k].name, name)) return gstubs[k].addr;
    for (int i = 0; i < nimpdata; i++) if (!strcmp(impdata[i].name, name)) return impdata[i].addr;
    for (int k = 0; k < ngimports; k++)
        if (!strcmp(gimports[k].name, name) && gimports[k].fn) {   // function referenced only via data (vtables)
            uint32_t a = rt_host_fn(gimports[k].fn);
            impdata[nimpdata].name = name; impdata[nimpdata].addr = a; nimpdata++;
            return a;
        }
    if (nimpdata == NIMPDATA) { fprintf(stderr, "[rt] too many data imports\n"); abort(); }
    uint32_t a = gmalloc(256); memset(M + a, 0, 256);
    impdata[nimpdata].name = name; impdata[nimpdata].addr = a; nimpdata++;
    if (!strncmp(name, "__ZTI", 5)) {
        // imported RTTI object (libstdc++ type): {vptr, name} (+ flags, pointee for pointer types)
        const char *mangled = name + 5;
        wr32(a + 4, gstrdup(mangled));
        if (mangled[0] == 'P') {
            uint32_t flags = 0; const char *p = mangled + 1;
            while (*p == 'K' || *p == 'V') { flags |= (*p == 'K') ? 1u : 2u; p++; }
            char pointee[128]; snprintf(pointee, sizeof pointee, "__ZTI%s", p);
            wr32(a, rt_import_addr("__ZTVN10__cxxabiv119__pointer_type_infoE") + 8);
            wr32(a + 8, flags); wr32(a + 12, rt_import_addr(pointee));
        } else if (strlen(mangled) == 1) {
            wr32(a, rt_import_addr("__ZTVN10__cxxabiv123__fundamental_type_infoE") + 8);
        } else {
            wr32(a, rt_import_addr("__ZTVN10__cxxabiv117__class_type_infoE") + 8);
        }
    }
    return a;
}

// data-import slots are filled by the shim layer
void rt_set_data_import(const char *name, uint32_t guest_addr) {
    for (int i = 0; i < ngptrs; i++) if (!strcmp(gptrs[i].name, name)) wr32(gptrs[i].addr, guest_addr);
}
