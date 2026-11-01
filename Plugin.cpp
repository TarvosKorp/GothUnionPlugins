// ═══════════════════════════════════════════════════════════════════
// zMirrorTest V1.4.1
// Плагин зеркального режима для Gothic 2 NotR (Union SDK, G2A).
//
// УПРАВЛЕНИЕ:
//   HOME   — цикл режимов: OFF → CPU → BLT → GPUv8 → GPUv10 → GPUv11 → OFF.
//   END    — телеметрия ON/OFF (лог zMirror_log.txt + дамп zMirror_dump.txt).
//   ]      — зеркалирование сущностей (все NPC) ON/OFF. Работает в ЛЮБОМ режиме,
//            в том числе при выключенном зеркале (OFF).
//   [      — тест "зеркало всего кадра на present" (для будущего режима карты).
//            Использовать вместе с режимом OFF. Мир + весь интерфейс зеркалятся.
//   INSERT — (при включённой телеметрии) дамп ОДНОГО кадра вызовов D3D7.
//   PAGEUP — (телеметрия) метка сценария: пишет MARK в лог и сбрасывает
//            дедупликацию "кто читает клавиши".
//
// РЕЖИМЫ ЗЕРКАЛА:
//   CPU    — эталон: переворот строк бэкбуфера (Lock + SSE2) после рендера мира.
//   BLT    — то же, но переворот делает DirectDraw (Blt с DDBLTFX_MIRRORLEFTRIGHT
//            через временную поверхность). Результат визуально идентичен CPU.
//            Сам себя проверяет (verify по строке пикселей) и при неудаче
//            автоматически откатывается на CPU-метод.
//   GPUv8  — (старый) отражение проекции в Activate + инверсия CULL в XD3D_SetRenderState.
//   GPUv10 — (старый) отражение трансформаций камеры.
//   GPUv11 — (НОВЫЙ, экспериментальный) зеркало на уровне D3D7-устройства:
//            вертикальная инверсия culling для ВСЕХ вызовов SetRenderState,
//            отражение проекции в SetTransform, зеркалирование pre-transformed
//            (XYZRHW) вершин в DrawPrimitive/DrawIndexedPrimitive.
//
// ОБЩИЕ ПРАВИЛА (для всех режимов):
//   - Своп LR-клавиш и инверсия мыши по X активны только в зеркале.
//   - В модальных окнах (zCView::modal) и в режиме взлома замков своп отключается.
//   - Надписи фокуса (NPC/предметы/Mobsi) зеркалятся по X в виртуальных
//     координатах view с учётом ширины текста (кроме режима "[": там весь кадр).
//
// ИСТОРИЯ ВЕРСИЙ:
//   V1.0 — только горизонтальная инверсия мыши.
//   V1.1 — CPU-отзеркаливание мира и управления в обход интерфейсов.
//   V1.2 — фокусные имена, отзеркаливание сущностей, тесты GPU.
//   V1.3 — взлом сундуков, режимы на HOME, чистка карт/телеметрии.
//   V1.4.0 — телеметрия нового образца (UI/ввод/карта/дамп D3D7), режим BLT,
//            режим GPUv11 (device-level), "]" вне зеркальных режимов +
//            device-level cull-флип для сущностей, тест "[" (present-mirror).
//   V1.4.1 — ФИКС вылета: хук SetRenderTarget стоял на слоте 9 (GetRenderTarget), теперь на 8.
// ═══════════════════════════════════════════════════════════════════

#include "resource.h"
#include <string>
#include <vector>
#include <set>
#include <map>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <intrin.h>
#include <thread>

// Если вылетает при открытии карты — поставь 0 (отключит хуки oCMapScreen::Show и
// oCViewDocumentMap::UpdatePosition в телеметрии).
#ifndef ZM_HOOK_MAP
#define ZM_HOOK_MAP 1
#endif

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
enum { MODE_OFF = 0, MODE_CPU = 1, MODE_BLT = 2, MODE_GPU8 = 3, MODE_GPU10 = 4, MODE_GPU11 = 5, MODE_COUNT = 6 };
const char* const kModeNames[MODE_COUNT] = { "OFF", "CPU", "BLT", "GPUv8", "GPUv10", "GPUv11" };

int  g_mode = MODE_OFF;
bool g_fullFrame = false;      // тест "[": зеркало всего кадра на present
bool g_entMirror = false;      // зеркалирование сущностей (все NPC)
bool g_teleOn = false;         // телеметрия
int  g_failStreak = 0;
int  g_frame = 0;
bool g_inMainWorld = false;
bool g_focusLockable = false;
bool g_uiPhase = false;
int  g_worldDepth = 0;

inline bool MirrorOn() { return g_mode != MODE_OFF || g_fullFrame; }
inline bool IsPostMirrorMode() { return g_mode == MODE_CPU || g_mode == MODE_BLT; }

void SyncMouseFlip();  // ранняя декларация

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
int g_tlMirrored = 0;     // сколько TL-вызовов отзеркалено (GPUv11)
int g_tlVbSkipped = 0;    // сколько TL-вызовов через VB пропущено (GPUv11)

void PerfReset() {
    memset(&g_perf, 0, sizeof(g_perf));
    g_tlMirrored = 0; g_tlVbSkipped = 0;
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
        Log(SFmt("PERF mode=%s ent=%d ff=%d frames=%d avgFrame=%.2f ms (%.1f fps) avgMirrorCall=%.2f ms tlMirrored=%d tlVbSkipped=%d",
            kModeNames[g_mode], g_entMirror ? 1 : 0, g_fullFrame ? 1 : 0, g_perf.frames, avg, 1000.0 / avg, am, g_tlMirrored, g_tlVbSkipped));
        g_perf.frameSum = 0; g_perf.frames = 0; g_perf.mirrorSum = 0; g_perf.mirrorN = 0;
        g_tlMirrored = 0; g_tlVbSkipped = 0;
    }
}

// ───────────────────────── Быстрый переворот строк (SSE2) — CPU-метод ─────────────────────────
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

// ───────────────────────── BLT-метод: зеркало силами DirectDraw ─────────────────────────
IDirectDrawSurface7* g_bltTmp = NULL;
DWORD g_bltW = 0, g_bltH = 0;
bool  g_bltBroken = false;
int   g_bltFails = 0;
bool  g_bltCapsLogged = false;
int   g_bltVerify = 0;        // 0 = ещё не проверено, 1 = зеркалит, 2 = НЕ зеркалит (флаг проигнорирован)
int   g_bltVerifyTries = 0;
bool  g_inMirror = false;     // защита от рекурсии в нашем же Blt-хуке

void ReleaseBltTmp() {
    if (g_bltTmp) { g_bltTmp->Release(); g_bltTmp = NULL; }
    g_bltW = g_bltH = 0;
}

void LogDDCaps(IDirectDraw7* dd) {
    DDCAPS hal, hel;
    ZeroMemory(&hal, sizeof(hal)); ZeroMemory(&hel, sizeof(hel));
    hal.dwSize = sizeof(DDCAPS); hel.dwSize = sizeof(DDCAPS);
    if (FAILED(dd->GetCaps(&hal, &hel))) { Log("DDCAPS: GetCaps failed"); return; }
    Log(SFmt("DDCAPS HAL: caps=0x%08lX fxcaps=0x%08lX mirrorLR=%d | HEL: caps=0x%08lX fxcaps=0x%08lX mirrorLR=%d | vidmem free=%lu total=%lu",
        (unsigned long)hal.dwCaps, (unsigned long)hal.dwFXCaps, (hal.dwFXCaps & DDFXCAPS_BLTMIRRORLEFTRIGHT) ? 1 : 0,
        (unsigned long)hel.dwCaps, (unsigned long)hel.dwFXCaps, (hel.dwFXCaps & DDFXCAPS_BLTMIRRORLEFTRIGHT) ? 1 : 0,
        (unsigned long)hal.dwVidMemFree, (unsigned long)hal.dwVidMemTotal));
}

bool EnsureBltTmp(IDirectDrawSurface7* rt) {
    DDSURFACEDESC2 d; ZeroMemory(&d, sizeof(d)); d.dwSize = sizeof(d);
    if (FAILED(rt->GetSurfaceDesc(&d))) { Log("BLT: GetSurfaceDesc failed"); return false; }
    if (g_bltTmp && g_bltW == d.dwWidth && g_bltH == d.dwHeight) return true;
    ReleaseBltTmp();

    IDirectDraw7* dd = NULL;
    if (FAILED(rt->GetDDInterface((LPVOID*)&dd)) || !dd) { Log("BLT: GetDDInterface failed"); return false; }
    if (!g_bltCapsLogged) { g_bltCapsLogged = true; LogDDCaps(dd); }

    DDSURFACEDESC2 td; ZeroMemory(&td, sizeof(td)); td.dwSize = sizeof(td);
    td.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    td.dwWidth = d.dwWidth; td.dwHeight = d.dwHeight;
    td.ddpfPixelFormat = d.ddpfPixelFormat;
    td.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY;
    HRESULT hr = dd->CreateSurface(&td, &g_bltTmp, NULL);
    const char* where = "VIDEO";
    if (FAILED(hr)) {
        td.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
        hr = dd->CreateSurface(&td, &g_bltTmp, NULL);
        where = "SYSTEM";
    }
    dd->Release();
    if (FAILED(hr) || !g_bltTmp) { Log("BLT: CreateSurface failed " + Hex(hr)); g_bltTmp = NULL; return false; }
    g_bltW = d.dwWidth; g_bltH = d.dwHeight;
    Log(SFmt("BLT: temp surface %lux%lu created in %s memory", (unsigned long)g_bltW, (unsigned long)g_bltH, where));
    return true;
}

bool ReadRow32(IDirectDrawSurface7* s, DWORD y, std::vector<DWORD>& row) {
    DDSURFACEDESC2 d; ZeroMemory(&d, sizeof(d)); d.dwSize = sizeof(d);
    if (FAILED(s->Lock(NULL, &d, DDLOCK_WAIT | DDLOCK_READONLY | DDLOCK_SURFACEMEMORYPTR, NULL))) return false;
    bool ok = false;
    if (d.lpSurface && d.ddpfPixelFormat.dwRGBBitCount == 32 && y < d.dwHeight) {
        const DWORD* src = (const DWORD*)((BYTE*)d.lpSurface + (size_t)y * d.lPitch);
        row.assign(src, src + d.dwWidth);
        ok = true;
    }
    s->Unlock(NULL);
    return ok;
}

// true = кадр зеркален. false = ничего не изменено (или нужен откат на Lock).
bool MirrorByBlt(IDirectDrawSurface7* rt) {
    if (!rt || !EnsureBltTmp(rt)) return false;
    g_inMirror = true;
    bool ok = false;

    std::vector<DWORD> pre, post;
    bool wantVerify = (g_bltVerify == 0 && g_bltVerifyTries < 20);
    DWORD sampleY = g_bltH / 3;
    bool havePre = wantVerify && ReadRow32(rt, sampleY, pre);

    DDBLTFX fx; ZeroMemory(&fx, sizeof(fx)); fx.dwSize = sizeof(fx);
    fx.dwDDFX = DDBLTFX_MIRRORLEFTRIGHT;
    HRESULT hr = g_bltTmp->Blt(NULL, rt, NULL, DDBLT_WAIT | DDBLT_DDFX, &fx);
    if (FAILED(hr)) {
        static int n1 = 0;
        if (n1++ < 3) Log("BLT: mirror Blt (rt->tmp) failed " + Hex(hr));
    }
    else {
        hr = rt->Blt(NULL, g_bltTmp, NULL, DDBLT_WAIT, NULL);
        if (FAILED(hr)) {
            static int n2 = 0;
            if (n2++ < 3) Log("BLT: copy-back Blt (tmp->rt) failed " + Hex(hr));
        }
        else ok = true;
    }

    if (ok && havePre && ReadRow32(rt, sampleY, post) && post.size() == pre.size() && pre.size() > 64) {
        int informative = 0, match = 0;
        DWORD w = (DWORD)pre.size();
        for (DWORD k = 1; k <= 7; k++) {
            DWORD x = w * k / 8, mx = w - 1 - x;
            DWORD a = pre[x] & 0x00FFFFFF, b = pre[mx] & 0x00FFFFFF;
            if (a != b) { informative++; if ((post[mx] & 0x00FFFFFF) == a) match++; }
        }
        if (informative >= 3) {
            g_bltVerify = (match == informative) ? 1 : 2;
            Log(SFmt("BLT verify: informative=%d match=%d => %s", informative, match,
                g_bltVerify == 1 ? "MIRROR WORKS (hardware/driver)" : "blit succeeded but did NOT mirror (flag ignored) -> fallback to CPU"));
            if (g_bltVerify == 2) { g_bltBroken = true; ok = false; }
        }
        else g_bltVerifyTries++;
    }
    g_inMirror = false;
    return ok;
}

// Зеркалит указанную поверхность выбранным методом режима. Вызывать вне сцены (между EndFrame и BeginFrame).
bool MirrorSurfaceNow(IDirectDrawSurface7* rt) {
    bool ok = false;
    if (g_mode == MODE_BLT || g_fullFrame) {
        if (!g_bltBroken) {
            ok = MirrorByBlt(rt);
            if (!ok && ++g_bltFails >= 3 && !g_bltBroken) {
                g_bltBroken = true;
                Log("BLT: 3 failures in a row -> permanent fallback to CPU Lock-method");
            }
            if (ok) g_bltFails = 0;
        }
    }
    if (!ok) ok = MirrorByLock(rt);
    return ok;
}

void MirrorWorldPost() {
    zCRnd_D3D* rnd = (zCRnd_D3D*)zrenderer;
    if (!rnd) return;
    IDirectDrawSurface7* rt = AcquireTarget();
    if (!rt) return;

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);

    rnd->EndFrame();
    bool ok = MirrorSurfaceNow(rt);
    rnd->BeginFrame();
    rt->Release();

    QueryPerformanceCounter(&t1);
    g_perf.mirrorSum += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    g_perf.mirrorN++;

    if (ok) g_failStreak = 0;
    else if (++g_failStreak >= 30) { g_mode = MODE_OFF; SyncMouseFlip(); Log("mirror auto-disabled: 30 failures in a row"); }
}

// ───────────────────────── D3D7-устройство: типы функций и состояние ─────────────────────────
typedef HRESULT (STDMETHODCALLTYPE *PFN_BeginScene)(IDirect3DDevice7*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_EndScene)(IDirect3DDevice7*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetRenderTarget)(IDirect3DDevice7*, IDirectDrawSurface7*, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetTransform)(IDirect3DDevice7*, D3DTRANSFORMSTATETYPE, LPD3DMATRIX);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetViewport)(IDirect3DDevice7*, D3DVIEWPORT7*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetRenderState)(IDirect3DDevice7*, D3DRENDERSTATETYPE, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawPrimitive)(IDirect3DDevice7*, D3DPRIMITIVETYPE, DWORD, LPVOID, DWORD, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawIndexedPrimitive)(IDirect3DDevice7*, D3DPRIMITIVETYPE, DWORD, LPVOID, DWORD, LPWORD, DWORD, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawPrimitiveVB)(IDirect3DDevice7*, D3DPRIMITIVETYPE, void*, DWORD, DWORD, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawIndexedPrimitiveVB)(IDirect3DDevice7*, D3DPRIMITIVETYPE, void*, DWORD, DWORD, LPWORD, DWORD, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SurfBlt)(IDirectDrawSurface7*, LPRECT, IDirectDrawSurface7*, LPRECT, DWORD, LPDDBLTFX);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SurfFlip)(IDirectDrawSurface7*, IDirectDrawSurface7*, DWORD);

PFN_BeginScene            g_oBeginScene = NULL;
PFN_EndScene              g_oEndScene = NULL;
PFN_SetRenderTarget       g_oSetRT = NULL;
PFN_SetTransform          g_oSetTransform = NULL;
PFN_SetViewport           g_oSetViewport = NULL;
PFN_SetRenderState        g_oSetRS = NULL;
PFN_DrawPrimitive         g_oDP = NULL;
PFN_DrawIndexedPrimitive  g_oDIP = NULL;
PFN_DrawPrimitiveVB       g_oDPVB = NULL;
PFN_DrawIndexedPrimitiveVB g_oDIPVB = NULL;
PFN_SurfBlt               g_oSurfBlt = NULL;
PFN_SurfFlip              g_oSurfFlip = NULL;

bool g_devHooked = false;
DWORD g_engCull = D3DCULL_CCW;        // последнее значение CULLMODE, которое запросил ДВИЖОК
D3DMATRIX g_engProj;                  // последняя проекция, которую выставил ДВИЖОК
bool g_haveEngProj = false;
D3DVIEWPORT7 g_vp;                    // последний вьюпорт
bool g_haveVp = false;
IDirectDrawSurface7* g_backBuf = NULL; // указатель бэкбуфера (без AddRef, только для сравнения)

bool g_devWorldMirror = false;        // GPUv11: идёт рендер главного мира в зеркале
bool g_worldCullFlip = false;         // паритет отражений на уровне мира
bool g_entCullFlip = false;           // паритет отражения сущности (матрица NPC)
int  g_presentHits = 0;
int  g_presentMirrorFrame = -1;
int  g_ffFrames = 0;

inline bool CullFlipNow() { return g_worldCullFlip != g_entCullFlip; }

inline DWORD InvertCull(DWORD v) {
    if (v == D3DCULL_CW) return D3DCULL_CCW;
    if (v == D3DCULL_CCW) return D3DCULL_CW;
    return v;
}

inline void MirrorProjCol0(D3DMATRIX& p) {   // D3D: v' = v * M, столбец 0 формирует x'
    p._11 = -p._11; p._21 = -p._21; p._31 = -p._31; p._41 = -p._41;
}

// ───────────────────────── Размер вершины по FVF (константы DX7 заданы числами) ─────────────────────────
inline bool IsTLFvf(DWORD f) { return (f & 0x00E) == 0x004; }   // D3DFVF_XYZRHW

DWORD FvfStride(DWORD f) {
    DWORD s = 0;
    switch (f & 0x00E) {
    case 0x002: s = 12; break;  // XYZ
    case 0x004: s = 16; break;  // XYZRHW
    case 0x006: s = 16; break;  // XYZB1
    case 0x008: s = 20; break;  // XYZB2
    case 0x00A: s = 24; break;  // XYZB3
    case 0x00C: s = 28; break;  // XYZB4
    case 0x00E: s = 32; break;  // XYZB5
    default: return 0;
    }
    if (f & 0x010) s += 12;   // NORMAL
    if (f & 0x020) s += 4;    // RESERVED1 / point size
    if (f & 0x040) s += 4;    // DIFFUSE
    if (f & 0x080) s += 4;    // SPECULAR
    DWORD tc = (f & 0xF00) >> 8;
    if ((f >> 16) == 0) s += tc * 8;
    else {
        for (DWORD i = 0; i < tc; i++) {
            DWORD bits = (f >> (16 + 2 * i)) & 3;
            s += (bits == 0) ? 8 : (bits == 1) ? 12 : (bits == 2) ? 16 : 4;
        }
    }
    return s;
}

std::string FvfStr(DWORD f) {
    std::string s;
    switch (f & 0x00E) {
    case 0x002: s = "XYZ"; break;
    case 0x004: s = "XYZRHW"; break;
    case 0x006: s = "XYZB1"; break;
    case 0x008: s = "XYZB2"; break;
    case 0x00A: s = "XYZB3"; break;
    case 0x00C: s = "XYZB4"; break;
    case 0x00E: s = "XYZB5"; break;
    default: s = "POS?"; break;
    }
    if (f & 0x010) s += "|NORMAL";
    if (f & 0x020) s += "|PSIZE";
    if (f & 0x040) s += "|DIFFUSE";
    if (f & 0x080) s += "|SPECULAR";
    DWORD tc = (f & 0xF00) >> 8;
    if (tc) s += SFmt("|TEX%lu", (unsigned long)tc);
    return s;
}

std::string MatStr(const D3DMATRIX& m) {
    return SFmt("[%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]",
        m._11, m._12, m._13, m._14, m._21, m._22, m._23, m._24,
        m._31, m._32, m._33, m._34, m._41, m._42, m._43, m._44);
}

// ───────────────────────── Дамп одного кадра вызовов D3D7 (телеметрия, INSERT) ─────────────────────────
struct DumpEntry {
    int kind;                 // 0=текст, 1=DP, 2=DIP, 3=DPVB, 4=DIPVB, 12=агрегат SetTransform(WORLD)
    std::string text;
    DWORD prim, fvf, cull, caller;
    char phase;               // W = главный мир, w = другой мир, U = UI/после мира
    bool flip, tlMirrored;
    int calls;
    unsigned long long verts, idx;
    float minX, maxX, minY, maxY, rhw0;
    bool tlSamples;
    DumpEntry() : kind(0), prim(0), fvf(0), cull(0), caller(0), phase('?'), flip(false), tlMirrored(false),
        calls(0), verts(0), idx(0), minX(1e30f), maxX(-1e30f), minY(1e30f), maxY(-1e30f), rhw0(0), tlSamples(false) {}
};

const size_t kDumpCap = 12000;
std::vector<DumpEntry> g_dump;
bool g_dumpRequested = false;
bool g_dumpActive = false;
int  g_dumpNo = 0;
DWORD g_dumpStartTick = 0;
int  g_dumpDropped = 0;
int  g_dumpSurfEvents = 0;
bool g_dumpSawPrint = false;
std::map<void*, DWORD> g_vbFvf;

inline char CurPhase() { return g_inMainWorld ? 'W' : (g_worldDepth > 0 ? 'w' : 'U'); }

void DumpText(const std::string& s) {
    if (!g_dumpActive) return;
    if (g_dump.size() >= kDumpCap) { g_dumpDropped++; return; }
    DumpEntry e; e.kind = 0; e.text = s; e.phase = CurPhase();
    g_dump.push_back(e);
}

void SampleTL(DumpEntry& e, const void* verts, DWORD fvf, DWORD n) {
    DWORD stride = FvfStride(fvf);
    if (!verts || stride < 16 || n == 0 || n > 200000) return;
    for (DWORD i = 0; i < n; i++) {
        const float* p = (const float*)((const BYTE*)verts + (size_t)i * stride);
        if (i == 0 && !e.tlSamples) e.rhw0 = p[3];
        if (p[0] < e.minX) e.minX = p[0];
        if (p[0] > e.maxX) e.maxX = p[0];
        if (p[1] < e.minY) e.minY = p[1];
        if (p[1] > e.maxY) e.maxY = p[1];
        e.tlSamples = true;
    }
}

void DumpDraw(int kind, DWORD prim, DWORD fvf, DWORD vcount, DWORD icount, const void* verts, DWORD caller, bool tlMirrored) {
    if (!g_dumpActive) return;
    bool flip = CullFlipNow();
    char ph = CurPhase();
    if (!g_dump.empty()) {
        DumpEntry& l = g_dump.back();
        if (l.kind == kind && l.prim == prim && l.fvf == fvf && l.cull == g_engCull && l.caller == caller &&
            l.phase == ph && l.flip == flip && l.tlMirrored == tlMirrored) {
            l.calls++; l.verts += vcount; l.idx += icount;
            if ((kind == 1 || kind == 2) && IsTLFvf(fvf)) SampleTL(l, verts, fvf, vcount);
            return;
        }
    }
    if (g_dump.size() >= kDumpCap) { g_dumpDropped++; return; }
    DumpEntry e;
    e.kind = kind; e.prim = prim; e.fvf = fvf; e.cull = g_engCull; e.caller = caller; e.phase = ph;
    e.flip = flip; e.tlMirrored = tlMirrored; e.calls = 1; e.verts = vcount; e.idx = icount;
    if ((kind == 1 || kind == 2) && IsTLFvf(fvf)) SampleTL(e, verts, fvf, vcount);
    g_dump.push_back(e);
}

void DumpTransform(DWORD type, const D3DMATRIX* m) {
    if (!g_dumpActive || !m) return;
    const char* n = (type == 1) ? "WORLD" : (type == 2) ? "VIEW" : (type == 3) ? "PROJ" : "OTHER";
    if (type == 1) {
        if (!g_dump.empty() && g_dump.back().kind == 12 && g_dump.back().phase == CurPhase()) { g_dump.back().calls++; return; }
        if (g_dump.size() >= kDumpCap) { g_dumpDropped++; return; }
        DumpEntry e; e.kind = 12; e.phase = CurPhase(); e.calls = 1;
        e.text = std::string("SetTransform WORLD first=") + MatStr(*m);
        g_dump.push_back(e);
        return;
    }
    DumpText(std::string("SetTransform ") + n + (g_devWorldMirror ? " (device-mirrored: col0 negated)" : "") + " " + MatStr(*m));
}

void StartDump() {
    g_dump.clear();
    g_vbFvf.clear();
    g_dumpDropped = 0; g_dumpSurfEvents = 0; g_dumpSawPrint = false;
    g_dumpRequested = false;
    g_dumpActive = true;
    g_dumpNo++;
    g_dumpStartTick = GetTickCount();
    DumpText(SFmt("viewport: x=%lu y=%lu w=%lu h=%lu (known=%d)", (unsigned long)g_vp.dwX, (unsigned long)g_vp.dwY,
        (unsigned long)g_vp.dwWidth, (unsigned long)g_vp.dwHeight, g_haveVp ? 1 : 0));
    DumpText(SFmt("engine cull=%lu, engine proj=%s", (unsigned long)g_engCull, g_haveEngProj ? MatStr(g_engProj).c_str() : "unknown"));
}

void FinishDump() {
    if (!g_dumpActive) return;
    g_dumpActive = false;

    struct Sum { int tl, xyz, other, vb; unsigned long long tlV, xyzV; } sum[3];
    memset(sum, 0, sizeof(sum));
    for (size_t i = 0; i < g_dump.size(); i++) {
        const DumpEntry& e = g_dump[i];
        if (e.kind < 1 || e.kind > 4) continue;
        int pi = (e.phase == 'W') ? 0 : (e.phase == 'w') ? 1 : 2;
        if (e.kind >= 3) sum[pi].vb += e.calls;
        if (IsTLFvf(e.fvf)) { sum[pi].tl += e.calls; sum[pi].tlV += e.verts; }
        else if ((e.fvf & 0x00E) != 0) { sum[pi].xyz += e.calls; sum[pi].xyzV += e.verts; }
        else sum[pi].other += e.calls;
    }

    static const char* kn[5] = { "", "DP", "DIP", "DPVB", "DIPVB" };
    std::string out;
    out += SFmt("\n===== DUMP #%d  mode=%s ent=%d ff=%d frame=%d entries=%d dropped=%d =====\n",
        g_dumpNo, kModeNames[g_mode], g_entMirror ? 1 : 0, g_fullFrame ? 1 : 0, g_frame, (int)g_dump.size(), g_dumpDropped);
    static const char* pn[3] = { "W (main world)", "w (other worlds)", "U (after world / UI)" };
    for (int p = 0; p < 3; p++)
        out += SFmt("SUMMARY phase %-20s : TL(XYZRHW) calls=%d verts=%llu | XYZ(3D) calls=%d verts=%llu | other=%d | of which VB-calls=%d\n",
            pn[p], sum[p].tl, sum[p].tlV, sum[p].xyz, sum[p].xyzV, sum[p].other, sum[p].vb);
    out += "-----\n";

    for (size_t i = 0; i < g_dump.size(); i++) {
        const DumpEntry& e = g_dump[i];
        if (e.kind == 0) { out += SFmt("[%c]       %s\n", e.phase, e.text.c_str()); continue; }
        if (e.kind == 12) { out += SFmt("[%c]       %s  (x%d calls)\n", e.phase, e.text.c_str(), e.calls); continue; }
        std::string line = SFmt("[%c] %-5s prim=%lu fvf=0x%03lX %-28s calls=%d verts=%llu idx=%llu cull=%lu%s caller=0x%08lX",
            e.phase, kn[e.kind], (unsigned long)e.prim, (unsigned long)e.fvf, FvfStr(e.fvf).c_str(), e.calls, e.verts, e.idx,
            (unsigned long)e.cull, e.flip ? "(FLIPPED)" : "", (unsigned long)e.caller);
        if (e.tlMirrored) line += " TL-MIRRORED";
        if (e.tlSamples) line += SFmt(" sx=[%.1f..%.1f] sy=[%.1f..%.1f] rhw0=%.5f", e.minX, e.maxX, e.minY, e.maxY, e.rhw0);
        out += line + "\n";
    }
    WriteText("zMirror_dump.txt", out, true);
    Log(SFmt("dump #%d written to zMirror_dump.txt (%d entries)", g_dumpNo, (int)g_dump.size()));
    g_dump.clear();
}

// ───────────────────────── Хуки D3D7-устройства ─────────────────────────
// Состояние движка (cull/проекция) отслеживаем на уровне ЛЮБЫХ вызовов устройства.
void SyncCull() {
    IDirect3DDevice7* dev = zCRnd_D3D::xd3d_pd3dDevice7;
    if (!dev || !g_oSetRS) return;
    g_oSetRS(dev, D3DRENDERSTATE_CULLMODE, CullFlipNow() ? InvertCull(g_engCull) : g_engCull);
}

void SyncProj() {
    IDirect3DDevice7* dev = zCRnd_D3D::xd3d_pd3dDevice7;
    if (!dev || !g_oSetTransform || !g_haveEngProj) return;
    D3DMATRIX p = g_engProj;
    if (g_devWorldMirror) MirrorProjCol0(p);
    g_oSetTransform(dev, D3DTRANSFORMSTATE_PROJECTION, &p);
}

HRESULT STDMETHODCALLTYPE Hook_Dev_BeginScene(IDirect3DDevice7* self) {
    if (g_dumpActive) DumpText("device BeginScene");
    return g_oBeginScene(self);
}

HRESULT STDMETHODCALLTYPE Hook_Dev_EndScene(IDirect3DDevice7* self) {
    if (g_dumpActive) DumpText("device EndScene");
    return g_oEndScene(self);
}

HRESULT STDMETHODCALLTYPE Hook_Dev_SetRenderTarget(IDirect3DDevice7* self, IDirectDrawSurface7* s, DWORD fl) {
    if (g_dumpActive) DumpText(SFmt("SetRenderTarget surf=0x%08lX %s flags=0x%lX caller=0x%08lX", (unsigned long)(size_t)s,
        (s == g_backBuf) ? "(== backbuffer)" : "(OTHER surface)", (unsigned long)fl, (unsigned long)(size_t)_ReturnAddress()));
    return g_oSetRT(self, s, fl);
}

HRESULT STDMETHODCALLTYPE Hook_Dev_SetViewport(IDirect3DDevice7* self, D3DVIEWPORT7* vp) {
    if (vp) { g_vp = *vp; g_haveVp = true; }
    if (g_dumpActive && vp) DumpText(SFmt("SetViewport x=%lu y=%lu w=%lu h=%lu", (unsigned long)vp->dwX, (unsigned long)vp->dwY,
        (unsigned long)vp->dwWidth, (unsigned long)vp->dwHeight));
    return g_oSetViewport(self, vp);
}

HRESULT STDMETHODCALLTYPE Hook_Dev_SetTransform(IDirect3DDevice7* self, D3DTRANSFORMSTATETYPE type, LPD3DMATRIX m) {
    if (m && (DWORD)type == 3) {   // D3DTRANSFORMSTATE_PROJECTION
        g_engProj = *m; g_haveEngProj = true;
        DumpTransform((DWORD)type, m);
        if (g_devWorldMirror) {
            D3DMATRIX p = *m;
            MirrorProjCol0(p);
            return g_oSetTransform(self, type, &p);
        }
        return g_oSetTransform(self, type, m);
    }
    if (g_dumpActive) DumpTransform((DWORD)type, m);
    return g_oSetTransform(self, type, m);
}

HRESULT STDMETHODCALLTYPE Hook_Dev_SetRenderState(IDirect3DDevice7* self, D3DRENDERSTATETYPE t, DWORD v) {
    if (t == D3DRENDERSTATE_CULLMODE) {
        g_engCull = v;
        if (g_dumpActive) DumpText(SFmt("SetRenderState CULLMODE engine=%lu -> device=%lu%s caller=0x%08lX", (unsigned long)v,
            (unsigned long)(CullFlipNow() ? InvertCull(v) : v), CullFlipNow() ? " (flipped)" : "", (unsigned long)(size_t)_ReturnAddress()));
        if (CullFlipNow()) v = InvertCull(v);
    }
    return g_oSetRS(self, t, v);
}

// Зеркалирует x у pre-transformed вершин в КОПИИ массива; возвращает указатель на копию (или NULL).
LPVOID MirrorTLCopy(DWORD fvf, LPVOID verts, DWORD count) {
    DWORD stride = FvfStride(fvf);
    if (stride < 16 || !verts || count == 0 || count > 400000) return NULL;
    static std::vector<BYTE> buf;
    size_t bytes = (size_t)stride * count;
    if (buf.size() < bytes) buf.resize(bytes);
    memcpy(&buf[0], verts, bytes);
    float cx = (float)(2 * g_vp.dwX + g_vp.dwWidth);
    for (DWORD i = 0; i < count; i++) {
        float* p = (float*)&buf[(size_t)i * stride];
        p[0] = cx - p[0];
    }
    return &buf[0];
}

HRESULT STDMETHODCALLTYPE Hook_Dev_DrawPrimitive(IDirect3DDevice7* self, D3DPRIMITIVETYPE pt, DWORD fvf, LPVOID verts, DWORD count, DWORD fl) {
    bool mir = g_devWorldMirror && g_haveVp && IsTLFvf(fvf);
    if (g_dumpActive) DumpDraw(1, (DWORD)pt, fvf, count, 0, verts, (DWORD)(size_t)_ReturnAddress(), mir);
    if (mir) {
        LPVOID mv = MirrorTLCopy(fvf, verts, count);
        if (mv) { g_tlMirrored++; return g_oDP(self, pt, fvf, mv, count, fl); }
    }
    return g_oDP(self, pt, fvf, verts, count, fl);
}

HRESULT STDMETHODCALLTYPE Hook_Dev_DrawIndexedPrimitive(IDirect3DDevice7* self, D3DPRIMITIVETYPE pt, DWORD fvf, LPVOID verts, DWORD vcount, LPWORD idx, DWORD icount, DWORD fl) {
    bool mir = g_devWorldMirror && g_haveVp && IsTLFvf(fvf);
    if (g_dumpActive) DumpDraw(2, (DWORD)pt, fvf, vcount, icount, verts, (DWORD)(size_t)_ReturnAddress(), mir);
    if (mir) {
        LPVOID mv = MirrorTLCopy(fvf, verts, vcount);
        if (mv) { g_tlMirrored++; return g_oDIP(self, pt, fvf, mv, vcount, idx, icount, fl); }
    }
    return g_oDIP(self, pt, fvf, verts, vcount, idx, icount, fl);
}

DWORD VbFvf(void* vb) {
    if (!vb) return 0;
    std::map<void*, DWORD>::iterator it = g_vbFvf.find(vb);
    if (it != g_vbFvf.end()) return it->second;
    if (g_vbFvf.size() > 512) g_vbFvf.clear();
    DWORD f = 0;
    D3DVERTEXBUFFERDESC d; ZeroMemory(&d, sizeof(d)); d.dwSize = sizeof(d);
    if (SUCCEEDED(((IDirect3DVertexBuffer7*)vb)->GetVertexBufferDesc(&d))) f = d.dwFVF;
    g_vbFvf[vb] = f;
    return f;
}

HRESULT STDMETHODCALLTYPE Hook_Dev_DrawPrimitiveVB(IDirect3DDevice7* self, D3DPRIMITIVETYPE pt, void* vb, DWORD start, DWORD count, DWORD fl) {
    if (g_dumpActive || g_devWorldMirror) {
        DWORD f = VbFvf(vb);
        if (g_devWorldMirror && IsTLFvf(f)) {
            g_tlVbSkipped++;
            static bool warned = false;
            if (!warned) { warned = true; Log("GPUv11 WARNING: pre-transformed (XYZRHW) VertexBuffer draw encountered - NOT mirrored by this method"); }
        }
        if (g_dumpActive) DumpDraw(3, (DWORD)pt, f, count, 0, NULL, (DWORD)(size_t)_ReturnAddress(), false);
    }
    return g_oDPVB(self, pt, vb, start, count, fl);
}

HRESULT STDMETHODCALLTYPE Hook_Dev_DrawIndexedPrimitiveVB(IDirect3DDevice7* self, D3DPRIMITIVETYPE pt, void* vb, DWORD start, DWORD vcount, LPWORD idx, DWORD icount, DWORD fl) {
    if (g_dumpActive || g_devWorldMirror) {
        DWORD f = VbFvf(vb);
        if (g_devWorldMirror && IsTLFvf(f)) {
            g_tlVbSkipped++;
            static bool warned = false;
            if (!warned) { warned = true; Log("GPUv11 WARNING: pre-transformed (XYZRHW) VertexBuffer draw encountered - NOT mirrored by this method"); }
        }
        if (g_dumpActive) DumpDraw(4, (DWORD)pt, f, vcount, icount, NULL, (DWORD)(size_t)_ReturnAddress(), false);
    }
    return g_oDIPVB(self, pt, vb, start, vcount, idx, icount, fl);
}

// Present: windowed -> Blt(back -> front), fullscreen -> Flip.
void MirrorBackbufferOnPresent() {
    if (!g_backBuf) return;
    if (!(g_fullFrame && MirrorOn())) return;
    if (g_presentMirrorFrame == g_frame) return;   // максимум один раз за кадр движка
    g_presentMirrorFrame = g_frame;
    MirrorSurfaceNow(g_backBuf);
}

HRESULT STDMETHODCALLTYPE Hook_Surf_Blt(IDirectDrawSurface7* self, LPRECT dr, IDirectDrawSurface7* src, LPRECT sr, DWORD fl, LPDDBLTFX fx) {
    if (!g_inMirror) {
        bool present = (src && src == g_backBuf && self != g_backBuf);
        if (g_dumpActive && (present || g_dumpSurfEvents < 120)) {
            g_dumpSurfEvents++;
            DumpText(SFmt("Surface.Blt dest=0x%08lX src=0x%08lX flags=0x%08lX%s%s caller=0x%08lX", (unsigned long)(size_t)self, (unsigned long)(size_t)src,
                (unsigned long)fl, present ? "  <== PRESENT (backbuffer -> other)" : "", (self == g_backBuf) ? "  (dest==backbuffer)" : "",
                (unsigned long)(size_t)_ReturnAddress()));
        }
        if (present) { g_presentHits++; MirrorBackbufferOnPresent(); }
    }
    return g_oSurfBlt(self, dr, src, sr, fl, fx);
}

HRESULT STDMETHODCALLTYPE Hook_Surf_Flip(IDirectDrawSurface7* self, IDirectDrawSurface7* target, DWORD fl) {
    if (!g_inMirror) {
        if (g_dumpActive) DumpText(SFmt("Surface.Flip self=0x%08lX target=0x%08lX flags=0x%08lX  <== PRESENT (flip) caller=0x%08lX",
            (unsigned long)(size_t)self, (unsigned long)(size_t)target, (unsigned long)fl, (unsigned long)(size_t)_ReturnAddress()));
        g_presentHits++;
        MirrorBackbufferOnPresent();
    }
    return g_oSurfFlip(self, target, fl);
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

void RefreshBackBuf() {
    IDirectDrawSurface7* rt = AcquireTarget();
    g_backBuf = rt;
    if (rt) rt->Release();
}

// Устанавливает vtable-хуки на D3D7-устройство и DirectDraw-поверхность. Один раз за сессию.
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
    ok = PatchSlot(vt, 5,  &Hook_Dev_BeginScene,            g_oBeginScene) && ok;
    ok = PatchSlot(vt, 6,  &Hook_Dev_EndScene,              g_oEndScene) && ok;
    ok = PatchSlot(vt, 8,  &Hook_Dev_SetRenderTarget,       g_oSetRT) && ok;   // слот 8 = SetRenderTarget (слот 9 = GetRenderTarget!)
    ok = PatchSlot(vt, 11, &Hook_Dev_SetTransform,          g_oSetTransform) && ok;
    ok = PatchSlot(vt, 13, &Hook_Dev_SetViewport,           g_oSetViewport) && ok;
    ok = PatchSlot(vt, 20, &Hook_Dev_SetRenderState,        g_oSetRS) && ok;
    ok = PatchSlot(vt, 25, &Hook_Dev_DrawPrimitive,         g_oDP) && ok;
    ok = PatchSlot(vt, 26, &Hook_Dev_DrawIndexedPrimitive,  g_oDIP) && ok;
    ok = PatchSlot(vt, 31, &Hook_Dev_DrawPrimitiveVB,       g_oDPVB) && ok;
    ok = PatchSlot(vt, 32, &Hook_Dev_DrawIndexedPrimitiveVB, g_oDIPVB) && ok;

    RefreshBackBuf();
    if (g_backBuf) {
        void** svt = *(void***)g_backBuf;
        ok = PatchSlot(svt, 5,  &Hook_Surf_Blt,  g_oSurfBlt) && ok;
        ok = PatchSlot(svt, 11, &Hook_Surf_Flip, g_oSurfFlip) && ok;
    }
    else ok = false;

    g_devHooked = ok;
    Log(ok ? SFmt("device hooks installed (cull=%lu, viewport %lux%lu)", (unsigned long)g_engCull, (unsigned long)g_vp.dwWidth, (unsigned long)g_vp.dwHeight)
           : std::string("device hooks: FAILED (see slots)"));
    return ok;
}

// ───────────────────────── GPUv8: отражение первой строки проекции ─────────────────────────
void ApplyMirrorProjectionV8() {
    IDirect3DDevice7* dev = zCRnd_D3D::xd3d_pd3dDevice7;
    if (!dev) return;
    D3DMATRIX p;
    if (SUCCEEDED(dev->GetTransform(D3DTRANSFORMSTATE_PROJECTION, &p))) {
        p._11 = -p._11; p._12 = -p._12; p._13 = -p._13; p._14 = -p._14;
        // Обходим наш хук, чтобы g_engProj не запоминал уже отражённую матрицу.
        if (g_oSetTransform) g_oSetTransform(dev, D3DTRANSFORMSTATE_PROJECTION, &p);
        else dev->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &p);
    }
}

// ───────────────────────── GPUv10: отражение трасформаций камеры ─────────────────────────
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
    if (g_mode == MODE_GPU8) ApplyMirrorProjectionV8();
    else if (g_mode == MODE_GPU10) ApplyMirrorView(self);
}

// zCRnd_D3D::XD3D_SetRenderState (используется только старым GPUv8)
int __fastcall Hook_SetRenderState(zCRnd_D3D* self, void* edx, D3DRENDERSTATETYPE t, DWORD v);
CInvoke<int(__fastcall*)(zCRnd_D3D*, void*, D3DRENDERSTATETYPE, DWORD)> Ivk_SetRenderState(0x00644EF0, &Hook_SetRenderState);
int __fastcall Hook_SetRenderState(zCRnd_D3D* self, void* edx, D3DRENDERSTATETYPE t, DWORD v) {
    if (t == D3DRENDERSTATE_CULLMODE && g_inMainWorld && g_mode == MODE_GPU8 && MirrorOn())
        v = InvertCull(v);
    return Ivk_SetRenderState(self, edx, t, v);
}

inline bool IsMainWorld(zCWorld* w) {
    return ogame && w == (zCWorld*)ogame->GetGameWorld();
}

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

// ───────────────────────── Класс объекта (для детекта UI-режимов) ─────────────────────────
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

// ───────────────────────── Кандидаты имени фокуса (кэш на кадр) ─────────────────────────
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

// В режиме "[" (зеркало всего кадра) надписи зеркалятся вместе с кадром — вручную их двигать не нужно.
inline bool ShouldMirrorLabel() {
    return MirrorOn() && !g_fullFrame && g_uiPhase && g_worldDepth == 0;
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

// ───────────────────────── Хуки печати (только зеркалирование надписей) ─────────────────────────
void __fastcall Hook_View_Print(zCView* self, void* edx, int x, int y, zSTRING const& s);
CInvoke<void(__fastcall*)(zCView*, void*, int, int, zSTRING const&)> Ivk_View_Print(0x007A9A40, &Hook_View_Print);
void __fastcall Hook_View_Print(zCView* self, void* edx, int x, int y, zSTRING const& s) {
    if (g_dumpActive && g_uiPhase && !g_dumpSawPrint) { g_dumpSawPrint = true; DumpText("first zCView::Print in UI phase"); }
    MaybeMirrorLabel(self, x, s);
    Ivk_View_Print(self, edx, x, y, s);
}

zCViewText* __fastcall Hook_View_CreateText3(zCView* self, void* edx, int x, int y, zSTRING const& s);
CInvoke<zCViewText*(__fastcall*)(zCView*, void*, int, int, zSTRING const&)> Ivk_View_CreateText3(0x007AA2B0, &Hook_View_CreateText3);
zCViewText* __fastcall Hook_View_CreateText3(zCView* self, void* edx, int x, int y, zSTRING const& s) {
    MaybeMirrorLabel(self, x, s);
    return Ivk_View_CreateText3(self, edx, x, y, s);
}

// ───────────────────────── Телеметрия: кто читает клавиши ─────────────────────────
// Дедупликация по (адрес вызывающего, клавиша, флаги). PAGEUP очищает набор и пишет MARK.
std::set<unsigned long long> g_inputSeen;
int g_mark = 0;

std::string ModalName() {
    return zCView::modal ? ClassOf((zCObject*)zCView::modal) : std::string("none");
}

void TeleInput(unsigned short key, DWORD caller) {
    unsigned long long flags = (zCView::modal ? 1ULL : 0ULL) | ((ogame && ogame->IsOnPause()) ? 2ULL : 0ULL);
    unsigned long long k = ((unsigned long long)caller << 32) | ((unsigned long long)key << 8) | flags;
    if (g_inputSeen.size() > 3000) return;
    if (!g_inputSeen.insert(k).second) return;
    Log(SFmt("INPUT mark=%d caller=0x%08lX key=%u modal=%s pause=%d", g_mark, (unsigned long)caller, (unsigned)key,
        ModalName().c_str(), (ogame && ogame->IsOnPause()) ? 1 : 0));
}

// ───────────────────────── Клавиши: влево <-> вправо ─────────────────────────
// Вызывающие коды режима взлома/навигации по замку (найдены телеметрией ранее).
inline bool IsLockpickCaller(DWORD c) {
    return c == 0x0069857D || c == 0x00698593 || c == 0x006985B9 || c == 0x006985CF ||
           c == 0x00723E6D || c == 0x00723E7E;
}

inline unsigned short SwapLR(unsigned short key, DWORD caller) {
    if (g_teleOn) TeleInput(key, caller);
    if (!MirrorOn() || !ogame || ogame->IsOnPause()) return key;
    if (zCView::modal) return key;   // инвентарь, торговля и т.п.
    if (IsFocusLockableNow() && IsLockpickCaller(caller)) return key;   // взлом замков — без свопа
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

// ───────────────────────── Телеметрия карты ─────────────────────────
#if ZM_HOOK_MAP
int g_mapShowCount = 0;
int g_mapUpdCount = 0;

// oCMapScreen::Show(int)
int __fastcall Hook_MapScreen_Show(void* self, void* edx, int a);
CInvoke<int(__fastcall*)(void*, void*, int)> Ivk_MapScreen_Show(0x0047F9C0, &Hook_MapScreen_Show);
int __fastcall Hook_MapScreen_Show(void* self, void* edx, int a) {
    DWORD caller = (DWORD)_ReturnAddress();
    g_mapShowCount++;
    if (g_teleOn) Log(SFmt("MAP oCMapScreen::Show(arg=%d) BEGIN self=0x%08lX caller=0x%08lX frame=%d modal=%s pause=%d mode=%s ff=%d",
        a, (unsigned long)(size_t)self, (unsigned long)caller, g_frame, ModalName().c_str(), (ogame && ogame->IsOnPause()) ? 1 : 0, kModeNames[g_mode], g_fullFrame ? 1 : 0));
    DumpText(SFmt("EVENT oCMapScreen::Show(arg=%d) BEGIN", a));
    int r = Ivk_MapScreen_Show(self, edx, a);
    if (g_teleOn) Log(SFmt("MAP oCMapScreen::Show(arg=%d) END   frame=%d modal=%s pause=%d", a, g_frame, ModalName().c_str(), (ogame && ogame->IsOnPause()) ? 1 : 0));
    DumpText(SFmt("EVENT oCMapScreen::Show(arg=%d) END", a));
    return r;
}

// oCViewDocumentMap::UpdatePosition()
void __fastcall Hook_MapView_UpdatePosition(zCView* self, void* edx);
CInvoke<void(__fastcall*)(zCView*, void*)> Ivk_MapView_UpdatePosition(0x0068D7B0, &Hook_MapView_UpdatePosition);
void __fastcall Hook_MapView_UpdatePosition(zCView* self, void* edx) {
    g_mapUpdCount++;
    if (g_teleOn && g_mapUpdCount <= 60) Log(SFmt("MAP oCViewDocumentMap::UpdatePosition #%d class=%s self=0x%08lX caller=0x%08lX frame=%d modal=%s",
        g_mapUpdCount, ClassOf((zCObject*)self).c_str(), (unsigned long)(size_t)self, (unsigned long)(size_t)_ReturnAddress(), g_frame, ModalName().c_str()));
    DumpText("EVENT oCViewDocumentMap::UpdatePosition");
    Ivk_MapView_UpdatePosition(self, edx);
}
#endif

// ───────────────────────── Зеркалирование сущностей (все NPC) ─────────────────────────
bool IsNpcVob(zCVob* v) {
    if (!v) return false;
    zCClassDef* cd = v->_GetClassDef();
    if (!cd) return false;
    static std::map<zCClassDef*, bool> cache;   // классы стабильны — кэшируем результат по указателю
    std::map<zCClassDef*, bool>::iterator it = cache.find(cd);
    if (it != cache.end()) return it->second;
    bool r = false;
    zCClassDef* d = cd;
    while (d) {
        const char* cn = d->className.ToChar();
        if (cn && strcmp(cn, "oCNpc") == 0) { r = true; break; }
        d = d->baseClassDef;
    }
    cache[cd] = r;
    return r;
}

// zCVob::Render(zTRenderContext&)
int __fastcall Hook_Vob_Render(zCVob* self, zTRenderContext& ctx);
CInvoke<int(__fastcall*)(zCVob*, zTRenderContext&)> Ivk_Vob_Render(0x006015D0, &Hook_Vob_Render);
int __fastcall Hook_Vob_Render(zCVob* self, zTRenderContext& ctx) {
    // V1.4: работает и при выключенном зеркале мира (OFF).
    bool entApply = g_entMirror && IsNpcVob(self);
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

    // Отражение матрицы меняет намотку полигонов -> инвертируем culling на уровне устройства
    // на время отрисовки этой сущности (устройство видит ВСЕ SetRenderState, в отличие от XD3D-хука).
    bool flip = g_devHooked || EnsureDeviceHooks();
    if (flip) { g_entCullFlip = true; SyncCull(); }

    int r = Ivk_Vob_Render(self, ctx);

    if (flip) { g_entCullFlip = false; SyncCull(); }
    memcpy(&self->trafoObjToWorld, orig, sizeof(orig));
    return r;
}

// ───────────────────────── Хук zCWorld::Render ─────────────────────────
void __fastcall Hook_World_Render(zCWorld* self, void* edx, zCCamera& cam);
CInvoke<void(__fastcall*)(zCWorld*, void*, zCCamera&)> Ivk_World_Render(0x00621700, &Hook_World_Render);
void __fastcall Hook_World_Render(zCWorld* self, void* edx, zCCamera& cam) {
    bool main = IsMainWorld(self);
    bool prevUi = g_uiPhase;
    bool prevInMain = g_inMainWorld;
    bool devMirrorStarted = false;

    g_uiPhase = false;
    g_worldDepth++;
    if (main) {
        if (g_dumpActive) FinishDump();        // предыдущий кадр (мир + его UI) завершён
        g_frame++;
        g_inMainWorld = true;
        if (g_dumpRequested) StartDump();
        if (g_teleOn) PerfFrame();
        if (g_devHooked) RefreshBackBuf();
        if (g_fullFrame && MirrorOn()) {
            g_ffFrames++;
            if (g_ffFrames == 180 && g_presentHits == 0) Log("WARNING: '[' full-frame mirror is ON but present (Surface Blt/Flip from backbuffer) was never detected");
        }
        DumpText(SFmt("main World_Render BEGIN (frame %d, mode %s)", g_frame, kModeNames[g_mode]));

        if (g_mode == MODE_GPU11) {
            if (EnsureDeviceHooks()) {
                g_devWorldMirror = true;
                g_worldCullFlip = true;
                devMirrorStarted = true;
                SyncCull();
                SyncProj();
            }
            else {
                static bool w = false;
                if (!w) { w = true; Log("GPUv11: device hooks could not be installed - world will NOT be mirrored"); }
            }
        }
    }

    Ivk_World_Render(self, edx, cam);

    g_worldDepth--;
    g_inMainWorld = prevInMain;
    if (main) {
        if (devMirrorStarted) {
            g_devWorldMirror = false;
            g_worldCullFlip = false;
            SyncCull();
            SyncProj();
        }
        DumpText("main World_Render END");
        // В режиме "[" мир НЕ зеркалим здесь: весь кадр зеркалится один раз на present.
        if (!g_fullFrame && IsPostMirrorMode()) MirrorWorldPost();
        g_uiPhase = true;
    }
    else g_uiPhase = prevUi;
}

// ───────────────────────── События Union ─────────────────────────
void Game_Entry() {
    WriteText("zMirror_log.txt", "Game_Entry: zMirrorTest V1.4.1 loaded\n", false);
    WriteText("zMirror_dump.txt", "zMirrorTest V1.4.1 dump file\n", false);
}

void Game_Init() { }

void Game_Exit() { ReleaseBltTmp(); }

void Game_PreLoop() { DumpText("event Game_PreLoop"); }

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
    DumpText("event Game_Loop");

    // HOME — цикл режимов: OFF -> CPU -> BLT -> GPUv8 -> GPUv10 -> GPUv11 -> OFF
    static bool wasHome = false;
    if (KeyEdge(VK_HOME, wasHome)) {
        g_mode = (g_mode + 1) % MODE_COUNT;
        g_failStreak = 0;
        PerfReset();
        SyncMouseFlip();
        Log(std::string("mirror mode: ") + kModeNames[g_mode]);
        Msg(SFmt("MIRROR MODE: %s", kModeNames[g_mode]));
    }

    // END — телеметрия
    static bool wasEnd = false;
    if (KeyEdge(VK_END, wasEnd)) {
        g_teleOn = !g_teleOn;
        Log(g_teleOn ? "telemetry ON" : "telemetry OFF");
        Msg(g_teleOn ? "TELEMETRY: ON" : "TELEMETRY: OFF");
    }

    // ] — зеркалирование сущностей (все NPC), в любом режиме
    static bool wasRB = false;
    if (KeyEdge(VK_OEM_6, wasRB)) {
        g_entMirror = !g_entMirror;
        if (g_entMirror) EnsureDeviceHooks();
        Log(std::string("entMirror=") + (g_entMirror ? "ON (ALL NPCs)" : "OFF"));
        Msg(g_entMirror ? "ENT-MIRROR: ALL NPCs" : "ENT-MIRROR: OFF");
    }

    // [ — тест зеркала всего кадра на present (мир + интерфейс), для карты
    static bool wasLB = false;
    if (KeyEdge(VK_OEM_4, wasLB)) {
        g_fullFrame = !g_fullFrame;
        g_presentHits = 0; g_ffFrames = 0; g_presentMirrorFrame = -1;
        if (g_fullFrame) EnsureDeviceHooks();
        SyncMouseFlip();
        Log(std::string("fullFrame(present-mirror)=") + (g_fullFrame ? "ON" : "OFF") + SFmt(" (mode=%s)", kModeNames[g_mode]));
        Msg(g_fullFrame ? "FULL-FRAME MIRROR: ON" : "FULL-FRAME MIRROR: OFF");
    }

    // INSERT — дамп одного кадра D3D7 (только при включённой телеметрии)
    static bool wasIns = false;
    if (KeyEdge(VK_INSERT, wasIns)) {
        if (!g_teleOn) Msg("ENABLE TELEMETRY FIRST (END)");
        else if (!EnsureDeviceHooks()) Msg("DUMP: device hooks failed");
        else { g_dumpRequested = true; Msg("DUMP: armed (1 frame)"); }
    }

    // PAGEUP — метка сценария
    static bool wasPgUp = false;
    if (KeyEdge(VK_PRIOR, wasPgUp)) {
        g_mark++;
        g_inputSeen.clear();
        Log(SFmt("=== MARK %d === mode=%s ent=%d ff=%d modal=%s pause=%d frame=%d", g_mark, kModeNames[g_mode], g_entMirror ? 1 : 0,
            g_fullFrame ? 1 : 0, ModalName().c_str(), (ogame && ogame->IsOnPause()) ? 1 : 0, g_frame));
        Msg(SFmt("MARK %d", g_mark));
    }

    // Страховка: дамп, который не успел закончиться (например, открыто меню)
    if (g_dumpActive && GetTickCount() - g_dumpStartTick > 3000) FinishDump();

    // Детектинг изменения UI-состояния (только при телеметрии)
    static zCView* lastModal = NULL;
    static int lastPause = -1;
    zCView* curModal = zCView::modal;
    int curPause = (ogame && ogame->IsOnPause()) ? 1 : 0;
    if (curModal != lastModal) {
        if (g_teleOn) {
            if (curModal) Log(SFmt("UI modal opened: %s ptr=0x%08lX mark=%d frame=%d", ClassOf((zCObject*)curModal).c_str(), (unsigned long)(size_t)curModal, g_mark, g_frame));
            else Log(SFmt("UI modal closed mark=%d frame=%d", g_mark, g_frame));
        }
        lastModal = curModal;
    }
    if (curPause != lastPause) {
        if (g_teleOn && lastPause != -1) Log(SFmt("UI pause=%d mark=%d frame=%d modal=%s", curPause, g_mark, g_frame, ModalName().c_str()));
        lastPause = curPause;
    }
}

void Game_PostLoop() { DumpText("event Game_PostLoop"); }

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
