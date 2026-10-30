// Трассировщик порядка кадра (диагностика).
// F9 в игре: записать ОДИН кадр в файл trace_frame.txt (папка, откуда запущен Gothic2.exe, обычно System).
#include "resource.h"
#include <vector>
#include <string>
#include <fstream>

namespace GOTHIC_ENGINE {

  // ---------- Лог одного кадра ----------
  std::vector<std::string> g_trace;
  bool g_log = false;

  inline void Trace(const char* text) {
    if (g_log) g_trace.push_back(text);
  }

  // ---------- Хуки (thiscall перехватываем как __fastcall: this, заглушка edx, аргументы) ----------

  // oCGame::Render()
  void __fastcall Hook_Game_Render(oCGame* self, void* edx);
  CInvoke<void(__fastcall*)(oCGame*, void*)> Ivk_Game_Render(0x006C86A0, &Hook_Game_Render);
  void __fastcall Hook_Game_Render(oCGame* self, void* edx) {
    Trace("oCGame::Render >");
    Ivk_Game_Render(self, edx);
    Trace("oCGame::Render <");
  }

  // oCGame::RenderBlit()
  void __fastcall Hook_Game_RenderBlit(oCGame* self, void* edx);
  CInvoke<void(__fastcall*)(oCGame*, void*)> Ivk_Game_RenderBlit(0x006CA910, &Hook_Game_RenderBlit);
  void __fastcall Hook_Game_RenderBlit(oCGame* self, void* edx) {
    Trace("oCGame::RenderBlit >");
    Ivk_Game_RenderBlit(self, edx);
    Trace("oCGame::RenderBlit <");
  }

  // zCWorld::Render(zCCamera&)
  void __fastcall Hook_World_Render(zCWorld* self, void* edx, zCCamera& cam);
  CInvoke<void(__fastcall*)(zCWorld*, void*, zCCamera&)> Ivk_World_Render(0x00621700, &Hook_World_Render);
  void __fastcall Hook_World_Render(zCWorld* self, void* edx, zCCamera& cam) {
    Trace("    zCWorld::Render >");
    Ivk_World_Render(self, edx, cam);
    Trace("    zCWorld::Render <");
  }

  // zCRnd_D3D::BeginFrame()
  void __fastcall Hook_Rnd_BeginFrame(zCRnd_D3D* self, void* edx);
  CInvoke<void(__fastcall*)(zCRnd_D3D*, void*)> Ivk_Rnd_BeginFrame(0x0064DD20, &Hook_Rnd_BeginFrame);
  void __fastcall Hook_Rnd_BeginFrame(zCRnd_D3D* self, void* edx) {
    Trace("        zCRnd_D3D::BeginFrame");
    Ivk_Rnd_BeginFrame(self, edx);
  }

  // zCRnd_D3D::EndFrame()
  void __fastcall Hook_Rnd_EndFrame(zCRnd_D3D* self, void* edx);
  CInvoke<void(__fastcall*)(zCRnd_D3D*, void*)> Ivk_Rnd_EndFrame(0x0064DF20, &Hook_Rnd_EndFrame);
  void __fastcall Hook_Rnd_EndFrame(zCRnd_D3D* self, void* edx) {
    Trace("        zCRnd_D3D::EndFrame");
    Ivk_Rnd_EndFrame(self, edx);
  }

  // zCRnd_D3D::Vid_Blit(int, tagRECT*, tagRECT*)
  void __fastcall Hook_Rnd_VidBlit(zCRnd_D3D* self, void* edx, int a, tagRECT* b, tagRECT* c);
  CInvoke<void(__fastcall*)(zCRnd_D3D*, void*, int, tagRECT*, tagRECT*)> Ivk_Rnd_VidBlit(0x00657670, &Hook_Rnd_VidBlit);
  void __fastcall Hook_Rnd_VidBlit(zCRnd_D3D* self, void* edx, int a, tagRECT* b, tagRECT* c) {
    Trace("        zCRnd_D3D::Vid_Blit");
    Ivk_Rnd_VidBlit(self, edx, a, b, c);
  }

  // zCView::Render()
  void __fastcall Hook_View_Render(zCView* self, void* edx);
  CInvoke<void(__fastcall*)(zCView*, void*)> Ivk_View_Render(0x007AC210, &Hook_View_Render);
  void __fastcall Hook_View_Render(zCView* self, void* edx) {
    Trace(self == screen ? "    zCView::Render (screen) >" : "    zCView::Render (other) >");
    Ivk_View_Render(self, edx);
    Trace(self == screen ? "    zCView::Render (screen) <" : "    zCView::Render (other) <");
  }

  // ---------- События Union ----------

  void Game_Entry() {
  }

  void Game_Init() {
  }

  void Game_Exit() {
  }

  void Game_PreLoop() {
  }

  // Вызывается каждый кадр: границы кадра для лога
  void Game_Loop() {
    if (g_log) {
      Trace("---- Game_Loop (конец записанного кадра) ----");
      std::ofstream file("trace_frame.txt");
      for (size_t i = 0; i < g_trace.size(); i++)
        file << g_trace[i] << "\n";
      g_trace.clear();
      g_log = false;
    }

    if (GetAsyncKeyState(VK_F9) & 1) {   // F9: записать следующий кадр
      g_trace.clear();
      g_log = true;
      Trace("---- Game_Loop (начало записанного кадра) ----");
    }
  }

  void Game_PostLoop() {
  }

  void Game_MenuLoop() {
  }

  // Information about current saving or loading world
  TSaveLoadGameInfo& SaveLoadGameInfo = UnionCore::SaveLoadGameInfo;

  void Game_SaveBegin() {
  }

  void Game_SaveEnd() {
  }

  void LoadBegin() {
  }

  void LoadEnd() {
  }

  void Game_LoadBegin_NewGame() {
    LoadBegin();
  }

  void Game_LoadEnd_NewGame() {
    LoadEnd();
  }

  void Game_LoadBegin_SaveGame() {
    LoadBegin();
  }

  void Game_LoadEnd_SaveGame() {
    LoadEnd();
  }

  void Game_LoadBegin_ChangeLevel() {
    LoadBegin();
  }

  void Game_LoadEnd_ChangeLevel() {
    LoadEnd();
  }

  void Game_LoadBegin_Trigger() {
  }

  void Game_LoadEnd_Trigger() {
  }

  void Game_Pause() {
  }

  void Game_Unpause() {
  }

  void Game_DefineExternals() {
  }

  void Game_ApplyOptions() {
  }

#define AppDefault True
  CApplication* lpApplication = !CHECK_THIS_ENGINE ? Null : CApplication::CreateRefApplication(
    Enabled( AppDefault ) Game_Entry,
    Enabled( AppDefault ) Game_Init,
    Enabled( AppDefault ) Game_Exit,
    Enabled( AppDefault ) Game_PreLoop,
    Enabled( AppDefault ) Game_Loop,
    Enabled( AppDefault ) Game_PostLoop,
    Enabled( AppDefault ) Game_MenuLoop,
    Enabled( AppDefault ) Game_SaveBegin,
    Enabled( AppDefault ) Game_SaveEnd,
    Enabled( AppDefault ) Game_LoadBegin_NewGame,
    Enabled( AppDefault ) Game_LoadEnd_NewGame,
    Enabled( AppDefault ) Game_LoadBegin_SaveGame,
    Enabled( AppDefault ) Game_LoadEnd_SaveGame,
    Enabled( AppDefault ) Game_LoadBegin_ChangeLevel,
    Enabled( AppDefault ) Game_LoadEnd_ChangeLevel,
    Enabled( AppDefault ) Game_LoadBegin_Trigger,
    Enabled( AppDefault ) Game_LoadEnd_Trigger,
    Enabled( AppDefault ) Game_Pause,
    Enabled( AppDefault ) Game_Unpause,
    Enabled( AppDefault ) Game_DefineExternals,
    Enabled( AppDefault ) Game_ApplyOptions
  );
}
