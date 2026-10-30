// zMirrorTest v3 (диагностика): HOME переключает режимы по кругу:
//   0 = выкл, 1 = зеркало после мира, 2 = КРАСНАЯ полоса слева после мира,
//   3 = СИНЯЯ полоса справа в самом конце кадра (перед Vid_Blit).
// Полосы показывают, попадают ли наши правки заднего буфера на экран.
// Принцип: сразу после zCWorld::Render (MAIN world) закрываем сцену (EndFrame), переворачиваем
// строки заднего буфера на CPU (Lock / Unlock), открываем сцену (BeginFrame) заново.
// Интерфейс рисуется позже, поэтому остаётся неотражённым.
// (Зеркальный Blt DirectDraw на этой системе не реализован: ошибка 0x80004001 = E_NOTIMPL.)
// Результаты, коды ошибок и замеры времени пишутся в zMirror_log.txt.
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
  void Log(const std::string& s) {            // не больше 60 строк за сессию
    if (g_logLines++ < 60) WriteText("zMirror_log.txt", s + "\n", true);
  }
  std::string Hex(unsigned long v) {
    char b[32]; sprintf_s(b, sizeof(b), "0x%08lX", v); return b;
  }

  // ---------- Состояние ----------
  int g_mode = 0;                             // текущий режим (0..3)
  int g_failStreak = 0;                       // подряд неудачных попыток
  bool g_disabledByError = false;
  double g_msSum = 0; int g_msCount = 0;      // замер времени отражения

  // Перевернуть строки заднего буфера на CPU
  bool MirrorByLock(IDirectDrawSurface7* s) {
    DDSURFACEDESC2 d; ZeroMemory(&d, sizeof(d)); d.dwSize = sizeof(d);
    HRESULT hr = s->Lock(NULL, &d, DDLOCK_WAIT | DDLOCK_SURFACEMEMORYPTR, NULL);
    if (FAILED(hr)) { Log("Lock failed " + Hex(hr)); return false; }

    DWORD bpp = d.ddpfPixelFormat.dwRGBBitCount;
    bool ok = true;
    BYTE* base = (BYTE*)d.lpSurface;
    if (!base) ok = false;
    else if (bpp == 32) {
      for (DWORD y = 0; y < d.dwHeight; y++) {
        DWORD* row = (DWORD*)(base + (size_t)y * d.lPitch);
        std::reverse(row, row + d.dwWidth);
      }
    }
    else if (bpp == 16) {
      for (DWORD y = 0; y < d.dwHeight; y++) {
        WORD* row = (WORD*)(base + (size_t)y * d.lPitch);
        std::reverse(row, row + d.dwWidth);
      }
    }
    else ok = false;

    s->Unlock(NULL);
    static bool reported = false;
    if (!ok || !reported) {
      reported = true;
      Log("Lock ok, " + std::to_string(d.dwWidth) + "x" + std::to_string(d.dwHeight) + ", bpp=" + std::to_string(bpp) +
          ", pitch=" + std::to_string(d.lPitch) + (ok ? "" : "  UNSUPPORTED FORMAT"));
    }
    return ok;
  }

  // Закрасить вертикальную полосу (1/8 ширины) слева или справа заданным цветом (маска канала)
  bool PaintStripe(IDirectDrawSurface7* s, bool left, bool red) {
    DDSURFACEDESC2 d; ZeroMemory(&d, sizeof(d)); d.dwSize = sizeof(d);
    HRESULT hr = s->Lock(NULL, &d, DDLOCK_WAIT | DDLOCK_SURFACEMEMORYPTR, NULL);
    if (FAILED(hr)) { Log("Stripe: Lock failed " + Hex(hr)); return false; }
    DWORD bpp = d.ddpfPixelFormat.dwRGBBitCount;
    DWORD color = red ? d.ddpfPixelFormat.dwRBitMask : d.ddpfPixelFormat.dwBBitMask;
    DWORD sw = d.dwWidth / 8, x0 = left ? 0 : d.dwWidth - sw;
    BYTE* base = (BYTE*)d.lpSurface;
    bool ok = base != NULL && (bpp == 32 || bpp == 16);
    if (ok) {
      for (DWORD y = 0; y < d.dwHeight; y++) {
        BYTE* row = base + (size_t)y * d.lPitch;
        if (bpp == 32) { DWORD* p = (DWORD*)row + x0; for (DWORD x = 0; x < sw; x++) p[x] = color; }
        else           { WORD*  p = (WORD*)row + x0;  for (DWORD x = 0; x < sw; x++) p[x] = (WORD)color; }
      }
    }
    s->Unlock(NULL);
    return ok;
  }

  void StripeAfterWorld() {
    zCRnd_D3D* rnd = (zCRnd_D3D*)zrenderer;
    if (!rnd || !rnd->xd3d_pbackBuffer) return;
    rnd->EndFrame();
    PaintStripe(rnd->xd3d_pbackBuffer, true, true);    // красная, слева
    rnd->BeginFrame();
  }

  // Зеркально отразить задний буфер (вызывается сразу после отрисовки основного мира)
  void MirrorBackBuffer() {
    zCRnd_D3D* rnd = (zCRnd_D3D*)zrenderer;
    if (!rnd || !rnd->xd3d_pbackBuffer) return;

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);

    rnd->EndFrame();                          // сбросить очередь и закрыть сцену (EndScene)
    bool ok = MirrorByLock(rnd->xd3d_pbackBuffer);
    rnd->BeginFrame();                        // открыть сцену снова для интерфейса

    QueryPerformanceCounter(&t1);
    g_msSum += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    if (++g_msCount == 120) {
      char b[96]; sprintf_s(b, sizeof(b), "avg mirror time over 120 frames: %.2f ms", g_msSum / g_msCount);
      Log(b);
      g_msSum = 0; g_msCount = 0;
    }

    if (ok) g_failStreak = 0;
    else if (++g_failStreak >= 30) { g_disabledByError = true; g_mode = 0; Log("mode reset to 0: 30 failures in a row"); }
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
    if (IsMainWorld(self)) {
      if (g_mode == 1) MirrorBackBuffer();
      else if (g_mode == 2) StripeAfterWorld();
    }
  }

  // ---------- Хук oCGame::RenderBlit (самый конец кадра, перед выводом на экран) ----------
  void __fastcall Hook_Game_RenderBlit(oCGame* self, void* edx);
  CInvoke<void(__fastcall*)(oCGame*, void*)> Ivk_Game_RenderBlit(0x006CA910, &Hook_Game_RenderBlit);
  void __fastcall Hook_Game_RenderBlit(oCGame* self, void* edx) {
    if (g_mode == 3) {
      zCRnd_D3D* rnd = (zCRnd_D3D*)zrenderer;
      if (rnd && rnd->xd3d_pbackBuffer) PaintStripe(rnd->xd3d_pbackBuffer, false, false);   // синяя, справа
    }
    Ivk_Game_RenderBlit(self, edx);
  }

  // ---------- События Union ----------

  void Game_Entry() {
    WriteText("zMirror_log.txt", "Game_Entry: zMirrorTest v3 loaded\n", false);
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
      if (g_disabledByError) { g_disabledByError = false; g_failStreak = 0; }
      g_mode = (g_mode + 1) % 4;
      const char* names[4] = { "MODE 0: OFF", "MODE 1: MIRROR after world", "MODE 2: RED stripe after world", "MODE 3: BLUE stripe end of frame" };
      Log(std::string("mode -> ") + names[g_mode]);
      if (screen) screen->PrintCXY(zSTRING(names[g_mode]));
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
