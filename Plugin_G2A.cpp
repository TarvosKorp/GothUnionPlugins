#include <stdio.h>
#include <stdarg.h>
#include <string.h>

// zMapUnlocker — iteration 3: find WHICH layer blocks movement while a document/map is open.
//
// Findings so far:
//  * Log works (absolute path next to Gothic2.exe).
//  * DocumentManager::HandleEvent receives DIK scancodes (W/A/S/D/Q/E/Esc). Returning 0
//    for them (already done) changed nothing -> the HandleEvent callback chain is NOT what
//    moves the hero. Movement is polled via zinput->GetState(GAME_*) (ids 1..4, 16..19 are
//    queried on the very first frame).
//
// This build answers, in ONE run:
//   A) Are GetState/GetMousePos/Game_Loop still polled while the doc is open? ([stats], per second)
//   B) What is the hero's state while the doc is open? ([hero]: interactItem, states, movlock, bodystate)
//   C) Are keyboard/mouse devices disabled? ([hero] kbd/mouse)
//   D) EXPERIMENT: while the doc is open, physically held W/S/A/D/Q/E are injected into
//      GetState(GAME_UP/DOWN/LEFT/RIGHT/STRAFELEFT/STRAFERIGHT). If the hero then walks,
//      the input layer is the lever. If not, the hero state (interactItem/animation) blocks.
//      Set MAPDIAG_FORCE_MOVE to 0 to disable the experiment.
#ifndef MAPDIAG_FORCE_MOVE
#define MAPDIAG_FORCE_MOVE 1
#endif

namespace GOTHIC_ENGINE {

// ---------------------------------------------------------------- logging
static const char* MapDiagPath() {
    static char path[MAX_PATH] = { 0 };
    static bool ready = false;
    if (!ready) {
        ready = true;
        DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            lstrcpyA(path, "zMapUnlocker.log");
        } else {
            char* slash = strrchr(path, '\\');
            if (slash) *(slash + 1) = 0;
            lstrcatA(path, "zMapUnlocker.log");
        }
    }
    return path;
}

void MapDiagReset() {
    FILE* file = fopen(MapDiagPath(), "w");
    if (file) fclose(file);
}

void MapDiagLog(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    _vsnprintf(line, sizeof(line) - 1, format, args);
    va_end(args);
    line[sizeof(line) - 1] = 0;

    OutputDebugStringA("[zMapUnlocker] ");
    OutputDebugStringA(line);
    OutputDebugStringA("\n");

    FILE* file = fopen(MapDiagPath(), "a");
    if (!file) return;
    fprintf(file, "%8lu | %s\n", (unsigned long)GetTickCount(), line);
    fclose(file);
}

void MapDiagCwd(const char* stage) {
    char cwd[MAX_PATH] = { 0 };
    GetCurrentDirectoryA(MAX_PATH, cwd);
    MapDiagLog("[cwd] %s cwd='%s'", stage, cwd);
}

// ---------------------------------------------------------------- shared state
static volatile bool g_docOpen = false;          // set by DocumentManager::HandleEvent, cleared on Esc / load
static unsigned long g_cLoop = 0, g_cPre = 0, g_cPost = 0, g_cMenu = 0;
static unsigned long g_cGetState = 0, g_cGetStateNZ = 0, g_cGetMouse = 0, g_cGetMouseNZ = 0, g_cToggled = 0;
static DWORD g_lastStats = 0, g_lastHero = 0;

static void MapDiagHeroProbe(const char* why) {
    oCNpc* hero = Gothic_II_Addon::player;
    zCInput* in = Gothic_II_Addon::zinput;
    int kbd = in ? in->GetDeviceEnabled(zINPUT_KEYBOARD) : -1;
    int mouse = in ? in->GetDeviceEnabled(zINPUT_MOUSE) : -1;
    if (!hero) {
        MapDiagLog("[hero] %s player=NULL kbdEnabled=%d mouseEnabled=%d", why, kbd, mouse);
        return;
    }
    MapDiagLog("[hero] %s player=%p interactItem=%p curState=%d tgtState=%d movlock=%d bodyState=%d weaponMode=%d kbdEnabled=%d mouseEnabled=%d",
        why, hero, hero->interactItem, hero->interactItemCurrentState, hero->interactItemTargetState,
        (int)hero->movlock, hero->GetBodyState(), hero->GetWeaponMode(), kbd, mouse);
}

// Called from several hooks/callbacks; rate-limited by tick count.
static void MapDiagTick() {
    DWORD now = GetTickCount();
    DWORD period = g_docOpen ? 1000 : 5000;
    if (g_lastStats == 0) g_lastStats = now;
    if (now - g_lastStats >= period) {
        MapDiagLog("[stats] doc=%d dt=%lu loop=%lu pre=%lu post=%lu menu=%lu GetState=%lu(nz=%lu) GetToggled=%lu GetMousePos=%lu(nz=%lu)",
            (int)g_docOpen, (unsigned long)(now - g_lastStats), g_cLoop, g_cPre, g_cPost, g_cMenu,
            g_cGetState, g_cGetStateNZ, g_cToggled, g_cGetMouse, g_cGetMouseNZ);
        g_cLoop = g_cPre = g_cPost = g_cMenu = 0;
        g_cGetState = g_cGetStateNZ = g_cGetMouse = g_cGetMouseNZ = g_cToggled = 0;
        g_lastStats = now;
    }
    if (g_docOpen && now - g_lastHero >= 1000) {
        g_lastHero = now;
        MapDiagHeroProbe("tick");
    }
}

// ---------------------------------------------------------------- input hooks
#if MAPDIAG_FORCE_MOVE
// Physically held key -> logical movement id (only used while a document is open).
static float MapDiagForcedValue(unsigned short logical) {
    int vk = 0;
    switch (logical) {
    case 3:  vk = 'W'; break;   // GAME_UP
    case 4:  vk = 'S'; break;   // GAME_DOWN
    case 1:  vk = 'A'; break;   // GAME_LEFT  (turn)
    case 2:  vk = 'D'; break;   // GAME_RIGHT (turn)
    case 18: vk = 'Q'; break;   // GAME_STRAFELEFT
    case 19: vk = 'E'; break;   // GAME_STRAFERIGHT
    default: return 0.0f;
    }
    return (GetAsyncKeyState(vk) & 0x8000) ? 1.0f : 0.0f;
}
#endif

// zCInput_Win32::GetState(unsigned short) — G2A 0x004D4D30.
float __fastcall Hook_Input_GetState(void* self, void* edx, unsigned short key);
CInvoke<float(__fastcall*)(void*, void*, unsigned short)> Ivk_Input_GetState(
    0x004D4D30, &Hook_Input_GetState, IvkEnabled(Engine_G2A));

float __fastcall Hook_Input_GetState(void* self, void* edx, unsigned short key) {
    float raw = Ivk_Input_GetState(self, edx, key);
    float value = raw;
    ++g_cGetState;
    if (raw != 0.0f) ++g_cGetStateNZ;

#if MAPDIAG_FORCE_MOVE
    if (g_docOpen && raw == 0.0f) {
        float forced = MapDiagForcedValue(key);
        if (forced != 0.0f) value = forced;
    }
#endif

    static DWORD lastLog[1024] = { 0 };
    static float lastValue[1024] = { 0.0f };
    static bool seen[1024] = { false };
    unsigned int slot = key < 1024 ? (unsigned int)key : 1023;
    DWORD now = GetTickCount();
    bool changed = !seen[slot] || value != lastValue[slot];
    if (changed || (value != 0.0f && now - lastLog[slot] >= 200)) {
        MapDiagLog("[input] GetState doc=%d key=%u raw=%.3f returned=%.3f",
            (int)g_docOpen, (unsigned)key, raw, value);
        lastLog[slot] = now;
        lastValue[slot] = value;
        seen[slot] = true;
    }
    MapDiagTick();
    return value;
}

// zCInput_Win32::GetToggled(unsigned short) — G2A 0x004D5020.
int __fastcall Hook_Input_GetToggled(void* self, void* edx, unsigned short key);
CInvoke<int(__fastcall*)(void*, void*, unsigned short)> Ivk_Input_GetToggled(
    0x004D5020, &Hook_Input_GetToggled, IvkEnabled(Engine_G2A));

int __fastcall Hook_Input_GetToggled(void* self, void* edx, unsigned short key) {
    int value = Ivk_Input_GetToggled(self, edx, key);
    ++g_cToggled;
    static DWORD lastLog[1024] = { 0 };
    unsigned int slot = key < 1024 ? (unsigned int)key : 1023;
    DWORD now = GetTickCount();
    if (value != 0 && now - lastLog[slot] >= 200) {
        MapDiagLog("[input] GetToggled doc=%d key=%u value=%d", (int)g_docOpen, (unsigned)key, value);
        lastLog[slot] = now;
    }
    return value;
}

// zCInput_Win32::GetMousePos(float&, float&, float&) — G2A 0x004D5730.
void __fastcall Hook_Input_GetMousePos(void* self, void* edx, float& x, float& y, float& z);
CInvoke<void(__fastcall*)(void*, void*, float&, float&, float&)> Ivk_Input_GetMousePos(
    0x004D5730, &Hook_Input_GetMousePos, IvkEnabled(Engine_G2A));

void __fastcall Hook_Input_GetMousePos(void* self, void* edx, float& x, float& y, float& z) {
    Ivk_Input_GetMousePos(self, edx, x, y, z);
    ++g_cGetMouse;
    static DWORD lastLog = 0;
    static bool wasMoving = false;
    DWORD now = GetTickCount();
    bool moving = x != 0.0f || y != 0.0f || z != 0.0f;
    if (moving) ++g_cGetMouseNZ;
    if ((moving && (!wasMoving || now - lastLog >= 200)) || (!moving && wasMoving)) {
        MapDiagLog("[input] GetMousePos doc=%d x=%.3f y=%.3f z=%.3f", (int)g_docOpen, x, y, z);
        lastLog = now;
    }
    wasMoving = moving;
    MapDiagTick();
}

// ---------------------------------------------------------------- DocumentManager::HandleEvent
// oCDocumentManager::HandleEvent(int) — G2A 0x0065F450.
// Kept from iteration 2 (movement keys are not swallowed). Also drives g_docOpen.
static bool MapDiagIsMovementKey(int event) {
    switch ((unsigned int)event & 0xFF) {
    case 0x11: case 0x1F: case 0x1E: case 0x20: case 0x10: case 0x12:   // W S A D Q E
    case 0xC8: case 0xD0: case 0xCB: case 0xCD:                          // arrows
        return true;
    default:
        return false;
    }
}

int __fastcall Hook_DocMan_HandleEvent(void* self, void* edx, int event);
CInvoke<int(__fastcall*)(void*, void*, int)> Ivk_DocMan_HandleEvent(
    0x0065F450, &Hook_DocMan_HandleEvent, IvkEnabled(Engine_G2A));

int __fastcall Hook_DocMan_HandleEvent(void* self, void* edx, int event) {
    if (!g_docOpen && ((unsigned int)event & 0xFF) != 0x01) {
        g_docOpen = true;
        g_lastStats = 0;
        MapDiagLog("[doc] OPEN (first HandleEvent event=0x%08X)", (unsigned)event);
        MapDiagHeroProbe("doc-open");
    }

    int originalResult = Ivk_DocMan_HandleEvent(self, edx, event);

    if (((unsigned int)event & 0xFF) == 0x01) {          // Esc closes the document
        MapDiagLog("[doc] ESC original=%d", originalResult);
        MapDiagHeroProbe("doc-esc");
        g_docOpen = false;
    }

    static DWORD lastLog[256] = { 0 };
    unsigned int slot = (unsigned int)event & 0xFF;
    DWORD now = GetTickCount();
    bool passMovement = MapDiagIsMovementKey(event);
    int result = passMovement ? 0 : originalResult;
    if (now - lastLog[slot] >= 200) {
        MapDiagLog("[map-input] HandleEvent event=0x%08X original=%d returned=%d", (unsigned)event, originalResult, result);
        lastLog[slot] = now;
    }
    return result;
}

// ---------------------------------------------------------------- hook state / runtime probes
void MapDiagHookState(const char* name, THookInfo& info) {
    MapDiagLog("[hook] %s hooked=%d exists=%d original=%p trampoline=%p detour=%p",
        name, (int)CCallBack::IsHooked(info), (int)CCallBack::IsExist(info),
        info.lpOriginal, info.lpPointer, info.lpDetour);
}

void MapDiagRuntimeProbe(const char* stage) {
    Gothic_II_Addon::zCInput* inputObject = Gothic_II_Addon::zinput;
    if (!inputObject) {
        MapDiagLog("[runtime] %s zinput=NULL", stage);
        return;
    }
    MapDiagLog("[runtime] %s zinput=%p", stage, inputObject);
}

// ---------------------------------------------------------------- Union application callbacks
void Game_Entry();
void Game_Init();
void Game_Exit();
void Game_PreLoop();
void Game_Loop();
void Game_PostLoop();
void Game_MenuLoop();
void Game_SaveBegin();
void Game_SaveEnd();
void Game_LoadBegin_NewGame();
void Game_LoadEnd_NewGame();
void Game_LoadBegin_SaveGame();
void Game_LoadEnd_SaveGame();
void Game_LoadBegin_ChangeLevel();
void Game_LoadEnd_ChangeLevel();
void Game_LoadBegin_Trigger();
void Game_LoadEnd_Trigger();
void Game_Pause();
void Game_Unpause();
void Game_DefineExternals();
void Game_ApplyOptions();

TSaveLoadGameInfo& SaveLoadGameInfo = UnionCore::SaveLoadGameInfo;

extern CApplication* lpApplication;

void Game_Entry() {
    MapDiagReset();
    MapDiagLog("[plugin] Game_Entry (iteration 3, FORCE_MOVE=%d)", (int)MAPDIAG_FORCE_MOVE);
    MapDiagCwd("Game_Entry");
    MapDiagHookState("Input::GetState", const_cast<THookInfo&>(Ivk_Input_GetState.GetHookInfo()));
    MapDiagHookState("Input::GetToggled", const_cast<THookInfo&>(Ivk_Input_GetToggled.GetHookInfo()));
    MapDiagHookState("Input::GetMousePos", const_cast<THookInfo&>(Ivk_Input_GetMousePos.GetHookInfo()));
    MapDiagHookState("DocumentManager::HandleEvent", const_cast<THookInfo&>(Ivk_DocMan_HandleEvent.GetHookInfo()));
    MapDiagRuntimeProbe("Game_Entry");
}

void Game_Init() { MapDiagLog("[callback] Game_Init"); MapDiagRuntimeProbe("Game_Init"); }
void Game_Exit() { MapDiagLog("[callback] Game_Exit"); }
void Game_PreLoop() { ++g_cPre; MapDiagTick(); }
void Game_Loop() {
    ++g_cLoop;
    MapDiagTick();
}
void Game_PostLoop() { ++g_cPost; }
void Game_MenuLoop() { ++g_cMenu; MapDiagTick(); }

void Game_SaveBegin() { MapDiagLog("[callback] Game_SaveBegin"); }
void Game_SaveEnd() { MapDiagLog("[callback] Game_SaveEnd"); }
void Game_LoadBegin_NewGame() { g_docOpen = false; MapDiagLog("[callback] Game_LoadBegin_NewGame"); }
void Game_LoadEnd_NewGame() { MapDiagLog("[callback] Game_LoadEnd_NewGame"); }
void Game_LoadBegin_SaveGame() { g_docOpen = false; MapDiagLog("[callback] Game_LoadBegin_SaveGame"); }
void Game_LoadEnd_SaveGame() { MapDiagLog("[callback] Game_LoadEnd_SaveGame"); }
void Game_LoadBegin_ChangeLevel() { g_docOpen = false; MapDiagLog("[callback] Game_LoadBegin_ChangeLevel"); }
void Game_LoadEnd_ChangeLevel() { MapDiagLog("[callback] Game_LoadEnd_ChangeLevel"); }
void Game_LoadBegin_Trigger() { }
void Game_LoadEnd_Trigger() { }
void Game_Pause() { MapDiagLog("[callback] Game_Pause doc=%d", (int)g_docOpen); }
void Game_Unpause() { MapDiagLog("[callback] Game_Unpause doc=%d", (int)g_docOpen); }
void Game_DefineExternals() { MapDiagLog("[callback] Game_DefineExternals"); }
void Game_ApplyOptions() { }

#define AppDefault True
CApplication* lpApplication = !CHECK_THIS_ENGINE ? Null : CApplication::CreateRefApplication(
    Enabled(AppDefault) Game_Entry,
    Enabled(AppDefault) Game_Init,
    Enabled(AppDefault) Game_Exit,
    Enabled(AppDefault) Game_PreLoop,
    Enabled(AppDefault) Game_Loop,
    Enabled(AppDefault) Game_PostLoop,
    Enabled(AppDefault) Game_MenuLoop,
    Enabled(AppDefault) Game_SaveBegin,
    Enabled(AppDefault) Game_SaveEnd,
    Enabled(AppDefault) Game_LoadBegin_NewGame,
    Enabled(AppDefault) Game_LoadEnd_NewGame,
    Enabled(AppDefault) Game_LoadBegin_SaveGame,
    Enabled(AppDefault) Game_LoadEnd_SaveGame,
    Enabled(AppDefault) Game_LoadBegin_ChangeLevel,
    Enabled(AppDefault) Game_LoadEnd_ChangeLevel,
    Enabled(AppDefault) Game_LoadBegin_Trigger,
    Enabled(AppDefault) Game_LoadEnd_Trigger,
    Enabled(AppDefault) Game_Pause,
    Enabled(AppDefault) Game_Unpause,
    Enabled(AppDefault) Game_DefineExternals,
    Enabled(AppDefault) Game_ApplyOptions
);

} // namespace GOTHIC_ENGINE
