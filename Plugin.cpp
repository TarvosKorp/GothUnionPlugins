// zMirrorTest v11 (диагностический билд):
//   HOME — вкл/выкл зеркало вида.
//   END  — телеметрия: 1-е нажатие старт, 2-е стоп + дамп.
//   O    — зеркалирование сущностей (эксперимент): выкл -> ГГ -> все NPC -> выкл.
//
// Методы зеркала ([ZHORIZMOUSEINVERSION] в Gothic.ini):
//   MirrorMethod=0 — CPU fast (SSE2).
//   MirrorMethod=1 — GPU: отражённая проекция + инверсия CULLMODE.
//   MirrorMethod=2 — GPU: отражённая проекция без инверсии CULLMODE.
//
// v11 (правки по тестам v10):
//   - Надписи: при отражении учитывается ширина текста W (FontSize):
//     x' = (vposx*2 + vsizex) - (x + W). Раньше был систематический сдвиг вправо ~W.
//   - GPU: убран trafoView-подход (мир не зеркалился, VOB "отрывались").
//     Теперь зеркалится проекция сразу после UpdateProjectionMatrix (0x0054A890) —
//     мир и VOB используют одну и ту же отражённую проекцию.
//   - Карта (эксперимент): хук oCViewDocumentMap::UpdatePosition (0x0068D7B0),
//     позиция стрелки ViewArrow зеркалится по X относительно центра документа.
//   - Телеметрия: добавлен лог диагностики надписи (x0/x1/W) и позиции стрелки карты.
#include "resource.h"
#include <string>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <intrin.h>
#include <thread>

namespace GOTHIC_ENGINE {

  // ---------- Конвертация cp1251 -> UTF-8 ----------
  std::string Cp1251ToUtf8(const std::string& in) {
    static const unsigned short cp1251[] = {
      0x0000,0x0001,0x0002,0x0003,0x0004,0x0005,0x0006,0x0007,0x0008,0x0009,0x000A,0x000B,0x000C,0x000D,0x000E,0x000F,
      0x0010,0x0011,0x0012,0x0013,0x0014,0x0015,0x0016,0x0017,0x0018,0x0019,0x001A,0x001B,0x001C,0x001D,0x001E,0x001F,
      0x0020,0x0021,0x0022,0x0023,0x0024,0x0025,0x0026,0x0027,0x0028,0x0029,0x002A,0x002B,0x002C,0x002D,0x002E,0x002F,
      0x0030,0x0031,0x0032,0x0033,0x0034,0x0035,0x0036,0x0037,0x0038,0x0039,0x003A,0x003B,0x003C,0x003D,0x003E,0x003F,
      0x0040,0x0041,0x0042,0x0043,0x0044,0x0045,0x0046,0x0047,0x0048,0x0049,0x004A,0x004B,0x004C,0x004D,0x004E,0x004F,
      0x0050,0x0051,0x0052,0x0053,0x0054,0x0055,0x0056,0x0057,0x0058,0x0059,0x005A,0x005B,0x005C,0x005D,0x005E,0x005F,
      0x0060,0x0061,0x0062,0x0063,0x0064,0x0065,0x0066,0x0067,0x0068,0x0069,0x006A,0x006B,0x006C,0x006D,0x006E,0x006F,
      0x0070,0x0071,0x0072,0x0073,0x0074,0x0075,0x0076,0x0077,0x0078,0x0079,0x007A,0x007B,0x007C,0x007D,0x007E,0x007F,
      0x0402,0x0403,0x201A,0x0453,0x201E,0x2026,0x2020,0x2021,0x20AC,0x2030,0x0409,0x2039,0x040A,0x040C,0x040B,0x040F,
      0x0452,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x0000,0x2122,0x0459,0x203A,0x045A,0x045C,0x045B,0x045F,
      0x00A0,0x040E,0x045E,0x0408,0x00A4,0x0490,0x00A6,0x00A7,0x0401,0x00A9,0x0404,0x00AB,0x00AC,0x00AD,0x00AE,0x0407,
      0x00B0,0x00B1,0x0406,0x0456,0x0491,0x00B5,0x00B6,0x00B7,0x0451,0x2116,0x0454,0x00BB,0x0458,0x0405,0x0455,0x0457,
      0x0410,0x0411,0x0412,0x0413,0x0414,0x0415,0x0416,0x0417,0x0418,0x0419,0x041A,0x041B,0x041C,0x041D,0x041E,0x041F,
      0x0420,0x0421,0x0422,0x0423,0x0424,0x0425,0x0426,0x0427,0x0428,0x0429,0x042A,0x042B,0x042C,0x042D,0x042E,0x042F,
      0x0430,0x0431,0x0432,0x0433,0x0434,0x0435,0x0436,0x0437,0x0438,0x0439,0x043A,0x043B,0x043C,0x043D,0x043E,0x043F,
      0x0440,0x0441,0x0442,0x0443,0x0444,0x0445,0x0446,0x0447,0x0448,0x0449,0x044A,0x044B,0x044C,0x044D,0x044E,0x044F
    };
    std::string out;
    for (size_t i = 0; i < in.size(); i++) {
      unsigned char c = (unsigned char)in[i];
      if (c < 0x80) { out += (char)c; continue; }
      unsigned short u = cp1251[c];
      if (u == 0) { out += '?'; continue; }
      if (u < 0x800) {
        out += (char)(0xC0 | (u >> 6));
        out += (char)(0x80 | (u & 0x3F));
      } else {
        out += (char)(0xE0 | (u >> 12));
        out += (char)(0x80 | ((u >> 6) & 0x3F));
        out += (char)(0x80 | (u & 0x3F));
      }
    }
    return out;
  }

  std::string HexBytes(const char* s, int maxlen = 16) {
    std::string r;
    for (int i = 0; i < maxlen && s && s[i]; i++) {
      char b[8]; sprintf_s(b, sizeof(b), "%02X", (unsigned char)s[i]); r += b;
    }
    return r;
  }

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
    if (g_logLines++ < 3000) WriteText("zMirror_log.txt", s + "\n", true);
  }
  std::string Hex(unsigned long v) {
    char b[32]; sprintf_s(b, sizeof(b), "0x%08lX", v); return b;
  }

  // ---------- Состояние ----------
  bool g_mirror = false;
  bool g_methodGPU = false;
  bool g_cullInvert = true;
  int g_entStage = 0;        // 0 выкл, 1 ГГ, 2 все NPC
  int g_failStreak = 0;
  double g_msSum = 0; int g_msCount = 0; int g_msReports = 0;
  int g_frame = 0;
  bool g_inMainWorld = false;
  int g_labelMirrored = 0;
  int g_cullSets = 0; unsigned long g_cullLastVal = 0;
  bool g_inLockpick = false;
  bool g_focusLockable = false;

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
    else if (++g_failStreak >= 30) { g_mirror = false; Log("mirror auto-disabled: 30 failures in a row"); }
  }

  // ---------- GPU-метод: отражённая проекция ----------
  // В D3DMATRIX (row-major) первая строка отвечает за X: v' = v * M.
  // Отрицание первой строки (_11.._14) отражает X на экране.
  void ApplyMirrorProjection() {
    IDirect3DDevice7* dev = zCRnd_D3D::xd3d_pd3dDevice7;
    if (!dev) return;
    D3DMATRIX p;
    if (SUCCEEDED(dev->GetTransform(D3DTRANSFORMSTATE_PROJECTION, &p))) {
      p._11 = -p._11; p._12 = -p._12; p._13 = -p._13; p._14 = -p._14;
      dev->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &p);
    }
  }

  // zCCamera::UpdateProjectionMatrix — вызывается каждый кадр перед рендером.
  // После неё сразу отражаем проекцию: и мир, и VOB используют её.
  void __fastcall Hook_Cam_UpdateProj(zCCamera* self, void* edx);
  CInvoke<void(__fastcall*)(zCCamera*, void*)> Ivk_Cam_UpdateProj(0x0054A890, &Hook_Cam_UpdateProj);
  void __fastcall Hook_Cam_UpdateProj(zCCamera* self, void* edx) {
    Ivk_Cam_UpdateProj(self, edx);
    if (g_inMainWorld && g_mirror && g_methodGPU) ApplyMirrorProjection();
  }

  inline DWORD InvertCull(DWORD v) {
    if (v == D3DCULL_CW) return D3DCULL_CCW;
    if (v == D3DCULL_CCW) return D3DCULL_CW;
    return v;
  }

  int __fastcall Hook_SetRenderState(zCRnd_D3D* self, void* edx, D3DRENDERSTATETYPE t, DWORD v);
  CInvoke<int(__fastcall*)(zCRnd_D3D*, void*, D3DRENDERSTATETYPE, DWORD)> Ivk_SetRenderState(0x00644EF0, &Hook_SetRenderState);
  int __fastcall Hook_SetRenderState(zCRnd_D3D* self, void* edx, D3DRENDERSTATETYPE t, DWORD v) {
    if (t == D3DRENDERSTATE_CULLMODE) {
      if (g_inMainWorld && g_mirror && g_methodGPU && g_cullInvert) v = InvertCull(v);
      if (g_inMainWorld) { g_cullSets++; g_cullLastVal = v; }
    }
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

  // ---------- Фаза интерфейса ----------
  bool g_uiPhase = false;
  int g_worldDepth = 0;
  int g_projDepth = 0;
  int g_cntPF = 0, g_cntPI = 0, g_cntPF2 = 0;

  inline bool ShouldMirrorProject() {
    return g_mirror && g_uiPhase && g_worldDepth == 0 && g_projDepth == 0;
  }

  // ---------- Телеметрия (агрегированная по строке) ----------
  struct TRec {
    const char* tag; DWORD caller; int a; int b; int count; char str[64]; char hex[64]; bool focus;
  };
  TRec g_recs[600];
  int g_recCount = 0;
  bool g_teleOn = false;

  bool IsOwnMessage(const char* s) {
    if (!s) return false;
    return strncmp(s, "TELEMETRY", 9) == 0 ||
           strncmp(s, "MIRROR:", 7) == 0 ||
           strncmp(s, "ENT-MIRROR", 10) == 0;
  }

  void RecEx(const char* tag, void* ret, int a, int b, const std::string& s = std::string(), bool focus = false) {
    if (!g_teleOn) return;
    const char* cs = s.c_str();
    if (IsOwnMessage(cs)) return;
    DWORD c = (DWORD)ret;
    for (int i = 0; i < g_recCount; i++) {
      TRec& r = g_recs[i];
      if (r.tag == tag && r.caller == c &&
          (s.empty() || strcmp(r.str, cs) == 0)) {
        r.count++; r.a = a; r.b = b; r.focus |= focus; return;
      }
    }
    if (g_recCount < 600) {
      TRec& r = g_recs[g_recCount++];
      r.tag = tag; r.caller = c; r.a = a; r.b = b; r.count = 1; r.focus = focus;
      r.str[0] = 0; r.hex[0] = 0;
      if (!s.empty()) {
        strncpy_s(r.str, cs, _TRUNCATE);
        std::string hx = HexBytes(cs, 20);
        strncpy_s(r.hex, hx.c_str(), _TRUNCATE);
      }
    }
  }

  void Rec(const char* tag, void* ret, int a = 0, int b = 0, bool matchA = false) {
    if (!g_teleOn) return;
    DWORD c = (DWORD)ret;
    for (int i = 0; i < g_recCount; i++)
      if (g_recs[i].tag == tag && g_recs[i].caller == c && (!matchA || g_recs[i].a == a)) {
        g_recs[i].count++; g_recs[i].a = a; g_recs[i].b = b; return;
      }
    if (g_recCount < 600) {
      TRec& r = g_recs[g_recCount++];
      r.tag = tag; r.caller = c; r.a = a; r.b = b; r.count = 1; r.focus = false; r.str[0] = 0; r.hex[0] = 0;
    }
  }

  void DumpFocusInfo() {
    char b[512];
    std::string focusName;
    if (ogame) {
      zSTRING n = oCNpcFocus::GetFocusName();
      const char* ch = n.ToChar();
      if (ch) focusName = ch;
    }
    sprintf_s(b, sizeof(b), "FOCUS: name=\"%s\" hex=%s len=%d",
              Cp1251ToUtf8(focusName).c_str(), HexBytes(focusName.c_str(), 20).c_str(), (int)focusName.size());
    Log(b);

    zCVob* fv = (player && ogame) ? player->GetFocusVob() : NULL;
    if (!fv) { Log("FOCUS: vob=null"); return; }
    zCClassDef* d = fv->_GetClassDef();
    std::string cls;
    while (d) { const char* cn = d->className.ToChar(); if (cn) { cls = cn; break; } d = d->baseClassDef; }
    std::string nm;
    if (cls == "oCNpc") { zSTRING t = ((oCNpc*)fv)->name[0]; const char* c = t.ToChar(); if (c) nm = c; }
    else if (cls == "oCItem") { zSTRING t = ((oCItem*)fv)->name; const char* c = t.ToChar(); if (c) nm = c; }
    else if (cls == "oCMOB") { zSTRING t = ((oCMOB*)fv)->GetName(); const char* c = t.ToChar(); if (c) nm = c; }
    sprintf_s(b, sizeof(b), "FOCUS: vob class=%s name=\"%s\" hex=%s", cls.c_str(), Cp1251ToUtf8(nm).c_str(), HexBytes(nm.c_str(), 20).c_str());
    Log(b);
  }

  void DumpRecs() {
    if (!g_teleOn) return;
    g_teleOn = false;
    Log("--- telemetry dump ---");
    char b[256];
    sprintf_s(b, sizeof(b), "mirror=%d methodGPU=%d cullInvert=%d entStage=%d labelMirrored=%d cullSets=%d cullLast=0x%08lX lockpick=%d",
              g_mirror ? 1 : 0, g_methodGPU ? 1 : 0, g_cullInvert ? 1 : 0, g_entStage,
              g_labelMirrored, g_cullSets, g_cullLastVal, g_inLockpick ? 1 : 0);
    Log(b);
    DumpFocusInfo();
    for (int i = 0; i < g_recCount; i++) {
      TRec& r = g_recs[i];
      std::string line = std::string(r.tag) + "  caller=" + Hex(r.caller) +
                         "  count=" + std::to_string(r.count) +
                         "  last a=" + std::to_string(r.a) + " b=" + std::to_string(r.b);
      if (r.str[0]) {
        line += "  str=\"" + Cp1251ToUtf8(r.str) + "\"";
        line += "  hex=" + std::string(r.hex);
      }
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
      g_focusLockable = false;
      zCVob* fv = (player && ogame) ? player->GetFocusVob() : NULL;
      if (fv) {
        zCClassDef* d = fv->_GetClassDef();
        std::string cls;
        while (d) { const char* cn = d->className.ToChar(); if (cn) { cls = cn; break; } d = d->baseClassDef; }
        if (cls == "oCNpc") { const char* c = ((oCNpc*)fv)->name[0].ToChar(); if (c) cached = c; }
        else if (cls == "oCItem") { const char* c = ((oCItem*)fv)->name.ToChar(); if (c) cached = c; }
        else if (cls == "oCMOB") { const char* c = ((oCMOB*)fv)->GetName().ToChar(); if (c) cached = c; }
        if (cls == "oCMobContainer" || cls == "oCMobDoor" || cls == "oCMobLockable") g_focusLockable = true;
      }
      if (cached.empty() && ogame) {
        zSTRING n = oCNpcFocus::GetFocusName();
        const char* ch = n.ToChar();
        if (ch) cached = ch;
      }
    }
    return cached.c_str();
  }

  std::string TrimStr(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    return s.substr(a, b - a + 1);
  }

  // Зеркалирование X в виртуальных координатах view с учётом ширины текста W.
  int GoMirrorXVirtual(zCView* v, int x, int w) {
    if (!v) return x;
    int vsiz = v->vsizex;
    if (vsiz <= 0) vsiz = 8192;
    int right = v->vposx * 2 + vsiz;
    if (w > 0 && w < vsiz / 2) return right - (x + w);
    return right - 1 - x;
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
    std::string ts = TrimStr(t);
    std::string fs = TrimStr(f);
    if (ts.empty() || fs.empty()) return;
    bool match = (_stricmp(ts.c_str(), fs.c_str()) == 0) ||
                 (ts.size() > fs.size() && _strnicmp(ts.c_str(), fs.c_str(), fs.size()) == 0) ||
                 (fs.size() > ts.size() && _strnicmp(fs.c_str(), ts.c_str(), ts.size()) == 0);
    if (match) {
      int w = self->FontSize(const_cast<zSTRING&>(s));
      int x0 = x;
      x = GoMirrorXVirtual(self, x, w);
      static int dbgFrame = -1;
      if (dbgFrame != g_frame) {
        dbgFrame = g_frame;
        char b[200];
        sprintf_s(b, sizeof(b), "FOCUS_LABEL diag: x0=%d x1=%d W=%d vsizex=%d vposx=%d",
                  x0, x, w, self->vsizex, self->vposx);
        Log(b);
      }
      g_labelMirrored++;
      RecEx("FOCUS_LABEL mirror", _ReturnAddress(), x, 0, std::string(t), true);
    }
  }

  inline void DetectLockpick(const char* t) {
    if (g_inLockpick) return;
    if (!t) return;
    if (g_focusLockable && (strstr(t, "<>") || strstr(t, "><"))) {
      g_inLockpick = true;
      Log("lockpick mode detected");
    }
  }

  // ---------- Хуки печати ----------
  void __fastcall Hook_View_Print(zCView* self, void* edx, int x, int y, zSTRING const& s);
  CInvoke<void(__fastcall*)(zCView*, void*, int, int, zSTRING const&)> Ivk_View_Print(0x007A9A40, &Hook_View_Print);
  void __fastcall Hook_View_Print(zCView* self, void* edx, int x, int y, zSTRING const& s) {
    const char* t = s.ToChar();
    DetectLockpick(t);
    RecEx(g_uiPhase ? "zCView::Print ui-phase" : "zCView::Print other", _ReturnAddress(), x, y, t ? t : "");
    MaybeMirrorLabel(self, x, s);
    Ivk_View_Print(self, edx, x, y, s);
  }

  void __fastcall Hook_View_PrintChars(zCView* self, void* edx, int x, int y, zSTRING const& s);
  CInvoke<void(__fastcall*)(zCView*, void*, int, int, zSTRING const&)> Ivk_View_PrintChars(0x007A9B10, &Hook_View_PrintChars);
  void __fastcall Hook_View_PrintChars(zCView* self, void* edx, int x, int y, zSTRING const& s) {
    const char* t = s.ToChar();
    DetectLockpick(t);
    RecEx(g_uiPhase ? "zCView::PrintChars ui-phase" : "zCView::PrintChars other", _ReturnAddress(), x, y, t ? t : "");
    Ivk_View_PrintChars(self, edx, x, y, s);
  }

  void __fastcall Hook_View_PrintCX(zCView* self, void* edx, int y, zSTRING const& s);
  CInvoke<void(__fastcall*)(zCView*, void*, int, zSTRING const&)> Ivk_View_PrintCX(0x007A9E50, &Hook_View_PrintCX);
  void __fastcall Hook_View_PrintCX(zCView* self, void* edx, int y, zSTRING const& s) {
    const char* t = s.ToChar();
    DetectLockpick(t);
    RecEx(g_uiPhase ? "zCView::PrintCX ui-phase" : "zCView::PrintCX other", _ReturnAddress(), 0, y, t ? t : "");
    Ivk_View_PrintCX(self, edx, y, s);
  }

  void __fastcall Hook_View_PrintCY(zCView* self, void* edx, int x, zSTRING const& s);
  CInvoke<void(__fastcall*)(zCView*, void*, int, zSTRING const&)> Ivk_View_PrintCY(0x007A9F40, &Hook_View_PrintCY);
  void __fastcall Hook_View_PrintCY(zCView* self, void* edx, int x, zSTRING const& s) {
    const char* t = s.ToChar();
    DetectLockpick(t);
    RecEx(g_uiPhase ? "zCView::PrintCY ui-phase" : "zCView::PrintCY other", _ReturnAddress(), x, 0, t ? t : "");
    Ivk_View_PrintCY(self, edx, x, s);
  }

  void __fastcall Hook_View_PrintCXY(zCView* self, void* edx, zSTRING const& s);
  CInvoke<void(__fastcall*)(zCView*, void*, zSTRING const&)> Ivk_View_PrintCXY(0x007AA030, &Hook_View_PrintCXY);
  void __fastcall Hook_View_PrintCXY(zCView* self, void* edx, zSTRING const& s) {
    const char* t = s.ToChar();
    DetectLockpick(t);
    RecEx(g_uiPhase ? "zCView::PrintCXY ui-phase" : "zCView::PrintCXY other", _ReturnAddress(), 0, 0, t ? t : "");
    Ivk_View_PrintCXY(self, edx, s);
  }

  void __fastcall Hook_View_PrintTimed(zCView* self, void* edx, int x, int y, zSTRING const& s, float tm, zCOLOR* c);
  CInvoke<void(__fastcall*)(zCView*, void*, int, int, zSTRING const&, float, zCOLOR*)> Ivk_View_PrintTimed(0x007A7D20, &Hook_View_PrintTimed);
  void __fastcall Hook_View_PrintTimed(zCView* self, void* edx, int x, int y, zSTRING const& s, float tm, zCOLOR* c) {
    const char* t = s.ToChar();
    DetectLockpick(t);
    RecEx(g_uiPhase ? "zCView::PrintTimed ui-phase" : "zCView::PrintTimed other", _ReturnAddress(), x, y, t ? t : "");
    Ivk_View_PrintTimed(self, edx, x, y, s, tm, c);
  }

  void __fastcall Hook_View_PrintTimedCX(zCView* self, void* edx, int y, zSTRING const& s, float tm, zCOLOR* c);
  CInvoke<void(__fastcall*)(zCView*, void*, int, zSTRING const&, float, zCOLOR*)> Ivk_View_PrintTimedCX(0x007A7DB0, &Hook_View_PrintTimedCX);
  void __fastcall Hook_View_PrintTimedCX(zCView* self, void* edx, int y, zSTRING const& s, float tm, zCOLOR* c) {
    const char* t = s.ToChar();
    DetectLockpick(t);
    RecEx(g_uiPhase ? "zCView::PrintTimedCX ui-phase" : "zCView::PrintTimedCX other", _ReturnAddress(), 0, y, t ? t : "");
    Ivk_View_PrintTimedCX(self, edx, y, s, tm, c);
  }

  void __fastcall Hook_View_PrintTimedCY(zCView* self, void* edx, int x, zSTRING const& s, float tm, zCOLOR* c);
  CInvoke<void(__fastcall*)(zCView*, void*, int, zSTRING const&, float, zCOLOR*)> Ivk_View_PrintTimedCY(0x007A7F00, &Hook_View_PrintTimedCY);
  void __fastcall Hook_View_PrintTimedCY(zCView* self, void* edx, int x, zSTRING const& s, float tm, zCOLOR* c) {
    const char* t = s.ToChar();
    DetectLockpick(t);
    RecEx(g_uiPhase ? "zCView::PrintTimedCY ui-phase" : "zCView::PrintTimedCY other", _ReturnAddress(), x, 0, t ? t : "");
    Ivk_View_PrintTimedCY(self, edx, x, s, tm, c);
  }

  void __fastcall Hook_View_PrintTimedCXY(zCView* self, void* edx, zSTRING const& s, float tm, zCOLOR* c);
  CInvoke<void(__fastcall*)(zCView*, void*, zSTRING const&, float, zCOLOR*)> Ivk_View_PrintTimedCXY(0x007A7FC0, &Hook_View_PrintTimedCXY);
  void __fastcall Hook_View_PrintTimedCXY(zCView* self, void* edx, zSTRING const& s, float tm, zCOLOR* c) {
    const char* t = s.ToChar();
    DetectLockpick(t);
    RecEx(g_uiPhase ? "zCView::PrintTimedCXY ui-phase" : "zCView::PrintTimedCXY other", _ReturnAddress(), 0, 0, t ? t : "");
    Ivk_View_PrintTimedCXY(self, edx, s, tm, c);
  }

  zCViewText* __fastcall Hook_View_CreateText3(zCView* self, void* edx, int x, int y, zSTRING const& s);
  CInvoke<zCViewText*(__fastcall*)(zCView*, void*, int, int, zSTRING const&)> Ivk_View_CreateText3(0x007AA2B0, &Hook_View_CreateText3);
  zCViewText* __fastcall Hook_View_CreateText3(zCView* self, void* edx, int x, int y, zSTRING const& s) {
    const char* t = s.ToChar();
    DetectLockpick(t);
    RecEx(g_uiPhase ? "zCView::CreateText(3) ui-phase" : "zCView::CreateText(3) other", _ReturnAddress(), x, y, t ? t : "");
    MaybeMirrorLabel(self, x, s);
    return Ivk_View_CreateText3(self, edx, x, y, s);
  }

  zCViewText* __fastcall Hook_View_CreateText7(zCView* self, void* edx, int x, int y, zSTRING const& s, float tm, zCOLOR& c, int a, int b);
  CInvoke<zCViewText*(__fastcall*)(zCView*, void*, int, int, zSTRING const&, float, zCOLOR&, int, int)> Ivk_View_CreateText7(0x007A7AB0, &Hook_View_CreateText7);
  zCViewText* __fastcall Hook_View_CreateText7(zCView* self, void* edx, int x, int y, zSTRING const& s, float tm, zCOLOR& c, int a, int b) {
    const char* t = s.ToChar();
    DetectLockpick(t);
    RecEx(g_uiPhase ? "zCView::CreateText(7) ui-phase" : "zCView::CreateText(7) other", _ReturnAddress(), x, y, t ? t : "");
    return Ivk_View_CreateText7(self, edx, x, y, s, tm, c, a, b);
  }

  // ---------- Клавиши: влево <-> вправо ----------
  inline unsigned short SwapLR(unsigned short key) {
    if (!g_mirror || !ogame || ogame->IsOnPause()) return key;
    if (zCView::modal || g_inLockpick) return key;
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
    if (IsLRKey(key)) Rec("GetState(LR key)", _ReturnAddress(), key, 0, true);
    return Ivk_Input_GetState(self, edx, SwapLR(key));
  }

  int __fastcall Hook_Input_GetToggled(zCInput_Win32* self, void* edx, unsigned short key);
  CInvoke<int(__fastcall*)(zCInput_Win32*, void*, unsigned short)> Ivk_Input_GetToggled(0x004D5020, &Hook_Input_GetToggled);
  int __fastcall Hook_Input_GetToggled(zCInput_Win32* self, void* edx, unsigned short key) {
    if (IsLRKey(key)) Rec("GetToggled(LR key)", _ReturnAddress(), key, 0, true);
    return Ivk_Input_GetToggled(self, edx, SwapLR(key));
  }

  // ---------- Зеркалирование сущностей (эксперимент) ----------
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

  DWORD g_savedCull = 0;
  void ApplyCullNone(bool on) {
    IDirect3DDevice7* dev = zCRnd_D3D::xd3d_pd3dDevice7;
    if (!dev) return;
    if (on) {
      if (SUCCEEDED(dev->GetRenderState(D3DRENDERSTATE_CULLMODE, &g_savedCull))) {
        if (g_savedCull != D3DCULL_NONE) dev->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
      }
    } else {
      dev->SetRenderState(D3DRENDERSTATE_CULLMODE, g_savedCull);
    }
  }

  int __fastcall Hook_Vob_Render(zCVob* self, zTRenderContext& ctx);
  CInvoke<int(__fastcall*)(zCVob*, zTRenderContext&)> Ivk_Vob_Render(0x006015D0, &Hook_Vob_Render);
  int __fastcall Hook_Vob_Render(zCVob* self, zTRenderContext& ctx) {
    bool apply = false;
    if (g_mirror && g_entStage != 0) {
      if (g_entStage == 1) apply = ((zCVob*)player == self);
      else apply = IsNpcVob(self);
    }
    if (!apply) return Ivk_Vob_Render(self, ctx);

    float orig[16];
    memcpy(orig, &self->trafoObjToWorld, sizeof(orig));
    float m[16];
    memcpy(m, orig, sizeof(m));
    m[0] = -m[0];
    m[4] = -m[4];
    m[8] = -m[8];
    memcpy(&self->trafoObjToWorld, m, sizeof(m));

    ApplyCullNone(true);
    int r = Ivk_Vob_Render(self, ctx);
    ApplyCullNone(false);
    memcpy(&self->trafoObjToWorld, orig, sizeof(orig));
    return r;
  }

  // ---------- Карта: зеркалирование позиции стрелки ----------
  // LevelCoords (zVEC4) = (xMin, yMin, xMax, yMax) в мировых координатах.
  // При зеркале X мира: xMin' = -xMax, xMax' = -xMin.
  // Подменяем координаты до вызова UpdatePosition — стрелка/маячки пересчитаются зеркально.
  void __fastcall Hook_Map_UpdatePosition(oCViewDocumentMap* self, void* edx);
  CInvoke<void(__fastcall*)(oCViewDocumentMap*, void*)> Ivk_Map_UpdatePosition(0x0068D7B0, &Hook_Map_UpdatePosition);
  void __fastcall Hook_Map_UpdatePosition(oCViewDocumentMap* self, void* edx) {
    if (!self) { Ivk_Map_UpdatePosition(self, edx); return; }
    if (g_mirror) {
      zVEC4 orig = self->LevelCoords;
      self->LevelCoords.n[0] = -orig.n[2];
      self->LevelCoords.n[2] = -orig.n[0];
      Ivk_Map_UpdatePosition(self, edx);
      self->LevelCoords = orig;
    } else {
      Ivk_Map_UpdatePosition(self, edx);
    }
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
      g_inLockpick = false;
    }

    Ivk_World_Render(self, edx, cam);

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
    WriteText("zMirror_log.txt", "Game_Entry: zMirrorTest v11 loaded\n", false);
    int m = GetPrivateProfileIntA("ZHORIZMOUSEINVERSION", "MirrorMethod", 0, ExeDirPath("Gothic.ini").c_str());
    if (m == 1) { g_methodGPU = true; g_cullInvert = true; }
    else if (m == 2) { g_methodGPU = true; g_cullInvert = false; }
    else { g_methodGPU = false; g_cullInvert = false; }
    char b[96];
    if (g_methodGPU) sprintf_s(b, sizeof(b), "mirror method: GPU (projection, cullInvert=%d)", g_cullInvert ? 1 : 0);
    else sprintf_s(b, sizeof(b), "mirror method: CPU fast (SSE2)");
    Log(b);
  }

  void Game_Init() { }
  void Game_Exit() { }
  void Game_PreLoop() { }

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
