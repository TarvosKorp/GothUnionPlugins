// zMirrorTest v8 (дим-версия для диагностики):
//   HOME — вкл/выкл зеркало вида (мир переворачивается, UI остаётся).
//   END  — телеметрия: 1-е нажатие старт записи, 2-е стоп + дамп в zMirror_log.txt.
//   O    — зеркалирование сущностей: выкл -> только ГГ -> все NPC -> выкл.
//
// Методы зеркала (секция [ZHORIZMOUSEINVERSION] в Gothic.ini):
//   MirrorMethod=0 (по умолчанию) — быстрый CPU-переворот (SSE2),
//   MirrorMethod=1                — экспериментальный GPU-метод (проекция), наблюдать артефакты.
//
// v8 изменения:
//   - Телеметрия: toggle, сохраняет сами строки, все варианты печати.
//   - Надпись фокуса: зеркалится, если строка равна имени текущего фокусного объекта.
//   - Быстрый переворот строк через SSE2 (+ опционально 2 потока).
//   - GPU-метод с зеркалом в zCCamera::Activate и переключением cull-режима.
//   - Зеркалирование сущностей (zCVob::Render): отрицание X-строки мировой матрицы.
#include "resource.h"
#include <string>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <intrin.h>
#include <thread>

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
  void Log(const std::string& s) {            // не больше 2000 строк за сессию
    if (g_logLines++ < 2000) WriteText("zMirror_log.txt", s + "\n", true);
  }
  std::string Hex(unsigned long v) {
    char b[32]; sprintf_s(b, sizeof(b), "0x%08lX", v); return b;
  }

  // ---------- Состояние ----------
  bool g_mirror = false;
  bool g_methodGPU = false;
  int g_entStage = 0;        // 0 выкл, 1 ГГ, 2 все NPC
  int g_failStreak = 0;
  double g_msSum = 0; int g_msCount = 0; int g_msReports = 0;
  int g_frame = 0;
  bool g_inMainWorld = false; // true: сейчас рисуется главный мир (для GPU-метода и телеметрии cull)
  int g_labelMirrored = 0;
  int g_cullSets = 0; unsigned long g_cullLastVal = 0;

  // Реальный render target устройства (вызывающий делает Release())
  IDirectDrawSurface7* AcquireTarget() {
    IDirectDrawSurface7* rt = NULL;
    IDirect3DDevice7* dev = zCRnd_D3D::xd3d_pd3dDevice7;
    if (dev && SUCCEEDED(dev->GetRenderTarget(&rt)) && rt) return rt;
    return NULL;
  }

  // ---------- Быстрый переворот строк (SSE2) ----------
  inline void FlipRow32(DWORD* row, int w) {
    DWORD* l = row;
    DWORD* r = row + w - 4;
    while (l + 4 <= r) {
      __m128i L = _mm_loadu_si128((const __m128i*)l);
      __m128i R = _mm_loadu_si128((const __m128i*)r);
      __m128i Lr = _mm_shuffle_epi32(L, 0x1B);
      __m128i Rr = _mm_shuffle_epi32(R, 0x1B);
      _mm_storeu_si128((__m128i*)r, Lr);
      _mm_storeu_si128((__m128i*)l, Rr);
      l += 4; r -= 4;
    }
    for (; l < r; ++l, --r) { DWORD t = *l; *l = *r; *r = t; }
  }

  struct FlipJob { BYTE* base; long pitch; DWORD w; DWORD h; DWORD bpp; };
  void DoFlipRows(const FlipJob& j) {
    for (DWORD y = 0; y < j.h; y++) {
      BYTE* row = j.base + (size_t)y * j.pitch;
      if (j.bpp == 32) FlipRow32((DWORD*)row, (int)j.w);
      else if (j.bpp == 16) std::reverse((WORD*)row, (WORD*)row + j.w);
    }
  }

  bool MirrorByLock(IDirectDrawSurface7* s) {
    DDSURFACEDESC2 d; ZeroMemory(&d, sizeof(d)); d.dwSize = sizeof(d);
    HRESULT hr = s->Lock(NULL, &d, DDLOCK_WAIT | DDLOCK_SURFACEMEMORYPTR, NULL);
    if (FAILED(hr)) { Log("Mirror: Lock failed " + Hex(hr)); return false; }
    DWORD bpp = d.ddpfPixelFormat.dwRGBBitCount;
    bool ok = true;
    BYTE* base = (BYTE*)d.lpSurface;
    if (!base) ok = false;
    else if (bpp == 32 || bpp == 16) {
      // 2 потока: верх и низ экрана
      std::thread t1(DoFlipRows, FlipJob{ base, d.lPitch, d.dwWidth, d.dwHeight / 2, bpp });
      DoFlipRows(FlipJob{ base + (size_t)(d.dwHeight / 2) * d.lPitch, d.lPitch, d.dwWidth, d.dwHeight - d.dwHeight / 2, bpp });
      t1.join();
    }
    else ok = false;
    s->Unlock(NULL);
    return ok;
  }

  void MirrorWorldFast() {
    zCRnd_D3D* rnd = (zCRnd_D3D*)zrenderer;
    if (!rnd) return;
    IDirectDrawSurface7* rt = AcquireTarget();
    if (!rt) return;

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);

    rnd->EndFrame();
    bool ok = MirrorByLock(rt);
    rnd->BeginFrame();
    rt->Release();

    QueryPerformanceCounter(&t1);
    g_msSum += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    if (++g_msCount == 300) {
      if (g_msReports++ < 5) { char b[96]; sprintf_s(b, sizeof(b), "avg mirror time over 300 frames: %.2f ms", g_msSum / g_msCount); Log(b); }
      g_msSum = 0; g_msCount = 0;
    }
    if (ok) g_failStreak = 0;
    else if (++g_failStreak >= 30) { g_mirror = false; Log("mirror auto-disabled: 30 failures in a row"); }
  }

  // ---------- GPU-метод ----------
  void ApplyMirrorProjection(zCCamera* cam) {
    if (!cam) return;
    IDirect3DDevice7* dev = zCRnd_D3D::xd3d_pd3dDevice7;
    if (!dev) return;
    D3DMATRIX p;
    if (SUCCEEDED(dev->GetTransform(D3DTRANSFORMSTATE_PROJECTION, &p))) {
      p._11 = -p._11; p._12 = -p._12; p._13 = -p._13; p._14 = -p._14;
      dev->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &p);
    }
  }

  // zCCamera::Activate
  void __fastcall Hook_Cam_Activate(zCCamera* self, void* edx);
  CInvoke<void(__fastcall*)(zCCamera*, void*)> Ivk_Cam_Activate(0x0054A700, &Hook_Cam_Activate);
  void __fastcall Hook_Cam_Activate(zCCamera* self, void* edx) {
    Ivk_Cam_Activate(self, edx);
    if (g_inMainWorld && g_mirror && g_methodGPU) ApplyMirrorProjection(self);
  }

  // zCRnd_D3D::XD3D_SetRenderState — телеметрия cull (сколько раз движок выставляет CULLMODE)
  int __fastcall Hook_SetRenderState(zCRnd_D3D* self, void* edx, D3DRENDERSTATETYPE t, DWORD v);
  CInvoke<int(__fastcall*)(zCRnd_D3D*, void*, D3DRENDERSTATETYPE, DWORD)> Ivk_SetRenderState(0x00644EF0, &Hook_SetRenderState);
  int __fastcall Hook_SetRenderState(zCRnd_D3D* self, void* edx, D3DRENDERSTATETYPE t, DWORD v) {
    int r = Ivk_SetRenderState(self, edx, t, v);
    if (g_inMainWorld && t == D3DRS_CULLMODE) {
      g_cullSets++; g_cullLastVal = v;
    }
    return r;
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
      g_baseFlipX = fx;
      zinput->SetMouseFlipXY(fx ? 0 : 1, fy);
      g_mouseFlipApplied = true;
    }
    else if (!g_mirror && g_mouseFlipApplied) {
      zinput->SetMouseFlipXY(g_baseFlipX, fy);
      g_mouseFlipApplied = false;
    }
  }

  // ---------- Фаза интерфейса и подписи ----------
  bool g_uiPhase = false;
  int g_worldDepth = 0;
  int g_projDepth = 0;
  int g_cntPF = 0, g_cntPI = 0, g_cntPF2 = 0;

  inline bool ShouldMirrorProject() {
    return g_mirror && g_uiPhase && g_worldDepth == 0 && g_projDepth == 0;
  }

  // ---------- Телеметрия ----------
  struct TRec {
    const char* tag; DWORD caller; int a; int b; int count; char str[64]; bool focus;
  };
  TRec g_recs[400];
  int g_recCount = 0;
  bool g_teleOn = false;

  void RecEx(const char* tag, void* ret, int a, int b, const std::string& s = std::string(), bool focus = false) {
    if (!g_teleOn) return;
    DWORD c = (DWORD)ret;
    for (int i = 0; i < g_recCount; i++) {
      TRec& r = g_recs[i];
      if (r.tag == tag && r.caller == c && r.a == a && r.b == b &&
          (s.empty() || strcmp(r.str, s.c_str()) == 0)) {
        r.count++; r.focus |= focus; return;
      }
    }
    if (g_recCount < 400) {
      TRec& r = g_recs[g_recCount++];
      r.tag = tag; r.caller = c; r.a = a; r.b = b; r.count = 1; r.focus = focus;
      r.str[0] = 0;
      if (!s.empty()) strncpy_s(r.str, s.c_str(), _TRUNCATE);
    }
  }

  void Rec(const char* tag, void* ret, int a = 0, int b = 0, bool matchA = false) {
    if (!g_teleOn) return;
    DWORD c = (DWORD)ret;
    for (int i = 0; i < g_recCount; i++)
      if (g_recs[i].tag == tag && g_recs[i].caller == c && (!matchA || g_recs[i].a == a)) {
        g_recs[i].count++; g_recs[i].a = a; g_recs[i].b = b; return;
      }
    if (g_recCount < 400) {
      TRec& r = g_recs[g_recCount++];
      r.tag = tag; r.caller = c; r.a = a; r.b = b; r.count = 1; r.focus = false; r.str[0] = 0;
    }
  }

  void DumpRecs() {
    if (!g_teleOn) return;
    g_teleOn = false;
    Log("--- telemetry dump ---");
    char b[256];
    sprintf_s(b, sizeof(b), "mirror=%d methodGPU=%d entStage=%d labelMirrored=%d cullSets=%d cullLast=0x%08lX",
              g_mirror ? 1 : 0, g_methodGPU ? 1 : 0, g_entStage, g_labelMirrored, g_cullSets, g_cullLastVal);
    Log(b);
    for (int i = 0; i < g_recCount; i++) {
      TRec& r = g_recs[i];
      std::string line = std::string(r.tag) + "  caller=" + Hex(r.caller) +
                         "  count=" + std::to_string(r.count) +
                         "  last a=" + std::to_string(r.a) + " b=" + std::to_string(r.b);
      if (r.str[0]) { line += "  str=\""; line += r.str; line += "\""; }
      if (r.focus) line += "  [FOCUS-MATCH]";
      Log(line);
    }
    Log("--- end of dump ---");
    g_recCount = 0;
    g_cullSets = 0; g_cullLastVal = 0;
  }

  // ---------- Имя текущего фокусного объекта (кэш на кадр) ----------
  const char* FocusNameCStr() {
    static std::string cached;
    static int cachedFrame = -1;
    if (cachedFrame != g_frame) {
      cachedFrame = g_frame;
      cached.clear();
      if (ogame) {
        zSTRING n = oCNpcFocus::GetFocusName();
        const char* ch = n.ToChar();
        if (ch) cached = ch;
      }
    }
    return cached.c_str();
  }

  int ScreenWidthPixels() {
    zCRnd_D3D* rnd = (zCRnd_D3D*)zrenderer;
    return rnd ? rnd->xd3d_scrWidth : 0;
  }

  int GoMirrorX(zCView* v, int x) {
    if (!v) return x;
    int W = ScreenWidthPixels();
    if (W <= 0) return x;
    int px = v->nax(x);        // виртуальная -> пиксельная
    int nx = W - 1 - px;
    return v->anx(nx);         // пиксельная -> виртуальная
  }

  inline bool ShouldMirrorLabel() {
    return g_mirror && g_uiPhase && g_worldDepth == 0;
  }

  void MaybeMirrorLabel(zCView* self, int& x, const zSTRING& s) {
    if (!ShouldMirrorLabel()) return;
    const char* f = FocusNameCStr();
    if (!f || !f[0]) return;
    const char* t = s.ToChar();
    if (!t) return;
    if (_stricmp(t, f) == 0) {
      x = GoMirrorX(self, x);
      g_labelMirrored++;
      RecEx("FOCUS_LABEL mirror", _ReturnAddress(), x, 0, std::string(t), true);
    }
  }

  // ---------- Хуки печати (телеметрия + зеркалирование надписи) ----------
  // Print(int, int, zSTRING const&)
  void __fastcall Hook_View_Print(zCView* self, void* edx, int x, int y, zSTRING const& s);
  CInvoke<void(__fastcall*)(zCView*, void*, int, int, zSTRING const&)> Ivk_View_Print(0x007A9A40, &Hook_View_Print);
  void __fastcall Hook_View_Print(zCView* self, void* edx, int x, int y, zSTRING const& s) {
    const char* t = s.ToChar();
    RecEx(g_uiPhase ? "zCView::Print ui-phase" : "zCView::Print other", _ReturnAddress(), x, y, t ? t : "");
    MaybeMirrorLabel(self, x, s);
    Ivk_View_Print(self, edx, x, y, s);
  }

  // PrintChars(int, int, zSTRING const&)
  void __fastcall Hook_View_PrintChars(zCView* self, void* edx, int x, int y, zSTRING const& s);
  CInvoke<void(__fastcall*)(zCView*, void*, int, int, zSTRING const&)> Ivk_View_PrintChars(0x007A9B10, &Hook_View_PrintChars);
  void __fastcall Hook_View_PrintChars(zCView* self, void* edx, int x, int y, zSTRING const& s) {
    const char* t = s.ToChar();
    RecEx(g_uiPhase ? "zCView::PrintChars ui-phase" : "zCView::PrintChars other", _ReturnAddress(), x, y, t ? t : "");
    Ivk_View_PrintChars(self, edx, x, y, s);
  }

  // PrintCX(int, zSTRING const&)
  void __fastcall Hook_View_PrintCX(zCView* self, void* edx, int y, zSTRING const& s);
  CInvoke<void(__fastcall*)(zCView*, void*, int, zSTRING const&)> Ivk_View_PrintCX(0x007A9E50, &Hook_View_PrintCX);
  void __fastcall Hook_View_PrintCX(zCView* self, void* edx, int y, zSTRING const& s) {
    const char* t = s.ToChar();
    RecEx(g_uiPhase ? "zCView::PrintCX ui-phase" : "zCView::PrintCX other", _ReturnAddress(), 0, y, t ? t : "");
    Ivk_View_PrintCX(self, edx, y, s);
  }

  // PrintCY(int, zSTRING const&)
  void __fastcall Hook_View_PrintCY(zCView* self, void* edx, int x, zSTRING const& s);
  CInvoke<void(__fastcall*)(zCView*, void*, int, zSTRING const&)> Ivk_View_PrintCY(0x007A9F40, &Hook_View_PrintCY);
  void __fastcall Hook_View_PrintCY(zCView* self, void* edx, int x, zSTRING const& s) {
    const char* t = s.ToChar();
    RecEx(g_uiPhase ? "zCView::PrintCY ui-phase" : "zCView::PrintCY other", _ReturnAddress(), x, 0, t ? t : "");
    Ivk_View_PrintCY(self, edx, x, s);
  }

  // PrintCXY(zSTRING const&)
  void __fastcall Hook_View_PrintCXY(zCView* self, void* edx, zSTRING const& s);
  CInvoke<void(__fastcall*)(zCView*, void*, zSTRING const&)> Ivk_View_PrintCXY(0x007AA030, &Hook_View_PrintCXY);
  void __fastcall Hook_View_PrintCXY(zCView* self, void* edx, zSTRING const& s) {
    const char* t = s.ToChar();
    RecEx(g_uiPhase ? "zCView::PrintCXY ui-phase" : "zCView::PrintCXY other", _ReturnAddress(), 0, 0, t ? t : "");
    Ivk_View_PrintCXY(self, edx, s);
  }

  // PrintTimed(int, int, zSTRING const&, float, zCOLOR*)
  void __fastcall Hook_View_PrintTimed(zCView* self, void* edx, int x, int y, zSTRING const& s, float tm, zCOLOR* c);
  CInvoke<void(__fastcall*)(zCView*, void*, int, int, zSTRING const&, float, zCOLOR*)> Ivk_View_PrintTimed(0x007A7D20, &Hook_View_PrintTimed);
  void __fastcall Hook_View_PrintTimed(zCView* self, void* edx, int x, int y, zSTRING const& s, float tm, zCOLOR* c) {
    const char* t = s.ToChar();
    RecEx(g_uiPhase ? "zCView::PrintTimed ui-phase" : "zCView::PrintTimed other", _ReturnAddress(), x, y, t ? t : "");
    Ivk_View_PrintTimed(self, edx, x, y, s, tm, c);
  }

  // PrintTimedCX(int, zSTRING const&, float, zCOLOR*)
  void __fastcall Hook_View_PrintTimedCX(zCView* self, void* edx, int y, zSTRING const& s, float tm, zCOLOR* c);
  CInvoke<void(__fastcall*)(zCView*, void*, int, zSTRING const&, float, zCOLOR*)> Ivk_View_PrintTimedCX(0x007A7DB0, &Hook_View_PrintTimedCX);
  void __fastcall Hook_View_PrintTimedCX(zCView* self, void* edx, int y, zSTRING const& s, float tm, zCOLOR* c) {
    const char* t = s.ToChar();
    RecEx(g_uiPhase ? "zCView::PrintTimedCX ui-phase" : "zCView::PrintTimedCX other", _ReturnAddress(), 0, y, t ? t : "");
    Ivk_View_PrintTimedCX(self, edx, y, s, tm, c);
  }

  // PrintTimedCY(int, zSTRING const&, float, zCOLOR*)
  void __fastcall Hook_View_PrintTimedCY(zCView* self, void* edx, int x, zSTRING const& s, float tm, zCOLOR* c);
  CInvoke<void(__fastcall*)(zCView*, void*, int, zSTRING const&, float, zCOLOR*)> Ivk_View_PrintTimedCY(0x007A7F00, &Hook_View_PrintTimedCY);
  void __fastcall Hook_View_PrintTimedCY(zCView* self, void* edx, int x, zSTRING const& s, float tm, zCOLOR* c) {
    const char* t = s.ToChar();
    RecEx(g_uiPhase ? "zCView::PrintTimedCY ui-phase" : "zCView::PrintTimedCY other", _ReturnAddress(), x, 0, t ? t : "");
    Ivk_View_PrintTimedCY(self, edx, x, s, tm, c);
  }

  // PrintTimedCXY(zSTRING const&, float, zCOLOR*)
  void __fastcall Hook_View_PrintTimedCXY(zCView* self, void* edx, zSTRING const& s, float tm, zCOLOR* c);
  CInvoke<void(__fastcall*)(zCView*, void*, zSTRING const&, float, zCOLOR*)> Ivk_View_PrintTimedCXY(0x007A7FC0, &Hook_View_PrintTimedCXY);
  void __fastcall Hook_View_PrintTimedCXY(zCView* self, void* edx, zSTRING const& s, float tm, zCOLOR* c) {
    const char* t = s.ToChar();
    RecEx(g_uiPhase ? "zCView::PrintTimedCXY ui-phase" : "zCView::PrintTimedCXY other", _ReturnAddress(), 0, 0, t ? t : "");
    Ivk_View_PrintTimedCXY(self, edx, s, tm, c);
  }

  // CreateText(int, int, zSTRING const&)
  zCViewText* __fastcall Hook_View_CreateText3(zCView* self, void* edx, int x, int y, zSTRING const& s);
  CInvoke<zCViewText*(__fastcall*)(zCView*, void*, int, int, zSTRING const&)> Ivk_View_CreateText3(0x007AA2B0, &Hook_View_CreateText3);
  zCViewText* __fastcall Hook_View_CreateText3(zCView* self, void* edx, int x, int y, zSTRING const& s) {
    const char* t = s.ToChar();
    RecEx(g_uiPhase ? "zCView::CreateText(3) ui-phase" : "zCView::CreateText(3) other", _ReturnAddress(), x, y, t ? t : "");
    MaybeMirrorLabel(self, x, s);
    return Ivk_View_CreateText3(self, edx, x, y, s);
  }

  // CreateText(int, int, zSTRING const&, float, zCOLOR&, int, int)
  zCViewText* __fastcall Hook_View_CreateText7(zCView* self, void* edx, int x, int y, zSTRING const& s, float tm, zCOLOR& c, int a, int b);
  CInvoke<zCViewText*(__fastcall*)(zCView*, void*, int, int, zSTRING const&, float, zCOLOR&, int, int)> Ivk_View_CreateText7(0x007A7AB0, &Hook_View_CreateText7);
  zCViewText* __fastcall Hook_View_CreateText7(zCView* self, void* edx, int x, int y, zSTRING const& s, float tm, zCOLOR& c, int a, int b) {
    const char* t = s.ToChar();
    RecEx(g_uiPhase ? "zCView::CreateText(7) ui-phase" : "zCView::CreateText(7) other", _ReturnAddress(), x, y, t ? t : "");
    return Ivk_View_CreateText7(self, edx, x, y, s, tm, c, a, b);
  }

  // ---------- Клавиши: влево <-> вправо ----------
  inline unsigned short SwapLR(unsigned short key) {
    if (!g_mirror || !ogame || ogame->IsOnPause()) return key;
    switch (key) {
      case GAME_LEFT:        return GAME_RIGHT;
      case GAME_RIGHT:       return GAME_LEFT;
      case GAME_STRAFELEFT:  return GAME_STRAFERIGHT;
      case GAME_STRAFERIGHT: return GAME_STRAFELEFT;
      case GAME_ACTIONLEFT:  return GAME_ACTIONRIGHT;
      case GAME_ACTIONRIGHT: return GAME_ACTIONLEFT;
      default:               return key;
    }
  }

  // zCInput_Win32::GetState(unsigned short)
  float __fastcall Hook_Input_GetState(zCInput_Win32* self, void* edx, unsigned short key);
  CInvoke<float(__fastcall*)(zCInput_Win32*, void*, unsigned short)> Ivk_Input_GetState(0x004D4D30, &Hook_Input_GetState);
  inline bool IsLRKey(unsigned short k) {
    return k == GAME_LEFT || k == GAME_RIGHT || k == GAME_STRAFELEFT || k == GAME_STRAFERIGHT ||
           k == GAME_ACTIONLEFT || k == GAME_ACTIONRIGHT;
  }
  float __fastcall Hook_Input_GetState(zCInput_Win32* self, void* edx, unsigned short key) {
    if (IsLRKey(key)) Rec("GetState(LR key)", _ReturnAddress(), key, 0, true);
    return Ivk_Input_GetState(self, edx, SwapLR(key));
  }

  // zCInput_Win32::GetToggled(unsigned short)
  int __fastcall Hook_Input_GetToggled(zCInput_Win32* self, void* edx, unsigned short key);
  CInvoke<int(__fastcall*)(zCInput_Win32*, void*, unsigned short)> Ivk_Input_GetToggled(0x004D5020, &Hook_Input_GetToggled);
  int __fastcall Hook_Input_GetToggled(zCInput_Win32* self, void* edx, unsigned short key) {
    if (IsLRKey(key)) Rec("GetToggled(LR key)", _ReturnAddress(), key, 0, true);
    return Ivk_Input_GetToggled(self, edx, SwapLR(key));
  }

  // ---------- Зеркалирование сущностей ----------
  bool IsNpcVob(zCVob* v) {
    if (!v) return false;
    zCClassDef* d = v->_GetClassDef();
    while (d) {
      const char* cn = d->className.ToChar();
      if (cn && strcmp(cn, "oCNpc") == 0) return true;
      d = d->baseClassDef;
    }
    return false;
  }

  // zCVob::Render(zTRenderContext&)
  int __fastcall Hook_Vob_Render(zCVob* self, zTRenderContext& ctx);
  CInvoke<int(__fastcall*)(zCVob*, zTRenderContext&)> Ivk_Vob_Render(0x006015D0, &Hook_Vob_Render);
  int __fastcall Hook_Vob_Render(zCVob* self, zTRenderContext& ctx) {
    bool apply = false;
    if (g_mirror && g_entStage != 0) {
      if (g_entStage == 1) apply = ((zCVob*)player == self);
      else apply = IsNpcVob(self);
    }
    if (!apply) return Ivk_Vob_Render(self, ctx);

    // Сохраняем мировую матрицу и строим зеркальную: negate X-строку, трансляция X остаётся.
    float m[16];
    memcpy(m, &self->trafoObjToWorld, sizeof(m));
    float orig[16];
    memcpy(orig, m, sizeof(orig));
    m[0] = -m[0]; m[1] = -m[1]; m[2] = -m[2]; m[3] = -m[3];
    m[12] = orig[12];
    memcpy(&self->trafoObjToWorld, m, sizeof(m));
    int r = Ivk_Vob_Render(self, ctx);
    memcpy(&self->trafoObjToWorld, orig, sizeof(orig));
    return r;
  }

  // ---------- Хук zCWorld::Render ----------
  void __fastcall Hook_World_Render(zCWorld* self, void* edx, zCCamera& cam);
  CInvoke<void(__fastcall*)(zCWorld*, void*, zCCamera&)> Ivk_World_Render(0x00621700, &Hook_World_Render);
  void __fastcall Hook_World_Render(zCWorld* self, void* edx, zCCamera& cam) {
    bool main = IsMainWorld(self);
    bool prevUi = g_uiPhase;
    bool prevInMain = g_inMainWorld;
    g_uiPhase = false;
    g_worldDepth++;
    if (main) {
      g_frame++;
      g_inMainWorld = true;
    }

    if (main && g_mirror && g_methodGPU) {
      IDirect3DDevice7* dev = zCRnd_D3D::xd3d_pd3dDevice7;
      DWORD oldCull = D3DCULL_NONE;
      if (dev && SUCCEEDED(dev->GetRenderState(D3DRS_CULLMODE, &oldCull))) {
        DWORD neu = (oldCull == D3DCULL_CW) ? D3DCULL_CCW
                  : (oldCull == D3DCULL_CCW) ? D3DCULL_CW
                  : D3DCULL_CW;
        dev->SetRenderState(D3DRS_CULLMODE, neu);
        Ivk_World_Render(self, edx, cam);
        dev->SetRenderState(D3DRS_CULLMODE, oldCull);
      }
      else Ivk_World_Render(self, edx, cam);
    }
    else {
      Ivk_World_Render(self, edx, cam);
    }

    g_worldDepth--;
    g_inMainWorld = prevInMain;
    if (main) {
      if (g_mirror && !g_methodGPU) MirrorWorldFast();
      g_uiPhase = true;
    }
    else g_uiPhase = prevUi;
  }

  // ---------- События Union ----------
  void Game_Entry() {
    WriteText("zMirror_log.txt", "Game_Entry: zMirrorTest v8 loaded\n", false);
    int m = GetPrivateProfileIntA("ZHORIZMOUSEINVERSION", "MirrorMethod", 0, ExeDirPath("Gothic.ini").c_str());
    g_methodGPU = (m == 1);
    char b[96];
    sprintf_s(b, sizeof(b), "mirror method: %s", g_methodGPU ? "GPU (experimental)" : "CPU fast (SSE2)");
    Log(b);
  }

  void Game_Init() {
  }

  void Game_Exit() {
  }

  void Game_PreLoop() {
  }

  void Game_Loop() {
    static bool wasDown = false;
    bool down = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
    if (down && !wasDown) {
      g_mirror = !g_mirror;
      g_failStreak = 0;
      SyncMouseFlip();
      Log(g_mirror ? "mirror ON" : "mirror OFF");
      if (!g_mirror) {
        char b[160]; sprintf_s(b, sizeof(b), "Project mirrored calls: float=%d int=%d float2=%d", g_cntPF, g_cntPI, g_cntPF2);
        Log(b);
      }
      if (screen) screen->PrintCXY(zSTRING(g_mirror ? "MIRROR: ON" : "MIRROR: OFF"));
    }
    wasDown = down;

    static bool wasEnd = false;
    bool endDown = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
    if (endDown && !wasEnd) {
      if (g_teleOn) DumpRecs();
      else {
        g_teleOn = true;
        g_recCount = 0;
        Log("telemetry recording ON");
      }
      if (screen) screen->PrintCXY(zSTRING(g_teleOn ? "TELEMETRY: ON" : "TELEMETRY DUMPED"));
    }
    wasEnd = endDown;

    static bool wasO = false;
    bool oDown = (GetAsyncKeyState('O') & 0x8000) != 0;
    if (oDown && !wasO) {
      g_entStage = (g_entStage + 1) % 3;
      const char* msg = g_entStage == 0 ? "ENT-MIRROR: OFF"
                       : g_entStage == 1 ? "ENT-MIRROR: PLAYER"
                       : "ENT-MIRROR: ALL NPCs";
      Log(std::string("entStage=") + std::to_string(g_entStage));
      if (screen) screen->PrintCXY(zSTRING(msg));
    }
    wasO = oDown;
  }

  void Game_PostLoop() {
  }

  void Game_MenuLoop() {
  }

  TSaveLoadGameInfo& SaveLoadGameInfo = UnionCore::SaveLoadGameInfo;

  void Game_SaveBegin() { }
  void Game_SaveEnd() { }
  void LoadBegin() { }
  void LoadEnd() { }
  void Game_LoadBegin_NewGame() { LoadBegin(); }
  void Game_LoadEnd_NewGame() { LoadEnd(); }
  void Game_LoadBegin_SaveGame() { LoadBegin(); }
  void Game_LoadEnd_SaveGame() { LoadEnd(); }
  void Game_LoadBegin_ChangeLevel() { LoadBegin(); }
  void Game_LoadEnd_ChangeLevel() { LoadEnd(); }
  void Game_LoadBegin_Trigger() { }
  void Game_LoadEnd_Trigger() { }
  void Game_Pause() { }
  void Game_Unpause() { }
  void Game_DefineExternals() { }
  void Game_ApplyOptions() { }

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
