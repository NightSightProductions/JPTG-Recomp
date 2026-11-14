// Steamworks replacement: the port runs without the Steam client.
// Interfaces are guest objects whose vtables point at native stubs that log their slot;
// methods the game really needs get behaviour as they are identified.
#include "rt.h"
#include "tables.h"
#include <stdio.h>
#include <stdlib.h>

#define RET(v) (c->eax = (uint32_t)(v))
#define NSLOTS 96
static const char *iface_name[4];
static uint8_t seen[4][NSLOTS];

static uint32_t app_id = 0;
static char ach_path[1024];
void steam_set_app_id(uint32_t id) {
    app_id = id;
    snprintf(ach_path, sizeof ach_path, "%s/Library/Application Support/Telltale Games/JurassicPark100/achievements_offline.txt",
             getenv("HOME") ? getenv("HOME") : ".");
}

static int stat_get(const char *name, char *val);
static void stat_set(const char *name, const char *val);
static void stats_received(void);
static void stats_stored(void);
static int ach_has(const char *name);
static void ach_set(const char *name);
static void vcall(CPU *c, uint32_t ctx) {
    int iface = (int)(ctx >> 8), slot = (int)(ctx & 0xFF);
    // known methods (Steamworks SDK of 2011 interface layouts)
    if (iface == 0 && slot == 1) { RET(1); return; }        // ISteamUser::BLoggedOn
    if (iface == 1 && slot == 34) { RET(1); return; }       // ISteamFriends::SetRichPresence (nothing to show offline)
    if (iface == 3 && slot == 9) { RET(app_id); return; }   // ISteamUtils::GetAppID
    if (iface == 2) {   // ISteamUserStats
        const char *name = ARG(1) ? (const char *)G2H(ARG(1)) : "";
        switch (slot) {
        case 0: stats_received(); RET(1); return;                          // RequestCurrentStats
        case 1: { char v[64]; int ok = stat_get(name, v); if (ARG(2)) wr32(ARG(2), ok ? (uint32_t)atoi(v) : 0); RET(1); return; }   // GetStat(name, int32*)
        case 2: { char v[64]; int ok = stat_get(name, v); if (ARG(2)) wrf(ARG(2), ok ? (float)atof(v) : 0.0f); RET(1); return; } // GetStat(name, float*)
        case 3: { char v[64]; snprintf(v, sizeof v, "%d", (int32_t)ARG(2)); stat_set(name, v); RET(1); return; }                   // SetStat(name, int32)
        case 4: { char v[64]; snprintf(v, sizeof v, "%g", (double)ARGF(2)); stat_set(name, v); RET(1); return; }                   // SetStat(name, float)
        case 6: if (ARG(2)) wr8(ARG(2), (uint8_t)ach_has(name)); RET(1); return;  // GetAchievement(name, bool*)
        case 7: ach_set(name); RET(1); return;                             // SetAchievement(name)
        case 10: stats_stored(); RET(1); return;                           // StoreStats
        }
    }
    if (!seen[iface][slot]) {
        seen[iface][slot] = 1;
        fprintf(stderr, "[steam] %s vtable slot %d (from %s) -> 0\n", iface_name[iface], slot, rt_func_name(rd32(c->esp)));
    }
    RET(0);
}
static uint32_t ifc[4];
static uint32_t get(int id, const char *name) {
    if (ifc[id]) return ifc[id];
    iface_name[id] = name;
    uint32_t vt = gmalloc(4 * NSLOTS), obj = gmalloc(16);
    for (int k = 0; k < NSLOTS; k++) wr32(vt + 4 * k, rt_host_fn_ctx(vcall, ((uint32_t)id << 8) | (uint32_t)k));
    wr32(obj, vt);
    return ifc[id] = obj;
}

void I__SteamAPI_Init(CPU *c) { fprintf(stderr, "[steam] SteamAPI_Init -> true (offline port)\n"); RET(1); }
// ---------- callbacks ----------
// CCallbackBase: vptr, uint8 flags @4, int iCallback @8; vtable: [0] Run(void *param)
#define NCB 64
static struct { uint32_t obj; int id; } cbs[NCB];
static struct { int id; uint8_t data[64]; uint32_t size; } pending[32];
static int npending;
static void queue_cb(int id, const void *data, uint32_t size) {
    if (npending < 32) { pending[npending].id = id; memcpy(pending[npending].data, data, size); pending[npending].size = size; npending++; }
}
void I__SteamAPI_RegisterCallback(CPU *c) {
    uint32_t obj = ARG(0); int id = (int)ARG(1);
    wr8(obj + 4, rd8(obj + 4) | 1); wr32(obj + 8, (uint32_t)id);   // k_ECallbackFlagsRegistered
    for (int i = 0; i < NCB; i++) if (!cbs[i].obj) { cbs[i].obj = obj; cbs[i].id = id; break; }
    fprintf(stderr, "[steam] RegisterCallback id %d\n", id);
}
void I__SteamAPI_UnregisterCallback(CPU *c) {
    for (int i = 0; i < NCB; i++) if (cbs[i].obj == ARG(0)) cbs[i].obj = 0;
}
void I__SteamAPI_RunCallbacks(CPU *c) {
    int n = npending; npending = 0;
    for (int k = 0; k < n; k++) {
        uint32_t buf = gmalloc(64); memcpy(M + buf, pending[k].data, pending[k].size);
        for (int i = 0; i < NCB; i++) if (cbs[i].obj && cbs[i].id == pending[k].id) {
            uint32_t a[2] = { cbs[i].obj, buf };
            guest_call(c, rd32(rd32(cbs[i].obj)), 2, a);   // virtual Run(void *)
        }
        gfree(buf);
    }
}

// ---------- local achievements (no Steam): kept in the game's user folder ----------
static int ach_has(const char *name) {
    FILE *f = fopen(ach_path, "r"); char line[256]; int r = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) { line[strcspn(line, "\n")] = 0; if (!strcmp(line, name)) { r = 1; break; } }
    fclose(f); return r;
}
static void ach_set(const char *name) {
    if (ach_has(name)) return;
    FILE *f = fopen(ach_path, "a"); if (f) { fprintf(f, "%s\n", name); fclose(f); }
    fprintf(stderr, "[steam] achievement unlocked: %s\n", name);
}
static void stats_received(void) {   // UserStatsReceived_t (1101): uint64 gameID, EResult, CSteamID
    uint8_t d[20] = {0}; uint64_t gid = app_id; uint32_t ok = 1; uint64_t sid = 0x0110000100000001ull;
    memcpy(d, &gid, 8); memcpy(d + 8, &ok, 4); memcpy(d + 12, &sid, 8);
    queue_cb(1101, d, sizeof d);
}
static void stats_stored(void) {     // UserStatsStored_t (1102): uint64 gameID, EResult
    uint8_t d[12] = {0}; uint64_t gid = app_id; uint32_t ok = 1;
    memcpy(d, &gid, 8); memcpy(d + 8, &ok, 4);
    queue_cb(1102, d, sizeof d);
}
void I__SteamUser(CPU *c) { RET(get(0, "ISteamUser")); }
void I__SteamFriends(CPU *c) { RET(get(1, "ISteamFriends")); }
void I__SteamUserStats(CPU *c) { RET(get(2, "ISteamUserStats")); }
void I__SteamUtils(CPU *c) { RET(get(3, "ISteamUtils")); }

// ---------- local stats: "name=value" lines next to the achievements file ----------
static void stats_path(char *p, size_t n) {
    snprintf(p, n, "%s/Library/Application Support/Telltale Games/JurassicPark100/stats_offline.txt", getenv("HOME") ? getenv("HOME") : ".");
}
static int stat_get(const char *name, char *val) {
    char p[1024], line[256]; stats_path(p, sizeof p);
    FILE *f = fopen(p, "r"); int found = 0;
    if (!f) return 0;
    size_t n = strlen(name);
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, name, n) && line[n] == '=') { snprintf(val, 64, "%s", line + n + 1); val[strcspn(val, "\n")] = 0; found = 1; }
    fclose(f);
    return found;
}
static void stat_set(const char *name, const char *val) {
    char p[1024], line[256]; stats_path(p, sizeof p);
    char *keep = calloc(1, 65536); size_t used = 0, n = strlen(name);
    FILE *f = fopen(p, "r");
    if (f) {
        while (fgets(line, sizeof line, f))
            if (!(strncmp(line, name, n) == 0 && line[n] == '=') && used + strlen(line) < 65535) { strcpy(keep + used, line); used += strlen(line); }
        fclose(f);
    }
    f = fopen(p, "w");
    if (f) { fputs(keep, f); fprintf(f, "%s=%s\n", name, val); fclose(f); }
    free(keep);
}
