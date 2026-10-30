// Трассировщик кадра, версия 3.
// INSERT в игре: записать ОДИН полный кадр (от oCGame::Render до конца oCGame::RenderBlit)
// в zFrameTrace_frame.txt. Повторяющиеся подряд события склеиваются: "строка  x1200".
// Файлы пишутся рядом с Gothic2.exe (System); если туда нельзя писать - в %TEMP%.
#include "resource.h"
#include <vector>
#include <string>
#include <fstream>

namespace GOTHIC_ENGINE {

  // ---------- Запись файлов ----------
  std::string ExeDirPath(const char* name) {
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, buf, MAX_PATH);
    std::string p = buf;
    size_t pos = p.find_last_of("\\/");
    return (pos == std::string::npos ? std::string() : p.substr(0, pos + 1)) + name;
  }

  void WriteText(const char* name, const std::string& text, bool append) {
    std::ios_base::openmode mode = std::ios::out | (append ? std::ios::app : std::ios::trunc);
    std::ofstream f(ExeDirPath(name).c_str(), mode);
    if (!f.is_open()) {                       // запасной вариант: папка %TEMP%
      char tmp[MAX_PATH] = {0};
      GetTempPathA(MAX_PATH, tmp);
      f.open((std::string(tmp) + name).c_str(), mode);
    }
    if (f.is_open()) f << text;
  }

  // ---------- Лог одного кадра ----------
  struct TraceItem { std::string text; int count; };
  std::vector<TraceItem> g_trace;
  bool g_armed = false;       // нажат INSERT, ждём начала следующего кадра
  bool g_recording = false;   // идёт запись кадра
  bool g_savedMsg = false;    // показать "TRACE SAVED"

  // счётчики вызовов хуков (за всю игру)
  int g_cnt_game = 0, g_cnt_world = 0, g_cnt_view = 0, g_cnt_begin = 0, g_cnt_end = 0, g_cnt_blit = 0;
  int g_cnt_poly = 0, g_cnt_vb = 0, g_cnt_viewblit = 0, g_cnt_trafo = 0, g_cnt_cam = 0;

  inline void Trace(const char* text) {
    if (!g_recording) return;
    if (!g_trace.empty() && g_trace.back().text == text) { g_trace.back().count++; return; }
    TraceItem it; it.text = text; it.count = 1;
    g_trace.push_back(it);
  }

  void FlushTrace() {
    std::string all;
    for (size_t i = 0; i < g_trace.size(); i++) {
      all += g_trace[i].text;
      if (g_trace[i].count > 1) { all += "  x"; all += std::to_string(g_trace[i].count); }
      all += "\n";
    }
    WriteText("zFrameTrace_frame.txt", all, false);
    g_trace.clear();
    g_recording = false;
    g_savedMsg = true;
  }

  // главный игровой мир (а не мини-миры предметов в инвентаре)
  inline bool IsMainWorld(zCWorld* w) {
    return ogame && w == (zCWorld*)ogame->GetGameWorld();
  }

  // ---------- Хуки (thiscall перехватываем как __fastcall: this, заглушка edx, аргументы) ----------

  // oCGame::Render(): здесь начинается запись кадра
  void __fastcall Hook_Game_Render(oCGame* self, void* edx);
  CInvoke<void(__fastcall*)(oCGame*, void*)> Ivk_Game_Render(0x006C86A0, &Hook_Game_Render);
  void __fastcall Hook_Game_Render(oCGame* self, void* edx) {
    g_cnt_game++;
    if (g_armed) { g_armed = false; g_trace.clear(); g_recording = true; }
    Trace("oCGame::Render >");
    Ivk_Game_Render(self, edx);
    Trace("oCGame::Render <");
  }

  // oCGame::RenderBlit(): здесь запись кадра заканчивается
  void __fastcall Hook_Game_RenderBlit(oCGame* self, void* edx);
  CInvoke<void(__fastcall*)(oCGame*, void*)> Ivk_Game_RenderBlit(0x006CA910, &Hook_Game_RenderBlit);
  void __fastcall Hook_Game_RenderBlit(oCGame* self, void* edx) {
    Trace("oCGame::RenderBlit >");
    Ivk_Game_RenderBlit(self, edx);
    Trace("oCGame::RenderBlit <");
    if (g_recording) FlushTrace();
  }

  // zCWorld::Render(zCCamera&)
  void __fastcall Hook_World_Render(zCWorld* self, void* edx, zCCamera& cam);
  CInvoke<void(__fastcall*)(zCWorld*, void*, zCCamera&)> Ivk_World_Render(0x00621700, &Hook_World_Render);
  void __fastcall Hook_World_Render(zCWorld* self, void* edx, zCCamera& cam) {
    g_cnt_world++;
    bool main = IsMainWorld(self);
    Trace(main ? "  zCWorld::Render (MAIN world) >" : "  zCWorld::Render (other world) >");
    Ivk_World_Render(self, edx, cam);
    Trace(main ? "  zCWorld::Render (MAIN world) <" : "  zCWorld::Render (other world) <");
  }

  // zCCamera::Activate()
  void __fastcall Hook_Cam_Activate(zCCamera* self, void* edx);
  CInvoke<void(__fastcall*)(zCCamera*, void*)> Ivk_Cam_Activate(0x0054A700, &Hook_Cam_Activate);
  void __fastcall Hook_Cam_Activate(zCCamera* self, void* edx) {
    g_cnt_cam++;
    Trace("    zCCamera::Activate");
    Ivk_Cam_Activate(self, edx);
  }

  // zCRnd_D3D::BeginFrame()
  void __fastcall Hook_Rnd_BeginFrame(zCRnd_D3D* self, void* edx);
  CInvoke<void(__fastcall*)(zCRnd_D3D*, void*)> Ivk_Rnd_BeginFrame(0x0064DD20, &Hook_Rnd_BeginFrame);
  void __fastcall Hook_Rnd_BeginFrame(zCRnd_D3D* self, void* edx) {
    g_cnt_begin++;
    Trace("    zCRnd_D3D::BeginFrame");
    Ivk_Rnd_BeginFrame(self, edx);
  }

  // zCRnd_D3D::EndFrame()
  void __fastcall Hook_Rnd_EndFrame(zCRnd_D3D* self, void* edx);
  CInvoke<void(__fastcall*)(zCRnd_D3D*, void*)> Ivk_Rnd_EndFrame(0x0064DF20, &Hook_Rnd_EndFrame);
  void __fastcall Hook_Rnd_EndFrame(zCRnd_D3D* self, void* edx) {
    g_cnt_end++;
    Trace("    zCRnd_D3D::EndFrame");
    Ivk_Rnd_EndFrame(self, edx);
  }

  // zCRnd_D3D::Vid_Blit(int, tagRECT*, tagRECT*)
  void __fastcall Hook_Rnd_VidBlit(zCRnd_D3D* self, void* edx, int a, tagRECT* b, tagRECT* c);
  CInvoke<void(__fastcall*)(zCRnd_D3D*, void*, int, tagRECT*, tagRECT*)> Ivk_Rnd_VidBlit(0x00657670, &Hook_Rnd_VidBlit);
  void __fastcall Hook_Rnd_VidBlit(zCRnd_D3D* self, void* edx, int a, tagRECT* b, tagRECT* c) {
    g_cnt_blit++;
    Trace("    zCRnd_D3D::Vid_Blit");
    Ivk_Rnd_VidBlit(self, edx, a, b, c);
  }

  // zCRnd_D3D::SetTransform(zTRnd_TrafoType, zMAT4 const&)
  int __fastcall Hook_Rnd_SetTransform(zCRnd_D3D* self, void* edx, zTRnd_TrafoType type, zMAT4 const& m);
  CInvoke<int(__fastcall*)(zCRnd_D3D*, void*, zTRnd_TrafoType, zMAT4 const&)> Ivk_Rnd_SetTransform(0x0064ED50, &Hook_Rnd_SetTransform);
  int __fastcall Hook_Rnd_SetTransform(zCRnd_D3D* self, void* edx, zTRnd_TrafoType type, zMAT4 const& m) {
    g_cnt_trafo++;
    if (type == zRND_TRAFO_VIEW)            Trace("      SetTransform VIEW");
    else if (type == zRND_TRAFO_PROJECTION) Trace("      SetTransform PROJECTION");
    else                                    Trace("      SetTransform (texture)");
    return Ivk_Rnd_SetTransform(self, edx, type, m);
  }

  // zCRnd_D3D::DrawPoly(zCPolygon*)
  void __fastcall Hook_Rnd_DrawPoly(zCRnd_D3D* self, void* edx, zCPolygon* poly);
  CInvoke<void(__fastcall*)(zCRnd_D3D*, void*, zCPolygon*)> Ivk_Rnd_DrawPoly(0x0064B260, &Hook_Rnd_DrawPoly);
  void __fastcall Hook_Rnd_DrawPoly(zCRnd_D3D* self, void* edx, zCPolygon* poly) {
    g_cnt_poly++;
    Trace("      DrawPoly");
    Ivk_Rnd_DrawPoly(self, edx, poly);
  }

  // zCRnd_D3D::DrawVertexBuffer(zCVertexBuffer*, int, int, unsigned short*, unsigned long)
  int __fastcall Hook_Rnd_DrawVB(zCRnd_D3D* self, void* edx, zCVertexBuffer* vb, int a, int b, unsigned short* idx, unsigned long c);
  CInvoke<int(__fastcall*)(zCRnd_D3D*, void*, zCVertexBuffer*, int, int, unsigned short*, unsigned long)> Ivk_Rnd_DrawVB(0x006519F0, &Hook_Rnd_DrawVB);
  int __fastcall Hook_Rnd_DrawVB(zCRnd_D3D* self, void* edx, zCVertexBuffer* vb, int a, int b, unsigned short* idx, unsigned long c) {
    g_cnt_vb++;
    Trace("      DrawVertexBuffer");
    return Ivk_Rnd_DrawVB(self, edx, vb, a, b, idx, c);
  }

  // zCView::Render()
  void __fastcall Hook_View_Render(zCView* self, void* edx);
  CInvoke<void(__fastcall*)(zCView*, void*)> Ivk_View_Render(0x007AC210, &Hook_View_Render);
  void __fastcall Hook_View_Render(zCView* self, void* edx) {
    g_cnt_view++;
    Trace(self == screen ? "    zCView::Render (screen) >" : "    zCView::Render (other) >");
    Ivk_View_Render(self, edx);
    Trace(self == screen ? "    zCView::Render (screen) <" : "    zCView::Render (other) <");
  }

  // zCView::Blit()
  void __fastcall Hook_View_Blit(zCView* self, void* edx);
  CInvoke<void(__fastcall*)(zCView*, void*)> Ivk_View_Blit(0x007A63C0, &Hook_View_Blit);
  void __fastcall Hook_View_Blit(zCView* self, void* edx) {
    g_cnt_viewblit++;
    Trace(self == screen ? "    zCView::Blit (screen) >" : "    zCView::Blit (other) >");
    Ivk_View_Blit(self, edx);
    Trace(self == screen ? "    zCView::Blit (screen) <" : "    zCView::Blit (other) <");
  }

  // ---------- События Union ----------

  void Game_Entry() {
    WriteText("zFrameTrace_log.txt", "Game_Entry: plugin loaded (v3)\n", false);
  }

  void Game_Init() {
    WriteText("zFrameTrace_log.txt", "Game_Init\n", true);
  }

  void Game_Exit() {
  }

  void Game_PreLoop() {
  }

  void Game_Loop() {
    static int frame = 0;
    static bool wasDown = false;
    frame++;

    if (frame == 300) {                       // один раз: сработали ли хуки
      char line[400];
      sprintf_s(line, sizeof(line),
        "frame 300: game=%d world=%d view=%d viewblit=%d begin=%d end=%d blit=%d poly=%d vb=%d trafo=%d cam=%d\n",
        g_cnt_game, g_cnt_world, g_cnt_view, g_cnt_viewblit, g_cnt_begin, g_cnt_end, g_cnt_blit,
        g_cnt_poly, g_cnt_vb, g_cnt_trafo, g_cnt_cam);
      WriteText("zFrameTrace_log.txt", line, true);
    }

    if (g_savedMsg) {
      g_savedMsg = false;
      if (screen) screen->PrintCXY(zSTRING("TRACE SAVED"));
    }

    bool down = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;   // клавиша INSERT
    if (down && !wasDown && !g_recording) {
      g_armed = true;
      if (screen) screen->PrintCXY(zSTRING("TRACE: armed"));
    }
    wasDown = down;
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
