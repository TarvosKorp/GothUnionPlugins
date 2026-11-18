#include <stdio.h>
#include <stdarg.h>

// zMapUnlocker — пассивная диагностика UpdatePosition.
// DocumentManager::Show отключён: его перехват сопровождался вылетом при открытии карты.

namespace GOTHIC_ENGINE {

void MapDiagReset() {
    // Start every game session with a clean diagnostic log.
    FILE* file = fopen("zMapUnlocker.log", "w");
    if (file) fclose(file);
}

void MapDiagLog(const char* format, ...) {
    FILE* file = fopen("zMapUnlocker.log", "a");
    if (!file) return;

    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fprintf(file, "\n");
    fclose(file);
}

// oCViewDocumentMap::UpdatePosition() — G2A: 0x0068D7B0.
void __fastcall Hook_MapView_UpdatePosition(void* self, void* edx);
CInvoke<void(__fastcall*)(void*, void*)> Ivk_MapView_UpdatePosition(
    0x0068D7B0, &Hook_MapView_UpdatePosition, IvkEnabled(Engine_G2A));

void __fastcall Hook_MapView_UpdatePosition(void* self, void* edx) {
    MapDiagLog("[hook] UpdatePosition ENTER self=%p", self);
    Ivk_MapView_UpdatePosition(self, edx);
    MapDiagLog("[hook] UpdatePosition RETURN self=%p", self);
}

// Callback declarations are needed here because Game_Entry logs their addresses.
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

// The application object is defined below; Game_Entry inspects its event table.
extern CApplication* lpApplication;

void Game_Entry() {
    // Truncate before writing any diagnostics for this session.
    MapDiagReset();

    THookInfo& hookInfo = const_cast<THookInfo&>(Ivk_MapView_UpdatePosition.GetHookInfo());
    bool32 isHooked = CCallBack::IsHooked(hookInfo);
    bool32 isExist = CCallBack::IsExist(hookInfo);

    MapDiagLog("[plugin] Game_Entry");
    MapDiagLog("[diag] UpdatePosition hookInfo original=%p pointer=%p detour=%p protected=%u",
        hookInfo.lpOriginal, hookInfo.lpPointer, hookInfo.lpDetour, hookInfo.ulProtected);
    MapDiagLog("[diag] UpdatePosition state isHooked=%d isExist=%d",
        (int)isHooked, (int)isExist);

    // Inspect the target entry after Union reports the hook as installed.
    unsigned char* originalBytes = (unsigned char*)hookInfo.lpOriginal;
    unsigned char* pointerBytes = (unsigned char*)hookInfo.lpPointer;
    MapDiagLog("[diag] target bytes=%02X %02X %02X %02X %02X %02X %02X %02X",
        originalBytes[0], originalBytes[1], originalBytes[2], originalBytes[3],
        originalBytes[4], originalBytes[5], originalBytes[6], originalBytes[7]);
    MapDiagLog("[diag] pointer bytes=%02X %02X %02X %02X %02X %02X %02X %02X",
        pointerBytes[0], pointerBytes[1], pointerBytes[2], pointerBytes[3],
        pointerBytes[4], pointerBytes[5], pointerBytes[6], pointerBytes[7]);

    // The 1.0k CreateRefApplication overload below receives 21 callbacks.
    // GAME_EVENTS_MAX belongs to the legacy 17-event enum, so it is NOT the
    // size of this application's callback table.
    const int appEventCount = 21;
    if (!lpApplication) {
        MapDiagLog("[diag] application pointer is NULL");
    }
    else {
        TAppFunc* events = lpApplication->lpEvents;
        MapDiagLog("[diag] application=%p eventTable=%p inspectedEventCount=%d",
            lpApplication, events, appEventCount);

        MapDiagLog("[diag] expected callbacks: entry=%p init=%p exit=%p preloop=%p loop=%p postloop=%p menuloop=%p",
            (void*)Game_Entry, (void*)Game_Init, (void*)Game_Exit,
            (void*)Game_PreLoop, (void*)Game_Loop, (void*)Game_PostLoop,
            (void*)Game_MenuLoop);
        MapDiagLog("[diag] expected callbacks: saveBegin=%p saveEnd=%p loadNewBegin=%p loadNewEnd=%p loadSaveBegin=%p loadSaveEnd=%p",
            (void*)Game_SaveBegin, (void*)Game_SaveEnd,
            (void*)Game_LoadBegin_NewGame, (void*)Game_LoadEnd_NewGame,
            (void*)Game_LoadBegin_SaveGame, (void*)Game_LoadEnd_SaveGame);
        MapDiagLog("[diag] expected callbacks: loadLevelBegin=%p loadLevelEnd=%p loadTriggerBegin=%p loadTriggerEnd=%p pause=%p unpause=%p defineExternals=%p applyOptions=%p",
            (void*)Game_LoadBegin_ChangeLevel, (void*)Game_LoadEnd_ChangeLevel,
            (void*)Game_LoadBegin_Trigger, (void*)Game_LoadEnd_Trigger,
            (void*)Game_Pause, (void*)Game_Unpause,
            (void*)Game_DefineExternals, (void*)Game_ApplyOptions);

        if (!events) {
            MapDiagLog("[diag] application event table is NULL");
        }
        else {
            static const char* eventNames[21] = {
                "GAME_ENTRY", "GAME_INIT", "GAME_EXIT",
                "GAME_PRE_LOOP", "GAME_LOOP", "GAME_POST_LOOP", "GAME_MENU_LOOP",
                "GAME_SAVE_BEGIN", "GAME_SAVE_END",
                "GAME_LOAD_BEGIN_NEWGAME", "GAME_LOAD_END_NEWGAME",
                "GAME_LOAD_BEGIN_SAVEGAME", "GAME_LOAD_END_SAVEGAME",
                "GAME_LOAD_BEGIN_CHANGELEVEL", "GAME_LOAD_END_CHANGELEVEL",
                "GAME_LOAD_BEGIN_TRIGGER", "GAME_LOAD_END_TRIGGER",
                "GAME_PAUSE", "GAME_UNPAUSE", "GAME_DEFINE_EXTERNALS",
                "GAME_APPLY_OPTIONS"
            };

            for (int i = 0; i < appEventCount; ++i)
                MapDiagLog("[diag] event[%02d] %s=%p",
                    i, eventNames[i], (void*)events[i]);
        }
    }
}
// Log the first call and then one sample every 300 calls. This keeps the
// log compact while revealing which Union application callbacks actually run.
void MapDiagPulse(const char* name, unsigned long& count) {
    ++count;
    if (count == 1 || count % 300 == 0)
        MapDiagLog("[callback] %s count=%lu", name, count);
}

void Game_Init() { MapDiagLog("[callback] Game_Init"); }
void Game_Exit() { MapDiagLog("[callback] Game_Exit"); }

void Game_PreLoop() {
    static unsigned long count = 0;
    MapDiagPulse("Game_PreLoop", count);
}
void Game_Loop() {
    static unsigned long count = 0;
    MapDiagPulse("Game_Loop", count);
}
void Game_PostLoop() {
    static unsigned long count = 0;
    MapDiagPulse("Game_PostLoop", count);
}
void Game_MenuLoop() {
    static unsigned long count = 0;
    MapDiagPulse("Game_MenuLoop", count);
}

void Game_SaveBegin() { MapDiagLog("[callback] Game_SaveBegin"); }
void Game_SaveEnd() { MapDiagLog("[callback] Game_SaveEnd"); }

void LoadBegin() { MapDiagLog("[callback] LoadBegin"); }
void LoadEnd() { MapDiagLog("[callback] LoadEnd"); }

void Game_LoadBegin_NewGame() { MapDiagLog("[callback] Game_LoadBegin_NewGame"); LoadBegin(); }
void Game_LoadEnd_NewGame() { MapDiagLog("[callback] Game_LoadEnd_NewGame"); LoadEnd(); }

void Game_LoadBegin_SaveGame() { MapDiagLog("[callback] Game_LoadBegin_SaveGame"); LoadBegin(); }
void Game_LoadEnd_SaveGame() { MapDiagLog("[callback] Game_LoadEnd_SaveGame"); LoadEnd(); }

void Game_LoadBegin_ChangeLevel() { MapDiagLog("[callback] Game_LoadBegin_ChangeLevel"); LoadBegin(); }
void Game_LoadEnd_ChangeLevel() { MapDiagLog("[callback] Game_LoadEnd_ChangeLevel"); LoadEnd(); }

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
