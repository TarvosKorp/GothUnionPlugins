// zMirrorTest v5: зеркальное отражение основного 3D-мира (интерфейс остаётся обычным).
// HOME в игре: включить / выключить отражение (вместе с ним переключается инверсия мыши по X).
//
// Принцип: сразу после zCWorld::Render (MAIN world) закрываем сцену (EndFrame), переворачиваем
// строки РЕАЛЬНОГО render target устройства (GetRenderTarget) на CPU, открываем сцену (BeginFrame).
// ВАЖНО: поле xd3d_pbackBuffer из заголовков в этой сборке на деле указывает на буфер глубины,
// поэтому используем только GetRenderTarget.
#include "resource.h"
#include <string>
#include <fstream>
#include <algorithm>

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
  void Log(const std::string& s) {            // не больше 100 строк за сессию
    if (g_logLines++ < 100) WriteText("zMirror_log.txt", s + "\n", true);
  }
  std::string Hex(unsigned long v) {
    char b[32]; sprintf_s(b, sizeof(b), "0x%08lX", v); return b;
  }

  // ---------- Состояние ----------
  bool g_mirror = false;                      // отражение включено?
  int g_failStreak = 0;
  double g_msSum = 0; int g_msCount = 0; int g_msReports = 0;

  // Реальный render target устройства (вызывающий делает Release())
  IDirectDrawSurface7* AcquireTarget() {
    IDirectDrawSurface7* rt = NULL;
    IDirect3DDevice7* dev = zCRnd_D3D::xd3d_pd3dDevice7;
    if (dev && SUCCEEDED(dev->GetRenderTarget(&rt)) && rt) return rt;
    return NULL;
  }

  // Перевернуть строки по горизонтали
  bool MirrorByLock(IDirectDrawSurface7* s) {
    DDSURFACEDESC2 d; ZeroMemory(&d, sizeof(d)); d.dwSize = sizeof(d);
    HRESULT hr = s->Lock(NULL, &d, DDLOCK_WAIT | DDLOCK_SURFACEMEMORYPTR, NULL);
    if (FAILED(hr)) { Log("Mirror: Lock failed " + Hex(hr)); return false; }
    DWORD bpp = d.ddpfPixelFormat.dwRGBBitCount;
    bool ok = true;
    BYTE* base = (BYTE*)d.lpSurface;
    if (!base) ok = false;
    else if (bpp == 32) {
      for (DWORD y = 0; y < d.dwHeight; y++) { DWORD* row = (DWORD*)(base + (size_t)y * d.lPitch); std::reverse(row, row + d.dwWidth); }
    }
    else if (bpp == 16) {
      for (DWORD y = 0; y < d.dwHeight; y++) { WORD* row = (WORD*)(base + (size_t)y * d.lPitch); std::reverse(row, row + d.dwWidth); }
    }
    else ok = false;
    s->Unlock(NULL);
    return ok;
  }

  void MirrorWorld() {
    zCRnd_D3D* rnd = (zCRnd_D3D*)zrenderer;
    if (!rnd) return;
    IDirectDrawSurface7* rt = AcquireTarget();
    if (!rt) return;

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);

    rnd->EndFrame();                          // сбросить очередь и закрыть сцену
    bool ok = MirrorByLock(rt);
    rnd->BeginFrame();                        // открыть сцену заново для интерфейса
    rt->Release();

    QueryPerformanceCounter(&t1);
    g_msSum += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    if (++g_msCount == 300) {
      if (g_msReports++ < 3) { char b[96]; sprintf_s(b, sizeof(b), "avg mirror time over 300 frames: %.2f ms", g_msSum / g_msCount); Log(b); }
      g_msSum = 0; g_msCount = 0;
    }
    if (ok) g_failStreak = 0;
    else if (++g_failStreak >= 30) { g_mirror = false; Log("mirror auto-disabled: 30 failures in a row"); }
  }

  // главный игровой мир (а не мини-миры предметов в инвентаре)
  inline bool IsMainWorld(zCWorld* w) {
    return ogame && w == (zCWorld*)ogame->GetGameWorld();
  }

  // ---------- Инверсия мыши по X вместе с зеркалом ----------
  bool g_mouseFlipApplied = false;
  int g_baseFlipX = 0;

  void SyncMouseFlip() {
    if (!zinput) return;
    int fx = 0, fy = 0;
    zinput->GetMouseFlipXY(fx, fy);
    if (g_mirror && !g_mouseFlipApplied) {
      g_baseFlipX = fx;                       // запоминаем исходную настройку, чтобы вернуть
      zinput->SetMouseFlipXY(fx ? 0 : 1, fy);
      g_mouseFlipApplied = true;
    }
    else if (!g_mirror && g_mouseFlipApplied) {
      zinput->SetMouseFlipXY(g_baseFlipX, fy);
      g_mouseFlipApplied = false;
    }
  }

  // ---------- Хук zCWorld::Render ----------
  void __fastcall Hook_World_Render(zCWorld* self, void* edx, zCCamera& cam);
  CInvoke<void(__fastcall*)(zCWorld*, void*, zCCamera&)> Ivk_World_Render(0x00621700, &Hook_World_Render);
  void __fastcall Hook_World_Render(zCWorld* self, void* edx, zCCamera& cam) {
    Ivk_World_Render(self, edx, cam);
    if (g_mirror && IsMainWorld(self))
      MirrorWorld();
  }

  // ---------- События Union ----------

  void Game_Entry() {
    WriteText("zMirror_log.txt", "Game_Entry: zMirrorTest v5 loaded\n", false);
  }

  void Game_Init() {
  }

  void Game_Exit() {
  }

  void Game_PreLoop() {
  }

  void Game_Loop() {
    static bool wasDown = false;
    bool down = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;      // клавиша HOME
    if (down && !wasDown) {
      g_mirror = !g_mirror;
      g_failStreak = 0;
      SyncMouseFlip();
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
