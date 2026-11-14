// Proof of concept: run Telltale's own (recompiled) Lua 5.1 VM natively on arm64.
// usage: test_lua ORIG_BINARY [script.lua | bytecode.luac ...]
#include "rt.h"
#include "tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>

static uint32_t L;
static CPU *c;
static uint32_t fn_loadbuffer, fn_pcall, fn_tolstring, fn_settop;

static uint32_t put(const void *p, uint32_t n) {
    uint32_t g = gmalloc(n + 1); memcpy(M + g, p, n); M[g + n] = 0; return g;
}

static int run_chunk(const char *name, const void *buf, uint32_t len, int exec) {
    uint32_t gb = put(buf, len), gn = put(name, (uint32_t)strlen(name));
    uint32_t a1[] = { L, gb, len, gn };
    int32_t r = (int32_t)guest_call(c, fn_loadbuffer, 4, a1);
    if (r == 0 && exec) {
        uint32_t a2[] = { L, 0, 0, 0 };
        r = (int32_t)guest_call(c, fn_pcall, 4, a2);
    }
    if (r != 0) {
        uint32_t a3[] = { L, (uint32_t)-1, 0 };
        uint32_t s = guest_call(c, fn_tolstring, 3, a3);
        printf("[%s] error %d: %s\n", name, r, s ? (char *)G2H(s) : "?");
    } else if (!exec) {
        printf("[%s] loaded OK (%u bytes)\n", name, len);
    }
    uint32_t a4[] = { L, 0 };
    guest_call(c, fn_settop, 2, a4);
    gfree(gb); gfree(gn);
    return r;
}

static int zlib_test(void) {
    // Compress with the recompiled zlib, decompress with the host zlib.
    static unsigned char src[200000];
    for (size_t i = 0; i < sizeof src; i++) src[i] = (unsigned char)("JURASSIC PARK "[i % 14] ^ (i / 997));
    uint32_t gsrc = put(src, sizeof src), cap = compressBound(sizeof src);
    uint32_t gdst = gmalloc(cap), glen = gmalloc(4);
    wr32(glen, cap);
    uint32_t a[] = { gdst, glen, gsrc, sizeof src, 9 };
    int32_t r = (int32_t)guest_call(c, guest_func("_compress2"), 5, a);
    uLongf outlen = sizeof src; static unsigned char back[sizeof src];
    int hr = uncompress(back, &outlen, M + gdst, rd32(glen));
    int ok = r == 0 && hr == Z_OK && outlen == sizeof src && !memcmp(back, src, sizeof src);
    printf("[zlib] guest compress2 -> %u bytes, host inflate %s\n", rd32(glen), ok ? "MATCHES" : "FAILED");
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s ORIG_BINARY [files...]\n", argv[0]); return 1; }
    c = rt_init(argv[1]);
    zlib_test();

    L = guest_call(c, guest_func("_luaL_newstate"), 0, NULL);
    printf("[lua] luaL_newstate %s\n", L ? "OK" : "FAILED");
    uint32_t a[] = { L };
    guest_call(c, guest_func("_luaL_openlibs"), 1, a);
    fn_loadbuffer = guest_func("_luaL_loadbuffer");
    fn_pcall = guest_func("_lua_pcall");
    fn_tolstring = guest_func("_lua_tolstring");
    fn_settop = guest_func("_lua_settop");

    for (int i = 2; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { perror(argv[i]); continue; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        char *b = malloc(n); fread(b, 1, n, f); fclose(f);
        int is_bytecode = n > 0 && b[0] == 0x1b;
        const char *base = strrchr(argv[i], '/'); base = base ? base + 1 : argv[i];
        run_chunk(base, b, (uint32_t)n, !is_bytecode);   // bytecode: load only (needs engine bindings to run)
        free(b);
    }
    return 0;
}
