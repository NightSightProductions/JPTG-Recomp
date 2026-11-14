// More libSystem shims: files/dirs (i386 struct layouts), formatted I/O, 64-bit math helpers,
// sorting with guest callbacks, sockets (disabled), misc.
#include "rt.h"
#include "tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/sysctl.h>
#include <time.h>
#include <math.h>

#define S(n) ((char *)G2H(n))
#define RET(v) (c->eax = (uint32_t)(v))
#define RET64(v) do { uint64_t _v = (uint64_t)(v); c->eax = (uint32_t)_v; c->edx = (uint32_t)(_v >> 32); } while (0)
#define RETF(v) FPUSH((double)(v))
#define ARG64(i) ((uint64_t)ARG(i) | ((uint64_t)ARG((i) + 1) << 32))

int gformat_ext(char *out, size_t cap, const char *fmt, uint32_t va);   // shims.c
uint32_t gfile_wrap(FILE *f);
FILE *gfile_host(uint32_t g);
uint32_t g_errno_addr(void);
static void seterr(void) { wr32(g_errno_addr(), (uint32_t)errno); }

// ---------- libgcc 64-bit division ----------
void I____udivdi3(CPU *c) { uint64_t b = ARG64(2); RET64(b ? ARG64(0) / b : 0); }
void I____umoddi3(CPU *c) { uint64_t b = ARG64(2); RET64(b ? ARG64(0) % b : 0); }
void I____divdi3(CPU *c)  { int64_t b = (int64_t)ARG64(2); RET64(b ? (int64_t)ARG64(0) / b : 0); }
void I____moddi3(CPU *c)  { int64_t b = (int64_t)ARG64(2); RET64(b ? (int64_t)ARG64(0) % b : 0); }

// ---------- strings / memory ----------
void I__memmove(CPU *c) { MC(ARG(0), ARG(2)); memmove(S(ARG(0)), S(ARG(1)), ARG(2)); RET(ARG(0)); }
void I____memmove_chk(CPU *c) { MC(ARG(0), ARG(2)); memmove(S(ARG(0)), S(ARG(1)), ARG(2)); RET(ARG(0)); }
void I____memcpy_chk(CPU *c) { MC(ARG(0), ARG(2)); memmove(S(ARG(0)), S(ARG(1)), ARG(2)); RET(ARG(0)); }
void I____memset_chk(CPU *c) { MC(ARG(0), ARG(2)); memset(S(ARG(0)), (int)ARG(1), ARG(2)); RET(ARG(0)); }
void I____strcpy_chk(CPU *c) { MC(ARG(0), (uint32_t)strlen(S(ARG(1))) + 1); strcpy(S(ARG(0)), S(ARG(1))); RET(ARG(0)); }
void I____strcat_chk(CPU *c) { MC(ARG(0) + (uint32_t)strlen(S(ARG(0))), (uint32_t)strlen(S(ARG(1))) + 1); strcat(S(ARG(0)), S(ARG(1))); RET(ARG(0)); }
void I__bzero(CPU *c) { memset(S(ARG(0)), 0, ARG(1)); }
void I__strncmp(CPU *c) { RET(strncmp(S(ARG(0)), S(ARG(1)), ARG(2))); }
void I__strcasecmp(CPU *c) { RET(strcasecmp(S(ARG(0)), S(ARG(1)))); }
void I__strncasecmp(CPU *c) { RET(strncasecmp(S(ARG(0)), S(ARG(1)), ARG(2))); }
void I__strdup(CPU *c) { RET(gstrdup(S(ARG(0)))); }
void I__strtol(CPU *c) { char *e; long v = strtol(S(ARG(0)), &e, (int)ARG(2)); if (ARG(1)) wr32(ARG(1), H2G(e)); RET(v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v); }
void I__strtoull(CPU *c) { char *e; unsigned long long v = strtoull(S(ARG(0)), &e, (int)ARG(2)); if (ARG(1)) wr32(ARG(1), H2G(e)); RET64(v); }
void I__atoi(CPU *c) { RET(atoi(S(ARG(0)))); }
void I__atof(CPU *c) { RETF(atof(S(ARG(0)))); }
void I__strtok(CPU *c) {
    static uint32_t last;
    uint32_t s = ARG(0) ? ARG(0) : last;
    if (!s) { RET(0); return; }
    const char *d = S(ARG(1));
    s += (uint32_t)strspn(S(s), d);
    if (!rd8(s)) { last = 0; RET(0); return; }
    uint32_t e = s + (uint32_t)strcspn(S(s), d);
    if (rd8(e)) { wr8(e, 0); last = e + 1; } else last = 0;
    RET(s);
}

// ---------- formatted output ----------
void I__sprintf(CPU *c)   { int n = gformat_ext(S(ARG(0)), 1u << 30, S(ARG(1)), c->esp + 4 + 8); MC(ARG(0), (uint32_t)n + 1); RET(n); }
void I__snprintf(CPU *c)  { RET(gformat_ext(S(ARG(0)), ARG(1), S(ARG(2)), c->esp + 4 + 12)); }
void I__vsprintf(CPU *c)  { int n = gformat_ext(S(ARG(0)), 1u << 30, S(ARG(1)), ARG(2)); MC(ARG(0), (uint32_t)n + 1); RET(n); }
void I__printf(CPU *c)    { static char b[1 << 16]; int n = gformat_ext(b, sizeof b, S(ARG(0)), c->esp + 4 + 4); fputs(b, stdout); RET(n); }
void I__vfprintf(CPU *c)  { static char b[1 << 16]; int n = gformat_ext(b, sizeof b, S(ARG(1)), ARG(2)); fputs(b, gfile_host(ARG(0))); RET(n); }
void I__puts(CPU *c)      { RET(puts(S(ARG(0)))); }
void I__NSLog(CPU *c)     { fprintf(stderr, "[NSLog] (CFString 0x%x)\n", ARG(0)); }

// sscanf: one conversion at a time through the host sscanf, tracking consumption with %n
void I__sscanf(CPU *c) {
    const char *in = S(ARG(0)), *f = S(ARG(1));
    uint32_t va = c->esp + 4 + 8;
    int assigned = 0; size_t pos = 0;
    while (*f) {
        if (isspace((unsigned char)*f)) { while (isspace((unsigned char)in[pos])) pos++; f++; continue; }
        if (*f != '%') { if (in[pos] != *f) break; pos++; f++; continue; }
        if (f[1] == '%') { if (in[pos] != '%') break; pos++; f += 2; continue; }
        char spec[64]; int k = 0; spec[k++] = *f++;
        int suppress = 0; if (*f == '*') { suppress = 1; spec[k++] = *f++; }
        while (isdigit((unsigned char)*f)) spec[k++] = *f++;
        int l = 0, h = 0;
        while (*f == 'l' || *f == 'h' || *f == 'L' || *f == 'q') { if (*f == 'l' || *f == 'q') l++; if (*f == 'h') h++; spec[k++] = *f++; }
        char conv = *f++;
        if (conv == '[') { spec[k++] = '['; if (*f == '^') spec[k++] = *f++; if (*f == ']') spec[k++] = *f++; while (*f && *f != ']') spec[k++] = *f++; if (*f) spec[k++] = *f++; }
        else spec[k++] = conv;
        strcpy(spec + k, "%n");
        int n = -1, r;
        union { int i; long long ll; short s; char ch; float fl; double d; char buf[4096]; } u;
        if (conv == 'n') { if (!suppress) wr32(rd32(va), (uint32_t)pos); va += 4; continue; }
        r = sscanf(in + pos, spec, (void *)&u, &n);
        if (suppress) { if (n < 0) break; pos += n; continue; }
        if (r != 1 || n < 0) break;
        uint32_t dst = rd32(va); va += 4;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
            if (l >= 2) wr64(dst, (uint64_t)u.ll); else if (h) wr16(dst, (uint16_t)u.s); else wr32(dst, (uint32_t)u.i); break;
        case 'f': case 'g': case 'e': case 'E': case 'G': case 'a':
            if (l) wrd(dst, u.d); else wrf(dst, u.fl); break;
        case 'c': wr8(dst, (uint8_t)u.ch); break;
        case 's': case '[': strcpy(S(dst), u.buf); break;
        default: fprintf(stderr, "[rt] sscanf %%%c unsupported\n", conv); abort();
        }
        pos += n; assigned++;
    }
    RET(assigned ? assigned : (in[0] ? 0 : -1));
}

// ---------- math ----------
void I__floor(CPU *c) { RETF(floor(ARGD(0))); }
void I__ceil(CPU *c)  { RETF(ceil(ARGD(0))); }
void I__sin(CPU *c)   { RETF(sin(ARGD(0))); }
void I__cos(CPU *c)   { RETF(cos(ARGD(0))); }
void I__tan(CPU *c)   { RETF(tan(ARGD(0))); }
void I__exp(CPU *c)   { RETF(exp(ARGD(0))); }
void I__log(CPU *c)   { RETF(log(ARGD(0))); }
void I__powf(CPU *c)  { RETF(powf(ARGF(0), ARGF(1))); }
void I__fmodf(CPU *c) { RETF(fmodf(ARGF(0), ARGF(1))); }
void I____fpclassifyf(CPU *c) { RET(fpclassify(ARGF(0))); }

// ---------- qsort with a guest comparator ----------
static void gqsort(CPU *c, uint32_t base, uint32_t n, uint32_t sz, uint32_t cmp) {
    // insertion sort for small, otherwise heap-free merge via temp buffer (stable enough)
    uint32_t tmp = gmalloc(sz);
    for (uint32_t i = 1; i < n; i++) {
        memcpy(M + tmp, M + base + i * sz, sz);
        uint32_t j = i;
        while (j > 0) {
            uint32_t a[2] = { base + (j - 1) * sz, tmp };
            if ((int32_t)guest_call(c, cmp, 2, a) <= 0) break;
            memcpy(M + base + j * sz, M + base + (j - 1) * sz, sz);
            j--;
        }
        memcpy(M + base + j * sz, M + tmp, sz);
    }
    gfree(tmp);
}
void I__qsort(CPU *c) { gqsort(c, ARG(0), ARG(1), ARG(2), ARG(3)); }

// ---------- time ----------
void I__gettimeofday(CPU *c) {
    struct timeval tv; int r = gettimeofday(&tv, NULL);
    if (ARG(0)) { wr32(ARG(0), (uint32_t)tv.tv_sec); wr32(ARG(0) + 4, (uint32_t)tv.tv_usec); }
    RET(r);
}
void I__nanosleep_UNIX2003(CPU *c) {
    struct timespec ts = { (time_t)(int32_t)rd32(ARG(0)), (long)(int32_t)rd32(ARG(0) + 4) };
    RET(nanosleep(&ts, NULL));
}
void I__Microseconds(CPU *c) {   // UnsignedWide* {lo, hi} on Intel
    struct timeval tv; gettimeofday(&tv, NULL);
    uint64_t us = (uint64_t)tv.tv_sec * 1000000u + tv.tv_usec;
    wr32(ARG(0), (uint32_t)us); wr32(ARG(0) + 4, (uint32_t)(us >> 32));
}
void I__asctime(CPU *c) { static uint32_t g; if (!g) g = gmalloc(64); strcpy(S(g), "Thu Jan  1 00:00:00 1970\n"); RET(g); }
void I__Gestalt(CPU *c) {
    uint32_t sel = ARG(0), v = 0;
    switch (sel) {
    case 0x73797376: v = 0x1068; break;   // 'sysv'
    case 0x73797331: v = 10; break;       // 'sys1'
    case 0x73797332: v = 6; break;        // 'sys2'
    case 0x73797333: v = 8; break;        // 'sys3'
    case 0x70636c6b: v = 2000000000; break; // 'pclk'
    case 0x6d637075: v = 2000; break;      // 'mcpu'
    case 0x72616d20: v = 0x7FFFFFFF; break; // 'ram '
    }
    if (ARG(1)) wr32(ARG(1), v);
    RET(0);
}
void I__sysctlbyname(CPU *c) {
    const char *name = S(ARG(0));
    uint32_t oldp = ARG(1), lenp = ARG(2);
    if (!strcmp(name, "hw.cpufrequency") || !strcmp(name, "hw.cpufrequency_max")) {
        uint64_t f = 2400000000ull;
        uint32_t cap = lenp ? rd32(lenp) : 8;
        if (oldp) { if (cap >= 8) wr64(oldp, f); else wr32(oldp, (uint32_t)f); }
        if (lenp) wr32(lenp, cap >= 8 ? 8 : 4);
        RET(0); return;
    }
    char buf[512]; size_t len = sizeof buf;
    int r = sysctlbyname(name, buf, &len, NULL, 0);
    if (r == 0 && oldp && lenp) { uint32_t cap = rd32(lenp); if (len > cap) len = cap; memcpy(M + oldp, buf, len); }
    if (r == 0 && lenp) wr32(lenp, (uint32_t)len);
    RET(r);
}

// ---------- processes / misc ----------
void I__getpid(CPU *c) { RET(getpid()); }
void I__getuid(CPU *c) { RET(getuid()); }
void I__geteuid(CPU *c) { RET(geteuid()); }
void I__getgid(CPU *c) { RET(getgid()); }
void I__getegid(CPU *c) { RET(getegid()); }
void I__abort(CPU *c) { fprintf(stderr, "[rt] guest abort() from %s\n", rt_func_name(rd32(c->esp))); abort(); }
void I____assert_rtn(CPU *c) {
    fprintf(stderr, "[rt] guest assertion failed: %s (%s:%d in %s)\n", S(ARG(3)), S(ARG(1)), (int)ARG(2), S(ARG(0)));
    abort();
}
void I__signal(CPU *c) { RET(0); }
void I__sigaction(CPU *c) { RET(0); }
void I__madvise(CPU *c) { RET(0); }
void I__tcgetattr(CPU *c) { RET(-1); }
void I__tcsetattr(CPU *c) { RET(-1); }

// ---------- file descriptors ----------
static int trace_files = -1;
static void tf(const char *what, const char *p, long r) {
    if (trace_files < 0) trace_files = getenv("JP_TRACE_FILES") != NULL;
    if (trace_files) fprintf(stderr, "[file] %s %s -> %ld\n", what, p, r);
}
void I__open_UNIX2003(CPU *c) { int fd = open(S(ARG(0)), (int)ARG(1), (int)ARG(2)); if (fd < 0) seterr(); tf("open", S(ARG(0)), fd); RET(fd); }
void I__close_UNIX2003(CPU *c) { RET(close((int)ARG(0))); }
void I__read_UNIX2003(CPU *c) { MC(ARG(1), ARG(2)); RET(read((int)ARG(0), S(ARG(1)), ARG(2))); }
void I__write_UNIX2003(CPU *c) { RET(write((int)ARG(0), S(ARG(1)), ARG(2))); }
void I__lseek(CPU *c) { RET64(lseek((int)ARG(0), (off_t)(int64_t)ARG64(1), (int)ARG(3))); }
void I__ftruncate(CPU *c) { RET(ftruncate((int)ARG(0), (off_t)(int64_t)ARG64(1))); }
void I__fcntl_UNIX2003(CPU *c) { RET(fcntl((int)ARG(0), (int)ARG(1), (int)ARG(2))); }
void I__access(CPU *c) { int r = access(S(ARG(0)), (int)ARG(1)); if (r) seterr(); RET(r); }
void I__unlink(CPU *c) { RET(unlink(S(ARG(0)))); }
void I__mkdir(CPU *c) { int r = mkdir(S(ARG(0)), (mode_t)ARG(1)); if (r) seterr(); RET(r); }
void I__chdir(CPU *c) { RET(chdir(S(ARG(0)))); }
void I__chmod_UNIX2003(CPU *c) { RET(chmod(S(ARG(0)), (mode_t)ARG(1))); }
void I__getcwd(CPU *c) {
    char b[1024]; if (!getcwd(b, sizeof b)) { RET(0); return; }
    uint32_t dst = ARG(0);
    if (!dst) dst = gmalloc((uint32_t)strlen(b) + 1);
    else if (strlen(b) + 1 > ARG(1)) { RET(0); return; }
    strcpy(S(dst), b); RET(dst);
}
// mmap: guest gets a private copy (writes are not written back; MAP_SHARED writers use msync)
void I__mmap_UNIX2003(CPU *c) {
    uint32_t len = ARG(1); int flags = (int)ARG(3), fd = (int)ARG(4);
    off_t off = (off_t)(int64_t)ARG64(5);
    uint32_t g = gmalloc(len);
    memset(M + g, 0, len);
    if (!(flags & MAP_ANON) && fd >= 0) { ssize_t r = pread(fd, M + g, len, off); (void)r; }
    RET(g);
}
void I__msync_UNIX2003(CPU *c) { RET(0); }

static void stat_to_guest(const struct stat *st, uint32_t g, int inode64) {
    if (!inode64) {   // legacy i386 struct stat (96 bytes)
        memset(M + g, 0, 96);
        wr32(g + 0, (uint32_t)st->st_dev); wr32(g + 4, (uint32_t)st->st_ino);
        wr16(g + 8, st->st_mode); wr16(g + 10, st->st_nlink);
        wr32(g + 12, st->st_uid); wr32(g + 16, st->st_gid); wr32(g + 20, (uint32_t)st->st_rdev);
        wr32(g + 24, (uint32_t)st->st_atimespec.tv_sec); wr32(g + 28, (uint32_t)st->st_atimespec.tv_nsec);
        wr32(g + 32, (uint32_t)st->st_mtimespec.tv_sec); wr32(g + 36, (uint32_t)st->st_mtimespec.tv_nsec);
        wr32(g + 40, (uint32_t)st->st_ctimespec.tv_sec); wr32(g + 44, (uint32_t)st->st_ctimespec.tv_nsec);
        wr64(g + 48, (uint64_t)st->st_size); wr64(g + 56, (uint64_t)st->st_blocks);
        wr32(g + 64, (uint32_t)st->st_blksize);
    } else {          // i386 struct stat64 (108 bytes, 4-byte aligned 64-bit fields)
        memset(M + g, 0, 108);
        wr32(g + 0, (uint32_t)st->st_dev); wr16(g + 4, st->st_mode); wr16(g + 6, st->st_nlink);
        wr64(g + 8, st->st_ino); wr32(g + 16, st->st_uid); wr32(g + 20, st->st_gid); wr32(g + 24, (uint32_t)st->st_rdev);
        wr32(g + 28, (uint32_t)st->st_atimespec.tv_sec); wr32(g + 32, (uint32_t)st->st_atimespec.tv_nsec);
        wr32(g + 36, (uint32_t)st->st_mtimespec.tv_sec); wr32(g + 40, (uint32_t)st->st_mtimespec.tv_nsec);
        wr32(g + 44, (uint32_t)st->st_ctimespec.tv_sec); wr32(g + 48, (uint32_t)st->st_ctimespec.tv_nsec);
        wr32(g + 52, (uint32_t)st->st_birthtimespec.tv_sec); wr32(g + 56, (uint32_t)st->st_birthtimespec.tv_nsec);
        wr64(g + 60, (uint64_t)st->st_size); wr64(g + 68, (uint64_t)st->st_blocks); wr32(g + 76, (uint32_t)st->st_blksize);
    }
}
void I__stat(CPU *c) { struct stat st; int r = stat(S(ARG(0)), &st); tf("stat", S(ARG(0)), r); if (!r) stat_to_guest(&st, ARG(1), 0); else seterr(); RET(r); }
void I__stat_INODE64(CPU *c) { struct stat st; int r = stat(S(ARG(0)), &st); tf("stat", S(ARG(0)), r); if (!r) stat_to_guest(&st, ARG(1), 1); else seterr(); RET(r); }
void I__fstat(CPU *c) { struct stat st; int r = fstat((int)ARG(0), &st); if (!r) stat_to_guest(&st, ARG(1), 0); RET(r); }
void I__fstat_INODE64(CPU *c) { struct stat st; int r = fstat((int)ARG(0), &st); if (!r) stat_to_guest(&st, ARG(1), 1); RET(r); }

// ---------- directories: guest DIR = {magic, host index, dirent buffer...} ----------
#define NDIRS 64
static DIR *hdirs[NDIRS];
static uint32_t gopendir(const char *p) {
    DIR *d = opendir(p);
    if (!d) { seterr(); return 0; }
    for (int i = 0; i < NDIRS; i++) if (!hdirs[i]) {
        hdirs[i] = d; uint32_t g = gmalloc(16 + 1080); wr32(g, 0x44495221); wr32(g + 4, (uint32_t)i); return g;
    }
    closedir(d); return 0;
}
void I__opendir_UNIX2003(CPU *c) { uint32_t d = gopendir(S(ARG(0))); tf("opendir", S(ARG(0)), d != 0); RET(d); }
void I__opendir_INODE64_UNIX2003(CPU *c) { uint32_t d = gopendir(S(ARG(0))); tf("opendir", S(ARG(0)), d != 0); RET(d); }
void I__closedir_UNIX2003(CPU *c) {
    uint32_t g = ARG(0); int i = (int)rd32(g + 4);
    closedir(hdirs[i]); hdirs[i] = NULL; gfree(g); RET(0);
}
static void greaddir(CPU *c, int inode64) {
    uint32_t g = ARG(0);
    struct dirent *e = readdir(hdirs[rd32(g + 4)]);
    if (!e) { RET(0); return; }
    uint32_t de = g + 16;
    memset(M + de, 0, 1080);
    if (!inode64) {
        wr32(de, (uint32_t)e->d_ino); wr16(de + 4, 264); wr8(de + 6, e->d_type);
        size_t n = strlen(e->d_name); if (n > 255) n = 255;
        wr8(de + 7, (uint8_t)n); memcpy(M + de + 8, e->d_name, n);
    } else {
        wr64(de, e->d_ino); wr64(de + 8, e->d_seekoff); wr16(de + 16, 1048);
        size_t n = strlen(e->d_name); if (n > 1023) n = 1023;
        wr16(de + 18, (uint16_t)n); wr8(de + 20, e->d_type); memcpy(M + de + 21, e->d_name, n);
    }
    RET(de);
}
void I__readdir(CPU *c) { greaddir(c, 0); }
void I__readdir_INODE64(CPU *c) { greaddir(c, 1); }

// ---------- stdio extras ----------
void I__fopen_DARWIN_EXTSN(CPU *c) { FILE *f = fopen(S(ARG(0)), S(ARG(1))); if (!f) seterr(); tf("fopen", S(ARG(0)), f != NULL); RET(gfile_wrap(f)); }
void I__feof(CPU *c) { RET(feof(gfile_host(ARG(0)))); }
void I__fgetc(CPU *c) { RET(fgetc(gfile_host(ARG(0)))); }
void I__fileno(CPU *c) { RET(fileno(gfile_host(ARG(0)))); }

// ---------- POSIX named semaphores ----------
#define NSEMS 256
static sem_t *hsems[NSEMS];
void I__sem_open(CPU *c) {
    sem_t *s = sem_open(S(ARG(0)), (int)ARG(1), (mode_t)ARG(2), ARG(3));
    if (s == SEM_FAILED) { seterr(); RET(-1); return; }
    for (int i = 1; i < NSEMS; i++) if (!hsems[i]) { hsems[i] = s; RET(i); return; }
    RET(-1);
}
void I__sem_close(CPU *c) { int i = (int)ARG(0); RET(sem_close(hsems[i])); hsems[i] = NULL; }
void I__sem_unlink(CPU *c) { RET(sem_unlink(S(ARG(0)))); }
void I__sem_post(CPU *c) { RET(sem_post(hsems[ARG(0)])); }
void I__sem_wait_UNIX2003(CPU *c) { RET(sem_wait(hsems[ARG(0)])); }
void I__sem_trywait(CPU *c) { RET(sem_trywait(hsems[ARG(0)])); }

// ---------- sockets: networking features are disabled in the port ----------
#define NONET(n) void I__##n(CPU *c) { errno = ENETDOWN; seterr(); RET(-1); }
NONET(socket) NONET(connect_UNIX2003) NONET(bind_UNIX2003) NONET(listen_UNIX2003) NONET(accept_UNIX2003)
NONET(send_UNIX2003) NONET(sendto_UNIX2003) NONET(recv_UNIX2003) NONET(recvfrom_UNIX2003) NONET(setsockopt)
NONET(getsockopt) NONET(shutdown) NONET(getpeername_UNIX2003) NONET(getsockname_UNIX2003)
NONET(select_UNIX2003) NONET(select_DARWIN_EXTSN)
void I__gethostbyname(CPU *c) { RET(0); }
