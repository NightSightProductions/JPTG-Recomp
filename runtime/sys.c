// System shims: Mach, sysctl, pthreads (guest threads run on host threads, one CPU each).
#include "rt.h"
#include "tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/sysctl.h>
#include <mach/mach_time.h>
#include <sched.h>
#include <unistd.h>

#define RET(v) (c->eax = (uint32_t)(v))

// ---------- guest-object -> host-object map (mutexes, conds, threads) ----------
typedef struct { uint32_t key; void *val; } Ent;
static Ent *tab; static uint32_t tabcap;
static pthread_mutex_t tablock = PTHREAD_MUTEX_INITIALIZER;
static void *map_get(uint32_t key, void *(*mk)(uint32_t)) {
    pthread_mutex_lock(&tablock);
    if (!tab) { tabcap = 1 << 16; tab = calloc(tabcap, sizeof(Ent)); }
    uint32_t h = (key * 2654435761u) & (tabcap - 1);
    while (tab[h].key && tab[h].key != key) h = (h + 1) & (tabcap - 1);
    void *v = tab[h].val;
    if (!tab[h].key && mk) { v = mk(key); tab[h].key = key; tab[h].val = v; }
    pthread_mutex_unlock(&tablock);
    return v;
}
static void map_set(uint32_t key, void *v) {
    pthread_mutex_lock(&tablock);
    if (!tab) { tabcap = 1 << 16; tab = calloc(tabcap, sizeof(Ent)); }
    uint32_t h = (key * 2654435761u) & (tabcap - 1);
    while (tab[h].key && tab[h].key != key) h = (h + 1) & (tabcap - 1);
    tab[h].key = key; tab[h].val = v;
    pthread_mutex_unlock(&tablock);
}

// ---------- time / machine ----------
void I__mach_absolute_time(CPU *c) { uint64_t t = mach_absolute_time(); c->eax = (uint32_t)t; c->edx = (uint32_t)(t >> 32); }
void I__mach_timebase_info(CPU *c) { mach_timebase_info_data_t i; mach_timebase_info(&i); wr32(ARG(0), i.numer); wr32(ARG(0) + 4, i.denom); RET(0); }
void I__mach_host_self(CPU *c) { RET(1); }
void I__mach_task_self(CPU *c) { RET(2); }
void I__host_page_size(CPU *c) { wr32(ARG(1), 4096); RET(0); }
void I__host_statistics(CPU *c) {   // (host, flavor, info_out, count*) : report 1 GiB free, little used
    uint32_t out = ARG(2), n = rd32(ARG(3));
    memset(M + out, 0, 4 * n);
    if (n > 0) wr32(out + 0, (1u << 30) / 4096);   // free_count
    if (n > 1) wr32(out + 4, (256u << 20) / 4096); // active_count
    RET(0);
}
void I__mstats(CPU *c) {   // struct mstats returned via hidden pointer (callee pops it)
    uint32_t p = ARG(0);
    memset(M + p, 0, 20);
    RET(p);
    c->esp += 4;
}
void I__sysctl(CPU *c) {   // (int *name, u_int namelen, void *oldp, size_t *oldlenp, void *newp, size_t newlen)
    int name[8]; uint32_t n = ARG(1);
    if (n > 8) { RET(-1); return; }
    for (uint32_t i = 0; i < n; i++) name[i] = (int)rd32(ARG(0) + 4 * i);
    uint32_t oldp = ARG(2), oldlenp = ARG(3);
    if (n == 2 && name[0] == CTL_HW && (name[1] == HW_PHYSMEM || name[1] == HW_USERMEM)) {
        if (oldp) wr32(oldp, 0x7FFFFFFFu);   // a 32-bit process sees at most 2 GiB
        if (oldlenp) wr32(oldlenp, 4);
        RET(0); return;
    }
    if (n == 2 && name[0] == CTL_HW && name[1] == HW_MEMSIZE) {
        if (oldp) wr64(oldp, 2ull << 30);
        if (oldlenp) wr32(oldlenp, 8);
        RET(0); return;
    }
    uint8_t buf[256]; size_t len = sizeof buf;
    int r = sysctl(name, n, buf, &len, NULL, 0);
    if (r == 0 && oldp && oldlenp) {
        uint32_t cap = rd32(oldlenp);
        if (len > cap) len = cap;
        memcpy(M + oldp, buf, len);
        wr32(oldlenp, (uint32_t)len);
    }
    RET(r);
}
void I__sched_yield(CPU *c) { RET(sched_yield()); }
void I__usleep_UNIX2003(CPU *c) { RET(usleep(ARG(0))); }
void I__usleep(CPU *c) { RET(usleep(ARG(0))); }

// ---------- mutexes (all made recursive-capable per attr type) ----------
// guest pthread_mutexattr_t: we keep the type in its first word
void I__pthread_mutexattr_init(CPU *c) { wr32(ARG(0), 0); wr32(ARG(0) + 4, 0); RET(0); }
void I__pthread_mutexattr_destroy(CPU *c) { RET(0); }
void I__pthread_mutexattr_settype(CPU *c) { wr32(ARG(0) + 4, ARG(1)); RET(0); }
static pthread_mutex_t *new_mutex(int type) {
    pthread_mutex_t *m = malloc(sizeof *m);
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, type == PTHREAD_MUTEX_RECURSIVE ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_NORMAL);
    pthread_mutex_init(m, &a);
    return m;
}
static void *mk_mutex(uint32_t k) { (void)k; return new_mutex(PTHREAD_MUTEX_NORMAL); }
static pthread_mutex_t *MX(uint32_t g) { return map_get(g, mk_mutex); }
void I__pthread_mutex_init(CPU *c) {
    uint32_t attr = ARG(1);
    map_set(ARG(0), new_mutex(attr ? (int)rd32(attr + 4) : 0));
    RET(0);
}
void I__pthread_mutex_destroy(CPU *c) { RET(0); }
void I__pthread_mutex_lock(CPU *c) { RET(pthread_mutex_lock(MX(ARG(0)))); }
void I__pthread_mutex_trylock(CPU *c) { RET(pthread_mutex_trylock(MX(ARG(0)))); }
void I__pthread_mutex_unlock(CPU *c) { RET(pthread_mutex_unlock(MX(ARG(0)))); }

// ---------- condition variables ----------
static void *mk_cond(uint32_t k) { (void)k; pthread_cond_t *v = malloc(sizeof *v); pthread_cond_init(v, NULL); return v; }
static pthread_cond_t *CV(uint32_t g) { return map_get(g, mk_cond); }
void I__pthread_cond_init_UNIX2003(CPU *c) { map_set(ARG(0), mk_cond(0)); RET(0); }
void I__pthread_cond_init(CPU *c) { map_set(ARG(0), mk_cond(0)); RET(0); }
void I__pthread_cond_destroy(CPU *c) { RET(0); }
void I__pthread_cond_signal(CPU *c) { RET(pthread_cond_signal(CV(ARG(0)))); }
void I__pthread_cond_broadcast(CPU *c) { RET(pthread_cond_broadcast(CV(ARG(0)))); }
void I__pthread_cond_wait_UNIX2003(CPU *c) { RET(pthread_cond_wait(CV(ARG(0)), MX(ARG(1)))); }
void I__pthread_cond_timedwait_UNIX2003(CPU *c) {
    struct timespec ts = { (time_t)(int32_t)rd32(ARG(2)), (long)(int32_t)rd32(ARG(2) + 4) };
    RET(pthread_cond_timedwait(CV(ARG(0)), MX(ARG(1)), &ts));
}

// ---------- threads ----------
// guest pthread_t is a small integer handle; attr keeps the stack size in word 1
typedef struct { pthread_t host; CPU *cpu; uint32_t start, arg, ret; } GThread;
static GThread threads[256];
static int nthreads = 1;          // handle 1 = main thread
static _Thread_local uint32_t cur_handle = 1;
static void *thread_main(void *p) {
    GThread *t = p;
    static const char *dummy; (void)dummy;
    cur_handle = (uint32_t)(t - threads) + 1;
    uint32_t a[1] = { t->arg };
    t->ret = guest_call(t->cpu, t->start, 1, a);
    return NULL;
}
void I__pthread_attr_init(CPU *c) { wr32(ARG(0), 0); wr32(ARG(0) + 4, 512 * 1024); RET(0); }
void I__pthread_attr_destroy(CPU *c) { RET(0); }
void I__pthread_attr_setstacksize(CPU *c) { wr32(ARG(0) + 4, ARG(1)); RET(0); }
void I__pthread_create(CPU *c) {   // (pthread_t *thr, attr, start, arg)
    int h = __atomic_fetch_add(&nthreads, 1, __ATOMIC_SEQ_CST);
    GThread *t = &threads[h];
    uint32_t ss = ARG(1) ? rd32(ARG(1) + 4) : 512 * 1024;
    if (ss < 256 * 1024) ss = 256 * 1024;
    t->cpu = cpu_new(ss);
    t->start = ARG(2); t->arg = ARG(3);
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8 << 20);  // host stack for deep recursion
    int r = pthread_create(&t->host, &a, thread_main, t);
    wr32(ARG(0), (uint32_t)h + 1);
    RET(r);
}
void I__pthread_join_UNIX2003(CPU *c) {
    GThread *t = &threads[ARG(0) - 1];
    int r = pthread_join(t->host, NULL);
    if (ARG(1)) wr32(ARG(1), t->ret);
    RET(r);
}
void I__pthread_self(CPU *c) { RET(cur_handle); }
void I__pthread_exit(CPU *c) { pthread_exit(NULL); }
void I__pthread_detach(CPU *c) { RET(0); }
