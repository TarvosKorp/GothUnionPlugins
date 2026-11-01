// ═══════════════════════════════════════════════════════════════════
//  zMirrorTest V1.5-diagnostic
//  Плагин зеркального режима для Gothic 2 NotR (Union SDK, G2A).
//
//  УПРАВЛЕНИЕ:
//    HOME — цикл режимов: OFF → CPU → BLT → GPUv8 → OFF.
//    END  — расширенная телеметрия ON/OFF.
//    ]    — экспериментальное зеркалирование NPC ON/OFF (с коррекцией culling).
//    INSERT — точечный диагностический лог на 2 секунды (не требует END).
//    PAGEUP — метка сценария в логе.
//
//  РЕЖИМЫ ЗЕРКАЛА (одинаковые правила управления/надписей во всех):
//    CPU   — переворот строк бэкбуфера после рендера мира (SSE2, 2 потока).
//            Эталон: зеркалятся мир, пропсы и PFX. Стоимость ~7-12 мс/кадр.
//    BLT   — DirectDraw mirror Blt через временную поверхность (синхронно, с проверкой).
//            Если проверка не проходит — автоматический переход на CPU.
//    GPUv8 — экспериментальное отражение первого столбца projection matrix (D3D row-vector)
//            в zCCamera::Activate + инверсия CULLMODE. GPUv10 намеренно исключён из этой сборки.
//            Результат должен подтвердить тест: движок может повторно выставлять projection.
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
//    V1.3 — взлом сундуков, режимы на HOME.
//    V1.5 — BLT-режим, исправленная ось GPUv8, NPC/model-node телеметрия,
//           безопасный точечный лог без vtable-хуков D3D7, карта/документы (опц.).
// ═══════════════════════════════════════════════════════════════════
#include "resource.h"
#include "ZenGin/Gothic_II_Addon/API/zModel.h"
#include "ZenGin/Gothic_II_Addon/API/oViewDocument.h"
#include "ZenGin/Gothic_II_Addon/API/oDocumentManager.h"
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <intrin.h>
#include <thread>
#include <set>
#include <map>
#include <cstdarg>

#ifndef ZM_HOOK_MAP
#define ZM_HOOK_MAP 1
#endif
#ifndef ZM_HOOK_LEGACY_MAP
#define ZM_HOOK_LEGACY_MAP 1
#endif

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
      f.clear();
      char tmp[MAX_PATH] = {0};
      GetTempPathA(MAX_PATH, tmp);
      f.open((std::string(tmp) + name).c_str(), mode);
    }
    if (f.is_open()) f << text;
  }

  int g_logLines = 0;
  void Log(const std::string& s) {
    ++g_logLines;
    if(g_logLines<=8000) WriteText("zMirror_log.txt",s+"\n",true);
    else if(g_logLines==8001) WriteText("zMirror_log.txt","[LOG TRUNCATED: 8000-line limit]\n",true);
  }
  std::string Hex(unsigned long v) {
    char b[32]; sprintf_s(b, sizeof(b), "0x%08lX", v); return b;
  }

  std::string ClassOf(zCObject* obj);

  std::string Fmt(const char* fmt, ...) {
    char b[1024]; va_list ap; va_start(ap, fmt);
    vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, ap); va_end(ap); return std::string(b);
  }

  void DumpEvent(const std::string& s);

  // ---------- Состояние ----------
  int g_mode = 0;          // 0=OFF, 1=CPU, 2=BLT, 3=GPUv8
  bool g_entMirror = false; // зеркалирование сущностей (все NPC)
  bool g_teleOn = false;    // телеметрия (детект UI-режимов)
  int g_failStreak = 0;
  double g_msSum = 0; int g_msCount = 0; int g_msReports = 0;
  int g_frame = 0;
  bool g_inMainWorld = false;
  bool g_focusLockable = false;
  bool g_dumpActive = false;
  DWORD g_dumpUntil = 0;
  std::ofstream g_pointFile;
  int g_pointEntries = 0;
  int g_mark = 0;
  std::set<unsigned long long> g_inputSeen;
  std::set<unsigned long long> g_cullSeen;
  std::set<zCModel*> g_modelTraceSeen;
  std::set<zCVob*> g_npcSeen;
  DWORD g_lastCull = D3DCULL_CCW;
  bool g_haveCull = false;
  int g_entityCullDepth = 0;
  int g_mapShowCount = 0;
  int g_mapUpdateCount = 0;
  int g_camLogCount = 0;
  LARGE_INTEGER g_perfLastFrame = {};
  double g_perfFrameSum = 0.0;
  int g_perfFrameSamples = 0;

  inline bool MirrorOn() { return g_mode != 0; }

  // Ранняя декларация (используется в MirrorWorldFast при автоотключении).
  void SyncMouseFlip();

  void PerfFrameReset() { g_perfLastFrame.QuadPart=0; g_perfFrameSum=0.0; g_perfFrameSamples=0; }
  void PerfFrameTick() {
    LARGE_INTEGER now,freq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&freq);
    if(g_perfLastFrame.QuadPart) {
      double ms=(double)(now.QuadPart-g_perfLastFrame.QuadPart)*1000.0/(double)freq.QuadPart;
      if(ms>0.0 && ms<250.0) { g_perfFrameSum+=ms; ++g_perfFrameSamples; }
    }
    g_perfLastFrame=now;
    if(g_perfFrameSamples>=120) {
      double avg=g_perfFrameSum/g_perfFrameSamples;
      Log(Fmt("FRAME PERF mode=%d average-world-frame=%.3f ms fps=%.2f",g_mode,avg,1000.0/avg));
      g_perfFrameSum=0.0; g_perfFrameSamples=0;
    }
  }

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

  // BLT-режим: две синхронные DirectDraw-копии. Намеренно НЕ DDBLT_ASYNC:
  // до BeginFrame/Present поверхность должна гарантированно содержать готовый кадр.
  IDirectDrawSurface7* g_bltTmp = NULL;
  DWORD g_bltW = 0, g_bltH = 0;
  bool g_bltBroken = false;
  int g_bltFailures = 0;
  bool g_bltVerifyDone = false;
  int g_bltVerifyTries = 0;

  void ReleaseBltTmp() {
    if (g_bltTmp) { g_bltTmp->Release(); g_bltTmp = NULL; }
    g_bltW = g_bltH = 0;
  }

  bool EnsureBltTmp(IDirectDrawSurface7* rt) {
    DDSURFACEDESC2 d; ZeroMemory(&d, sizeof(d)); d.dwSize = sizeof(d);
    if (FAILED(rt->GetSurfaceDesc(&d))) return false;
    if (g_bltTmp && g_bltW == d.dwWidth && g_bltH == d.dwHeight) return true;
    ReleaseBltTmp();
    IUnknown* ddUnknown = NULL; IDirectDraw7* dd = NULL;
    HRESULT getDD=rt->GetDDInterface((LPVOID*)&ddUnknown);
    if(FAILED(getDD) || !ddUnknown) { Log(Fmt("BLT: GetDDInterface failed %s",Hex(getDD).c_str())); return false; }
    HRESULT qi=ddUnknown->QueryInterface(IID_IDirectDraw7,(void**)&dd);
    ddUnknown->Release();
    if(FAILED(qi) || !dd) { Log(Fmt("BLT: QueryInterface(IDirectDraw7) failed %s",Hex(qi).c_str())); return false; }
    DDCAPS caps; ZeroMemory(&caps,sizeof(caps)); caps.dwSize=sizeof(caps);
    if(SUCCEEDED(dd->GetCaps(&caps,NULL))) Log(Fmt("BLT caps: dwCaps=0x%08lX dwFXCaps=0x%08lX mirrorLR=%d",
      (unsigned long)caps.dwCaps,(unsigned long)caps.dwFXCaps,(caps.dwFXCaps&DDFXCAPS_BLTMIRRORLEFTRIGHT)?1:0));
    else Log("BLT caps: GetCaps failed");
    DDSURFACEDESC2 td; ZeroMemory(&td, sizeof(td)); td.dwSize = sizeof(td);
    td.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    td.dwWidth = d.dwWidth; td.dwHeight = d.dwHeight; td.ddpfPixelFormat = d.ddpfPixelFormat;
    td.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY;
    HRESULT hr = dd->CreateSurface(&td, &g_bltTmp, NULL);
    const char* mem = "VIDEO";
    if (FAILED(hr)) {
      td.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
      hr = dd->CreateSurface(&td, &g_bltTmp, NULL); mem = "SYSTEM";
    }
    dd->Release();
    if (FAILED(hr) || !g_bltTmp) { g_bltTmp = NULL; Log("BLT: CreateSurface failed " + Hex(hr)); return false; }
    g_bltW = d.dwWidth; g_bltH = d.dwHeight;
    Log(Fmt("BLT: temp surface %lux%lu (%s memory)", (unsigned long)g_bltW, (unsigned long)g_bltH, mem));
    return true;
  }

  bool ReadRow32(IDirectDrawSurface7* surface, DWORD y, std::vector<DWORD>& row) {
    DDSURFACEDESC2 d; ZeroMemory(&d,sizeof(d)); d.dwSize=sizeof(d);
    if(FAILED(surface->Lock(NULL,&d,DDLOCK_WAIT|DDLOCK_READONLY|DDLOCK_SURFACEMEMORYPTR,NULL))) return false;
    bool ok=false;
    if(d.lpSurface && d.ddpfPixelFormat.dwRGBBitCount==32 && y<d.dwHeight) {
      const DWORD* src=(const DWORD*)((const BYTE*)d.lpSurface+(size_t)y*d.lPitch);
      row.assign(src,src+d.dwWidth); ok=true;
    }
    surface->Unlock(NULL); return ok;
  }

  bool MirrorByBlt(IDirectDrawSurface7* rt) {
    if (!rt || !EnsureBltTmp(rt)) return false;
    std::vector<DWORD> before,after;
    DWORD sampleY=g_bltH/3;
    bool tryVerify=!g_bltVerifyDone && g_bltVerifyTries<10;
    if(tryVerify) ++g_bltVerifyTries;
    bool canVerify=tryVerify && ReadRow32(rt,sampleY,before);
    DDBLTFX fx; ZeroMemory(&fx,sizeof(fx)); fx.dwSize=sizeof(fx); fx.dwDDFX=DDBLTFX_MIRRORLEFTRIGHT;
    LARGE_INTEGER fq,t0,t1; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
    HRESULT a=g_bltTmp->Blt(NULL,rt,NULL,DDBLT_WAIT|DDBLT_DDFX,&fx);
    HRESULT b=FAILED(a)?a:rt->Blt(NULL,g_bltTmp,NULL,DDBLT_WAIT,NULL);
    QueryPerformanceCounter(&t1);
    static double bltSum=0.0; static int bltN=0;
    bltSum+=(double)(t1.QuadPart-t0.QuadPart)*1000.0/(double)fq.QuadPart;
    if(++bltN==120) { Log(Fmt("BLT direct two-Blt average over 120 frames: %.3f ms",bltSum/bltN)); bltSum=0.0; bltN=0; }
    if(FAILED(a)||FAILED(b)) {
      if(g_bltFailures++<5) Log(Fmt("BLT failed: source-to-temp=%s temp-to-target=%s",Hex(a).c_str(),Hex(b).c_str()));
      return false;
    }
    g_bltFailures=0;
    if(canVerify && ReadRow32(rt,sampleY,after) && before.size()==after.size() && before.size()>64) {
      int informative=0,matched=0; DWORD w=(DWORD)before.size();
      for(DWORD k=1;k<=7;k++) { DWORD x=w*k/8, mx=w-1-x; DWORD v=before[x]&0x00FFFFFF;
        if(v!=(before[mx]&0x00FFFFFF)) { ++informative; if((after[mx]&0x00FFFFFF)==v) ++matched; }
      }
      if(informative>=3) {
        g_bltVerifyDone=true;
        Log(Fmt("BLT verify: informative=%d mirrored-pairs=%d/%d",informative,matched,informative));
        if(matched!=informative) { g_bltBroken=true; Log("BLT verification FAILED: flag did not produce a horizontal mirror; CPU fallback enabled"); return false; }
        Log("BLT verification PASSED: sampled pixels confirm left-right mirror");
      } else if(g_bltVerifyTries>=10) { g_bltVerifyDone=true; Log("BLT verification INCONCLUSIVE: scene samples were too symmetric/flat; visual confirmation required"); }
    } else if(g_bltVerifyTries>=10) { g_bltVerifyDone=true; Log("BLT verification INCONCLUSIVE: surface could not be sampled after 10 attempts"); }
    return true;
  }

  void MirrorWorldFast() {
    zCRnd_D3D* rnd = (zCRnd_D3D*)zrenderer;
    if (!rnd) return;
    IDirectDrawSurface7* rt = AcquireTarget();
    if (!rt) return;
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    rnd->EndFrame();
    bool ok = false;
    if (g_mode == 2 && !g_bltBroken) ok = MirrorByBlt(rt);
    if (!ok) {
      if (g_mode == 2 && !g_bltBroken) { g_bltBroken = true; Log("BLT: failed; switching this frame to CPU Lock fallback (BLT disabled for session)"); }
      ok = MirrorByLock(rt);
    }
    rnd->BeginFrame(); rt->Release();
    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart-t0.QuadPart)*1000.0/(double)f.QuadPart;
    g_msSum += ms;
    if (++g_msCount == 120) {
      if (g_msReports++ < 10) { char b[160]; sprintf_s(b,sizeof(b),"mirror mode=%s fallback=%d avg EndFrame+mirror+BeginFrame over 120 frames: %.3f ms",g_mode==2?"BLT":"CPU",(g_mode==2&&g_bltBroken)?1:0,g_msSum/g_msCount); Log(b); }
      g_msSum=0; g_msCount=0;
    }
    if (ok) g_failStreak=0;
    else if (++g_failStreak>=30) { g_mode=0; SyncMouseFlip(); Log("mirror auto-disabled: 30 failures in a row"); }
  }

  // ---------- GPUv8: отражение X-компоненты projection matrix ----------
  void ApplyMirrorProjectionV8(zCCamera* cam) {
    IDirect3DDevice7* dev = zCRnd_D3D::xd3d_pd3dDevice7;
    if (!dev || !cam) return;
    // Берём базовую матрицу из zCCamera, а не инвертируем текущее состояние устройства:
    // повторный Activate тогда не сможет случайно отменить отражение двойным знаком.
    D3DMATRIX p; memcpy(&p,&cam->trafoProjection,sizeof(p));
    p._11 = -p._11; p._21 = -p._21; p._31 = -p._31; p._41 = -p._41;
    dev->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &p);
  }

  // zCCamera::Activate
  void __fastcall Hook_Cam_Activate(zCCamera* self, void* edx);
  CInvoke<void(__fastcall*)(zCCamera*, void*)> Ivk_Cam_Activate(0x0054A700, &Hook_Cam_Activate);
  void __fastcall Hook_Cam_Activate(zCCamera* self, void* edx) {
    Ivk_Cam_Activate(self, edx);
    if (g_inMainWorld && MirrorOn()) {
      if (g_mode == 3) ApplyMirrorProjectionV8(self);
    }
    if (g_inMainWorld && (g_teleOn || g_dumpActive)) {
      IDirect3DDevice7* dev=zCRnd_D3D::xd3d_pd3dDevice7; D3DMATRIX p,base;
      memcpy(&base,&self->trafoProjection,sizeof(base));
      std::string line=Fmt("CAM_BASE_PROJ frame=%d mode=%d [%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f]",g_frame,g_mode,
        base._11,base._12,base._13,base._14,base._21,base._22,base._23,base._24,base._31,base._32,base._33,base._34,base._41,base._42,base._43,base._44);
      if(g_teleOn && g_camLogCount<400) { Log(line); ++g_camLogCount; }
      DumpEvent(line);
      if(dev && SUCCEEDED(dev->GetTransform(D3DTRANSFORMSTATE_PROJECTION,&p))) {
        line=Fmt("CAM_DEVICE_PROJ frame=%d mode=%d [%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f]",g_frame,g_mode,
          p._11,p._12,p._13,p._14,p._21,p._22,p._23,p._24,p._31,p._32,p._33,p._34,p._41,p._42,p._43,p._44);
        if(g_teleOn && g_camLogCount<400) { Log(line); ++g_camLogCount; }
        DumpEvent(line);
      }
    }
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
    if (t == D3DRENDERSTATE_CULLMODE) {
      g_lastCull = v; g_haveCull = true;
      bool worldFlip = (g_inMainWorld && g_mode == 3 && MirrorOn());
      bool entityFlip = (g_entityCullDepth > 0);
      if (worldFlip != entityFlip) v = InvertCull(v);
      if (g_teleOn || g_dumpActive) {
        DWORD caller=(DWORD)_ReturnAddress();
        std::string line=Fmt("CULL requested=%lu applied=%lu world=%d entityDepth=%d caller=%s",
          (unsigned long)g_lastCull,(unsigned long)v,g_inMainWorld?1:0,g_entityCullDepth,Hex(caller).c_str());
        unsigned long long id=((unsigned long long)caller<<32)|(unsigned long long)g_lastCull;
        if(g_teleOn && g_cullSeen.size()<400 && g_cullSeen.insert(id).second) Log(line);
        DumpEvent(line);
      }
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

  void DumpEvent(const std::string& s) {
    if (!g_dumpActive || !g_pointFile.is_open()) return;
    if(g_pointEntries<6000) { g_pointFile << s << "\n"; ++g_pointEntries; }
    else if(g_pointEntries==6000) { g_pointFile << "[POINT DUMP TRUNCATED: 6000-line limit]\n"; ++g_pointEntries; }
    g_pointFile.flush(); // сохраняем частичный лог даже при последующем аварийном завершении
  }

  void FinishPointDump() {
    if (!g_dumpActive) return;
    DumpEvent("===== POINT LOG END =====");
    g_dumpActive = false;
    if(g_pointFile.is_open()) g_pointFile.close();
    Log("INSERT point log written: zMirror_point.txt");
  }

  void BeginPointDump(const char* reason) {
    if(g_dumpActive) FinishPointDump();
    g_modelTraceSeen.clear(); g_npcSeen.clear();
    g_pointFile.close();
    g_pointFile.open(ExeDirPath("zMirror_point.txt").c_str(),std::ios::out|std::ios::app);
    if(!g_pointFile.is_open()) { char tmp[MAX_PATH]={0}; GetTempPathA(MAX_PATH,tmp); g_pointFile.open((std::string(tmp)+"zMirror_point.txt").c_str(),std::ios::out|std::ios::app); }
    if(!g_pointFile.is_open()) { Log("INSERT point log: cannot open zMirror_point.txt"); return; }
    g_dumpActive = true; g_dumpUntil = GetTickCount() + 2000; g_pointEntries=0;
    DumpEvent("\n===== POINT DUMP =====");
    DumpEvent(Fmt("reason=%s frame=%d mode=%d telemetry=%d ent=%d mark=%d", reason, g_frame, g_mode, g_teleOn?1:0, g_entMirror?1:0, g_mark));
    if(zCView::modal) DumpEvent(Fmt("modal=zCView ptr=%p vtable=%p flags=0x%08X viewID=%d vrect=(%d,%d,%d,%d) prect=(%d,%d,%d,%d) pause=%d worldDepth=%d inMain=%d",
      zCView::modal,*(void**)zCView::modal,(unsigned)zCView::modal->flags,(int)zCView::modal->viewID,zCView::modal->vposx,zCView::modal->vposy,
      zCView::modal->vsizex,zCView::modal->vsizey,zCView::modal->pposx,zCView::modal->pposy,zCView::modal->psizex,zCView::modal->psizey,
      (ogame && ogame->IsOnPause())?1:0,g_worldDepth,g_inMainWorld?1:0));
    else DumpEvent(Fmt("modal=none pause=%d worldDepth=%d inMain=%d",(ogame&&ogame->IsOnPause())?1:0,g_worldDepth,g_inMainWorld?1:0));
    zCVob* fv = (player && ogame) ? player->GetFocusVob() : NULL;
    DumpEvent(Fmt("focus=%p class=%s visual=%s", fv, fv?ClassOf((zCObject*)fv).c_str():"none",
      (fv && fv->visual)?ClassOf((zCObject*)fv->visual).c_str():"none"));
    DumpEvent("point dump captures the next 2 seconds of plugin-level events; no D3D7 vtable patching");
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
    if (g_teleOn || g_dumpActive) {
      const char* raw=s.ToChar();
      if(raw && raw[0]) {
        static std::set<std::string> seen;
        std::string value(raw); if(value.size()>100) value.resize(100);
        if(seen.size()<300 && seen.insert(value).second) {
          std::string line=Fmt("UI_PRINT view=%p class=%s x=%d y=%d text=\"%s\"",self,
            "zCView",x,y,value.c_str());
          if(g_teleOn) Log(line); DumpEvent(line);
        }
      }
    }
    MaybeMirrorLabel(self, x, s);
    Ivk_View_Print(self, edx, x, y, s);
  }

  zCViewText* __fastcall Hook_View_CreateText3(zCView* self, void* edx, int x, int y, zSTRING const& s);
  CInvoke<zCViewText*(__fastcall*)(zCView*, void*, int, int, zSTRING const&)> Ivk_View_CreateText3(0x007AA2B0, &Hook_View_CreateText3);
  zCViewText* __fastcall Hook_View_CreateText3(zCView* self, void* edx, int x, int y, zSTRING const& s) {
    if(g_teleOn || g_dumpActive) {
      const char* raw=s.ToChar();
      if(raw && raw[0]) { std::string line=Fmt("UI_CREATE_TEXT view=%p x=%d y=%d text=\"%.90s\"",self,x,y,raw);
        if(g_teleOn) Log(line); DumpEvent(line); }
    }
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
    if (g_teleOn || g_dumpActive) {
      unsigned long long flags=(zCView::modal?1ULL:0ULL)|((ogame&&ogame->IsOnPause())?2ULL:0ULL);
      unsigned long long id=((unsigned long long)caller<<32)|((unsigned long long)key<<8)|flags;
      if (g_inputSeen.size()<2500 && g_inputSeen.insert(id).second) {
        std::string line=Fmt("INPUT mark=%d caller=0x%08lX key=%u modal=%s pause=%d",g_mark,(unsigned long)caller,(unsigned)key,
          zCView::modal?"zCView":"none",(ogame&&ogame->IsOnPause())?1:0);
        if(g_teleOn) Log(line); DumpEvent(line);
      }
    }
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

  // ---------- Телеметрия карты (хуки можно отключить через ZM_HOOK_MAP=0) ----------
#if ZM_HOOK_MAP
  // oCGame::OpenView / CloseView — переключение штатных игровых экранов
  int __fastcall Hook_Game_OpenView(void* self, void* edx, int viewId);
  CInvoke<int(__fastcall*)(void*,void*,int)> Ivk_Game_OpenView(0x006D3C00,&Hook_Game_OpenView);
  int __fastcall Hook_Game_OpenView(void* self, void* edx, int viewId) {
    std::string line=Fmt("GAME OpenView id=%d self=%p caller=%s frame=%d modal=%p",viewId,self,Hex((DWORD)_ReturnAddress()).c_str(),g_frame,zCView::modal);
    if(g_teleOn) Log(line); DumpEvent(line);
    int r=Ivk_Game_OpenView(self,edx,viewId);
    if(g_teleOn) Log(Fmt("GAME OpenView id=%d result=%d modalAfter=%p",viewId,r,zCView::modal));
    return r;
  }
  int __fastcall Hook_Game_CloseView(void* self, void* edx, int viewId);
  CInvoke<int(__fastcall*)(void*,void*,int)> Ivk_Game_CloseView(0x006D3C30,&Hook_Game_CloseView);
  int __fastcall Hook_Game_CloseView(void* self, void* edx, int viewId) {
    std::string line=Fmt("GAME CloseView id=%d self=%p caller=%s frame=%d modal=%p",viewId,self,Hex((DWORD)_ReturnAddress()).c_str(),g_frame,zCView::modal);
    if(g_teleOn) Log(line); DumpEvent(line);
    int r=Ivk_Game_CloseView(self,edx,viewId);
    if(g_teleOn) Log(Fmt("GAME CloseView id=%d result=%d modalAfter=%p",viewId,r,zCView::modal));
    return r;
  }

  // oCDocumentManager::Show / UpdateMap — карта и документы как отдельная подсистема
  void __fastcall Hook_DocManager_Show(void* self, int docId);
  CInvoke<void(__fastcall*)(void*,int)> Ivk_DocManager_Show(0x0065F300,&Hook_DocManager_Show);
  void __fastcall Hook_DocManager_Show(void* self, int docId) {
    oCDocumentManager* manager=(oCDocumentManager*)self;
    std::string line=Fmt("DOCUMENT Show id=%d manager=%p list=%p mainDialog=%p caller=%s frame=%d modal=%p",docId,self,
      manager->ListDocuments,manager->DlgDocumentMain,Hex((DWORD)_ReturnAddress()).c_str(),g_frame,zCView::modal);
    if(g_teleOn) Log(line); DumpEvent(line);
    Ivk_DocManager_Show(self,docId);
    line=Fmt("DOCUMENT Show END id=%d modal=%p",docId,zCView::modal);
    if(g_teleOn) Log(line); DumpEvent(line);
  }
  oCViewDocument* __fastcall Hook_DocManager_GetDocumentView(void* self, int docId);
  CInvoke<oCViewDocument*(__fastcall*)(void*,int)> Ivk_DocManager_GetDocumentView(0x0065F3D0,&Hook_DocManager_GetDocumentView);
  oCViewDocument* __fastcall Hook_DocManager_GetDocumentView(void* self, int docId) {
    oCViewDocument* view=Ivk_DocManager_GetDocumentView(self,docId);
    if(view) {
      std::string line=Fmt("DOCUMENT GetDocumentView id=%d view=%p vtable=%p pages=%d activated=%d done=%d",
        docId,view,*(void**)view,view->Pages,view->IsActivated,view->IsDone);
      if(g_teleOn) Log(line); DumpEvent(line);
    } else {
      std::string line=Fmt("DOCUMENT GetDocumentView id=%d view=NULL",docId);
      if(g_teleOn) Log(line); DumpEvent(line);
    }
    return view;
  }
  void __fastcall Hook_DocManager_UpdateMap(void* self, int mapId);
  CInvoke<void(__fastcall*)(void*,int)> Ivk_DocManager_UpdateMap(0x0065F400,&Hook_DocManager_UpdateMap);
  void __fastcall Hook_DocManager_UpdateMap(void* self, int mapId) {
    std::string line=Fmt("DOCUMENT UpdateMap id=%d manager=%p caller=%s frame=%d",mapId,self,Hex((DWORD)_ReturnAddress()).c_str(),g_frame);
    if(g_teleOn) Log(line); DumpEvent(line);
    Ivk_DocManager_UpdateMap(self,mapId);
  }

#endif // ZM_HOOK_MAP

#if ZM_HOOK_LEGACY_MAP
  // Эти два адреса/сигнатуры взяты из предыдущего экспериментального варианта;
  // при проблемах с открытием карты отключай только ZM_HOOK_LEGACY_MAP.
  int __fastcall Hook_MapScreen_Show(void* self, void* edx, int arg);
  CInvoke<int(__fastcall*)(void*,void*,int)> Ivk_MapScreen_Show(0x0047F9C0,&Hook_MapScreen_Show);
  int __fastcall Hook_MapScreen_Show(void* self, void* edx, int arg) {
    ++g_mapShowCount;
    std::string line=Fmt("MAP Show #%d BEGIN arg=%d self=%p caller=%s frame=%d modal=%s pause=%d",g_mapShowCount,arg,self,
      Hex((DWORD)_ReturnAddress()).c_str(),g_frame,zCView::modal?"zCView":"none",
      (ogame&&ogame->IsOnPause())?1:0);
    if(g_teleOn) Log(line); DumpEvent(line);
    int r=Ivk_MapScreen_Show(self,edx,arg);
    line=Fmt("MAP Show END result=%d frame=%d modal=%s",r,g_frame,zCView::modal?"zCView":"none");
    if(g_teleOn) Log(line); DumpEvent(line); return r;
  }
  void __fastcall Hook_MapView_UpdatePosition(zCView* self, void* edx);
  CInvoke<void(__fastcall*)(zCView*,void*)> Ivk_MapView_UpdatePosition(0x0068D7B0,&Hook_MapView_UpdatePosition);
  void __fastcall Hook_MapView_UpdatePosition(zCView* self, void* edx) {
    ++g_mapUpdateCount;
    if(g_teleOn && g_mapUpdateCount<=80) Log(Fmt("MAP UpdatePosition #%d self=%p vrect=(%d,%d,%d,%d) prect=(%d,%d,%d,%d) caller=%s frame=%d modal=%p",
      g_mapUpdateCount,self,self->vposx,self->vposy,self->vsizex,self->vsizey,self->pposx,self->pposy,self->psizex,self->psizey,
      Hex((DWORD)_ReturnAddress()).c_str(),g_frame,zCView::modal));
    DumpEvent(Fmt("MAP UpdatePosition #%d self=%p",g_mapUpdateCount,self));
    Ivk_MapView_UpdatePosition(self,edx);
  }
#endif // ZM_HOOK_LEGACY_MAP

  // ---------- Зеркалирование сущностей: эксперимент + локальная коррекция culling ----------
  bool IsNpcVob(zCVob* v) {
    if (!v) return false;
    zCClassDef* d=v->_GetClassDef();
    while(d) { const char* n=d->className.ToChar(); if(n && strcmp(n,"oCNpc")==0) return true; d=d->baseClassDef; }
    return false;
  }

  int __fastcall Hook_Vob_Render(zCVob* self, zTRenderContext& ctx);
  CInvoke<int(__fastcall*)(zCVob*, zTRenderContext&)> Ivk_Vob_Render(0x006015D0, &Hook_Vob_Render);
  int __fastcall Hook_Vob_Render(zCVob* self, zTRenderContext& ctx) {
    if(!g_entMirror && !g_teleOn && !g_dumpActive) return Ivk_Vob_Render(self,ctx);
    bool npc=IsNpcVob(self);
    if(!npc) return Ivk_Vob_Render(self,ctx);
    bool apply=g_entMirror;
    float orig[16];
    if(apply) {
      memcpy(orig,&self->trafoObjToWorld,sizeof(orig));
      float m[16]; memcpy(m,orig,sizeof(m)); m[0]=-m[0]; m[4]=-m[4]; m[8]=-m[8];
      memcpy(&self->trafoObjToWorld,m,sizeof(m));
    }
    zCModel* modelTrace=NULL;
    if (g_teleOn || g_dumpActive) {
      if (g_npcSeen.size()<512 && g_npcSeen.insert(self).second) {
        std::string visualName=self->visual?ClassOf((zCObject*)self->visual):"none";
        std::string line=Fmt("NPC_RENDER firstSeen ptr=%p class=%s visual=%s entityMirror=%d",self,
          ClassOf((zCObject*)self).c_str(),visualName.c_str(),g_entMirror?1:0);
        if(g_teleOn) Log(line); DumpEvent(line);
      }
      if(self->visual && ClassOf((zCObject*)self->visual)=="zCModel") modelTrace=(zCModel*)self->visual;
    }
    if(apply) {
      ++g_entityCullDepth;
      if (g_haveCull && zrenderer) Hook_SetRenderState((zCRnd_D3D*)zrenderer,NULL,D3DRENDERSTATE_CULLMODE,g_lastCull);
    }
    int r=Ivk_Vob_Render(self,ctx);
    if(apply) {
      --g_entityCullDepth;
      if (g_haveCull && zrenderer) Hook_SetRenderState((zCRnd_D3D*)zrenderer,NULL,D3DRENDERSTATE_CULLMODE,g_lastCull);
    }
    if(modelTrace && g_modelTraceSeen.size()<256 && g_modelTraceSeen.insert(modelTrace).second) {
      int count=modelTrace->nodeList.GetNumInList();
      std::string line=Fmt("NPC_MODEL_TRACE model=%p nodeCount=%d activeAnis=%d doVobRot=%d modelScale=(%.3f,%.3f,%.3f) aniTransScale=(%.3f,%.3f,%.3f) rootPosLocal=(%.3f,%.3f,%.3f) vobTrans=(%.3f,%.3f,%.3f) velocity=(%.3f,%.3f,%.3f)",
        modelTrace,count,modelTrace->numActiveAnis,modelTrace->doVobRot,
        ((float*)&modelTrace->modelScale)[0],((float*)&modelTrace->modelScale)[1],((float*)&modelTrace->modelScale)[2],
        ((float*)&modelTrace->aniTransScale)[0],((float*)&modelTrace->aniTransScale)[1],((float*)&modelTrace->aniTransScale)[2],
        ((float*)&modelTrace->rootPosLocal)[0],((float*)&modelTrace->rootPosLocal)[1],((float*)&modelTrace->rootPosLocal)[2],
        ((float*)&modelTrace->vobTrans)[0],((float*)&modelTrace->vobTrans)[1],((float*)&modelTrace->vobTrans)[2],
        ((float*)&modelTrace->modelVelocity)[0],((float*)&modelTrace->modelVelocity)[1],((float*)&modelTrace->modelVelocity)[2]);
      if(g_teleOn) Log(line); DumpEvent(line);
      for(int i=0;i<count && i<3;i++) {
        zCModelNodeInst* node=modelTrace->nodeList[i]; if(!node) continue;
        zMAT4& a=node->trafo; zMAT4& b=node->trafoObjToCam;
        line=Fmt("NPC_NODE[%d] node=%p activeNodeAnis=%d trafoR0=(%.3f,%.3f,%.3f,%.3f) trafoR1=(%.3f,%.3f,%.3f,%.3f) objToCamR0=(%.3f,%.3f,%.3f,%.3f) objToCamR1=(%.3f,%.3f,%.3f,%.3f)",i,node,node->numNodeAnis,
          a.v[0][0],a.v[0][1],a.v[0][2],a.v[0][3],a.v[1][0],a.v[1][1],a.v[1][2],a.v[1][3],
          b.v[0][0],b.v[0][1],b.v[0][2],b.v[0][3],b.v[1][0],b.v[1][1],b.v[1][2],b.v[1][3]);
        if(g_teleOn) Log(line); DumpEvent(line);
      }
    }
    if(apply) memcpy(&self->trafoObjToWorld,orig,sizeof(orig));
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
      if (g_teleOn) PerfFrameTick();
      if (g_teleOn || g_dumpActive) DumpEvent(Fmt("WORLD_RENDER BEGIN frame=%d mode=%d camera=%p",g_frame,g_mode,&cam));
    }

    Ivk_World_Render(self, edx, cam);

    g_worldDepth--;
    g_inMainWorld = prevInMain;
    if (main) {
      if (g_teleOn || g_dumpActive) DumpEvent(Fmt("WORLD_RENDER END frame=%d modal=%s",g_frame,zCView::modal?"zCView":"none"));
      if (MirrorOn() && (g_mode == 1 || g_mode == 2)) MirrorWorldFast();
      g_uiPhase = true;
    }
    else g_uiPhase = prevUi;
  }

  // ---------- События Union ----------
  void Game_Entry() {
    WriteText("zMirror_log.txt", "Game_Entry: zMirrorTest V1.5-diagnostic loaded\n", false);
    WriteText("zMirror_point.txt", "zMirror point-dump log\n", false);
    Log("GPUv8: projection COLUMN 0 reflection is enabled for this test build");
    Log(Fmt("map/document hooks=%d legacy map-view hooks=%d",ZM_HOOK_MAP,ZM_HOOK_LEGACY_MAP));
  }

  void Game_Init() { }
  void Game_Exit() { FinishPointDump(); ReleaseBltTmp(); }
  void Game_PreLoop() { }

  inline bool KeyEdge(int vk, bool& was) {
    bool down=(GetAsyncKeyState(vk)&0x8000)!=0; bool edge=down&&!was; was=down; return edge;
  }
  void Notify(const std::string& s) { if(screen) screen->PrintCXY(zSTRING(s.c_str())); }

  void Game_Loop() {
    static bool wasHome=false,wasEnd=false,wasRB=false,wasInsert=false,wasPg=false;
    if(KeyEdge(VK_HOME,wasHome)) {
      g_mode=(g_mode+1)%4; g_failStreak=0; g_bltBroken=false; g_bltFailures=0; g_bltVerifyDone=false; g_bltVerifyTries=0;
      g_msSum=0; g_msCount=0; PerfFrameReset(); SyncMouseFlip();
      const char* names[4]={"OFF","CPU","BLT","GPUv8"};
      Log(std::string("mirror mode: ")+names[g_mode]); Notify(std::string("MIRROR MODE: ")+names[g_mode]);
    }
    if(KeyEdge(VK_END,wasEnd)) {
      g_teleOn=!g_teleOn; Log(g_teleOn?"telemetry ON":"telemetry OFF"); Notify(g_teleOn?"TELEMETRY: ON":"TELEMETRY: OFF");
      if(g_teleOn) { g_inputSeen.clear(); g_cullSeen.clear(); g_modelTraceSeen.clear(); g_npcSeen.clear(); g_mapShowCount=0; g_mapUpdateCount=0; g_camLogCount=0; PerfFrameReset(); }
    }
    if(KeyEdge(VK_OEM_6,wasRB)) {
      g_entMirror=!g_entMirror; Log(std::string("entity mirror=")+(g_entMirror?"ON":"OFF"));
      Notify(g_entMirror?"ENTITY MIRROR: ON":"ENTITY MIRROR: OFF");
    }
    if(KeyEdge(VK_INSERT,wasInsert)) { BeginPointDump("INSERT"); Notify("POINT LOG: 2 seconds"); }
    if(KeyEdge(VK_PRIOR,wasPg)) {
      ++g_mark; g_inputSeen.clear();
      std::string m=Fmt("===== MARK %d frame=%d mode=%d ent=%d modal=%s pause=%d =====",g_mark,g_frame,g_mode,g_entMirror?1:0,
        zCView::modal?"zCView":"none",(ogame&&ogame->IsOnPause())?1:0);
      Log(m); DumpEvent(m); Notify(Fmt("MARK %d",g_mark));
    }
    static zCView* lastModal=NULL; static int lastPause=-1; static zCVob* lastFocus=NULL;
    zCView* modal=zCView::modal; int pause=(ogame&&ogame->IsOnPause())?1:0;
    if(modal!=lastModal) {
      std::string line=modal ? Fmt("UI modal OPEN type=zCView ptr=%p vtable=%p flags=0x%08X viewID=%d vrect=(%d,%d,%d,%d) mark=%d frame=%d",
        modal,*(void**)modal,(unsigned)modal->flags,(int)modal->viewID,modal->vposx,modal->vposy,modal->vsizex,modal->vsizey,g_mark,g_frame)
        : Fmt("UI modal CLOSE mark=%d frame=%d",g_mark,g_frame);
      if(g_teleOn) Log(line); DumpEvent(line); lastModal=modal;
    }
    if(pause!=lastPause) {
      std::string line=Fmt("UI pause=%d mark=%d frame=%d",pause,g_mark,g_frame);
      if(g_teleOn && lastPause!=-1) Log(line); DumpEvent(line); lastPause=pause;
    }
    zCVob* focus=(player&&ogame)?player->GetFocusVob():NULL;
    if(focus!=lastFocus) {
      std::string line=Fmt("FOCUS change ptr=%p class=%s visual=%s frame=%d",focus,focus?ClassOf((zCObject*)focus).c_str():"none",
        (focus&&focus->visual)?ClassOf((zCObject*)focus->visual).c_str():"none",g_frame);
      if(g_teleOn) Log(line); DumpEvent(line); lastFocus=focus;
    }
    if(g_dumpActive && (LONG)(GetTickCount()-g_dumpUntil)>=0) FinishPointDump();
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
