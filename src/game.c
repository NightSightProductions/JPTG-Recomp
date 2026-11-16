// Jurassic Park: The Game on Recomp-Framework (mac32): settings, and the engine allocator's native replacement.
// Shared by the app and the Lua test harness; the host hooks are in engine.c.
#include "game32.h"
#include "tables.h"

const Rt32Game rt32_game = {
    .name = "Jurassic Park: The Game",
    .app_bundle = "JurassicPark100.app",
    .exe_name = "JurassicPark100",
    .steam_folder = "JurassicPark",
    .prefs_domain = "com.telltalegames.JurassicPark100",   // the original game's: settings are shared with it
    .support_dir = "Telltale Games/JurassicPark100",        // offline Steam stats and achievements
    .log_name = "jp-recomp",
    .width = 1024, .height = 768,
};

// Telltale's C_AllocMemory / C_FreeMemory (HLE "outside": only in partial translations, as in the Lua test)
#define RET(v) (c->eax = (uint32_t)(v))
void H_C_AllocMemory(CPU *c) { RET(gmalloc(ARG(0))); }
void H_C_FreeMemory(CPU *c)  { gfree(ARG(0)); }
