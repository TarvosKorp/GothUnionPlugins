// zMirrorTest: прототип горизонтального отражения ТОЛЬКО основного 3D-мира.
// HOME в игре: включить / выключить отражение.
// Принцип: сразу после zCWorld::Render (MAIN world) закрываем сцену (EndFrame), зеркально
// копируем задний буфер через DirectDraw (Blt с флагом MIRRORLEFTRIGHT), открываем сцену
// (BeginFrame) заново. Интерфейс рисуется позже, поэтому остаётся неотражённым.
// Результаты и коды ошибок пишутся в zMirror_log.txt (рядом с Gothic2.exe или в %TEMP%).
#include "resource.h"
#include <string>
#include <fstream>

namespace GOTHIC_ENGINE {

  // ---------- Запись логов ----------
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
    if (!f.is_open()) {
      char tmp[MAX_PATH] = {0};
      GetTempPathA(MAX_PATH, tmp);
      f.open((std::string(tmp) + name).c_str(), mode);
    }
    if (f.is_open()) f << text;
  }

  int g_logLines = 0;
  void Log(const std::string& s) {            // не больше 60 строк за сессию
    if (g_logLines++ < 60) WriteText("zMirror_log.txt", s + "\n", true);
  }
  std::string Hex(unsigned long v) {
    char b[32]; sprintf_s(b, sizeof(b), "0x%08lX", v); return b;
  }

  // ---------- Состояние ----------
  bool g_mirror = false;                      // отражение включено?
  IDirectDrawSurface7* g_tmp = NULL;          // временная поверхность
  DWORD g_tmpW = 0, g_tmpH = 0;
  int g_failStreak = 0;                       // подряд неудачных попыток
  bool g_disabledByError = false;

  void ReleaseTmp() {
    if (g_tmp) { g_tmp->Release(); g_tmp = NULL; }
    g_tmpW = g_tmpH = 0;
  }

  // Создать/проверить временную поверхность под размер заднего буфера
  bool EnsureTmp(zCRnd_D3D* rnd) {
    IDirectDrawSurface7* back = rnd->xd3d_pbackBuffer;
    DDSURFACEDESC2 bd; ZeroMemory(&bd, sizeof(bd)); bd.dwSize = sizeof(bd);
    HRESULT hr = back->GetSurfaceDesc(&bd);
    if (FAILED(hr)) { Log("GetSurfaceDesc failed " + Hex(hr)); return false; }

    if (g_tmp && g_tmpW == bd.dwWidth && g_tmpH == bd.dwHeight) return true;
    ReleaseTmp();

    DDSURFACEDESC2 d; ZeroMemory(&d, sizeof(d)); d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    d.dwWidth = bd.dwWidth; d.dwHeight = bd.dwHeight;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY;
    hr = rnd->xd3d_pdd7->CreateSurface(&d, &g_tmp, NULL);
    if (FAILED(hr)) {
      Log("CreateSurface(video) failed " + Hex(hr) + ", trying system memory");
      d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
      hr = rnd->xd3d_pdd7->CreateSurface(&d, &g_tmp, NULL);
      if (FAILED(hr)) { Log("CreateSurface(system) failed " + Hex(hr)); g_tmp = NULL; return false; }
    }
    g_tmpW = bd.dwWidth; g_tmpH = bd.dwHeight;
    Log("tmp surface created " + std::to_string(g_tmpW) + "x" + std::to_string(g_tmpH));
    return true;
  }

  // Зеркально отразить задний буфер (вызывается сразу после отрисовки основного мира)
  void MirrorBackBuffer() {
    zCRnd_D3D* rnd = (zCRnd_D3D*)zrenderer;
    if (!rnd || !rnd->xd3d_pbackBuffer || !zCRnd_D3D::xd3d_pdd7) return;

    rnd->EndFrame();                          // сбросить очередь и закрыть сцену (EndScene)

    bool ok = false;
    if (EnsureTmp(rnd)) {
      DDBLTFX fx; ZeroMemory(&fx, sizeof(fx)); fx.dwSize = sizeof(fx);
      fx.dwDDFX = DDBLTFX_MIRRORLEFTRIGHT;
      HRESULT h1 = g_tmp->Blt(NULL, rnd->xd3d_pbackBuffer, NULL, DDBLT_WAIT | DDBLT_DDFX, &fx);
      HRESULT h2 = FAILED(h1) ? h1 : rnd->xd3d_pbackBuffer->Blt(NULL, g_tmp, NULL, DDBLT_WAIT, NULL);
      ok = SUCCEEDED(h1) && SUCCEEDED(h2);
      static int reports = 0;
      if (!ok || reports < 3) { reports++; Log("Blt mirror=" + Hex(h1) + " Blt back=" + Hex(h2)); }
    }

    rnd->BeginFrame();                        // открыть сцену снова для интерфейса

    if (ok) g_failStreak = 0;
    else if (++g_failStreak >= 30) { g_disabledByError = true; g_mirror = false; Log("mirror disabled: 30 failures in a row"); }
  }

  // главный игровой мир (а не мини-миры предметов в инвентаре)
  inline bool IsMainWorld(zCWorld* w) {
    return ogame && w == (zCWorld*)ogame->GetGameWorld();
  }

  // ---------- Хук zCWorld::Render ----------
  void __fastcall Hook_World_Render(zCWorld* self, void* edx, zCCamera& cam);
  CInvoke<void(__fastcall*)(zCWorld*, void*, zCCamera&)> Ivk_World_Render(0x00621700, &Hook_World_Render);
  void __fastcall Hook_World_Render(zCWorld* self, void* edx, zCCamera& cam) {
    Ivk_World_Render(self, edx, cam);
    if (g_mirror && IsMainWorld(self))
      MirrorBackBuffer();
  }

  // ---------- События Union ----------

  void Game_Entry() {
    WriteText("zMirror_log.txt", "Game_Entry: zMirrorTest loaded\n", false);
  }

  void Game_Init() {
  }

  void Game_Exit() {
    ReleaseTmp();
  }

  void Game_PreLoop() {
  }

  void Game_Loop() {
    static bool wasDown = false;
    bool down = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;      // клавиша HOME
    if (down && !wasDown) {
      if (g_disabledByError) { g_disabledByError = false; g_failStreak = 0; }
      g_mirror = !g_mirror;
      Log(g_mirror ? "mirror ON" : "mirror OFF");
      if (screen) screen->PrintCXY(zSTRING(g_mirror ? "MIRROR: ON" : "MIRROR: OFF"));
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
