#include <stdio.h>
#include <stdarg.h>

// zMapUnlocker — пассивная диагностика UpdatePosition.
// DocumentManager::Show отключён: его перехват сопровождался вылетом при открытии карты.

namespace GOTHIC_ENGINE {

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

TSaveLoadGameInfo& SaveLoadGameInfo = UnionCore::SaveLoadGameInfo;

void Game_Entry() {
    THookInfo& hookInfo = const_cast<THookInfo&>(Ivk_MapView_UpdatePosition.GetHookInfo());
    bool32 isHooked = CCallBack::IsHooked(hookInfo);
    bool32 isExist = CCallBack::IsExist(hookInfo);

    MapDiagLog("[plugin] Game_Entry");
    MapDiagLog("[diag] UpdatePosition hookInfo original=%p pointer=%p detour=%p protected=%u",
        hookInfo.lpOriginal, hookInfo.lpPointer, hookInfo.lpDetour, hookInfo.ulProtected);
    MapDiagLog("[diag] UpdatePosition state isHooked=%d isExist=%d",
        (int)isHooked, (int)isExist);
}
void Game_Init() {}
void Game_Exit() {}

void Game_PreLoop() {}
void Game_Loop() {}
void Game_PostLoop() {}
void Game_MenuLoop() {}

void Game_SaveBegin() {}
void Game_SaveEnd() {}

void LoadBegin() {}
void LoadEnd() {}

void Game_LoadBegin_NewGame() { LoadBegin(); }
void Game_LoadEnd_NewGame() { LoadEnd(); }

void Game_LoadBegin_SaveGame() { LoadBegin(); }
void Game_LoadEnd_SaveGame() { LoadEnd(); }

void Game_LoadBegin_ChangeLevel() { LoadBegin(); }
void Game_LoadEnd_ChangeLevel() { LoadEnd(); }

void Game_LoadBegin_Trigger() {}
void Game_LoadEnd_Trigger() {}

void Game_Pause() {}
void Game_Unpause() {}

void Game_DefineExternals() {}
void Game_ApplyOptions() {}

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
