// Boot the whole recompiled engine (no window yet): static ctors, then GameEngine_Start.
// usage: boot ORIG_BINARY GAME_RESOURCES_DIR
#include "rt.h"
#include "tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s ORIG_BINARY RESOURCES_DIR\n", argv[0]); return 1; }
    CPU *c = rt_init(argv[1]);
    if (chdir(argv[2])) { perror(argv[2]); return 1; }
    extern void platform_set_resources(const char *); platform_set_resources(argv[2]);
    {   // Steam app id from the game's steam_appid.txt (next to the executable)
        char p[2048]; snprintf(p, sizeof p, "%s/../MacOS/steam_appid.txt", argv[2]);
        FILE *f = fopen(p, "r"); unsigned id = 0;
        if (f) { if (fscanf(f, "%u", &id) != 1) id = 0; fclose(f); }
        extern void steam_set_app_id(uint32_t); steam_set_app_id(id);
    }
    if (getenv("JP_SELFTEST")) {   // CRC64 symbol hashing check
        const char *keys[] = { "System Script", "Project Script", "_boot.lua", "Game Language" };
        for (int i = 0; i < 4; i++) {
            uint32_t a[3] = { 0, 0, gstrdup(keys[i]) };
            guest_call(c, guest_func("__Z21CRC64_CaseInsensitiveyPKc"), 3, a);
            printf("crc64 %s %08x%08x\n", keys[i], c->edx, c->eax);
        }
        return 0;
    }
    fprintf(stderr, "[boot] running %d static constructors\n", ngmodinit);
    for (int i = 0; i < ngmodinit; i++) {
        uint32_t a[4] = { 0, 0, 0, 0 };
        guest_call(c, gmodinit[i], 4, a);
    }
    fprintf(stderr, "[boot] static constructors done; calling GameEngine_Start\n");
    uint32_t args = gmalloc(16);
    memset(M + args, 0, 16);
    uint32_t a[4] = { gstrdup(""), args, args + 4, args + 8 };
    wr8(args + 8, 1);   // windowed
    uint32_t ok = guest_call(c, guest_func("_GameEngine_Start"), 4, a);
    fprintf(stderr, "[boot] GameEngine_Start -> %u (window %gx%g windowed=%d)\n",
            ok & 0xFF, rdf(args), rdf(args + 4), rd8(args + 8));
    if (getenv("JP_DUMP")) {   // JP_DUMP=addr:len[,addr:len...]
        char buf[1024]; snprintf(buf, sizeof buf, "%s", getenv("JP_DUMP"));
        for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
            uint32_t a = (uint32_t)strtoul(t, NULL, 16), n = strchr(t, ':') ? (uint32_t)strtoul(strchr(t, ':') + 1, NULL, 0) : 16;
            fprintf(stderr, "[dump] %08x:", a);
            for (uint32_t i = 0; i < n; i++) fprintf(stderr, " %02x", rd8(a + i));
            fprintf(stderr, "\n");
        }
    }
    if (getenv("JP_PROPTEST")) {   // look up "LUA System File" in the preferences ourselves
        uint32_t h = guest_call(c, guest_func("__ZN10GameEngine14GetPreferencesEv"), 0, NULL);
        uint32_t info = rd32(h), ps = rd32(info + 0x18);
        fprintf(stderr, "[prop] prefs handle %x info %x propset %x flags %x\n", h, info, ps, rd32(info + 0x20));
        uint32_t str = gmalloc(4); wr32(str, rd32(rd32(0x4ff024)));
        uint32_t a[4] = { ps, 0x57d030, str, 1 };
        uint32_t r = guest_call(c, guest_func("__ZNK11PropertySet11GetKeyValueI6StringEEbRK6SymbolRT_NS_17eParentSearchModeE"), 4, a);
        fprintf(stderr, "[prop] GetKeyValue<String>(LUA System File) -> %d '%s'\n", r & 0xFF, (char *)G2H(rd32(str)));
        uint32_t a2[2] = { ps, 0 };
        (void)a2;
    }
    if (getenv("JP_PREFKEYS")) {   // JP_PREFKEYS="Key Name,Other Key": print int values from the preferences
        uint32_t h = guest_call(c, guest_func("__ZN10GameEngine14GetPreferencesEv"), 0, NULL);
        uint32_t ps = rd32(rd32(h) + 0x18);
        char buf[1024]; snprintf(buf, sizeof buf, "%s", getenv("JP_PREFKEYS"));
        for (char *k = strtok(buf, ","); k; k = strtok(NULL, ",")) {
            uint32_t a[3] = { 0, 0, gstrdup(k) };
            guest_call(c, guest_func("__Z21CRC64_CaseInsensitiveyPKc"), 3, a);
            uint32_t sym = gmalloc(8); wr32(sym, c->eax); wr32(sym + 4, c->edx);
            uint32_t out = gmalloc(8); wr32(out, 0xDEADBEEF);
            uint32_t q[4] = { ps, sym, out, 1 };
            uint32_t r = guest_call(c, guest_func("__ZNK11PropertySet11GetKeyValueIiEEbRK6SymbolRT_NS_17eParentSearchModeE"), 4, q) & 0xFF;
            fprintf(stderr, "[pref] %s = %s%d\n", k, r ? "" : "(missing) ", (int)rd32(out));
        }
    }
    int frames = getenv("JP_FRAMES") ? atoi(getenv("JP_FRAMES")) : 0;
    uint32_t loop = guest_func("_GameEngine_Loop"), quit = guest_func("_GameEngine_RequestedQuit");
    for (int f = 0; f < frames; f++) {
        guest_call(c, loop, 0, NULL);
        int q = guest_call(c, quit, 0, NULL) & 0xFF;
        if (f % 50 == 0 || q) fprintf(stderr, "[boot] frame %d done%s\n", f, q ? " - game requested quit" : "");
        if (q) break;
    }
    return 0;
}
