// ═══════════════════════════════════════════════════════════════════
// zMirrorTest V1.7.4
// Плагин зеркального режима для Gothic 2 NotR (Union SDK, G2A).
//
// УПРАВЛЕНИЕ:
//   HOME   — зеркало OFF <-> GPU (бэкенд авто: ванилла = FLIP, GD3D11 = PROJ/FLIP11).
//   END    — телеметрия ON/OFF (PERF-замеры в zMirror_log.txt).
//   [      — метод зеркала на GD3D11: PROJ <-> FLIP11.
//              PROJ    — отражение проекции мира + коррекция culling на D3D7-слое.
//              FLIP11  — GPU-FLIP мира на границе zCWorld::Render, до HUD/UI;
//                        использует D3D7-совместимый GPU-путь GD3D11.
//   ]      — диагностический тест отсечения для GD3D11 (PROJ): OFF <-> CULLNONE.
//            Не переключает FrontCounterClockwise: PROJ уже инвертирует D3D7 CULLMODE.
//
// ЧТО ИЗМЕНИЛОСЬ ПО СРАВНЕНИЮ С V1.6.0:
//   - Карта: режим FINAL убран. Всегда SELECT (только квады карты в DrawPolySimple:
//     картинка + маячок). HUD не затрагивается, доп. прохода нет, работает на обоих
//     рендерах. Телеметрия карты и дамп PgUp убраны.
//   - D3D11 vtable offsets исправлены по фактическому порядку методов COM:
//     Context RSSetState=43, RSSetViewports=44, OMSetRenderTargets=33,
//     Draw=13, CopyResource=47; Device CreatePixelShader=15,
//     CreateRasterizerState=22, CreateSamplerState=23, GetImmediateContext=40;
//     RasterizerState::GetDesc=7, Texture2D::GetDesc=10.
//     Прежние смещения вызывали/перехватывали другие методы с несовместимыми
//     сигнатурами — это и объясняет чёрный экран и вылеты.
//       * ранний хук экспортов D3D11CreateDevice/D3D11CreateDeviceAndSwapChain
//         (инлайн-патч) — если устройство создаётся после загрузки плагина;
//       * fallback: «пробный» свопчейн на скрытом окне -> хук IDXGISwapChain::Present
//         (слот 8) -> первый чужой Present выдаёт устройство/контекст GD3D11.
//   - FLIP11 перенесён с IDXGISwapChain::Present на границу zCWorld::Render:
//     используется проверенный GPU-FLIP quad из ванильного пути через D3D7-совместимый
//     интерфейс GD3D11. Проходит после мира и до HUD, поэтому UI не зеркалится.
//   - ] переведена на CULLNONE-диагностику, чтобы не делать вторую инверсию winding.
//   - Все используемые D3D11 vtable offsets сверены с порядком методов интерфейсов.
//
// РАБОТАЕТ (проверено):
//   - GPU-FLIP (ванилла): переворот готового кадра на GPU без потерь FPS.
//   - Карта SELECT на обоих рендерах.
//   - Своп LR-клавиш/мыши, надписи фокуса, анти-своп в модалках и взломе.
// ═══════════════════════════════════════════════════════════════════

#include "resource.h"
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <intrin.h>
#include <thread>

namespace GOTHIC_ENGINE {

// ───────────────────────── Запись логов ─────────────────────────
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

std::string SFmt(const char* fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, ap);
    va_end(ap);
    return std::string(b);
}

int g_logLines = 0;
void Log(const std::string& s) {
    if (g_logLines++ < 8000) WriteText("zMirror_log.txt", s + "\n", true);
}

std::string Hex(unsigned long v) {
    char b[32]; sprintf_s(b, sizeof(b), "0x%08lX", v); return b;
}

// ───────────────────────── Состояние ─────────────────────────
enum { MODE_OFF = 0, MODE_GPU = 1, MODE_COUNT = 2 };
const char* const kModeNames[MODE_COUNT] = { "OFF", "GPU" };

int  g_mode = MODE_OFF;
bool g_teleOn = false;         // телеметрия (PERF)
int  g_failStreak = 0;
int  g_frame = 0;
bool g_inMainWorld = false;
bool g_focusLockable = false;
bool g_uiPhase = false;
int  g_worldDepth = 0;

inline bool MirrorOn() { return g_mode != MODE_OFF; }

// ───────────────────────── Бэкенды GPU-режима ─────────────────────────
enum { BK_AUTO = 0, BK_FLIP = 1, BK_PROJ = 2, BK_COUNT = 3 };
const char* const kBackendNames[BK_COUNT] = { "AUTO", "FLIP", "PROJ" };
int  g_backendPref = BK_AUTO;
bool g_wrapperChecked = false;
bool g_wrapperDetected = false;   // ddraw.dll загружен не из папки Windows (GD3D11/dgVoodoo/DDrawCompat)

void DetectWrapper() {
    if (g_wrapperChecked) return;
    g_wrapperChecked = true;
    char path[MAX_PATH] = {0}, win[MAX_PATH] = {0};
    HMODULE h = GetModuleHandleA("ddraw.dll");
    if (h) GetModuleFileNameA(h, path, MAX_PATH);
    GetWindowsDirectoryA(win, MAX_PATH);
    bool fromWin = path[0] && win[0] && _strnicmp(path, win, strlen(win)) == 0;
    g_wrapperDetected = (path[0] != 0) && !fromWin;
    Log(SFmt("DETECT ddraw.dll path='%s' windowsDir='%s' wrapper=%d", path, win, g_wrapperDetected ? 1 : 0));
}

// true: GPU работает как FLIP (переворот кадра), false: как PROJ/FLIP11
inline bool GpuUsesFlip() {
    DetectWrapper();
    return g_backendPref == BK_FLIP || (g_backendPref == BK_AUTO && !g_wrapperDetected);
}
// true: активный режим переворачивает готовый кадр (GPU-FLIP, ванилла)
inline bool FlipModeActive() { return g_mode == MODE_GPU && GpuUsesFlip(); }

// Метод зеркала на GD3D11 ("[")
enum { METH_PROJ = 0, METH_SCFLIP = 1, METH_COUNT = 2 };
const char* const kMethodNames[METH_COUNT] = { "PROJ", "FLIP11" };
int g_method = METH_PROJ;

inline bool IsScreenFlipActive() {
    return g_mode == MODE_GPU && !GpuUsesFlip() && g_method == METH_SCFLIP;
}

// Карта: выборочный режим (квады карты в DrawPolySimple) — всегда включён
bool g_frameMapOpen = false;    // карта открыта в этом кадре
bool g_mapPageSeen = false;     // картинка карты уже встречена в этом кадре
float g_mapL = 0, g_mapR = 0;   // рамка картинки карты по X (пиксели)
int   g_selCand = 0;            // кандидаты (квады карты) в этом кадре
int   g_selApplied = 0;          // зеркалено квадов (для PERF)

// Тест намотки GD3D11 ("]")
enum { PT_OFF = 0, PT_CULLNONE = 1, PT_COUNT = 2 };
const char* const kPolyTestNames[PT_COUNT] = { "OFF", "CULLNONE" };
int  g_polyTest = PT_OFF;
int  g_rsCalls = 0, g_rsSubst = 0, g_rsInstances = 0;
bool g_d11Ok = false;      // D3D11: хук контекста установлен (для PERF)

void SyncMouseFlip();      // forward (определена ниже)

// ───────────────────────── Карта: ранние глобалы детекта ─────────────────────────
#ifndef ZM_HOOK_MAP
#define ZM_HOOK_MAP 1
#endif

zCView* g_mapView = NULL;   // последний открытый oCViewDocumentMap
bool g_mapDocIsMap = false;     // последний показанный документ — карта
bool g_mapUpdInShow = false;    // UpdatePosition вызван внутри Show()
int  g_docShowDepth = 0;
int  g_mapUpdFrame = -100;      // кадр последнего UpdatePosition

// ───────────────────────── GPU-FLIP: состояние ─────────────────────────
const int kCalN = 32;

struct GpuFlipState {
    IDirectDrawSurface7* tex;
    DWORD texW, texH;     // размер текстуры (степень двойки)
    DWORD rtW, rtH;       // размер RT
    int state;            // 0 = калибровка, 1 = ок, -1 = отказ (CPU-фолбэк)
    int calTries;
    int adjX, adjY;       // подстройка UV в пикселях
    long framesOk;
    int initFails;
};
GpuFlipState g_gf = {};
int g_calX[kCalN], g_calY[kCalN];
DWORD g_calOld[kCalN][9];

void GfRelease() {
    if (g_gf.tex) { g_gf.tex->Release(); g_gf.tex = NULL; }
}

void GfReset() {
    GfRelease();
    memset(&g_gf, 0, sizeof(g_gf));
}

DWORD Pow2Ceil(DWORD v) { DWORD p = 1; while (p < v) p <<= 1; return p; }

IDirectDrawSurface7* AcquireTarget() {
    IDirectDrawSurface7* rt = NULL;
    IDirect3DDevice7* dev = zCRnd_D3D::xd3d_pd3dDevice7;
    if (dev && SUCCEEDED(dev->GetRenderTarget(&rt)) && rt) return rt;
    return NULL;
}

// ───────────────────────── Замер FPS / стоимости зеркала ─────────────────────────
struct PerfStat {
    LARGE_INTEGER last;
    double frameSum; int frames;
    double mirrorSum; int mirrorN;
};
PerfStat g_perf = {};
int g_psMirrored = 0;      // сколько вершин DrawPolySimple отзеркалено (GPU-PROJ)
int g_vbTlMirrored = 0;    // сколько TL-вершин DrawVertexBuffer отзеркалено (GPU-PROJ)

void PerfReset() {
    memset(&g_perf, 0, sizeof(g_perf));
    g_psMirrored = 0; g_vbTlMirrored = 0;
}

void PerfFrame() {
    LARGE_INTEGER now, f;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    if (g_perf.last.QuadPart != 0) {
        double ms = (double)(now.QuadPart - g_perf.last.QuadPart) * 1000.0 / (double)f.QuadPart;
        if (ms < 250.0) { g_perf.frameSum += ms; g_perf.frames++; }
    }
    g_perf.last = now;
    if (g_perf.frames >= 300) {
        double avg = g_perf.frameSum / g_perf.frames;
        double am = g_perf.mirrorN ? g_perf.mirrorSum / g_perf.mirrorN : 0.0;
        Log(SFmt("PERF mode=%s backend=%s method=%s frames=%d avgFrame=%.2f ms (%.1f fps) avgMirrorCall=%.2f ms psMirrored=%d vbTlMirrored=%d gf(state=%d ok=%ld) poly(test=%s d11(hook=%d calls=%d subst=%d inst=%d)",
            kModeNames[g_mode], g_mode == MODE_GPU ? (GpuUsesFlip() ? "FLIP" : "PROJ") : "-",
            g_mode == MODE_GPU && !GpuUsesFlip() ? kMethodNames[g_method] : "-",
            g_perf.frames, avg, 1000.0 / avg, am,
            g_psMirrored, g_vbTlMirrored, g_gf.state, g_gf.framesOk,
            kPolyTestNames[g_polyTest], g_d11Ok ? 1 : 0, g_rsCalls, g_rsSubst, g_rsInstances));
        g_perf.frameSum = 0; g_perf.frames = 0; g_perf.mirrorSum = 0; g_perf.mirrorN = 0;
        g_psMirrored = 0; g_vbTlMirrored = 0; g_selApplied = 0; g_rsCalls = 0; g_rsSubst = 0;
    }
}

// ───────────────────────── Быстрый переворот строк (SSE2) — CPU-фолбэк ─────────────────────────
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

// ───────────────────────── D3D7-устройство: хуки ядра PROJ ─────────────────────────
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetTransform)(IDirect3DDevice7*, D3DTRANSFORMSTATETYPE, LPD3DMATRIX);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetViewport)(IDirect3DDevice7*, D3DVIEWPORT7*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetRenderState)(IDirect3DDevice7*, D3DRENDERSTATETYPE, DWORD);

PFN_SetTransform          g_oSetTransform = NULL;
PFN_SetViewport           g_oSetViewport = NULL;
PFN_SetRenderState        g_oSetRS = NULL;

bool g_devHooked = false;
DWORD g_engCull = D3DCULL_CCW;        // последнее значение CULLMODE от движка
D3DMATRIX g_engProj;                  // последняя проекция от движка
bool g_haveEngProj = false;
D3DVIEWPORT7 g_vp;                    // последний вьюпорт
bool g_haveVp = false;

bool g_devWorldMirror = false;        // GPU-PROJ: идёт рендер главного мира в зеркале
bool g_worldCullFlip = false;         // паритет отражений на уровне мира

inline DWORD InvertCull(DWORD v) {
    if (v == D3DCULL_CW) return D3DCULL_CCW;
    if (v == D3DCULL_CCW) return D3DCULL_CW;
    return v;
}

inline void MirrorProjCol0(D3DMATRIX& p) {   // D3D: v' = v * M, столбец 0 формирует x'
    p._11 = -p._11; p._21 = -p._21; p._31 = -p._31; p._41 = -p._41;
}

void SyncCull() {
    IDirect3DDevice7* dev = zCRnd_D3D::xd3d_pd3dDevice7;
    if (!dev || !g_oSetRS) return;
    g_oSetRS(dev, D3DRENDERSTATE_CULLMODE, g_worldCullFlip ? InvertCull(g_engCull) : g_engCull);
}

void SyncProj() {
    IDirect3DDevice7* dev = zCRnd_D3D::xd3d_pd3dDevice7;
    if (!dev || !g_oSetTransform || !g_haveEngProj) return;
    D3DMATRIX p = g_engProj;
    if (g_devWorldMirror) MirrorProjCol0(p);
    g_oSetTransform(dev, D3DTRANSFORMSTATE_PROJECTION, &p);
}

HRESULT STDMETHODCALLTYPE Hook_Dev_SetViewport(IDirect3DDevice7* self, D3DVIEWPORT7* vp) {
    if (vp) { g_vp = *vp; g_haveVp = true; }
    return g_oSetViewport(self, vp);
}

HRESULT STDMETHODCALLTYPE Hook_Dev_SetTransform(IDirect3DDevice7* self, D3DTRANSFORMSTATETYPE type, LPD3DMATRIX m) {
    if (m && (DWORD)type == 3) {   // D3DTRANSFORMSTATE_PROJECTION
        g_engProj = *m; g_haveEngProj = true;
        if (g_devWorldMirror) {
            D3DMATRIX p = *m;
            MirrorProjCol0(p);
            return g_oSetTransform(self, type, &p);
        }
        return g_oSetTransform(self, type, m);
    }
    return g_oSetTransform(self, type, m);
}

HRESULT STDMETHODCALLTYPE Hook_Dev_SetRenderState(IDirect3DDevice7* self, D3DRENDERSTATETYPE t, DWORD v) {
    if (t == D3DRENDERSTATE_CULLMODE) {
        g_engCull = v;
        if (g_worldCullFlip) v = InvertCull(v);
    }
    return g_oSetRS(self, t, v);
}

template <class F>
bool PatchSlot(void** vt, int idx, F hook, F& orig) {
    DWORD oldp = 0;
    if (!VirtualProtect(&vt[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldp)) return false;
    F cur = reinterpret_cast<F>(vt[idx]);
    if (cur != hook) { orig = cur; vt[idx] = reinterpret_cast<void*>(hook); }
    DWORD tmp = 0;
    VirtualProtect(&vt[idx], sizeof(void*), oldp, &tmp);
    return orig != NULL;
}

// Устанавливает vtable-хуки на D3D7-устройство. Один раз за сессию.
bool EnsureDeviceHooks() {
    if (g_devHooked) return true;
    IDirect3DDevice7* dev = zCRnd_D3D::xd3d_pd3dDevice7;
    if (!dev) return false;

    DWORD c = D3DCULL_CCW;
    if (SUCCEEDED(dev->GetRenderState(D3DRENDERSTATE_CULLMODE, &c))) g_engCull = c;
    if (SUCCEEDED(dev->GetViewport(&g_vp))) g_haveVp = true;
    D3DMATRIX p;
    if (SUCCEEDED(dev->GetTransform(D3DTRANSFORMSTATE_PROJECTION, &p))) { g_engProj = p; g_haveEngProj = true; }

    void** vt = *(void***)dev;
    bool ok = true;
    ok = PatchSlot(vt, 11, &Hook_Dev_SetTransform,          g_oSetTransform) && ok;
    ok = PatchSlot(vt, 13, &Hook_Dev_SetViewport,           g_oSetViewport) && ok;
    ok = PatchSlot(vt, 20, &Hook_Dev_SetRenderState,        g_oSetRS) && ok;

    g_devHooked = ok;
    Log(ok ? SFmt("device hooks installed (cull=%lu, viewport %lux%lu, slots SetTransform11/SetViewport13/SetRenderState20)",
        (unsigned long)g_engCull, (unsigned long)g_vp.dwWidth, (unsigned long)g_vp.dwHeight)
           : std::string("device hooks: FAILED (see slots)"));
    return ok;
}

// ───────────────────────── GD3D11: доступ к настоящему контексту ─────────────────────────
// V1.6.0 хукал «пробный» контекст через ПРАВИЛЬНЫЕ слоты 43/44,
// но это был не контекст GD3D11. В V1.7.1 при переносе hook на реальный
// контекст номера слотов ошибочно заменили на 23/21 — из-за этого подменялись
// GSSetShader и DrawInstanced несовместимыми обработчиками.
// Теперь получаем НАСТОЯЩИЙ контекст двумя путями:
//   1) ранний инлайн-хук экспортов D3D11CreateDevice/D3D11CreateDeviceAndSwapChain
//      (если устройство создаётся после загрузки плагина);
//   2) fallback: «пробный» свопчейн на скрытом окне -> хук IDXGISwapChain::Present
//      (слот 8) -> первый чужой Present принадлежит свопчейну GD3D11 -> через него
//      достаём устройство/контекст.

struct ZmD3D11 {
    bool tried, ok;
    bool hookedCtx;
    void* dev;   // ID3D11Device*
    void* ctx;   // ID3D11DeviceContext* (immediate)
    void* sc;    // IDXGISwapChain* (GD3D11)
};
ZmD3D11 g_d11 = {};

// --- vtable-вспомогательные ---
inline void** Vt(void* o) { return *(void***)o; }

// --- инлайн-хук (детур) для экспортов d3d11.dll ---
struct ZmNearJump {
    void* site; BYTE orig[24]; void* tramp; bool installed;
};

BYTE* ZmAllocNear(void* site, DWORD size) {
    BYTE* base = (BYTE*)((DWORD_PTR)site & ~(DWORD_PTR)0xFFFFF);
    long best = 0;
    for (long pass = 0; pass < 4000; pass++) {
        BYTE* cand = (pass & 1) ? base - ((pass >> 1) + 1) * 0x10000
                                : base + ((pass >> 1) + 1) * 0x10000;
        void* p = VirtualAlloc(cand, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) {
            INT64 delta = (INT64)((BYTE*)p - (BYTE*)site);
            if (delta > -0x7FF00000LL && delta < 0x7FF00000LL) return (BYTE*)p;
            VirtualFree(p, 0, MEM_RELEASE);
            if (++best > 64) break;
        }
    }
    return NULL;
}

// Длина инструкции (только нужный минимум для прологов MSVC). 0 = неизвестно.
unsigned ZmInsnLen(const BYTE* p) {
    const BYTE* s = p;
    while (*p == 0xF0 || *p == 0xF2 || *p == 0xF3 || *p == 0x2E || *p == 0x36 ||
           *p == 0x3E || *p == 0x26 || *p == 0x64 || *p == 0x65 || *p == 0x66 || *p == 0x67) p++;
    BYTE op = *p;
    if (op == 0x0F) {
        BYTE op2 = p[1];
        if ((op2 >= 0x80 && op2 <= 0x8F)) return (unsigned)(p - s) + 6;
        if (op2 == 0x05 || op2 == 0x31 || op2 == 0x34 || op2 == 0x35 || op2 == 0x3F || op2 == 0xAE || op2 == 0xB0) return (unsigned)(p - s) + 3;
        unsigned m = 3;
        BYTE modrm = p[2];
        if ((modrm & 0xC0) == 0x40) m += 1;
        else if ((modrm & 0xC0) == 0x80) m += 4;
        else if ((modrm & 0xC0) == 0x00 && (modrm & 0x07) == 0x05) m += 4;
        return (unsigned)(p - s) + m;
    }
    if (op >= 0x50 && op <= 0x5F) return (unsigned)(p - s) + 1;      // push/pop
    if (op >= 0xB0 && op <= 0xB7) return (unsigned)(p - s) + 2;      // mov reg8, imm8
    if (op >= 0xB8 && op <= 0xBF) return (unsigned)(p - s) + 5;      // mov reg32, imm32
    if (op == 0x68 || op == 0xA1 || op == 0xA9 || op == 0x25 || op == 0x0D || op == 0x35 || op == 0x3D || op == 0xE8 || op == 0xE9) return (unsigned)(p - s) + 5;
    if (op == 0x6A || op == 0xEB || (op >= 0x70 && op <= 0x7F) || op == 0xCD) return (unsigned)(p - s) + 2;
    if (op == 0x9C || op == 0x9D || op == 0x9E || op == 0x9F || op == 0xCC || op == 0xC3 || op == 0xC9 || op == 0x90) return (unsigned)(p - s) + 1;
    if (op == 0x8B || op == 0x89 || op == 0x88 || op == 0x8A || op == 0x8D || op == 0x8F ||
        op == 0xC7 || op == 0xC6 || op == 0x80 || op == 0x81 || op == 0x83 || op == 0xFF ||
        op == 0xF6 || op == 0xF7 || op == 0xFE || (op & 0xC7) == 0x84 || (op & 0xC7) == 0x85) {
        unsigned m = 2;
        BYTE modrm = p[1];
        if ((modrm & 0xC0) == 0x40) m += 1;
        else if ((modrm & 0xC0) == 0x80) m += 4;
        else if ((modrm & 0xC0) == 0x00 && (modrm & 0x07) == 0x05) m += 4;
        if (op == 0xC7 || op == 0xC6 || op == 0x80 || op == 0x81 || op == 0x83) m += (op == 0xC6 || op == 0x80 || op == 0x83) ? 1 : 4;
        return (unsigned)(p - s) + m;
    }
    return 0;
}

bool ZmInstallJump(void* site, void* hook, ZmNearJump& nj) {
    memset(&nj, 0, sizeof(nj));
    BYTE* s = (BYTE*)site;
    if (!s) return false;
    if (s[0] == 0xC3 || s[0] == 0xEB) return false;   // ret/jmp — не трогаем
    int off = 0;
    while (off < 5) {
        unsigned L = ZmInsnLen(s + off);
        if (!L || off + (int)L > 24) return false;      // не смогли разобрать пролог
        off += (int)L;
    }
    BYTE* tr = ZmAllocNear(site, 40);
    if (!tr) return false;
    memcpy(nj.orig, s, off);
    memcpy(tr, s, off);                    // трэмполин: оригинальные байты
    tr[off] = 0xE9;                         // jmp обратно на site+off
    *(INT*)(tr + off + 1) = (INT)((BYTE*)s + off - (tr + off + 5));
    DWORD old = 0;
    VirtualProtect(s, 8, PAGE_EXECUTE_READWRITE, &old);
    s[0] = 0xE9;                            // site: jmp hook
    *(INT*)(s + 1) = (INT)((BYTE*)hook - (s + 5));
    VirtualProtect(s, 8, old, &old);
    nj.site = site; nj.tramp = tr; nj.installed = true;
    return true;
}

typedef HRESULT (WINAPI *PFN_CD_T)(void*, UINT, HMODULE, UINT, const UINT*, UINT, UINT, void**, UINT*, void**);
typedef HRESULT (WINAPI *PFN_CDASC_T)(void*, UINT, HMODULE, UINT, const UINT*, UINT, UINT, const void*, void**, UINT*, void**, void**);
ZmNearJump g_njCD, g_njCDASC;

void ZmInstallCtxHooks();
void ZmCaptureDevice(void** ppDev, void** ppCtx);
void* g_probeDev = NULL;   // пробное устройство D3D11 (игнорируем в захвате)

HRESULT WINAPI Hook_CD(void* a, UINT b, HMODULE c, UINT d, const UINT* e, UINT f, UINT g, void** h, UINT* i, void** j) {
    HRESULT hr = ((PFN_CD_T)g_njCD.tramp)(a, b, c, d, e, f, g, h, i, j);
    if (SUCCEEDED(hr) && h && j) ZmCaptureDevice(h, j);
    return hr;
}

HRESULT WINAPI Hook_CDASC(void* a, UINT b, HMODULE c, UINT d, const UINT* e, UINT f, UINT g, const void* h, void** i, UINT* j, void** k, void** l) {
    HRESULT hr = ((PFN_CDASC_T)g_njCDASC.tramp)(a, b, c, d, e, f, g, h, i, j, k, l);
    if (SUCCEEDED(hr) && i && k) ZmCaptureDevice(i, k);
    return hr;
}

void ZmInstallEarlyHooks() {
    HMODULE h = LoadLibraryA("d3d11.dll");
    if (!h) return;
    FARPROC cd = GetProcAddress(h, "D3D11CreateDevice");
    FARPROC cdasc = GetProcAddress(h, "D3D11CreateDeviceAndSwapChain");
    if (cd) ZmInstallJump((void*)cd, (void*)Hook_CD, g_njCD);
    if (cdasc) ZmInstallJump((void*)cdasc, (void*)Hook_CDASC, g_njCDASC);
    Log(SFmt("D3D11: early export hooks %s/%s", cd ? "installed" : "absent", cdasc ? "installed" : "absent"));
}

// --- vtable ID3D11DeviceContext / ID3D11Device / IDXGI (слоты, индексация с нуля) ---
// ID3D11DeviceContext: PSSetShaderResources=8, PSSetShader=9, PSSetSamplers=10,
//   VSSetShader=11, Draw=13, GSSetShader=23, IASetPrimitiveTopology=24,
//   OMSetRenderTargets=33, OMSetBlendState=35, OMSetDepthStencilState=36,
//   RSSetState=43, RSSetViewports=44, CopyResource=47.
// ID3D11Device: CreateTexture2D=5, CreateShaderResourceView=7,
//   CreateRenderTargetView=9, CreateVertexShader=12, CreatePixelShader=15,
//   CreateRasterizerState=22, CreateSamplerState=23, GetImmediateContext=40.
// ID3D11DeviceChild: GetDevice=3; ID3D11RasterizerState::GetDesc=7.
// ID3D11Texture2D::GetDesc=10; IDXGISwapChain::Present=8, GetBuffer=9.
// IDXGIFactory2::CreateSwapChainForHwnd=15 (IDXGIFactory1::IsCurrent=13).

struct ZmRastDesc { int Fill, Cull, FrontCCW, DepthBias; float DepthBiasClamp, SlopeBias; int DepthClip, Scissor, Multisample, AALine; };
typedef void (STDMETHODCALLTYPE *PFN_RSSetState)(void*, void*);
typedef void (STDMETHODCALLTYPE *PFN_RSSetViewports)(void*, UINT, const float*);
typedef void (STDMETHODCALLTYPE *PFN_RsGetDesc)(void*, ZmRastDesc*);
typedef void (STDMETHODCALLTYPE *PFN_GetDev)(void*, void**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateRS)(void*, const ZmRastDesc*, void**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetImmCtx)(void*, void**);
typedef ULONG (STDMETHODCALLTYPE *PFN_Rel)(void*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present_T)(void*, UINT, UINT);

PFN_RSSetState     g_oRSSetState = NULL;
PFN_RSSetViewports g_oRSSetVP = NULL;
std::map<size_t, void*> g_rsCache;
float g_rsVpW = 0.0f;
void* g_rsLastOrig = NULL;

bool g_rsHaveOrig = false;

void* RsVariant(void* st, int kind) {   // kind 1 = инверт FrontCounterClockwise, 2 = CullMode NONE
    if (!st) return st;
    size_t key = ((size_t)st << 2) | (size_t)kind;
    std::map<size_t, void*>::iterator it = g_rsCache.find(key);
    if (it != g_rsCache.end()) return it->second;
    void* res = st;
    ZmRastDesc d; memset(&d, 0, sizeof(d));
    ((PFN_RsGetDesc)Vt(st)[7])(st, &d);            // ID3D11RasterizerState::GetDesc = 7
    if (kind == 1) d.FrontCCW = d.FrontCCW ? 0 : 1; else d.Cull = 1;   // D3D11_CULL_NONE = 1
    void* dev = NULL;
    ((PFN_GetDev)Vt(st)[3])(st, &dev);             // GetDevice = 3
    if (dev) {
        void* ns = NULL;
        if (SUCCEEDED(((PFN_CreateRS)Vt(dev)[22])(dev, &d, &ns)) && ns) { res = ns; g_rsInstances++; }
        ((PFN_Rel)Vt(dev)[2])(dev);
    }
    g_rsCache[key] = res;
    return res;
}

inline bool RsSubstActive() {
    // PROJ уже меняет D3D7 CULLMODE. Единственная дополнительная диагностика —
    // временно отключить culling, не инвертируя FrontCounterClockwise второй раз.
    return g_polyTest == PT_CULLNONE && g_devWorldMirror;
}

void RsReapply() {   // переустановить текущий растеризатор (смена области действия)
    if (!g_d11.ctx || !g_oRSSetState || !g_rsHaveOrig) return;
    void* st = g_rsLastOrig;
    if (RsSubstActive()) st = RsVariant(st, g_polyTest == PT_CULLNONE ? 2 : 1);
    g_oRSSetState(g_d11.ctx, st);
}

void STDMETHODCALLTYPE Hook_RSSetState(void* ctx, void* st) {
    g_rsCalls++;
    // SetRasterizerState не даёт плагину права хранить входной COM-указатель
    // после возврата. Удерживаем оригинал, который понадобится при смене режима.
    if (st != g_rsLastOrig) {
        if (st) ((ULONG (STDMETHODCALLTYPE *)(void*))Vt(st)[1])(st); // AddRef
        if (g_rsLastOrig) ((PFN_Rel)Vt(g_rsLastOrig)[2])(g_rsLastOrig); // Release
        g_rsLastOrig = st;
    }
    g_rsHaveOrig = true;
    if (RsSubstActive()) { st = RsVariant(st, g_polyTest == PT_CULLNONE ? 2 : 1); g_rsSubst++; }
    g_oRSSetState(ctx, st);
}

void STDMETHODCALLTYPE Hook_RSSetViewports(void* ctx, UINT n, const float* vp) {
    if (n && vp) g_rsVpW = vp[2];   // D3D11_VIEWPORT: TopLeftX, TopLeftY, Width, Height, MinDepth, MaxDepth
    g_oRSSetVP(ctx, n, vp);
}

void ZmInstallCtxHooks() {
    if (g_d11.hookedCtx || !g_d11.ctx) return;
    void** vt = Vt(g_d11.ctx);
    bool ok = PatchSlot(vt, 43, &Hook_RSSetState, g_oRSSetState);
    ok = PatchSlot(vt, 44, &Hook_RSSetViewports, g_oRSSetVP) && ok;
    g_d11.hookedCtx = ok;
    g_d11Ok = ok;
    Log(ok ? "D3D11: real context vtable hooked (RSSetState=43, RSSetViewports=44)"
           : "D3D11: real context vtable hook FAILED");
}

void ZmCaptureDevice(void** ppDev, void** ppCtx) {
    if (!ppDev || !ppCtx || g_d11.ok) return;
    if (g_probeDev && g_probeDev == *ppDev) return;   // пробное устройство — не настоящее
    g_d11.dev = *ppDev; g_d11.ctx = *ppCtx;
    g_d11.ok = true;
    Log("D3D11: device captured via creation hook");
    ZmInstallCtxHooks();
}

// --- пробный свопчейн (fallback) ---
void* g_probeSC = NULL;
void* g_vtProbeF = NULL;   // vtable FLIP-пробника
void* g_vtProbeL = NULL;   // vtable legacy-пробника
PFN_Present_T g_oPresentF = NULL;
PFN_Present_T g_oPresentL = NULL;

void ZmFlipScDo(void* sc);   // legacy Present-pass reference; intentionally not called

HRESULT STDMETHODCALLTYPE Hook_Present(void* sc, UINT sync, UINT flags) {
    PFN_Present_T orig = g_oPresentL ? g_oPresentL : g_oPresentF;
    if (g_oPresentF && g_oPresentL) orig = (Vt(sc) == g_vtProbeF) ? g_oPresentF : g_oPresentL;

    // Устройство может быть захвачено ранним D3D11CreateDevice hook ещё до
    // появления swap chain. Поэтому ищем swap chain даже при g_d11.ok == true.
    if (sc && sc != g_probeSC && (!g_d11.ok || !g_d11.sc)) {
        static const GUID IID_T2D = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };
        typedef HRESULT (STDMETHODCALLTYPE *PFN_GetBuffer)(void*, UINT, const GUID*, void**);
        void* tex = NULL;
        if (SUCCEEDED(((PFN_GetBuffer)Vt(sc)[9])(sc, 0, &IID_T2D, &tex)) && tex) {
            void* dev = NULL;
            ((PFN_GetDev)Vt(tex)[3])(tex, &dev);
            ((PFN_Rel)Vt(tex)[2])(tex);
            if (dev) {
                if (!g_d11.ok) {
                    void* ctx = NULL;
                    ((PFN_GetImmCtx)Vt(dev)[40])(dev, &ctx);
                    if (ctx) {
                        g_d11.sc = sc; g_d11.dev = dev; g_d11.ctx = ctx; g_d11.ok = true;
                        Log("D3D11: device/context/swapchain captured via Present");
                        ZmInstallCtxHooks();
                    } else {
                        ((PFN_Rel)Vt(dev)[2])(dev);
                    }
                } else {
                    if (dev == g_d11.dev) {
                        g_d11.sc = sc;
                        Log("D3D11: swapchain associated with early-captured device");
                    }
                    ((PFN_Rel)Vt(dev)[2])(dev); // GetDevice вернул дополнительную COM-ссылку
                }
            }
        }
    }
    // FLIP11 больше не меняет swapchain здесь: к этому моменту GD3D11 уже
    // мог наложить HUD/ImGui. Переворот выполняется на границе мира и UI.
    return orig ? orig(sc, sync, flags) : E_FAIL;
}

// Создаёт пробный свопчейн и вешает хук Present на его vtable.
bool ZmProbeSwapChain() {
    if (g_probeSC) return true;

    typedef HRESULT (WINAPI *PFN_CD)(void*, UINT, HMODULE, UINT, const UINT*, UINT, UINT, void**, UINT*, void**);
    PFN_CD cd = (PFN_CD)GetProcAddress(LoadLibraryA("d3d11.dll"), "D3D11CreateDevice");
    if (!cd) return false;
    // Если ранний хук установлен — создаём пробное устройство через оригинал (трэмполин),
    // иначе наше же Hook_CD захватит пробное устройство как «настоящее».
    PFN_CD cdReal = (PFN_CD)(g_njCD.installed ? (void*)g_njCD.tramp : (void*)cd);
    void* dev = NULL; void* ctx = NULL; UINT fl = 0;
    const UINT levels[3] = { 0xB000, 0xA100, 0xA000 };
    HRESULT hr = cdReal(NULL, 1 /*HARDWARE*/, NULL, 0, levels, 3, 7, &dev, &fl, &ctx);
    if (FAILED(hr) || !ctx) hr = cdReal(NULL, 5 /*WARP*/, NULL, 0, levels, 3, 7, &dev, &fl, &ctx);
    if (FAILED(hr) || !ctx) { Log("D3D11: probe device failed"); return false; }
    g_probeDev = dev;

    static bool clsRegistered = false;
    if (!clsRegistered) {
        WNDCLASS wc; ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = "ZmProbeWnd";
        RegisterClassA(&wc);
        clsRegistered = true;
    }
    HWND hwnd = CreateWindowExA(0, "ZmProbeWnd", "ZmProbe", WS_POPUP, 0, 0, 32, 32, NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!hwnd) { Log("D3D11: probe window failed"); return false; }

    HMODULE hdxgi = LoadLibraryA("dxgi.dll");
    if (!hdxgi) return false;

    // IID_IDXGIFactory1 = 770aae78-f26f-4dba-a829-253c83d1b387
    // IID_IDXGIFactory2 = 50c83a1c-e072-4c48-87b0-3630fa36a6d0
    static const GUID IID_F1 = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
    static const GUID IID_F2 = { 0x50c83a1c, 0xe072, 0x4c48, { 0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0 } };

    // Попытка 1: IDXGIFactory2::CreateSwapChainForHwnd (FLIP_DISCARD), слот 15
    typedef HRESULT (WINAPI *PFN_CGF1)(const GUID*, void**);
    PFN_CGF1 cgf1 = (PFN_CGF1)GetProcAddress(hdxgi, "CreateDXGIFactory1");
    if (cgf1) {
        void* factory = NULL;
        if (SUCCEEDED(cgf1(&IID_F2, &factory))) {
            // DXGI_SWAP_CHAIN_DESC1
            struct SCDesc1 { UINT Width, Height, Format, Stereo; UINT SampleDesc_Count, SampleDesc_Quality; UINT BufferUsage, BufferCount; UINT Scaling, SwapEffect, AlphaMode, Flags; };
            struct FSD { UINT RefreshRate_Numer, RefreshRate_Denom, ScanlineOrdering, Scaling, Windowed; };
            typedef HRESULT (STDMETHODCALLTYPE *PFN_CSCFH)(void*, void*, void*, const SCDesc1*, const FSD*, void*, void**);
            typedef ULONG (STDMETHODCALLTYPE *PFN_RelF)(void*);
            SCDesc1 d1; ZeroMemory(&d1, sizeof(d1));
            d1.Width = 64; d1.Height = 64;
            d1.Format = 87;                 // DXGI_FORMAT_B8G8R8A8_UNORM (как у GD3D11 в Log.txt)
            d1.SampleDesc_Count = 1;
            d1.BufferUsage = 0x20;          // DXGI_USAGE_RENDER_TARGET_OUTPUT
            d1.BufferCount = 2;
            d1.Scaling = 1;                 // DXGI_SCALING_STRETCH
            d1.SwapEffect = 4;              // DXGI_SWAP_EFFECT_FLIP_DISCARD
            d1.AlphaMode = 3;               // DXGI_ALPHA_MODE_IGNORE
            void* sc = NULL;
            HRESULT scHr = ((PFN_CSCFH)Vt(factory)[15])(factory, dev, hwnd, &d1, NULL, NULL, &sc);
            if (SUCCEEDED(scHr) && sc) {
                g_probeSC = sc;
                g_vtProbeF = Vt(sc);
                PatchSlot(Vt(sc), 8, &Hook_Present, g_oPresentF);
                Log("D3D11: probe swapchain FLIP_DISCARD installed (Present hooked)");
                ((PFN_RelF)Vt(factory)[2])(factory);
                return true;
            }
            Log(SFmt("D3D11: probe CreateSwapChainForHwnd failed hr=0x%08lX", (unsigned long)scHr));
            ((PFN_RelF)Vt(factory)[2])(factory);
        }
    }

    // Попытка 2: IDXGIFactory1::CreateSwapChain (legacy DISCARD), слот 10
    if (cgf1) {
        void* factory = NULL;
        if (SUCCEEDED(cgf1(&IID_F1, &factory))) {
            // DXGI_SWAP_CHAIN_DESC (без Stereo: ModeDesc = 7 UINT)
            struct SCD { UINT BufferDesc_Width, BufferDesc_Height, BufferDesc_Format, BufferDesc_RefreshRate_Numer, BufferDesc_RefreshRate_Denom, BufferDesc_ScanlineOrdering, BufferDesc_Scaling; UINT SampleDesc_Count, SampleDesc_Quality; UINT BufferUsage, BufferCount; void* OutputWindow; int Windowed; UINT SwapEffect, Flags; };
            typedef HRESULT (STDMETHODCALLTYPE *PFN_CSC)(void*, void*, const SCD*, void**);
            typedef ULONG (STDMETHODCALLTYPE *PFN_RelF)(void*);
            SCD d; ZeroMemory(&d, sizeof(d));
            d.BufferDesc_Width = 64; d.BufferDesc_Height = 64;
            d.BufferDesc_Format = 87;       // DXGI_FORMAT_B8G8R8A8_UNORM
            d.SampleDesc_Count = 1;
            d.BufferUsage = 0x20;
            d.BufferCount = 1;
            d.OutputWindow = hwnd;
            d.Windowed = TRUE;
            d.SwapEffect = 0;               // DXGI_SWAP_EFFECT_DISCARD
            void* sc = NULL;
            HRESULT scHr = ((PFN_CSC)Vt(factory)[10])(factory, dev, &d, &sc);
            if (SUCCEEDED(scHr) && sc) {
                g_probeSC = sc;
                g_vtProbeL = Vt(sc);
                PatchSlot(Vt(sc), 8, &Hook_Present, g_oPresentL);
                Log("D3D11: probe swapchain DISCARD installed (Present hooked)");
                ((PFN_RelF)Vt(factory)[2])(factory);
                return true;
            }
            Log(SFmt("D3D11: probe CreateSwapChain(DISCARD) failed hr=0x%08lX", (unsigned long)scHr));
            ((PFN_RelF)Vt(factory)[2])(factory);
        }
    }
    Log("D3D11: probe swapchain failed (both effects)");
    return false;
}

void AcquireD3D11() {
    if (g_d11.sc || g_d11.tried) return;
    g_d11.tried = true;
    if (!GpuUsesFlip()) {
        // Даже если ранний hook уже захватил Device/Context, всё равно нужен
        // probe swap chain, чтобы поставить hook Present и найти настоящий SC.
        ZmProbeSwapChain();
    }
}

// ───────────────────────── FLIP11: переворот кадра на GD3D11 ─────────────────────────
// Диагностический backend: при Present копируем swapchain backbuffer в scratch и
// рисуем полноэкранный треугольник с зеркальными UV. Он переворачивает весь финальный
// кадр, включая уже нарисованный HUD/UI; для режима «мир зеркальный, интерфейс нормальный»
// это НЕ конечное решение, а проверка точки перехвата/копирования.

struct ZmFlipSc {
    bool initTried;
    void* tex;    // ID3D11Texture2D (scratch)
    void* srv;    // ID3D11ShaderResourceView
    void* sampler; // ID3D11SamplerState
    void* vs;     // ID3D11VertexShader
    void* ps;     // ID3D11PixelShader
    void* rtv;    // ID3D11RenderTargetView (бэкбуфер)
    void* rs;     // ID3D11RasterizerState (CullMode=NONE для fullscreen triangle)
    UINT w, h;
    int frames;
};
ZmFlipSc g_fs = {};

typedef HRESULT (WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, void**, void**);

void ZmCompileShaders() {
    typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateVS)(void*, const void*, SIZE_T, void*, void**);
    typedef HRESULT (STDMETHODCALLTYPE *PFN_CreatePS)(void*, const void*, SIZE_T, void*, void**);
    HMODULE hc = LoadLibraryA("d3dcompiler_47.dll");
    if (!hc) hc = LoadLibraryA("d3dcompiler_46.dll");
    if (!hc) { Log("FLIP11: d3dcompiler not found"); return; }
    PFN_D3DCompile compile = (PFN_D3DCompile)GetProcAddress(hc, "D3DCompile");
    if (!compile) { Log("FLIP11: D3DCompile not found"); return; }

    const char* vssrc =
        "struct VSOut{float4 p:SV_Position;float2 uv:TEXCOORD0;};\n"
        "VSOut main(uint id:SV_VertexID){\n"
        "  VSOut o;\n"
        "  float2 p=float2((id<<1)&2,id&2);\n"
        "  o.p=float4(p*2.0-1.0,0,1);\n"
        "  o.uv=p;\n"
        "  return o;\n"
        "}\n";
    const char* pssrc =
        "Texture2D t:register(t0);\n"
        "SamplerState s:register(s0);\n"
        "float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{\n"
        "  return t.Sample(s,float2(1.0-uv.x,uv.y));\n"
        "}\n";

    void* vsCode = NULL, *psCode = NULL, *err = NULL;
    HRESULT hr = compile(vssrc, strlen(vssrc), "vs.hlsl", NULL, NULL, "main", "vs_4_0", 0, 0, &vsCode, &err);
    if (FAILED(hr) || !vsCode) { Log(SFmt("FLIP11: VS compile failed hr=0x%08lX", (unsigned long)hr)); return; }
    hr = compile(pssrc, strlen(pssrc), "ps.hlsl", NULL, NULL, "main", "ps_4_0", 0, 0, &psCode, &err);
    if (FAILED(hr) || !psCode) { Log(SFmt("FLIP11: PS compile failed hr=0x%08lX", (unsigned long)hr)); return; }

    // ID3DBlob: GetBufferPointer = 3, GetBufferSize = 4
    typedef void* (STDMETHODCALLTYPE *PFN_GetBufPtr)(void*);
    typedef SIZE_T (STDMETHODCALLTYPE *PFN_GetBufSize)(void*);
    const void* vptr = ((PFN_GetBufPtr)Vt(vsCode)[3])(vsCode);
    SIZE_T vsize = ((PFN_GetBufSize)Vt(vsCode)[4])(vsCode);
    const void* pptr = ((PFN_GetBufPtr)Vt(psCode)[3])(psCode);
    SIZE_T psize = ((PFN_GetBufSize)Vt(psCode)[4])(psCode);
    ((PFN_CreateVS)Vt(g_d11.dev)[12])(g_d11.dev, vptr, vsize, NULL, &g_fs.vs);
    ((PFN_CreatePS)Vt(g_d11.dev)[15])(g_d11.dev, pptr, psize, NULL, &g_fs.ps);
    ((PFN_Rel)Vt(vsCode)[2])(vsCode);
    ((PFN_Rel)Vt(psCode)[2])(psCode);
    Log("FLIP11: shaders compiled");
}

void ZmFlipScDo(void* sc) {
    void* ctx = g_d11.ctx, *dev = g_d11.dev;
    if (!ctx || !dev) return;

    if (!g_fs.initTried) {
        g_fs.initTried = true;
        static const GUID IID_T2D = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };
        typedef HRESULT (STDMETHODCALLTYPE *PFN_GetBuffer)(void*, UINT, const GUID*, void**);
        typedef void (STDMETHODCALLTYPE *PFN_GetDescTex)(void*, void*);
        typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateTex)(void*, const void*, const void*, void**);
        typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSRV)(void*, void*, const void*, void**);
        typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateRTV)(void*, void*, const void*, void**);
        typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSampler)(void*, const void*, void**);
        typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateRS)(void*, const void*, void**);

        void* bb = NULL;
        if (FAILED(((PFN_GetBuffer)Vt(sc)[9])(sc, 0, &IID_T2D, &bb)) || !bb) { Log("FLIP11: GetBuffer failed"); return; }

        // D3D11_TEXTURE2D_DESC
        struct T2DDesc { UINT Width, Height, MipLevels, ArraySize; UINT Format; UINT SampleDesc_Count, SampleDesc_Quality; UINT Usage, BindFlags, CPUAccessFlags, MiscFlags; };
        T2DDesc d; ZeroMemory(&d, sizeof(d));
        ((PFN_GetDescTex)Vt(bb)[10])(bb, &d);          // ID3D11Texture2D::GetDesc = 10
        g_fs.w = d.Width; g_fs.h = d.Height;

        T2DDesc sd = d;
        sd.BindFlags = 0x8;                            // D3D11_BIND_SHADER_RESOURCE
        if (FAILED(((PFN_CreateTex)Vt(dev)[5])(dev, &sd, NULL, &g_fs.tex))) { Log("FLIP11: CreateTexture2D failed"); return; }
        if (FAILED(((PFN_CreateSRV)Vt(dev)[7])(dev, g_fs.tex, NULL, &g_fs.srv))) { Log("FLIP11: CreateShaderResourceView failed"); return; }
        if (FAILED(((PFN_CreateRTV)Vt(dev)[9])(dev, bb, NULL, &g_fs.rtv))) { Log("FLIP11: CreateRenderTargetView failed"); ((PFN_Rel)Vt(bb)[2])(bb); return; }
        ((PFN_Rel)Vt(bb)[2])(bb);

        // Полноэкранный треугольник обязан рисоваться независимо от winding
        // и текущего rasterizer state игры.
        struct RastDesc { UINT FillMode, CullMode; BOOL FrontCCW; INT DepthBias; float DepthBiasClamp, SlopeScaledDepthBias; BOOL DepthClipEnable, ScissorEnable, MultisampleEnable, AntialiasedLineEnable; };
        RastDesc rd; ZeroMemory(&rd, sizeof(rd));
        rd.FillMode = 3; // D3D11_FILL_SOLID
        rd.CullMode = 1; // D3D11_CULL_NONE
        rd.DepthClipEnable = TRUE;
        if (FAILED(((PFN_CreateRS)Vt(dev)[22])(dev, &rd, &g_fs.rs))) { Log("FLIP11: CreateRasterizerState failed"); return; }

        // D3D11_SAMPLER_DESC: FILTER_MIN_MAG_MIP_POINT=0, CLAMP=3
        struct SmpDesc { UINT Filter, AddressU, AddressV, AddressW; float MipLODBias; UINT MaxAnisotropy, ComparisonFunc; float BorderColor[4]; float MinLOD, MaxLOD; };
        SmpDesc sm; ZeroMemory(&sm, sizeof(sm));
        sm.AddressU = sm.AddressV = sm.AddressW = 3;
        sm.MaxLOD = 3.402823466e+38F;
        if (FAILED(((PFN_CreateSampler)Vt(dev)[23])(dev, &sm, &g_fs.sampler))) { Log("FLIP11: CreateSamplerState failed"); return; }

        ZmCompileShaders();
        if (!g_fs.vs || !g_fs.ps) { Log("FLIP11: shader creation failed"); return; }
        Log(SFmt("FLIP11: resources ready (%lux%lu)", (unsigned long)g_fs.w, (unsigned long)g_fs.h));
    }

    if (!g_fs.tex || !g_fs.srv || !g_fs.rtv || !g_fs.rs || !g_fs.vs || !g_fs.ps) return;

    typedef void (STDMETHODCALLTYPE *PFN_CopyResource)(void*, void*, void*);
    typedef void (STDMETHODCALLTYPE *PFN_OMSetRTs)(void*, UINT, void**, void*);
    typedef void (STDMETHODCALLTYPE *PFN_RSSetVP2)(void*, UINT, const float*);
    typedef void (STDMETHODCALLTYPE *PFN_IASetTopo)(void*, UINT);
    typedef void (STDMETHODCALLTYPE *PFN_SetVS)(void*, void*, void*, UINT);
    typedef void (STDMETHODCALLTYPE *PFN_SetPS)(void*, void*, void*, UINT);
    typedef void (STDMETHODCALLTYPE *PFN_SetPSResources)(void*, UINT, UINT, void**);
    typedef void (STDMETHODCALLTYPE *PFN_SetPSSamplers)(void*, UINT, UINT, void**);
    typedef void (STDMETHODCALLTYPE *PFN_SetBlend)(void*, void*, const float*, UINT);
    typedef void (STDMETHODCALLTYPE *PFN_SetDepth)(void*, void*, UINT);
    typedef void (STDMETHODCALLTYPE *PFN_Draw)(void*, UINT, UINT);
    typedef HRESULT (STDMETHODCALLTYPE *PFN_GetBuffer)(void*, UINT, const GUID*, void**);

    static const GUID IID_T2D = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };
    void* bb = NULL;
    if (FAILED(((PFN_GetBuffer)Vt(sc)[9])(sc, 0, &IID_T2D, &bb)) || !bb) return;
    struct T2DDesc { UINT Width, Height, MipLevels, ArraySize; UINT Format; UINT SampleDesc_Count, SampleDesc_Quality; UINT Usage, BindFlags, CPUAccessFlags, MiscFlags; };
    T2DDesc d; ((void (STDMETHODCALLTYPE *)(void*, void*))Vt(bb)[10])(bb, &d);
    if (d.Width != g_fs.w || d.Height != g_fs.h) {
        // размер сменился — освободить и пересоздать при следующем Present
        ((PFN_Rel)Vt(bb)[2])(bb);
        if (g_fs.rtv) { ((PFN_Rel)Vt(g_fs.rtv)[2])(g_fs.rtv); g_fs.rtv = NULL; }
        if (g_fs.rs) { ((PFN_Rel)Vt(g_fs.rs)[2])(g_fs.rs); g_fs.rs = NULL; }
        if (g_fs.srv) { ((PFN_Rel)Vt(g_fs.srv)[2])(g_fs.srv); g_fs.srv = NULL; }
        if (g_fs.tex) { ((PFN_Rel)Vt(g_fs.tex)[2])(g_fs.tex); g_fs.tex = NULL; }
        g_fs.w = d.Width; g_fs.h = d.Height;
        g_fs.initTried = false;
        return;
    }

    ((PFN_CopyResource)Vt(ctx)[47])(ctx, g_fs.tex, bb);
    ((PFN_Rel)Vt(bb)[2])(bb);

    float vp[6] = { 0, 0, (float)g_fs.w, (float)g_fs.h, 0, 1 };
    ((PFN_RSSetVP2)Vt(ctx)[44])(ctx, 1, vp);
    ((PFN_OMSetRTs)Vt(ctx)[33])(ctx, 1, &g_fs.rtv, NULL);
    ((PFN_RSSetState)Vt(ctx)[43])(ctx, g_fs.rs);
    ((PFN_IASetTopo)Vt(ctx)[24])(ctx, 4);                 // D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST
    ((PFN_SetVS)Vt(ctx)[11])(ctx, g_fs.vs, NULL, 0);
    ((PFN_SetPS)Vt(ctx)[9])(ctx, g_fs.ps, NULL, 0);
    // Не позволяем унаследованному geometry shader перехватить вершины треугольника.
    ((PFN_SetVS)Vt(ctx)[23])(ctx, NULL, NULL, 0);
    ((PFN_SetPSResources)Vt(ctx)[8])(ctx, 0, 1, &g_fs.srv);
    ((PFN_SetPSSamplers)Vt(ctx)[10])(ctx, 0, 1, &g_fs.sampler);
    ((PFN_SetBlend)Vt(ctx)[35])(ctx, NULL, NULL, 0xFFFFFFFF);
    ((PFN_SetDepth)Vt(ctx)[36])(ctx, NULL, 0);
    ((PFN_Draw)Vt(ctx)[13])(ctx, 3, 0);
    // Не оставляем scratch-текстуру привязанной как SRV между Present.
    void* nullSrv = NULL;
    ((PFN_SetPSResources)Vt(ctx)[8])(ctx, 0, 1, &nullSrv);
    g_fs.frames++;
}

// ───────────────────────── GPU-FLIP: переворот готового кадра на видеокарте ─────────────────────────
inline void RsRaw(IDirect3DDevice7* dev, D3DRENDERSTATETYPE t, DWORD v) {
    if (g_oSetRS) g_oSetRS(dev, t, v); else dev->SetRenderState(t, v);
}

bool GfEnsure(IDirectDrawSurface7* rt, IDirect3DDevice7* dev) {
    DDSURFACEDESC2 rd; ZeroMemory(&rd, sizeof(rd)); rd.dwSize = sizeof(rd);
    if (FAILED(rt->GetSurfaceDesc(&rd))) return false;
    if (rd.dwWidth < 32 || rd.dwHeight < 32) return false;
    if (g_gf.tex && g_gf.rtW == rd.dwWidth && g_gf.rtH == rd.dwHeight) return true;

    int keepState = (g_gf.state < 0) ? -1 : 0;
    GfRelease();
    g_gf.rtW = rd.dwWidth; g_gf.rtH = rd.dwHeight;
    g_gf.state = keepState; g_gf.calTries = 0; g_gf.adjX = 0; g_gf.adjY = 0;

    DWORD tw = Pow2Ceil(rd.dwWidth), th = Pow2Ceil(rd.dwHeight);
    DWORD maxW = 0, maxH = 0; bool sq = false;
    D3DDEVICEDESC7 caps; ZeroMemory(&caps, sizeof(caps));
    if (SUCCEEDED(dev->GetCaps(&caps))) {
        maxW = caps.dwMaxTextureWidth; maxH = caps.dwMaxTextureHeight;
        sq = (caps.dpcTriCaps.dwTextureCaps & D3DPTEXTURECAPS_SQUAREONLY) != 0;
    }
    if (sq) { DWORD m = tw > th ? tw : th; tw = th = m; }
    if ((maxW && tw > maxW) || (maxH && th > maxH)) {
        Log(SFmt("GPUFLIP init: texture %lux%lu exceeds device max %lux%lu", (unsigned long)tw, (unsigned long)th, (unsigned long)maxW, (unsigned long)maxH));
        return false;
    }

    IDirectDraw7* dd = NULL;
    if (FAILED(rt->GetDDInterface((LPVOID*)&dd)) || !dd) { Log("GPUFLIP init: GetDDInterface failed"); return false; }
    IDirectDrawSurface7* t = NULL;
    HRESULT hr = E_FAIL;
    for (int attempt = 0; attempt < 2 && !t; attempt++) {
        DDSURFACEDESC2 d; ZeroMemory(&d, sizeof(d)); d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        d.dwWidth = tw; d.dwHeight = th;
        d.ddpfPixelFormat = rd.ddpfPixelFormat;
        d.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
        if (attempt == 0) d.ddsCaps.dwCaps |= DDSCAPS_VIDEOMEMORY;
        else d.ddsCaps.dwCaps2 = DDSCAPS2_TEXTUREMANAGE;
        hr = dd->CreateSurface(&d, &t, NULL);
        Log(SFmt("GPUFLIP init: CreateSurface attempt=%d %lux%lu bpp=%lu -> hr=0x%08lX", attempt, (unsigned long)tw, (unsigned long)th,
            (unsigned long)rd.ddpfPixelFormat.dwRGBBitCount, (unsigned long)hr));
        if (FAILED(hr)) t = NULL;
    }
    dd->Release();
    if (!t) return false;
    g_gf.tex = t; g_gf.texW = tw; g_gf.texH = th;
    Log(SFmt("GPUFLIP init: RT %lux%lu, texture %lux%lu OK (square=%d)", (unsigned long)rd.dwWidth, (unsigned long)rd.dwHeight,
        (unsigned long)tw, (unsigned long)th, sq ? 1 : 0));
    return true;
}

struct LockedRT { BYTE* base; long pitch; DWORD bpp; };

bool LockRT(IDirectDrawSurface7* s, LockedRT& L) {
    DDSURFACEDESC2 d; ZeroMemory(&d, sizeof(d)); d.dwSize = sizeof(d);
    HRESULT hr = s->Lock(NULL, &d, DDLOCK_WAIT | DDLOCK_SURFACEMEMORYPTR | DDLOCK_READONLY, NULL);
    if (FAILED(hr)) return false;
    L.base = (BYTE*)d.lpSurface; L.pitch = d.lPitch; L.bpp = d.ddpfPixelFormat.dwRGBBitCount;
    if (!L.base || (L.bpp != 32 && L.bpp != 16)) { s->Unlock(NULL); return false; }
    return true;
}

inline DWORD PixAt(const LockedRT& L, int x, int y) {
    if (L.bpp == 32) return *(DWORD*)(L.base + (size_t)y * L.pitch + (size_t)x * 4) & 0x00FFFFFF;
    return *(WORD*)(L.base + (size_t)y * L.pitch + (size_t)x * 2);
}

bool GfReadOld(IDirectDrawSurface7* rt) {
    LockedRT L;
    if (!LockRT(rt, L)) return false;
    int W = (int)g_gf.rtW, H = (int)g_gf.rtH;
    for (int i = 0; i < kCalN; i++) {
        int x = 4 + (int)((unsigned)(i * 7919 + 131) % (unsigned)(W - 8));
        int y = 4 + (int)((unsigned)(i * 104729 + 977) % (unsigned)(H - 8));
        g_calX[i] = x; g_calY[i] = y;
        for (int sy = -1; sy <= 1; sy++)
            for (int sx = -1; sx <= 1; sx++)
                g_calOld[i][(sy + 1) * 3 + (sx + 1)] = PixAt(L, W - 1 - x + sx, y + sy);
    }
    rt->Unlock(NULL);
    return true;
}

int GfEvaluate(IDirectDrawSurface7* rt) {
    LockedRT L;
    if (!LockRT(rt, L)) return 0;
    int tally[9]; memset(tally, 0, sizeof(tally));
    int inf = 0;
    for (int i = 0; i < kCalN; i++) {
        bool informative = false;
        for (int k = 1; k < 9; k++) if (g_calOld[i][k] != g_calOld[i][0]) { informative = true; break; }
        if (!informative) continue;
        DWORD nv = PixAt(L, g_calX[i], g_calY[i]);
        inf++;
        for (int k = 0; k < 9; k++) if (nv == g_calOld[i][k]) tally[k]++;
    }
    rt->Unlock(NULL);
    if (inf < 6) return 0;
    int best = 4;
    for (int k = 0; k < 9; k++) if (tally[k] > tally[best]) best = k;
    if (tally[4] * 100 >= tally[best] * 95) best = 4;
    Log(SFmt("GPUFLIP calibrate: informative=%d center=%d best=%d(sx=%d sy=%d) tally=[%d %d %d|%d %d %d|%d %d %d]",
        inf, tally[4], tally[best], best % 3 - 1, best / 3 - 1,
        tally[0], tally[1], tally[2], tally[3], tally[4], tally[5], tally[6], tally[7], tally[8]));
    if (tally[best] < 5 || tally[best] * 100 < inf * 70) return 2;
    if (best == 4) return 1;
    int sx = best % 3 - 1, sy = best / 3 - 1;
    g_gf.adjX += sx; g_gf.adjY -= sy;
    Log(SFmt("GPUFLIP calibrate: shifting UV by adjX=%d adjY=%d, re-checking", g_gf.adjX, g_gf.adjY));
    return 0;
}

struct TLV { float x, y, z, rhw, u, v; };

bool GfDrawQuad(IDirect3DDevice7* dev) {
    static const D3DRENDERSTATETYPE kRs[] = {
        D3DRENDERSTATE_ZENABLE, D3DRENDERSTATE_ZWRITEENABLE, D3DRENDERSTATE_ALPHABLENDENABLE,
        D3DRENDERSTATE_ALPHATESTENABLE, D3DRENDERSTATE_CULLMODE, D3DRENDERSTATE_LIGHTING,
        D3DRENDERSTATE_FOGENABLE, D3DRENDERSTATE_SPECULARENABLE, D3DRENDERSTATE_STENCILENABLE,
        D3DRENDERSTATE_FILLMODE, D3DRENDERSTATE_COLORKEYENABLE
    };
    static const DWORD kRsNew[] = {
        FALSE, FALSE, FALSE, FALSE, D3DCULL_NONE, FALSE, FALSE, FALSE, FALSE, D3DFILL_SOLID, FALSE
    };
    const int nRs = (int)(sizeof(kRs) / sizeof(kRs[0]));

    struct TssItem { DWORD stage; D3DTEXTURESTAGESTATETYPE t; DWORD nv; };
    static const TssItem kTss[] = {
        { 0, D3DTSS_COLOROP,       D3DTOP_SELECTARG1 },
        { 0, D3DTSS_COLORARG1,     D3DTA_TEXTURE },
        { 0, D3DTSS_ALPHAOP,       D3DTOP_DISABLE },
        { 0, D3DTSS_ADDRESS,       D3DTADDRESS_CLAMP },
        { 0, D3DTSS_MAGFILTER,     D3DTFG_POINT },
        { 0, D3DTSS_MINFILTER,     D3DTFN_POINT },
        { 0, D3DTSS_MIPFILTER,     D3DTFP_NONE },
        { 0, D3DTSS_TEXCOORDINDEX, 0 },
        { 1, D3DTSS_COLOROP,       D3DTOP_DISABLE },
        { 1, D3DTSS_ALPHAOP,       D3DTOP_DISABLE }
    };
    const int nTss = (int)(sizeof(kTss) / sizeof(kTss[0]));

    DWORD rsOld[16]; bool rsHave[16];
    DWORD tssOld[16]; bool tssHave[16];
    for (int i = 0; i < nRs; i++) rsHave[i] = SUCCEEDED(dev->GetRenderState(kRs[i], &rsOld[i]));
    for (int i = 0; i < nTss; i++) tssHave[i] = SUCCEEDED(dev->GetTextureStageState(kTss[i].stage, kTss[i].t, &tssOld[i]));

    D3DVIEWPORT7 oldVp; ZeroMemory(&oldVp, sizeof(oldVp));
    bool haveVp = SUCCEEDED(dev->GetViewport(&oldVp));
    IDirectDrawSurface7* oldTex = NULL;
    dev->GetTexture(0, &oldTex);

    D3DVIEWPORT7 vp; ZeroMemory(&vp, sizeof(vp));
    vp.dwX = 0; vp.dwY = 0; vp.dwWidth = g_gf.rtW; vp.dwHeight = g_gf.rtH; vp.dvMinZ = 0.0f; vp.dvMaxZ = 1.0f;
    if (g_oSetViewport) g_oSetViewport(dev, &vp); else dev->SetViewport(&vp);

    for (int i = 0; i < nRs; i++) RsRaw(dev, kRs[i], kRsNew[i]);
    for (int i = 0; i < nTss; i++) dev->SetTextureStageState(kTss[i].stage, kTss[i].t, kTss[i].nv);
    dev->SetTexture(0, g_gf.tex);

    float W = (float)g_gf.rtW, H = (float)g_gf.rtH;
    float tw = (float)g_gf.texW, th = (float)g_gf.texH;
    float uL = ((float)g_gf.rtW - (float)g_gf.adjX) / tw;
    float uR = (-(float)g_gf.adjX) / tw;
    float vT = ((float)g_gf.adjY) / th;
    float vB = (H + (float)g_gf.adjY) / th;
    TLV q[4] = {
        { -0.5f,    -0.5f,    0.0f, 1.0f, uL, vT },
        { W - 0.5f, -0.5f,    0.0f, 1.0f, uR, vT },
        { -0.5f,    H - 0.5f, 0.0f, 1.0f, uL, vB },
        { W - 0.5f, H - 0.5f, 0.0f, 1.0f, uR, vB }
    };
    HRESULT hr = dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_TEX1, q, 4, 0);

    dev->SetTexture(0, oldTex);
    if (oldTex) oldTex->Release();
    for (int i = 0; i < nTss; i++) if (tssHave[i]) dev->SetTextureStageState(kTss[i].stage, kTss[i].t, tssOld[i]);
    for (int i = 0; i < nRs; i++) if (rsHave[i]) RsRaw(dev, kRs[i], rsOld[i]);
    if (haveVp) { if (g_oSetViewport) g_oSetViewport(dev, &oldVp); else dev->SetViewport(&oldVp); }

    if (FAILED(hr)) Log(SFmt("GPUFLIP draw: DrawPrimitive failed hr=0x%08lX", (unsigned long)hr));
    return SUCCEEDED(hr);
}

int GfFlip(zCRnd_D3D* rnd, IDirectDrawSurface7* rt, bool inScene) {
    IDirect3DDevice7* dev = zCRnd_D3D::xd3d_pd3dDevice7;
    if (!dev || !rnd) return 0;
    if (!GfEnsure(rt, dev)) {
        if (++g_gf.initFails >= 3) { g_gf.state = -1; Log("GPUFLIP init failed 3 times -> GPU path disabled; caller decides fallback"); }
        return 0;
    }
    if (g_gf.state < 0) return 0;

    // GD3D11 предоставляет target через свой D3D7-совместимый слой. Для него
    // используем тот же GPU Blt + TL-quad, но не читаем render target обратно
    // на CPU ради пиксельной калибровки: это добавляет синхронизацию GPU и
    // не требуется для стандартной D3D7 screen-space UV-развёртки.
    if (IsScreenFlipActive() && g_gf.state == 0) {
        g_gf.state = 1;
        Log("GPUFLIP: GD3D11 compatibility path, CPU pixel calibration skipped");
    }

    bool cal = (g_gf.state == 0);
    if (inScene) rnd->EndFrame();

    bool haveOld = false;
    if (cal) haveOld = GfReadOld(rt);

    if (g_gf.tex->IsLost() == DDERR_SURFACELOST) g_gf.tex->Restore();
    RECT r; r.left = 0; r.top = 0; r.right = (LONG)g_gf.rtW; r.bottom = (LONG)g_gf.rtH;
    HRESULT hr = g_gf.tex->Blt(&r, rt, &r, DDBLT_WAIT, NULL);
    if (FAILED(hr)) {
        Log(SFmt("GPUFLIP blit RT->tex failed hr=0x%08lX", (unsigned long)hr));
        if (inScene) rnd->BeginFrame();
        return 0;
    }

    rnd->BeginFrame();
    bool drew = GfDrawQuad(dev);

    int result = drew ? 1 : 0;
    if (cal && drew && haveOld) {
        rnd->EndFrame();
        int ev = GfEvaluate(rt);
        if (inScene) rnd->BeginFrame();
        if (ev == 1) { g_gf.state = 1; Log(SFmt("GPUFLIP verified OK (adjX=%d adjY=%d)", g_gf.adjX, g_gf.adjY)); }
        else if (ev == 2) { g_gf.state = -1; Log("GPUFLIP verification FAILED -> GPU path disabled; caller decides fallback"); result = 2; }
        else if (++g_gf.calTries >= 90) { g_gf.state = 1; Log("GPUFLIP: no informative frames in 90 tries -> assumed OK"); }
    }
    else {
        if (!inScene) rnd->EndFrame();
        if (cal && drew && !haveOld && ++g_gf.calTries >= 90) { g_gf.state = 1; Log("GPUFLIP: pixel read unavailable -> assumed OK"); }
    }
    if (!drew) {
        g_gf.state = -1;
        Log("GPUFLIP draw failed -> GPU path disabled; caller decides fallback");
    }
    if (result == 1) g_gf.framesOk++;
    return result;
}

void MirrorFrame(bool inScene) {
    zCRnd_D3D* rnd = (zCRnd_D3D*)zrenderer;
    if (!rnd) return;
    IDirectDrawSurface7* rt = AcquireTarget();
    if (!rt) return;

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);

    bool ok = false;
    if (g_mode == MODE_GPU && GpuUsesFlip() && g_gf.state >= 0) {
        int r = GfFlip(rnd, rt, inScene);
        if (r == 1 || r == 2) ok = true;
    }
    if (!ok) {
        if (inScene) rnd->EndFrame();
        ok = MirrorByLock(rt);
        if (inScene) rnd->BeginFrame();
    }
    rt->Release();

    QueryPerformanceCounter(&t1);
    g_perf.mirrorSum += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    g_perf.mirrorN++;

    if (ok) g_failStreak = 0;
    else if (++g_failStreak >= 30) { g_mode = MODE_OFF; SyncMouseFlip(); Log("mirror auto-disabled: 30 failures in a row"); }
}

// GD3D11 FLIP11: тот же GPU-FLIP quad, что и в ванильном backend, но на
// D3D7-совместимом render target GD3D11. Вызов из Hook_World_Render выполняется
// после сцены и до HUD/интерфейса. При ошибке не переходим на CPU Lock/переворот:
// лучше оставить кадр обычным, чем блокировать GPU или зеркалить UI.
void MirrorFrameFlip11Scene() {
    zCRnd_D3D* rnd = (zCRnd_D3D*)zrenderer;
    if (!rnd) return;
    IDirectDrawSurface7* rt = AcquireTarget();
    if (!rt) {
        static bool warnedNoTarget = false;
        if (!warnedNoTarget) { warnedNoTarget = true; Log("FLIP11: scene render target unavailable through D3D7 wrapper"); }
        return;
    }

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);

    static bool announced = false;
    if (!announced) {
        announced = true;
        Log("FLIP11: scene-boundary GPU-FLIP active (after zCWorld::Render, before HUD)");
    }

    int r = GfFlip(rnd, rt, true);
    bool ok = (r == 1 || r == 2);
    rt->Release();

    QueryPerformanceCounter(&t1);
    g_perf.mirrorSum += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    g_perf.mirrorN++;

    if (ok) {
        g_failStreak = 0;
    } else {
        static bool warnedFailure = false;
        if (!warnedFailure) {
            warnedFailure = true;
            Log(SFmt("FLIP11: scene GPU-FLIP failed (state=%d); CPU fallback intentionally disabled", g_gf.state));
        }
        if (++g_failStreak >= 30) {
            g_mode = MODE_OFF;
            SyncMouseFlip();
            Log("FLIP11: mirror auto-disabled after 30 scene-target failures");
        }
    }
}

// ───────────────────────── Хуки на уровне движка (zCRnd_D3D) — TL-зеркалирование PROJ ─────────────────────────
void __fastcall Hook_Rnd_DrawPolySimple(void* self, void* edx, zCTexture* tex, zTRndSimpleVertex* verts, int count);
CInvoke<void(__fastcall*)(void*, void*, zCTexture*, zTRndSimpleVertex*, int)> Ivk_Rnd_DrawPolySimple(0x0064AC30, &Hook_Rnd_DrawPolySimple);
void __fastcall Hook_Rnd_DrawPolySimple(void* self, void* edx, zCTexture* tex, zTRndSimpleVertex* verts, int count) {
    // Карта (SELECT): зеркалим ТОЛЬКО квады карты — картинку и всё, что нарисовано после неё в её рамке (маячок).
    if (g_frameMapOpen && verts && count > 0 && count <= 64) {
        float mn = 1e30f, mx = -1e30f, my0 = 1e30f, my1 = -1e30f;
        for (int i = 0; i < count; i++) {
            float x = verts[i].pos.n[0], y = verts[i].pos.n[1];
            if (x < mn) mn = x; if (x > mx) mx = x; if (y < my0) my0 = y; if (y > my1) my1 = y;
        }
        std::string nm = "<null>";
        bool isPage = false;
        if (tex) {
            zSTRING zn = tex->GetObjectName();
            const char* c = zn.ToChar();
            nm = c ? c : "";
            isPage = (c && _strnicmp(c, "MAP", 3) == 0);
            if (!isPage && !g_mapPageSeen) {   // запасной признак: крупный текстурный квад
                zCRnd_D3D* rnd = (zCRnd_D3D*)zrenderer;
                if (rnd && rnd->xd3d_scrWidth > 0 && rnd->xd3d_scrHeight > 0 &&
                    (mx - mn) >= 0.3f * (float)rnd->xd3d_scrWidth && (my1 - my0) >= 0.4f * (float)rnd->xd3d_scrHeight) isPage = true;
            }
        }
        bool hit = false;
        if (isPage) { g_mapL = mn; g_mapR = mx; g_mapPageSeen = true; hit = true; }
        else if (g_mapPageSeen && mn >= g_mapL - 1.0f && mx <= g_mapR + 1.0f) hit = true;
        if (hit) {
            g_selCand++;
            static std::vector<zTRndSimpleVertex> mcopy;
            if (mcopy.size() < (size_t)count) mcopy.resize(count);
            memcpy(&mcopy[0], verts, sizeof(zTRndSimpleVertex) * (size_t)count);
            float cx = g_mapL + g_mapR;
            for (int i = 0; i < count; i++) mcopy[i].pos.n[0] = cx - mcopy[i].pos.n[0];
            g_selApplied++;
            Ivk_Rnd_DrawPolySimple(self, edx, tex, &mcopy[0], count);
            return;
        }
    }
    if (g_devWorldMirror && g_haveVp && verts && count > 0 && count <= 200000) {
        static std::vector<zTRndSimpleVertex> copy;
        if (copy.size() < (size_t)count) copy.resize(count);
        memcpy(&copy[0], verts, sizeof(zTRndSimpleVertex) * (size_t)count);
        float cx = (float)(2 * g_vp.dwX + g_vp.dwWidth);
        for (int i = 0; i < count; i++) copy[i].pos.n[0] = cx - copy[i].pos.n[0];
        g_psMirrored += count;
        Ivk_Rnd_DrawPolySimple(self, edx, tex, &copy[0], count);
        return;
    }
    Ivk_Rnd_DrawPolySimple(self, edx, tex, verts, count);
}

int __fastcall Hook_Rnd_DrawVertexBuffer(void* self, void* edx, zCVertexBuffer* vb, int start, int count, unsigned short* idx, unsigned long fl);
CInvoke<int(__fastcall*)(void*, void*, zCVertexBuffer*, int, int, unsigned short*, unsigned long)> Ivk_Rnd_DrawVertexBuffer(0x006519F0, &Hook_Rnd_DrawVertexBuffer);
int __fastcall Hook_Rnd_DrawVertexBuffer(void* self, void* edx, zCVertexBuffer* vb, int start, int count, unsigned short* idx, unsigned long fl) {
    bool mirrored = false;
    if (g_devWorldMirror && g_haveVp && vb && count > 0 && vb->array.XYZRHWPtr) {
        if (start >= 0 && start + count <= (int)vb->numVertex) {
            float cx = (float)(2 * g_vp.dwX + g_vp.dwWidth);
            zVEC4* p = vb->array.XYZRHWPtr + start;
            for (int i = 0; i < count; i++) p[i].n[0] = cx - p[i].n[0];
            mirrored = true;
            g_vbTlMirrored += count;
        }
    }
    int r = Ivk_Rnd_DrawVertexBuffer(self, edx, vb, start, count, idx, fl);
    if (mirrored) {
        float cx = (float)(2 * g_vp.dwX + g_vp.dwWidth);
        zVEC4* p = vb->array.XYZRHWPtr + start;
        for (int i = 0; i < count; i++) p[i].n[0] = cx - p[i].n[0];
    }
    return r;
}

// ───────────────────────── Карта: детект ─────────────────────────
#if ZM_HOOK_MAP
bool DocDlgOnScreen() {
    if (!screen) return false;
    oCDocumentManager& dm = oCDocumentManager::GetDocumentManager();
    zCViewDialog* dlg = dm.DlgDocumentMain;
    if (!dlg) return false;
    int guard = 0;
    for (zCView* v = screen->childs.root; v && guard < 512; v = v->next, guard++)
        if ((void*)v == (void*)dlg) return true;
    return false;
}

bool DocMgrEnabled() {
    oCDocumentManager& dm = oCDocumentManager::GetDocumentManager();
    return dm.GetEnableHandleEvent() != 0;
}

int MapSignals() {
    int s = 0;
    if (g_mapView) s |= 1;
    if (g_mapDocIsMap) s |= 2;
    if (ogame && screen) {
        if (DocDlgOnScreen()) s |= 4;
        if (DocMgrEnabled()) s |= 8;
    }
    return s;
}

bool MapOnScreenNow() {
    if (!ogame || !screen) return false;
    int s = MapSignals();
    if (!(s & 1) || !(s & 2)) return false;
    return (s & 8) != 0;
}

void __fastcall Hook_MapView_Dtor(zCView* self, void* edx);
CInvoke<void(__fastcall*)(zCView*, void*)> Ivk_MapView_Dtor(0x0068D470, &Hook_MapView_Dtor);
void __fastcall Hook_MapView_Dtor(zCView* self, void* edx) {
    if (g_mapView == self) { g_mapView = NULL; g_mapDocIsMap = false; }
    Ivk_MapView_Dtor(self, edx);
}

int __fastcall Hook_MapScreen_Show(void* self, void* edx, int a);
CInvoke<int(__fastcall*)(void*, void*, int)> Ivk_MapScreen_Show(0x0047F9C0, &Hook_MapScreen_Show);
int __fastcall Hook_MapScreen_Show(void* self, void* edx, int a) {
    bool outer = (g_docShowDepth == 0);
    if (outer) g_mapUpdInShow = false;
    g_docShowDepth++;
    int r = Ivk_MapScreen_Show(self, edx, a);
    g_docShowDepth--;
    if (outer && (g_mapUpdInShow || g_frame - g_mapUpdFrame <= 2)) g_mapDocIsMap = true;
    return r;
}

void __fastcall Hook_DocMan_Show(void* self, void* edx);
CInvoke<void(__fastcall*)(void*, void*)> Ivk_DocMan_Show(0x0065F300, &Hook_DocMan_Show);
void __fastcall Hook_DocMan_Show(void* self, void* edx) {
    int id = (int)(INT_PTR)edx;
    bool outer = (g_docShowDepth == 0);
    if (outer) g_mapUpdInShow = false;
    g_docShowDepth++;
    Ivk_DocMan_Show(self, edx);
    g_docShowDepth--;
    if (!outer) return;
    bool isMap = g_mapUpdInShow || (g_frame - g_mapUpdFrame <= 2);
    oCViewDocument* d = ((oCDocumentManager*)self)->GetDocumentView(id);
    if (!isMap && d && g_mapView && (void*)d == (void*)g_mapView) isMap = true;
    g_mapDocIsMap = isMap;
}

void __fastcall Hook_MapView_UpdatePosition(zCView* self, void* edx);
CInvoke<void(__fastcall*)(zCView*, void*)> Ivk_MapView_UpdatePosition(0x0068D7B0, &Hook_MapView_UpdatePosition);
void __fastcall Hook_MapView_UpdatePosition(zCView* self, void* edx) {
    Ivk_MapView_UpdatePosition(self, edx);
    g_mapView = self;
    g_mapUpdFrame = g_frame;
    if (g_docShowDepth > 0) g_mapUpdInShow = true;
}

// zCRnd_D3D::Vid_Blit — конец кадра (сброс флагов карты)
void __fastcall Hook_Rnd_VidBlit(void* self, void* edx, int a, tagRECT* r1, tagRECT* r2);
CInvoke<void(__fastcall*)(void*, void*, int, tagRECT*, tagRECT*)> Ivk_Rnd_VidBlit(0x00657670, &Hook_Rnd_VidBlit);
void __fastcall Hook_Rnd_VidBlit(void* self, void* edx, int a, tagRECT* r1, tagRECT* r2) {
    g_frameMapOpen = false;
    g_mapPageSeen = false;
    g_selCand = 0;
    Ivk_Rnd_VidBlit(self, edx, a, r1, r2);
}
#else
int MapSignals() { return 0; }
bool MapOnScreenNow() { return false; }
#endif

// ───────────────────────── Инверсия мыши по X вместе с зеркалом ─────────────────────────
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

// ───────────────────────── Класс объекта фокуса (для антисвопа/надписей) ─────────────────────────
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

// ───────────────────────── Клавиши: влево <-> вправо ─────────────────────────
inline bool IsLockpickCaller(DWORD c) {
    return c == 0x0069857D || c == 0x00698593 || c == 0x006985B9 || c == 0x006985CF ||
           c == 0x00723E6D || c == 0x00723E7E;
}

inline unsigned short SwapLR(unsigned short key, DWORD caller) {
    if (!MirrorOn() || !ogame || ogame->IsOnPause()) return key;
    if (zCView::modal) return key;
    if (IsFocusLockableNow() && IsLockpickCaller(caller)) return key;
    switch (key) {
    case GAME_LEFT:        return GAME_RIGHT;
    case GAME_RIGHT:       return GAME_LEFT;
    case GAME_STRAFELEFT:  return GAME_STRAFERIGHT;
    case GAME_STRAFERIGHT: return GAME_STRAFELEFT;
    case GAME_ACTIONLEFT:  return GAME_ACTIONRIGHT;
    case GAME_ACTIONRIGHT: return GAME_ACTIONLEFT;
    default: return key;
    }
}

float __fastcall Hook_Input_GetState(zCInput_Win32* self, void* edx, unsigned short key);
CInvoke<float(__fastcall*)(zCInput_Win32*, void*, unsigned short)> Ivk_Input_GetState(0x004D4D30, &Hook_Input_GetState);
float __fastcall Hook_Input_GetState(zCInput_Win32* self, void* edx, unsigned short key) {
    return Ivk_Input_GetState(self, edx, SwapLR(key, (DWORD)_ReturnAddress()));
}

int __fastcall Hook_Input_GetToggled(zCInput_Win32* self, void* edx, unsigned short key);
CInvoke<int(__fastcall*)(zCInput_Win32*, void*, unsigned short)> Ivk_Input_GetToggled(0x004D5020, &Hook_Input_GetToggled);
int __fastcall Hook_Input_GetToggled(zCInput_Win32* self, void* edx, unsigned short key) {
    return Ivk_Input_GetToggled(self, edx, SwapLR(key, (DWORD)_ReturnAddress()));
}

// ───────────────────────── Хук zCWorld::Render ─────────────────────────
inline bool IsMainWorld(zCWorld* w) {
    return ogame && w == (zCWorld*)ogame->GetGameWorld();
}

void __fastcall Hook_World_Render(zCWorld* self, void* edx, zCCamera& cam);
CInvoke<void(__fastcall*)(zCWorld*, void*, zCCamera&)> Ivk_World_Render(0x00621700, &Hook_World_Render);
void __fastcall Hook_World_Render(zCWorld* self, void* edx, zCCamera& cam) {
    bool main = IsMainWorld(self);
    bool prevUi = g_uiPhase;
    bool prevInMain = g_inMainWorld;
    bool devMirrorStarted = false;
    bool useFlip = FlipModeActive();                        // GPU-FLIP (ванилла)
    bool useProj = (g_mode == MODE_GPU) && !GpuUsesFlip() && g_method == METH_PROJ;
    bool useScFlip = IsScreenFlipActive();                  // FLIP11 (GD3D11)

    if (useScFlip) AcquireD3D11();

    g_uiPhase = false;
    g_worldDepth++;
    if (main) {
        g_frame++;
        g_inMainWorld = true;
        if (g_teleOn) PerfFrame();
        g_frameMapOpen = MirrorOn() && MapOnScreenNow();

        if (useProj) {
            if (EnsureDeviceHooks()) {
                g_devWorldMirror = true;
                g_worldCullFlip = true;
                devMirrorStarted = true;
                SyncCull();
                SyncProj();
                if (g_polyTest != PT_OFF) RsReapply();
            }
            else {
                static bool w = false;
                if (!w) { w = true; Log("GPU(PROJ): device hooks could not be installed - world will NOT be mirrored"); }
            }
        }
    }

    Ivk_World_Render(self, edx, cam);

    // Как и ванильный GPU-FLIP, отражаем уже готовый мир до начала UI-фазы.
    // Не используем Present: там HUD/ImGui уже могут быть частью backbuffer.
    if (main && useScFlip) MirrorFrameFlip11Scene();

    g_worldDepth--;
    g_inMainWorld = prevInMain;
    if (main) {
        if (devMirrorStarted) {
            g_devWorldMirror = false;
            g_worldCullFlip = false;
            SyncCull();
            SyncProj();
            if (g_polyTest != PT_OFF) RsReapply();
        }
        if (useFlip) MirrorFrame(true);
        g_uiPhase = true;
    }
    else g_uiPhase = prevUi;
}

// ───────────────────────── События Union ─────────────────────────
void Game_Entry() {
    WriteText("zMirror_log.txt", "Game_Entry: zMirrorTest V1.7.4 loaded\n", false);
}

void Game_Init() { }

void Game_Exit() { GfRelease(); }

void Game_PreLoop() { }

inline bool KeyEdge(int vk, bool& was) {
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !was;
    was = down;
    return edge;
}

void Msg(const std::string& s) {
    if (screen) screen->PrintCXY(zSTRING(s.c_str()));
}

void Game_Loop() {
    // HOME — переключение зеркала: OFF <-> GPU (бэкенд автоматически)
    static bool wasHome = false;
    if (KeyEdge(VK_HOME, wasHome)) {
        g_mode = (g_mode == MODE_GPU) ? MODE_OFF : MODE_GPU;
        g_failStreak = 0;
        PerfReset();
        SyncMouseFlip();
        Log(std::string("mirror mode: ") + kModeNames[g_mode]);
        if (g_mode == MODE_GPU) {
            if (GpuUsesFlip()) Msg("MIRROR MODE: GPU [FLIP]");
            else { AcquireD3D11(); Msg(SFmt("MIRROR MODE: GPU [%s]", kMethodNames[g_method])); }
        }
        else Msg("MIRROR MODE: OFF");
    }

    // END — телеметрия (PERF)
    static bool wasEnd = false;
    if (KeyEdge(VK_END, wasEnd)) {
        g_teleOn = !g_teleOn;
        Log(g_teleOn ? "telemetry ON" : "telemetry OFF");
        Msg(g_teleOn ? "TELEMETRY: ON" : "TELEMETRY: OFF");
    }

    // [ — метод зеркала на GD3D11: PROJ <-> FLIP11
    static bool wasLB = false;
    if (KeyEdge(VK_OEM_4, wasLB)) {
        g_method = (g_method + 1) % METH_COUNT;
        if (g_method == METH_SCFLIP) {
            AcquireD3D11();
            if (!g_d11.ok) Msg("FLIP11: D3D11 not available (see log)");
        }
        Log(SFmt("method: %s (backend=%s)", kMethodNames[g_method], GpuUsesFlip() ? "FLIP" : "PROJ"));
        Msg(SFmt("METHOD: %s", kMethodNames[g_method]));
    }

    // ] — диагностический тест GD3D11 (PROJ): OFF <-> CULLNONE.
    // FrontCCW не трогаем: D3D7 Hook_Dev_SetRenderState уже инвертирует CULLMODE.
    static bool wasRB = false;
    if (KeyEdge(VK_OEM_6, wasRB)) {
        if (g_method != METH_PROJ) {
            Msg("POLY TEST: applies to PROJ method only");
        }
        else {
            // CULLNONE — диагностический режим без второй инверсии winding.
            g_polyTest = (g_polyTest == PT_OFF) ? PT_CULLNONE : PT_OFF;
            if (g_polyTest != PT_OFF) {
                AcquireD3D11();
                if (!g_d11.ok) { g_polyTest = PT_OFF; Msg("POLY TEST: D3D11 not available (see log)"); }
                else {
                    RsReapply();
                    Log(SFmt("poly test: %s (D3D11 hook=%d backend=%s)", kPolyTestNames[g_polyTest], g_d11Ok ? 1 : 0, GpuUsesFlip() ? "FLIP" : "PROJ"));
                    Msg(SFmt("POLY TEST: %s", kPolyTestNames[g_polyTest]));
                }
            }
            else {
                Log("poly test: OFF");
                Msg("POLY TEST: OFF");
            }
        }
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

// Ранние хуки D3D11CreateDevice — ставим при загрузке DLL (до Game_Entry),
// чтобы поймать создание устройства, если GD3D11 создаёт его позже плагина.
struct ZmEarlyHooksInit {
    ZmEarlyHooksInit() { ZmInstallEarlyHooks(); }
};
static ZmEarlyHooksInit g_earlyHooks;

} // namespace GOTHIC_ENGINE
