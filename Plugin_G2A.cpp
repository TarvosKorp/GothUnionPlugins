#include <stdio.h>
#include <stdarg.h>
#include <string.h>

// zMapUnlocker — focused input diagnostics for Gothic II Addon.
// The log uses an absolute path next to Gothic2.exe: the game changes its
// current working directory during startup, so relative paths split the log.

namespace GOTHIC_ENGINE {

// Resolve once, independently of later SetCurrentDirectory/_chdir calls.
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

    // Also emit to the Windows debugger, even if the file cannot be opened.
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
    MapDiagLog("[cwd] %s cwd='%s' log='%s'", stage, cwd, MapDiagPath());
}

// zCInput_Win32::GetState(unsigned short) — G2A 0x004D4D30.
// Log zero results too: a quiet key state must not hide a successful hook.
float __fastcall Hook_Input_GetState(void* self, void* edx, unsigned short key);
CInvoke<float(__fastcall*)(void*, void*, unsigned short)> Ivk_Input_GetState(
    0x004D4D30, &Hook_Input_GetState, IvkEnabled(Engine_G2A));

float __fastcall Hook_Input_GetState(void* self, void* edx, unsigned short key) {
    float value = Ivk_Input_GetState(self, edx, key);
    static unsigned long count = 0;
    ++count;
    if (count <= 40 || count % 500 == 0)
        {
            if (count == 1) MapDiagCwd("first GetState");
            MapDiagLog("[input] GetState count=%lu self=%p key=0x%04X value=%.3f",
                count, self, (unsigned)key, value);
        }
    return value;
}

// zCInput_Win32::GetToggled(unsigned short) — G2A 0x004D5020.
int __fastcall Hook_Input_GetToggled(void* self, void* edx, unsigned short key);
CInvoke<int(__fastcall*)(void*, void*, unsigned short)> Ivk_Input_GetToggled(
    0x004D5020, &Hook_Input_GetToggled, IvkEnabled(Engine_G2A));

int __fastcall Hook_Input_GetToggled(void* self, void* edx, unsigned short key) {
    int value = Ivk_Input_GetToggled(self, edx, key);
    static unsigned long count = 0;
    ++count;
    if (count <= 40 || count % 500 == 0)
        MapDiagLog("[input] GetToggled count=%lu self=%p key=0x%04X value=%d",
            count, self, (unsigned)key, value);
    return value;
}

// zCInput_Win32::GetMousePos(float&, float&, float&) — G2A 0x004D5730.
void __fastcall Hook_Input_GetMousePos(void* self, void* edx, float& x, float& y, float& z);
CInvoke<void(__fastcall*)(void*, void*, float&, float&, float&)> Ivk_Input_GetMousePos(
    0x004D5730, &Hook_Input_GetMousePos, IvkEnabled(Engine_G2A));

void __fastcall Hook_Input_GetMousePos(void* self, void* edx, float& x, float& y, float& z) {
    Ivk_Input_GetMousePos(self, edx, x, y, z);
    static unsigned long count = 0;
    ++count;
    if (count <= 12 || count % 500 == 0)
        MapDiagLog("[input] GetMousePos count=%lu self=%p x=%.3f y=%.3f z=%.3f",
            count, self, x, y, z);
}

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
    void** vt = *(void***)inputObject;
    MapDiagLog("[runtime] %s zinput=%p vtable=%p GetState[1]=%p GetToggled[3]=%p GetMousePos[20]=%p ProcessInputEvents[29]=%p",
        stage, inputObject, vt, vt[1], vt[3], vt[20], vt[29]);
}

// Union application callbacks. These remain as lifecycle probes; all old
// unrelated render, map-screen, document-manager and input-pump hooks are removed.
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
    MapDiagLog("[plugin] Game_Entry");
    MapDiagCwd("Game_Entry");
    MapDiagHookState("Input::GetState", const_cast<THookInfo&>(Ivk_Input_GetState.GetHookInfo()));
    MapDiagHookState("Input::GetToggled", const_cast<THookInfo&>(Ivk_Input_GetToggled.GetHookInfo()));
    MapDiagHookState("Input::GetMousePos", const_cast<THookInfo&>(Ivk_Input_GetMousePos.GetHookInfo()));
    MapDiagRuntimeProbe("Game_Entry");
}

void MapDiagCallback(const char* name, unsigned long& count) {
    ++count;
    if (count == 1 || count % 1000 == 0)
        MapDiagLog("[callback] %s count=%lu", name, count);
}

void Game_Init() { MapDiagLog("[callback] Game_Init"); MapDiagCwd("Game_Init"); MapDiagRuntimeProbe("Game_Init"); }
void Game_Exit() { MapDiagLog("[callback] Game_Exit"); }
void Game_PreLoop() { static unsigned long n = 0; MapDiagCallback("Game_PreLoop", n); }
void Game_Loop() {
    static unsigned long n = 0;
    MapDiagCallback("Game_Loop", n);
    if (n == 1) { MapDiagCwd("Game_Loop"); MapDiagRuntimeProbe("Game_Loop"); }
}
void Game_PostLoop() { static unsigned long n = 0; MapDiagCallback("Game_PostLoop", n); }
void Game_MenuLoop() { static unsigned long n = 0; MapDiagCallback("Game_MenuLoop", n); }

void Game_SaveBegin() { MapDiagLog("[callback] Game_SaveBegin"); }
void Game_SaveEnd() { MapDiagLog("[callback] Game_SaveEnd"); }
void Game_LoadBegin_NewGame() { MapDiagLog("[callback] Game_LoadBegin_NewGame"); }
void Game_LoadEnd_NewGame() { MapDiagLog("[callback] Game_LoadEnd_NewGame"); }
void Game_LoadBegin_SaveGame() { MapDiagLog("[callback] Game_LoadBegin_SaveGame"); }
void Game_LoadEnd_SaveGame() { MapDiagLog("[callback] Game_LoadEnd_SaveGame"); }
void Game_LoadBegin_ChangeLevel() { MapDiagLog("[callback] Game_LoadBegin_ChangeLevel"); }
void Game_LoadEnd_ChangeLevel() { MapDiagLog("[callback] Game_LoadEnd_ChangeLevel"); }
void Game_LoadBegin_Trigger() { MapDiagLog("[callback] Game_LoadBegin_Trigger"); }
void Game_LoadEnd_Trigger() { MapDiagLog("[callback] Game_LoadEnd_Trigger"); }
void Game_Pause() { MapDiagLog("[callback] Game_Pause"); }
void Game_Unpause() { MapDiagLog("[callback] Game_Unpause"); }
void Game_DefineExternals() { MapDiagLog("[callback] Game_DefineExternals"); }
void Game_ApplyOptions() { MapDiagLog("[callback] Game_ApplyOptions"); }

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
