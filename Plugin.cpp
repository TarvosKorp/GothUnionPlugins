// ═══════════════════════════════════════════════════════════════════
//  zMirrorTest V1.3
//  Плагин зеркального режима для Gothic 2 NotR (Union SDK, G2A).
//
//  УПРАВЛЕНИЕ:
//    HOME — цикл режимов отзеркаливания: OFF → CPU → GPUv8 → GPUv10 → OFF.
//    END  — телеметрия (детектинг UI-режимов) ON/OFF.
//    ]    — зеркалирование сущностей (все NPC) ON/OFF.
//
//  РЕЖИМЫ ЗЕРКАЛА (одинаковые правила управления/надписей во всех):
//    CPU   — переворот строк бэкбуфера после рендера мира (SSE2, 2 потока).
//            Эталон: зеркалятся мир, пропсы и PFX. Стоимость ~7-12 мс/кадр.
//    GPUv8 — отражение первой строки матрицы проекции в zCCamera::Activate
//            + инверсия CULLMODE (CW<->CCW) в XD3D_SetRenderState.
//            Мир и пропсы зеркалятся, пропсы вывернуты, PFX не зеркалятся.
//            Почти бесплатно по FPS.
//    GPUv10— отражение трасформаций камеры (trafoView/trafoViewInv/стеки).
//            Локация НЕ зеркалится, пропсы зеркалятся (вывернуты).
//            Почти бесплатно по FPS.
//
//  ОБЩИЕ ПРАВИЛА (для всех режимов):
//    - Своп LR-клавиш и инверсия мыши по X активны только в зеркале.
//    - В модальных окнах (zCView::modal) и в режиме взлома замков своп
//      отключается (управление остаётся «ванильным»).
//    - Надписи фокуса (NPC/предметы/Mobsi) зеркалятся по X в виртуальных
//      координатах view с учётом ширины текста.
//
//  ИСТОРИЯ ВЕРСИЙ:
//    V1.0 — только горизонтальная инверсия мыши (оригинальный zHorizMouseInversion).
//    V1.1 — CPU-отзеркаливание мира и управления в обход интерфейсов (Клод).
//    V1.2 — фокусные имена, отзеркаливание сущностей, тесты GPU.
//    V1.3 — взлом сундуков, режимы на HOME, чистка карт/телеметрии.
// ═══════════════════════════════════════════════════════════════════
#include "resource.h"
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
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
  void Log(const std::string& s) {
    if (g_logLines++ < 2500) WriteText("zMirror_log.txt", s + "\n", true);
  }
  std::string Hex(unsigned long v) {
    char b[32]; sprintf_s(b, sizeof(b), "0x%08lX", v); return b;
  }

  // ---------- Состояние ----------
  int g_mode = 0;          // 0=OFF, 1=CPU, 2=GPUv8, 3=GPUv10
  bool g_entMirror = false; // зеркалирование сущностей (все NPC)
  bool g_teleOn = false;    // телеметрия (детект UI-режимов)
  int g_failStreak = 0;
  double g_msSum = 0; int g_msCount = 0; int g_msReports = 0;
  int g_frame = 0;
  bool g_inMainWorld = false;
  bool g_focusLockable = false;

  inline bool MirrorOn() { return g_mode != 0; }

  // ---------- Фаза интерфейса ----------
  bool g_uiPhase = false;
  int g_worldDepth = 0;

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
    else if (++g_failStreak >= 30) { g_mode = 0; SyncMouseFlip(); Log("mirror auto-disabled: 30 failures in a row"); }
  }

  // ---------- GPUv8: отражение первой строки проекции ----------
  void ApplyMirrorProjectionV8() {
    IDirect3DDevice7* dev = zCRnd_D3D::xd3d_pd3dDevice7;
    if (!dev) return;
    D3DMATRIX p;
    if (SUCCEEDED(dev->GetTransform(D3DTRANSFORMSTATE_PROJECTION, &p))) {
      p._11 = -p._11; p._12 = -p._12; p._13 = -p._13; p._14 = -p._14;
      dev->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &p);
    }
  }

  // ---------- GPUv10: отражение трасформаций камеры ----------
  void MirrorMatrixRow0(zMAT4& m) {
    m.v[0][0] = -m.v[0][0];
    m.v[0][1] = -m.v[0][1];
    m.v[0][2] = -m.v[0][2];
    m.v[0][3] = -m.v[0][3];
  }
  void MirrorMatrixCol0(zMAT4& m) {
    m.v[0][0] = -m.v[0][0];
    m.v[1][0] = -m.v[1][0];
    m.v[2][0] = -m.v[2][0];
    m.v[3][0] = -m.v[3][0];
  }

  void ApplyMirrorView(zCCamera* cam) {
    if (!cam) return;
    MirrorMatrixRow0(cam->trafoView);
    MirrorMatrixCol0(cam->trafoViewInv);
    // zCMatrixStack: int pos (4 байта) + T stack[SIZE] — элементы на смещении 4.
    zMAT4* viewStack = reinterpret_cast<zMAT4*>((char*)&cam->trafoViewStack + 4);
    zMAT4* wvStack = reinterpret_cast<zMAT4*>((char*)&cam->trafoWorldViewStack + 4);
    for (int i = 0; i < 8; i++) {
      MirrorMatrixRow0(viewStack[i]);
      MirrorMatrixRow0(wvStack[i]);
    }
  }

  // zCCamera::Activate
  void __fastcall Hook_Cam_Activate(zCCamera* self, void* edx);
  CInvoke<void(__fastcall*)(zCCamera*, void*)> Ivk_Cam_Activate(0x0054A700, &Hook_Cam_Activate);
  void __fastcall Hook_Cam_Activate(zCCamera* self, void* edx) {
    Ivk_Cam_Activate(self, edx);
    if (!g_inMainWorld || !MirrorOn()) return;
    if (g_mode == 2) ApplyMirrorProjectionV8();
    else if (g_mode == 3) ApplyMirrorView(self);
  }

  // Инверсия CULLMODE для GPUv8 (движок сам ставит режим много раз).
  inline DWORD InvertCull(DWORD v) {
    if (v == D3DCULL_CW) return D3DCULL_CCW;
    if (v == D3DCULL_CCW) return D3DCULL_CW;
    return v;
  }

  // zCRnd_D3D::XD3D_SetRenderState
  int __fastcall Hook_SetRenderState(zCRnd_D3D* self, void* edx, D3DRENDERSTATETYPE t, DWORD v);
  CInvoke<int(__fastcall*)(zCRnd_D3D*, void*, D3DRENDERSTATETYPE, DWORD)> Ivk_SetRenderState(0x00644EF0, &Hook_SetRenderState);
  int __fastcall Hook_SetRenderState(zCRnd_D3D* self, void* edx, D3DRENDERSTATETYPE t, DWORD v) {
    if (t == D3DRENDERSTATE_CULLMODE && g_inMainWorld && g_mode == 2 && MirrorOn())
      v = InvertCull(v);
    return Ivk_SetRenderState(self, edx, t, v);
  }

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
    if (MirrorOn() && !g_mouseFlipApplied) {
      g_baseFlipX = fx;
      zinput->SetMouseFlipXY(fx ? 0 : 1, fy);
      g_mouseFlipApplied = true;
    }
    else if (!MirrorOn() && g_mouseFlipApplied) {
      zinput->SetMouseFlipXY(g_baseFlipX, fy);
      g_mouseFlipApplied = false;
    }
  }

  // ---------- Класс объекта (для детекта UI-режимов) ----------
  std::string ClassOf(zCObject* obj) {
    if (!obj) return "null";
    zCClassDef* d = obj->_GetClassDef();
    while (d) {
      const char* cn = d->className.ToChar();
      if (cn && cn[0]) return std::string(cn);
      d = d->baseClassDef;
    }
    return "unknown";
  }

  bool ClassChainHas(zCVob* v, const char* wanted) {
    if (!v) return false;
    zCClassDef* d = v->_GetClassDef();
    while (d) {
      const char* cn = d->className.ToChar();
      if (cn && strcmp(cn, wanted) == 0) return true;
      d = d->baseClassDef;
    }
    return false;
  }

  bool IsFocusLockableNow() {
    zCVob* fv = (player && ogame) ? player->GetFocusVob() : NULL;
    if (!fv) return false;
    return ClassChainHas(fv, "oCMobContainer") || ClassChainHas(fv, "oCMobDoor") || ClassChainHas(fv, "oCMobLockable");
  }

  // ---------- Кандидаты имени фокуса (кэш на кадр) ----------
  // Используется ТОЛЬКО для зеркалирования надписей (не для телеметрии).
  const std::vector<std::string>& FocusNamesCached() {
    static std::vector<std::string> cached;
    static int cachedFrame = -1;
    if (cachedFrame != g_frame) {
      cachedFrame = g_frame;
      cached.clear();
      g_focusLockable = false;
      zCVob* fv = (player && ogame) ? player->GetFocusVob() : NULL;
      if (fv) {
        if (ClassChainHas(fv, "oCNpc")) {
          const char* c = ((oCNpc*)fv)->name[0].ToChar();
          if (c && c[0]) cached.push_back(c);
        }
        else if (ClassChainHas(fv, "oCItem")) {
          const char* c = ((oCItem*)fv)->name.ToChar();
          if (c && c[0]) cached.push_back(c);
        }
        else if (ClassChainHas(fv, "oCMOB")) {
          const char* c = ((oCMOB*)fv)->GetName().ToChar();
          if (c && c[0]) cached.push_back(c);
          const char* c2 = ((oCMOB*)fv)->name.ToChar();
          if (c2 && c2[0] && strcmp(c2, c) != 0) cached.push_back(c2);
        }
        if (IsFocusLockableNow()) g_focusLockable = true;
      }
      if (ogame) {
        zSTRING n = oCNpcFocus::GetFocusName();
        const char* ch = n.ToChar();
        // GetFocusName() может вернуть имя РЕЖИМА ("FOCUS_*") — пропускаем.
        if (ch && ch[0] && strncmp(ch, "FOCUS_", 6) != 0) {
          bool exists = false;
          for (size_t i = 0; i < cached.size(); i++) if (cached[i] == ch) { exists = true; break; }
          if (!exists) cached.push_back(ch);
        }
      }
    }
    return cached;
  }

  std::string TrimStr(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    return s.substr(a, b - a + 1);
  }

  // Зеркалирование X в виртуальных координатах view с учётом ширины текста.
  int GoMirrorXVirtual(zCView* v, int x, int w) {
    if (!v) return x;
    int vsiz = v->vsizex;
    if (vsiz <= 0) vsiz = 8192;
    int right = v->vposx * 2 + vsiz;
    if (w > 0 && w < vsiz / 2) return right - (x + w);
    return right - 1 - x;
  }

  inline bool ShouldMirrorLabel() {
    return MirrorOn() && g_uiPhase && g_worldDepth == 0;
  }

  void MaybeMirrorLabel(zCView* self, int& x, const zSTRING& s) {
    if (!ShouldMirrorLabel()) return;
    const std::vector<std::string>& names = FocusNamesCached();
    if (names.empty()) return;
    const char* t = s.ToChar();
    if (!t) return;
    std::string ts = TrimStr(t);
    if (ts.empty()) return;
    bool match = false;
    for (size_t i = 0; i < names.size(); i++) {
      std::string fs = TrimStr(names[i]);
      if (fs.empty()) continue;
      if (_stricmp(ts.c_str(), fs.c_str()) == 0 ||
          (ts.size() > fs.size() && _strnicmp(ts.c_str(), fs.c_str(), fs.size()) == 0) ||
          (fs.size() > ts.size() && _strnicmp(fs.c_str(), ts.c_str(), ts.size()) == 0)) {
        match = true;
        break;
      }
    }
    if (!match) return;
    int w = self->FontSize(const_cast<zSTRING&>(s));
    x = GoMirrorXVirtual(self, x, w);
  }

  // ---------- Хуки печати (только зеркалирование надписей) ----------
  void __fastcall Hook_View_Print(zCView* self, void* edx, int x, int y, zSTRING const& s);
  CInvoke<void(__fastcall*)(zCView*, void*, int, int, zSTRING const&)> Ivk_View_Print(0x007A9A40, &Hook_View_Print);
  void __fastcall Hook_View_Print(zCView* self, void* edx, int x, int y, zSTRING const& s) {
    MaybeMirrorLabel(self, x, s);
    Ivk_View_Print(self, edx, x, y, s);
  }

  zCViewText* __fastcall Hook_View_CreateText3(zCView* self, void* edx, int x, int y, zSTRING const& s);
  CInvoke<zCViewText*(__fastcall*)(zCView*, void*, int, int, zSTRING const&)> Ivk_View_CreateText3(0x007AA2B0, &Hook_View_CreateText3);
  zCViewText* __fastcall Hook_View_CreateText3(zCView* self, void* edx, int x, int y, zSTRING const& s) {
    MaybeMirrorLabel(self, x, s);
    return Ivk_View_CreateText3(self, edx, x, y, s);
  }

  // ---------- Клавиши: влево <-> вправо ----------
  // Вызывающие коды режима взлома/навигации по замку (найдены телеметрией).
  inline bool IsLockpickCaller(DWORD c) {
    return c == 0x0069857D || c == 0x00698593 || c == 0x006985B9 || c == 0x006985CF ||
           c == 0x00723E6D || c == 0x00723E7E;
  }

  inline unsigned short SwapLR(unsigned short key, DWORD caller) {
    if (!MirrorOn() || !ogame || ogame->IsOnPause()) return key;
    if (zCView::modal) return key;                       // инвентарь, торговля и т.п.
    if (IsFocusLockableNow() && IsLockpickCaller(caller)) {
      if (g_teleOn) {
        static bool lockpickLogged = false;
        if (!lockpickLogged) { lockpickLogged = true; Log("lockpick mode detected"); }
      }
      return key;                                        // взлом замков — без свопа
    }
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

  float __fastcall Hook_Input_GetState(zCInput_Win32* self, void* edx, unsigned short key);
  CInvoke<float(__fastcall*)(zCInput_Win32*, void*, unsigned short)> Ivk_Input_GetState(0x004D4D30, &Hook_Input_GetState);
  inline bool IsLRKey(unsigned short k) {
    return k == GAME_LEFT || k == GAME_RIGHT || k == GAME_STRAFELEFT || k == GAME_STRAFERIGHT ||
           k == GAME_ACTIONLEFT || k == GAME_ACTIONRIGHT;
  }
  float __fastcall Hook_Input_GetState(zCInput_Win32* self, void* edx, unsigned short key) {
    return Ivk_Input_GetState(self, edx, SwapLR(key, (DWORD)_ReturnAddress()));
  }

  int __fastcall Hook_Input_GetToggled(zCInput_Win32* self, void* edx, unsigned short key);
  CInvoke<int(__fastcall*)(zCInput_Win32*, void*, unsigned short)> Ivk_Input_GetToggled(0x004D5020, &Hook_Input_GetToggled);
  int __fastcall Hook_Input_GetToggled(zCInput_Win32* self, void* edx, unsigned short key) {
    return Ivk_Input_GetToggled(self, edx, SwapLR(key, (DWORD)_ReturnAddress()));
  }

  // ---------- Зеркалирование сущностей (все NPC, вывернутые полигоны) ----------
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
    bool entApply = MirrorOn() && g_entMirror && IsNpcVob(self);
    if (!entApply) return Ivk_Vob_Render(self, ctx);

    // zMAT4 хранит строки: v[0]=(right.x,up.x,at.x,pos.x), v[1]=(right.y,...), v[2]=(right.z,...).
    // Отрицаем только right-вектор (m[0], m[4], m[8]); позицию не трогаем.
    float orig[16];
    memcpy(orig, &self->trafoObjToWorld, sizeof(orig));
    float m[16];
    memcpy(m, orig, sizeof(m));
    m[0] = -m[0];
    m[4] = -m[4];
    m[8] = -m[8];
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

    Ivk_World_Render(self, edx, cam);

    g_worldDepth--;
    g_inMainWorld = prevInMain;
    if (main) {
      if (MirrorOn() && g_mode == 1) MirrorWorldFast();
      g_uiPhase = true;
    }
    else g_uiPhase = prevUi;
  }

  // ---------- События Union ----------
  void Game_Entry() {
    WriteText("zMirror_log.txt", "Game_Entry: zMirrorTest V1.3 loaded\n", false);
  }

  void Game_Init() { }
  void Game_Exit() { }
  void Game_PreLoop() { }

  void Game_Loop() {
    // HOME — цикл режимов: OFF -> CPU -> GPUv8 -> GPUv10 -> OFF
    static bool wasHome = false;
    bool homeDown = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
    if (homeDown && !wasHome) {
      g_mode = (g_mode + 1) % 4;
      g_failStreak = 0;
      SyncMouseFlip();
      static const char* names[4] = { "OFF", "CPU", "GPUv8", "GPUv10" };
      Log(std::string("mirror mode: ") + names[g_mode]);
      if (screen) screen->PrintCXY(zSTRING(std::string("MIRROR MODE: ") + names[g_mode]));
    }
    wasHome = homeDown;

    // END — телеметрия (детект UI-режимов)
    static bool wasEnd = false;
    bool endDown = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
    if (endDown && !wasEnd) {
      g_teleOn = !g_teleOn;
      Log(g_teleOn ? "telemetry ON" : "telemetry OFF");
      if (screen) screen->PrintCXY(zSTRING(g_teleOn ? "TELEMETRY: ON" : "TELEMETRY: OFF"));
    }
    wasEnd = endDown;

    // ] — зеркалирование сущностей (все NPC)
    static bool wasBracket = false;
    bool bracketDown = (GetAsyncKeyState(VK_OEM_6) & 0x8000) != 0;
    if (bracketDown && !wasBracket) {
      g_entMirror = !g_entMirror;
      Log(std::string("entMirror=") + (g_entMirror ? "ON (ALL NPCs)" : "OFF"));
      if (screen) screen->PrintCXY(zSTRING(g_entMirror ? "ENT-MIRROR: ALL NPCs" : "ENT-MIRROR: OFF"));
    }
    wasBracket = bracketDown;

    // Детектинг модальных окон (универсальный, только при телеметрии)
    static zCView* lastModal = NULL;
    zCView* curModal = zCView::modal;
    if (curModal != lastModal) {
      if (g_teleOn) {
        if (curModal) Log("UI modal opened: " + ClassOf((zCObject*)curModal));
        else Log("UI modal closed");
      }
      lastModal = curModal;
    }
  }

  void Game_PostLoop() { }
  void Game_MenuLoop() { }

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
