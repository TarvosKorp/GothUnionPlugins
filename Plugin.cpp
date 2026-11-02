// ═══════════════════════════════════════════════════════════════════
// zMirrorTest V1.5.0
// Плагин зеркального режима для Gothic 2 NotR (Union SDK, G2A).
//
// УПРАВЛЕНИЕ:
//   HOME     — цикл режимов: OFF → CPU → GPU → OFF.
//   END      — телеметрия ON/OFF (лог zMirror_log.txt + дамп zMirror_dump.txt).
//   ]        — зеркалирование сущностей (все NPC) ON/OFF (любой режим).
//   [        — принудительная инверсия полигонов/culling ON/OFF (для GD3D11).
//   DELETE   — бэкенд GPU-режима: AUTO → FLIP → PROJ → AUTO.
//              AUTO: ванилла = FLIP, внешний ddraw-враппер (GD3D11/dgVoodoo) = PROJ.
//   PAGEDOWN — режим детекта карты: AUTO(строгий) → LENIENT → DOCONLY → FORCE_ON → FORCE_OFF.
//   INSERT   — (телеметрия включена) дамп ОДНОГО кадра.
//   PAGEUP   — метка сценария в логе.
//
// РЕЖИМЫ ЗЕРКАЛА:
//   CPU  — переворот строк бэкбуфера (Lock + SSE2).
//   GPU  — два бэкенда:
//     FLIP (новое в 1.5.0, по умолчанию в ванилле) — переворот ГОТОВОГО кадра на
//        видеокарте: копия RT в текстуру (DirectDraw Blt) + полноэкранный TL-квад с
//        зеркальными UV. Результат пиксель-в-пиксель как у CPU (мир, скайбокс, ВСЕ PFX,
//        тени, облака), но без Lock и без потерь FPS. Геометрию/намотку полигонов
//        не трогает → ничего не вывернуто. Есть самокалибровка (проверка по пикселям)
//        и автоматический откат на CPU, если GPU-путь не работает.
//     PROJ (наследие v1.4.x) — отражение столбца 0 проекции + инверсия CULLMODE +
//        зеркалирование TL-вершин. Нужен для GD3D11/dgVoodoo (там работает штатно).
//
// КАРТА (новое в 1.5.0):
//   Пока открыта карта (M-карта или карта-предмет), кадр зеркалится ЦЕЛИКОМ в конце
//   кадра (хук zCRnd_D3D::Vid_Blit) вместо зеркалирования сразу после мира:
//   мир (не отзеркаленный в этом кадре) + картинка карты + маячок отражаются вместе,
//   положение маячка на карте остаётся верным. Работает в CPU и GPU-FLIP.
//
// ОБЩИЕ ПРАВИЛА (для всех режимов):
//   - Своп LR-клавиш и инверсия мыши по X активны только в зеркале.
//   - В модальных окнах (zCView::modal) и в режиме взлома замков своп отключается.
//   - Надписи фокуса (NPC/предметы/Mobsi) зеркалятся по X в виртуальных координатах.
//
// ИСТОРИЯ ВЕРСИЙ:
//   V1.0 — только горизонтальная инверсия мыши.
//   V1.1 — CPU-отзеркаливание мира и управления в обход интерфейсов.
//   V1.2 — фокусные имена, отзеркаливание сущностей, тесты GPU.
//   V1.3 — взлом сундуков, режимы на HOME, чистка карт/телеметрии.
//   V1.4.x — GPU device-level (проекция+cull+TL), телеметрия, чистка.
//   V1.5.0 — GPU-FLIP (переворот кадра на GPU), зеркалирование карты в конце кадра,
//            детект бэкенда, расширенная телеметрия (GPUFLIP/MAPSIG).
// ═══════════════════════════════════════════════════════════════════

#include "resource.h"
#include <string>
#include <vector>
#include <map>
#include <utility>
#include <functional>
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
enum { MODE_OFF = 0, MODE_CPU = 1, MODE_GPU = 2, MODE_COUNT = 3 };
const char* const kModeNames[MODE_COUNT] = { "OFF", "CPU", "GPU" };

int  g_mode = MODE_OFF;
bool g_entMirror = false;      // зеркалирование сущностей (все NPC)
bool g_teleOn = false;         // телеметрия
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
    bool gdDir = GetFileAttributesA(ExeDirPath("GD3D11").c_str()) != INVALID_FILE_ATTRIBUTES;
    Log(SFmt("DETECT ddraw.dll path='%s' windowsDir='%s' wrapper=%d GD3D11dir=%d", path, win, g_wrapperDetected ? 1 : 0, gdDir ? 1 : 0));
}

// true: GPU-режим работает как FLIP (переворот кадра), false: как PROJ (проекция+cull+TL)
inline bool GpuUsesFlip() {
    DetectWrapper();
    return g_backendPref == BK_FLIP || (g_backendPref == BK_AUTO && !g_wrapperDetected);
}
// true: активный режим переворачивает готовый кадр (CPU или GPU-FLIP)
inline bool FlipModeActive() { return g_mode == MODE_CPU || (g_mode == MODE_GPU && GpuUsesFlip()); }

// Состояние «финального» переворота кадра (карта открыта)
bool g_frameFinalFlip = false;        // решение, принятое при рендере мира в этом кадре
bool g_worldRenderedThisFrame = false;

// Forward-декларации (определены ниже)
int MapSignals();
bool MapOnScreenNow();
void MirrorFrame(bool inScene);

// ───────────────────────── Карта: ранние глобалы детекта ─────────────────────────
#ifndef ZM_HOOK_MAP
#define ZM_HOOK_MAP 1
#endif

zCView* g_mapView = NULL;   // последний открытый oCViewDocumentMap
int g_mapUpdCount = 0;
int g_finalFlips = 0;        // сколько раз сделан финальный переворот (для лога)

// Детект открытой карты (режимы PAGEDOWN)
enum { MAPD_AUTO = 0, MAPD_LENIENT = 1, MAPD_DOCONLY = 2, MAPD_ON = 3, MAPD_OFF = 4, MAPD_COUNT = 5 };
const char* const kMapDetNames[MAPD_COUNT] = { "AUTO", "LENIENT", "DOCONLY", "FORCE_ON", "FORCE_OFF" };
int  g_mapDet = MAPD_AUTO;
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
int g_dumpPS = 0;          // дамп: всего вершин DrawPolySimple за кадр
int g_dumpVBTL = 0;        // дамп: всего TL-вершин DrawVertexBuffer за кадр
float g_psMinX = 1e30f, g_psMaxX = -1e30f;      // диапазон X у DrawPolySimple (мир)
float g_vbTlMinX = 1e30f, g_vbTlMaxX = -1e30f;  // диапазон X у TL-DrawVertexBuffer (мир)

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
        Log(SFmt("PERF mode=%s backend=%s ent=%d frames=%d avgFrame=%.2f ms (%.1f fps) avgMirrorCall=%.2f ms psMirrored=%d vbTlMirrored=%d gf(state=%d ok=%ld)",
            kModeNames[g_mode], g_mode == MODE_GPU ? (GpuUsesFlip() ? "FLIP" : "PROJ") : "-", g_entMirror ? 1 : 0, g_perf.frames, avg, 1000.0 / avg, am,
            g_psMirrored, g_vbTlMirrored, g_gf.state, g_gf.framesOk));
        g_perf.frameSum = 0; g_perf.frames = 0; g_perf.mirrorSum = 0; g_perf.mirrorN = 0;
        g_psMirrored = 0; g_vbTlMirrored = 0;
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

// ───────────────────────── D3D7-устройство: типы функций и состояние ─────────────────────────
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetTransform)(IDirect3DDevice7*, D3DTRANSFORMSTATETYPE, LPD3DMATRIX);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetViewport)(IDirect3DDevice7*, D3DVIEWPORT7*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetRenderState)(IDirect3DDevice7*, D3DRENDERSTATETYPE, DWORD);

PFN_SetTransform          g_oSetTransform = NULL;
PFN_SetViewport           g_oSetViewport = NULL;
PFN_SetRenderState        g_oSetRS = NULL;

bool g_devHooked = false;
DWORD g_engCull = D3DCULL_CCW;        // последнее значение CULLMODE, которое запросил ДВИЖОК
D3DMATRIX g_engProj;                  // последняя проекция, которую выставил ДВИЖОК
bool g_haveEngProj = false;
D3DVIEWPORT7 g_vp;                    // последний вьюпорт
bool g_haveVp = false;

bool g_devWorldMirror = false;        // GPU-PROJ: идёт рендер главного мира в зеркале
bool g_worldCullFlip = false;         // паритет отражений на уровне мира
bool g_entCullFlip = false;           // паритет отражения сущности (матрица NPC)
bool g_polyFlip = false;              // «[»: принудительная инверсия culling (для GD3D11)

inline bool CullFlipNow() { return (g_worldCullFlip != g_entCullFlip) != g_polyFlip; }

inline DWORD InvertCull(DWORD v) {
    if (v == D3DCULL_CW) return D3DCULL_CCW;
    if (v == D3DCULL_CCW) return D3DCULL_CW;
    return v;
}

inline void MirrorProjCol0(D3DMATRIX& p) {   // D3D: v' = v * M, столбец 0 формирует x'
    p._11 = -p._11; p._21 = -p._21; p._31 = -p._31; p._41 = -p._41;
}

std::string MatStr(const D3DMATRIX& m) {
    return SFmt("[%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]",
        m._11, m._12, m._13, m._14, m._21, m._22, m._23, m._24,
        m._31, m._32, m._33, m._34, m._41, m._42, m._43, m._44);
}

// ───────────────────────── Дамп одного кадра (телеметрия, INSERT) ─────────────────────────
struct DumpEntry {
    int kind;                 // 0=текст, 12=SetTransform(WORLD), 13=SetTransform(PROJ/OTHER)
    std::string text;
    char phase;               // W = главный мир, w = другой мир, U = UI/после мира
    int calls;
    DumpEntry() : kind(0), phase('?'), calls(0) {}
};

const size_t kDumpCap = 256;   // лимит ДЕТАЛЬНЫХ записей; дальше — только агрегаты
std::vector<DumpEntry> g_dump;
bool g_dumpRequested = false;
bool g_dumpActive = false;
int  g_dumpNo = 0;
DWORD g_dumpStartTick = 0;
int  g_dumpDropped = 0;
bool g_dumpSawPrint = false;

long g_aggWorldExtra[3];          // SetTransform WORLD сверх деталей
long g_aggProjOtherExtra[3][2];   // [фаза][0=PROJ, 1=OTHER] сверх деталей
long g_worldDetailN = 0;          // сколько детальных WORLD-матриц уже записано
long g_projOtherDetailN[3][2];
std::map<DWORD, int> g_dumpCallers;  // адрес вызывающего -> сколько вызовов

inline char CurPhase() { return g_inMainWorld ? 'W' : (g_worldDepth > 0 ? 'w' : 'U'); }

void DumpText(const std::string& s) {
    if (!g_dumpActive) return;
    if (g_dump.size() >= kDumpCap) { g_dumpDropped++; return; }
    DumpEntry e; e.kind = 0; e.text = s; e.phase = CurPhase();
    g_dump.push_back(e);
}

void DumpTransform(DWORD type, const D3DMATRIX* m) {
    if (!g_dumpActive || !m) return;
    int pi = (g_inMainWorld ? 0 : (g_worldDepth > 0 ? 1 : 2));
    const char* n = (type == 1) ? "WORLD" : (type == 2) ? "VIEW" : (type == 3) ? "PROJ" : "OTHER";
    if (type == 1) {
        if (g_dump.size() >= kDumpCap || g_worldDetailN >= 12) { g_aggWorldExtra[pi]++; return; }
        g_worldDetailN++;
        DumpEntry e; e.kind = 12; e.phase = CurPhase(); e.calls = 1;
        e.text = std::string("SetTransform WORLD first=") + MatStr(*m);
        g_dump.push_back(e);
        return;
    }
    int tj = (type == 3) ? 0 : 1;
    if (g_dump.size() >= kDumpCap || g_projOtherDetailN[pi][tj] >= 6) { g_aggProjOtherExtra[pi][tj]++; return; }
    g_projOtherDetailN[pi][tj]++;
    std::string t = std::string("SetTransform ") + n + (g_devWorldMirror ? " (device-mirrored: col0 negated)" : "") + " " + MatStr(*m);
    DumpEntry e; e.kind = 13; e.phase = CurPhase(); e.calls = 1; e.text = t;
    g_dump.push_back(e);
}

void StartDump() {
    g_dump.clear();
    g_dumpDropped = 0; g_dumpSawPrint = false;
    g_dumpPS = 0; g_dumpVBTL = 0;
    g_psMinX = 1e30f; g_psMaxX = -1e30f;
    g_vbTlMinX = 1e30f; g_vbTlMaxX = -1e30f;
    memset(g_aggWorldExtra, 0, sizeof(g_aggWorldExtra));
    memset(g_aggProjOtherExtra, 0, sizeof(g_aggProjOtherExtra));
    g_worldDetailN = 0;
    memset(g_projOtherDetailN, 0, sizeof(g_projOtherDetailN));
    g_dumpCallers.clear();
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

    std::string out;
    out += SFmt("\n===== DUMP #%d  mode=%s ent=%d frame=%d entries=%d dropped=%d =====\n",
        g_dumpNo, kModeNames[g_mode], g_entMirror ? 1 : 0, g_frame, (int)g_dump.size(), g_dumpDropped);
    out += "-----\n";
    out += SFmt("ENGINE this frame: DrawPolySimple verts=%d x=[%.0f..%.0f] | DrawVertexBuffer-TL verts=%d x=[%.0f..%.0f]\n",
        g_dumpPS, g_psMinX, g_psMaxX, g_dumpVBTL, g_vbTlMinX, g_vbTlMaxX);
    out += SFmt("BACKEND mode=%s pref=%s usesFlip=%d wrapper=%d | GPUFLIP state=%d adj=(%d,%d) tex=%lux%lu framesOk=%ld | MAP det=%s sig=0x%X finalFlip=%d finalFlipsTotal=%d\n",
        kModeNames[g_mode], kBackendNames[g_backendPref], GpuUsesFlip() ? 1 : 0, g_wrapperDetected ? 1 : 0,
        g_gf.state, g_gf.adjX, g_gf.adjY, (unsigned long)g_gf.texW, (unsigned long)g_gf.texH, g_gf.framesOk,
        kMapDetNames[g_mapDet], MapSignals(), g_frameFinalFlip ? 1 : 0, g_finalFlips);
    {
        long wE = 0, pE = 0, oE = 0;
        for (int p = 0; p < 3; p++) { wE += g_aggWorldExtra[p]; pE += g_aggProjOtherExtra[p][0]; oE += g_aggProjOtherExtra[p][1]; }
        out += SFmt("SetTransform (not detailed): WORLD=%ld PROJ=%ld OTHER=%ld\n", wE, pE, oE);
    }
    if (g_dumpDropped > 0) out += SFmt("TRUNCATED details (budget %u entries) — aggregates above are full\n", (unsigned)kDumpCap);
    if (!g_dumpCallers.empty()) {
        out += "TOP CALLERS (draw/engine, by calls):\n";
        std::vector<std::pair<int, DWORD> > v;
        for (std::map<DWORD, int>::iterator it = g_dumpCallers.begin(); it != g_dumpCallers.end(); ++it)
            v.push_back(std::make_pair(it->second, it->first));
        std::sort(v.begin(), v.end(), std::greater<std::pair<int, DWORD> >());
        int n = (int)v.size(); if (n > 20) n = 20;
        for (int i = 0; i < n; i++)
            out += SFmt("  0x%08lX x%d\n", (unsigned long)v[i].second, v[i].first);
    }
    out += "-----\n";

    for (size_t i = 0; i < g_dump.size(); i++) {
        const DumpEntry& e = g_dump[i];
        if (e.kind == 0) { out += SFmt("[%c]       %s\n", e.phase, e.text.c_str()); continue; }
        out += SFmt("[%c]       %s  (x%d calls)\n", e.phase, e.text.c_str(), e.calls);
    }
    WriteText("zMirror_dump.txt", out, true);
    Log(SFmt("dump #%d written to zMirror_dump.txt (%d entries)", g_dumpNo, (int)g_dump.size()));
    g_dump.clear();
}

// ───────────────────────── Хуки на уровне движка (zCRnd_D3D) ─────────────────────────
void __fastcall Hook_Rnd_DrawPolySimple(void* self, void* edx, zCTexture* tex, zTRndSimpleVertex* verts, int count);
CInvoke<void(__fastcall*)(void*, void*, zCTexture*, zTRndSimpleVertex*, int)> Ivk_Rnd_DrawPolySimple(0x0064AC30, &Hook_Rnd_DrawPolySimple);
void __fastcall Hook_Rnd_DrawPolySimple(void* self, void* edx, zCTexture* tex, zTRndSimpleVertex* verts, int count) {
    if (g_dumpActive && verts && count > 0) {
        g_dumpPS += count;
        if (g_dumpCallers.size() < 128) g_dumpCallers[(DWORD)(size_t)_ReturnAddress()]++;
    }
    bool mirrorBranch = g_devWorldMirror && g_haveVp && verts && count > 0 && count <= 200000;
    if (mirrorBranch) {
        for (int i = 0; i < count && i < 64; i++) {
            if (verts[i].pos.n[0] < g_psMinX) g_psMinX = verts[i].pos.n[0];
            if (verts[i].pos.n[0] > g_psMaxX) g_psMaxX = verts[i].pos.n[0];
        }
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
    if (g_dumpActive && g_dumpCallers.size() < 128) g_dumpCallers[(DWORD)(size_t)_ReturnAddress()]++;
    if (g_devWorldMirror && g_haveVp && vb && count > 0 && vb->array.XYZRHWPtr) {
        if (start >= 0 && start + count <= (int)vb->numVertex) {
            float cx = (float)(2 * g_vp.dwX + g_vp.dwWidth);
            zVEC4* p = vb->array.XYZRHWPtr + start;
            for (int i = 0; i < count && i < 64; i++) {
                if (p[i].n[0] < g_vbTlMinX) g_vbTlMinX = p[i].n[0];
                if (p[i].n[0] > g_vbTlMaxX) g_vbTlMaxX = p[i].n[0];
            }
            for (int i = 0; i < count; i++) p[i].n[0] = cx - p[i].n[0];
            mirrored = true;
            g_vbTlMirrored += count;
            if (g_dumpActive) g_dumpVBTL += count;
        }
    }
    int r = Ivk_Rnd_DrawVertexBuffer(self, edx, vb, start, count, idx, fl);
    if (mirrored) {
        float cx = (float)(2 * g_vp.dwX + g_vp.dwWidth);
        zVEC4* p = vb->array.XYZRHWPtr + start;
        for (int i = 0; i < count; i++) p[i].n[0] = cx - p[i].n[0];   // откат
    }
    return r;
}

// ───────────────────────── Хуки D3D7-устройства ─────────────────────────
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

HRESULT STDMETHODCALLTYPE Hook_Dev_SetViewport(IDirect3DDevice7* self, D3DVIEWPORT7* vp) {
    if (vp) { g_vp = *vp; g_haveVp = true; }
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
    ok = PatchSlot(vt, 20, &Hook_Dev_SetRenderState,        g_oSetRS) && ok;   // SDK-порядок: SetRenderState = 20 (нестандартный vtable!)

    g_devHooked = ok;
    Log(ok ? SFmt("device hooks installed (cull=%lu, viewport %lux%lu, slots SetTransform11/SetViewport13/SetRenderState20)",
        (unsigned long)g_engCull, (unsigned long)g_vp.dwWidth, (unsigned long)g_vp.dwHeight)
           : std::string("device hooks: FAILED (see slots)"));
    return ok;
}

// ───────────────────────── GPU-FLIP: переворот готового кадра на видеокарте ─────────────────────────
// Идея: после рендера мира (мир + скайбокс + PFX уже в RT) копируем RT в текстуру
// (DirectDraw Blt, GPU→GPU), затем рисуем на весь экран TL-квад с зеркальными UV.
// Не зависит от того, КАКИМ путём ванилла рисовала PFX/скайбокс — зеркалится результат.
// Самопроверка: при первых кадрах сравниваем пиксели до/после (калибровка сдвига ±1 px).
// Если что-то не работает — автоматический откат на CPU-переворот (g_gf.state = -1).
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

// Перед переворотом: читаем 3x3 окрестность ЗЕРКАЛЬНОЙ позиции для каждой контрольной точки.
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

// После переворота: 1 = ок, 0 = неубедительно/подстроили, 2 = провал.
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
    if (tally[4] * 100 >= tally[best] * 95) best = 4;   // при ничьей предпочитаем «без сдвига»
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
    float uL = ((float)g_gf.rtW - (float)g_gf.adjX) / tw;   // левая вершина экрана показывает ПРАВЫЙ край источника
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

    // восстановление состояния (движок кэширует состояния — они обязаны остаться прежними)
    dev->SetTexture(0, oldTex);
    if (oldTex) oldTex->Release();
    for (int i = 0; i < nTss; i++) if (tssHave[i]) dev->SetTextureStageState(kTss[i].stage, kTss[i].t, tssOld[i]);
    for (int i = 0; i < nRs; i++) if (rsHave[i]) RsRaw(dev, kRs[i], rsOld[i]);
    if (haveVp) { if (g_oSetViewport) g_oSetViewport(dev, &oldVp); else dev->SetViewport(&oldVp); }

    if (FAILED(hr)) Log(SFmt("GPUFLIP draw: DrawPrimitive failed hr=0x%08lX", (unsigned long)hr));
    return SUCCEEDED(hr);
}

// Результат: 1 = кадр перевёрнут, 0 = не удалось ДО отрисовки (кадр не тронут), 2 = нарисовано, но калибровка провалена.
// inScene: true — вызывается внутри сцены (после рендера мира); false — вне сцены (Vid_Blit).
int GfFlip(zCRnd_D3D* rnd, IDirectDrawSurface7* rt, bool inScene) {
    IDirect3DDevice7* dev = zCRnd_D3D::xd3d_pd3dDevice7;
    if (!dev || !rnd) return 0;
    if (!GfEnsure(rt, dev)) {
        if (++g_gf.initFails >= 3) { g_gf.state = -1; Log("GPUFLIP init failed 3 times -> fallback to CPU flip"); }
        return 0;
    }
    if (g_gf.state < 0) return 0;

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
        rnd->EndFrame();                       // Lock возможен только вне сцены
        int ev = GfEvaluate(rt);
        if (inScene) rnd->BeginFrame();
        if (ev == 1) { g_gf.state = 1; Log(SFmt("GPUFLIP verified OK (adjX=%d adjY=%d)", g_gf.adjX, g_gf.adjY)); }
        else if (ev == 2) { g_gf.state = -1; Log("GPUFLIP verification FAILED -> fallback to CPU flip"); result = 2; }
        else if (++g_gf.calTries >= 90) { g_gf.state = 1; Log("GPUFLIP: no informative frames in 90 tries -> assumed OK"); }
    }
    else {
        if (!inScene) rnd->EndFrame();
        if (cal && drew && !haveOld && ++g_gf.calTries >= 90) { g_gf.state = 1; Log("GPUFLIP: pixel read unavailable -> assumed OK"); }
    }
    if (!drew) {
        g_gf.state = -1;
        Log("GPUFLIP draw failed -> fallback to CPU flip");
    }
    if (result == 1) g_gf.framesOk++;
    return result;
}

// ───────────────────────── Переворот кадра (CPU / GPU-FLIP) ─────────────────────────
// inScene: true — сразу после рендера мира (внутри сцены), false — на конце кадра (Vid_Blit).
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
        if (r == 1 || r == 2) ok = true;   // 2: кадр уже нарисован, дальше CPU
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

// ───────────────────────── Карта: детект + переворот в конце кадра ─────────────────────────
// Пока карта открыта, кадр зеркалится ЦЕЛИКОМ в Hook_Rnd_VidBlit (после отрисовки интерфейса);
// сразу после мира переворот в этом кадре пропускается (иначе мир перевернулся бы дважды).
// Признаки «карта на экране» (MapSignals):
//   бит0 — известен объект oCViewDocumentMap (UpdatePosition), бит1 — последний показанный
//   документ — карта (Show/UpdatePosition), бит2 — окно документов в screen->childs,
//   бит3 — oCDocumentManager принимает ввод (HandleEvent включён).
#if ZM_HOOK_MAP
bool DocDlgOnScreen() {
    if (!screen) return false;
    oCDocumentManager& dm = oCDocumentManager::GetDocumentManager();
    zCViewDialog* dlg = dm.DlgDocumentMain;
    if (!dlg) return false;
    zCView* target = dlg;
    int guard = 0;
    for (zCView* v = screen->childs.root; v && guard < 512; v = v->next, guard++)
        if (v == target) return true;
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
    if (g_mapDet == MAPD_ON) return true;
    if (g_mapDet == MAPD_OFF) return false;
    if (!ogame || !screen) return false;
    int s = MapSignals();
    if (!(s & 1) || !(s & 2)) return false;
    if (g_mapDet == MAPD_DOCONLY) return true;
    if (g_mapDet == MAPD_LENIENT) return (s & (4 | 8)) != 0;
    return (s & 4) && (s & 8);   // AUTO: строго
}

// ~oCViewDocumentMap — очистка указателя карты при закрытии/удалении
void __fastcall Hook_MapView_Dtor(zCView* self, void* edx);
CInvoke<void(__fastcall*)(zCView*, void*)> Ivk_MapView_Dtor(0x0068D470, &Hook_MapView_Dtor);
void __fastcall Hook_MapView_Dtor(zCView* self, void* edx) {
    if (g_mapView == self) { g_mapView = NULL; g_mapDocIsMap = false; }
    Ivk_MapView_Dtor(self, edx);
}

// oCMapScreen::Show(int)
int __fastcall Hook_MapScreen_Show(void* self, void* edx, int a);
CInvoke<int(__fastcall*)(void*, void*, int)> Ivk_MapScreen_Show(0x0047F9C0, &Hook_MapScreen_Show);
int __fastcall Hook_MapScreen_Show(void* self, void* edx, int a) {
    DWORD caller = (DWORD)_ReturnAddress();
    bool outer = (g_docShowDepth == 0);
    if (outer) g_mapUpdInShow = false;
    if (g_teleOn) Log(SFmt("MAP oCMapScreen::Show(arg=%d) BEGIN self=0x%08lX caller=0x%08lX frame=%d modal=%s pause=%d mode=%s sig=0x%X",
        a, (unsigned long)(size_t)self, (unsigned long)caller, g_frame, ModalName().c_str(), (ogame && ogame->IsOnPause()) ? 1 : 0, kModeNames[g_mode], (ogame && screen) ? MapSignals() : 0));
    g_docShowDepth++;
    int r = Ivk_MapScreen_Show(self, edx, a);
    g_docShowDepth--;
    if (outer && (g_mapUpdInShow || g_frame - g_mapUpdFrame <= 2)) g_mapDocIsMap = true;
    if (g_teleOn) Log(SFmt("MAP oCMapScreen::Show(arg=%d) END   frame=%d modal=%s pause=%d docIsMap=%d sig=0x%X", a, g_frame, ModalName().c_str(),
        (ogame && ogame->IsOnPause()) ? 1 : 0, g_mapDocIsMap ? 1 : 0, (ogame && screen) ? MapSignals() : 0));
    return r;
}

// oCDocumentManager::Show(int) — открытие документа (в т.ч. карты-предмета)
void __fastcall Hook_DocMan_Show(void* self, void* edx, int id);
CInvoke<void(__fastcall*)(void*, void*, int)> Ivk_DocMan_Show(0x0065F300, &Hook_DocMan_Show);
void __fastcall Hook_DocMan_Show(void* self, void* edx, int id) {
    bool outer = (g_docShowDepth == 0);
    if (outer) g_mapUpdInShow = false;
    g_docShowDepth++;
    Ivk_DocMan_Show(self, edx, id);
    g_docShowDepth--;
    if (!outer) return;   // вложенный вызов из oCMapScreen::Show — решает внешний хук
    bool isMap = g_mapUpdInShow || (g_frame - g_mapUpdFrame <= 2);
    oCViewDocument* d = ((oCDocumentManager*)self)->GetDocumentView(id);
    if (!isMap && d && g_mapView && (void*)d == (void*)g_mapView) isMap = true;
    g_mapDocIsMap = isMap;
    if (g_teleOn) Log(SFmt("MAP oCDocumentManager::Show(id=%d) docView=0x%08lX mapView=0x%08lX isMap=%d frame=%d sig=0x%X", id,
        (unsigned long)(size_t)d, (unsigned long)(size_t)g_mapView, isMap ? 1 : 0, g_frame, (ogame && screen) ? MapSignals() : 0));
}

// oCViewDocumentMap::UpdatePosition()
void __fastcall Hook_MapView_UpdatePosition(zCView* self, void* edx);
CInvoke<void(__fastcall*)(zCView*, void*)> Ivk_MapView_UpdatePosition(0x0068D7B0, &Hook_MapView_UpdatePosition);
void __fastcall Hook_MapView_UpdatePosition(zCView* self, void* edx) {
    g_mapUpdCount++;
    if (g_teleOn && g_mapUpdCount <= 10)
        Log(SFmt("MAP oCViewDocumentMap::UpdatePosition #%d self=0x%08lX caller=0x%08lX frame=%d modal=%s inShow=%d",
            g_mapUpdCount, (unsigned long)(size_t)self, (unsigned long)(size_t)_ReturnAddress(), g_frame, ModalName().c_str(), g_docShowDepth));
    Ivk_MapView_UpdatePosition(self, edx);
    g_mapView = self;
    g_mapUpdFrame = g_frame;
    if (g_docShowDepth > 0) g_mapUpdInShow = true;
}

// zCRnd_D3D::Vid_Blit(int, tagRECT*, tagRECT*) — конец кадра (после интерфейса)
void __fastcall Hook_Rnd_VidBlit(void* self, void* edx, int a, tagRECT* r1, tagRECT* r2);
CInvoke<void(__fastcall*)(void*, void*, int, tagRECT*, tagRECT*)> Ivk_Rnd_VidBlit(0x00657670, &Hook_Rnd_VidBlit);
void __fastcall Hook_Rnd_VidBlit(void* self, void* edx, int a, tagRECT* r1, tagRECT* r2) {
    bool doFlip = false;
    if (FlipModeActive()) {
        if (g_frameFinalFlip) doFlip = true;
        else if (!g_worldRenderedThisFrame && MapOnScreenNow()) doFlip = true;
    }
    g_frameFinalFlip = false;
    g_worldRenderedThisFrame = false;
    if (doFlip) {
        zCRnd_D3D* rnd = (zCRnd_D3D*)zrenderer;
        bool inScene = rnd && rnd->xd3d_inScene != 0;
        if (g_finalFlips < 3 || (g_teleOn && g_finalFlips < 50))
            Log(SFmt("MAP final-frame flip #%d (inScene=%d mode=%s backend=%s sig=0x%X frame=%d)", g_finalFlips + 1, inScene ? 1 : 0,
                kModeNames[g_mode], g_mode == MODE_GPU ? (GpuUsesFlip() ? "FLIP" : "PROJ") : "-", MapSignals(), g_frame));
        g_finalFlips++;
        MirrorFrame(inScene);
    }
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

// ───────────────────────── Телеметрия: общие помощники ─────────────────────────
int g_mark = 0;

std::string ClassOf(zCObject* obj);
std::string ModalName() {
    return zCView::modal ? ClassOf((zCObject*)zCView::modal) : std::string("none");
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
    return MirrorOn() && g_uiPhase && g_worldDepth == 0 && !g_frameFinalFlip;
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

// ───────────────────────── Клавиши: влево <-> вправо ─────────────────────────
inline bool IsLockpickCaller(DWORD c) {
    return c == 0x0069857D || c == 0x00698593 || c == 0x006985B9 || c == 0x006985CF ||
           c == 0x00723E6D || c == 0x00723E7E;
}

inline unsigned short SwapLR(unsigned short key, DWORD caller) {
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

// ───────────────────────── Зеркалирование сущностей (все NPC) ─────────────────────────
bool IsNpcVob(zCVob* v) {
    if (!v) return false;
    zCClassDef* cd = v->_GetClassDef();
    if (!cd) return false;
    static std::map<zCClassDef*, bool> cache;
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
    bool entApply = g_entMirror && IsNpcVob(self);
    if (!entApply) return Ivk_Vob_Render(self, ctx);

    float orig[16];
    memcpy(orig, &self->trafoObjToWorld, sizeof(orig));
    float m[16];
    memcpy(m, orig, sizeof(m));
    m[0] = -m[0];
    m[4] = -m[4];
    m[8] = -m[8];
    memcpy(&self->trafoObjToWorld, m, sizeof(m));

    bool flip = g_devHooked || EnsureDeviceHooks();
    if (flip) { g_entCullFlip = true; SyncCull(); }

    int r = Ivk_Vob_Render(self, ctx);

    if (flip) { g_entCullFlip = false; SyncCull(); }
    memcpy(&self->trafoObjToWorld, orig, sizeof(orig));
    return r;
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
    bool useFlip = FlipModeActive();                        // CPU или GPU-FLIP: переворот готового кадра
    bool useProj = (g_mode == MODE_GPU) && !GpuUsesFlip();  // GPU-PROJ: проекция + cull + TL

    g_uiPhase = false;
    g_worldDepth++;
    if (main) {
        if (g_dumpActive) FinishDump();        // предыдущий кадр (мир + его UI) завершён
        g_frame++;
        g_inMainWorld = true;
        if (g_dumpRequested) StartDump();
        if (g_teleOn) PerfFrame();
        g_worldRenderedThisFrame = true;
        g_frameFinalFlip = useFlip && MapOnScreenNow();
        DumpText(SFmt("main World_Render BEGIN (frame %d, mode %s, backend %s, finalFlip=%d)", g_frame, kModeNames[g_mode],
            g_mode == MODE_GPU ? (useProj ? "PROJ" : "FLIP") : "-", g_frameFinalFlip ? 1 : 0));

        if (useProj) {
            if (EnsureDeviceHooks()) {
                g_devWorldMirror = true;
                g_worldCullFlip = true;
                devMirrorStarted = true;
                SyncCull();
                SyncProj();
            }
            else {
                static bool w = false;
                if (!w) { w = true; Log("GPU(PROJ): device hooks could not be installed - world will NOT be mirrored"); }
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
        if (useFlip && !g_frameFinalFlip) MirrorFrame(true);   // карта открыта → переворот будет в конце кадра
        g_uiPhase = true;
    }
    else g_uiPhase = prevUi;
}

// ───────────────────────── События Union ─────────────────────────
void Game_Entry() {
    WriteText("zMirror_log.txt", "Game_Entry: zMirrorTest V1.5.0 loaded\n", false);
    WriteText("zMirror_dump.txt", "zMirrorTest V1.5.0 dump file\n", false);
}

void Game_Init() { }

void Game_Exit() { GfRelease(); }

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

    // HOME — цикл режимов: OFF -> CPU -> GPU -> OFF
    static bool wasHome = false;
    if (KeyEdge(VK_HOME, wasHome)) {
        g_mode = (g_mode + 1) % MODE_COUNT;
        g_failStreak = 0;
        PerfReset();
        SyncMouseFlip();
        Log(std::string("mirror mode: ") + kModeNames[g_mode]);
        if (g_mode == MODE_GPU) Msg(SFmt("MIRROR MODE: GPU [%s]", GpuUsesFlip() ? "FLIP" : "PROJ"));
        else Msg(SFmt("MIRROR MODE: %s", kModeNames[g_mode]));
    }

    // DELETE — бэкенд GPU-режима: AUTO -> FLIP -> PROJ
    static bool wasDel = false;
    if (KeyEdge(VK_DELETE, wasDel)) {
        g_backendPref = (g_backendPref + 1) % BK_COUNT;
        GfReset();
        Log(SFmt("GPU backend pref: %s (effective: %s, wrapper=%d)", kBackendNames[g_backendPref], GpuUsesFlip() ? "FLIP" : "PROJ", g_wrapperDetected ? 1 : 0));
        Msg(SFmt("GPU BACKEND: %s -> %s", kBackendNames[g_backendPref], GpuUsesFlip() ? "FLIP" : "PROJ"));
    }

    // PAGEDOWN — режим детекта карты
    static bool wasPgDn = false;
    if (KeyEdge(VK_NEXT, wasPgDn)) {
        g_mapDet = (g_mapDet + 1) % MAPD_COUNT;
        Log(SFmt("map detect mode: %s (sig=0x%X)", kMapDetNames[g_mapDet], (ogame && screen) ? MapSignals() : 0));
        Msg(SFmt("MAP DETECT: %s", kMapDetNames[g_mapDet]));
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

    // [ — принудительная инверсия полигонов/culling (для GD3D11 и экспериментов)
    static bool wasPolyFlip = false;
    if (KeyEdge(VK_OEM_4, wasPolyFlip)) {
        g_polyFlip = !g_polyFlip;
        Log(std::string("polyFlip=") + (g_polyFlip ? "ON" : "OFF"));
        Msg(g_polyFlip ? "POLY-FLIP: ON" : "POLY-FLIP: OFF");
    }

    // INSERT — дамп одного кадра (только при включённой телеметрии)
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
        Log(SFmt("=== MARK %d === mode=%s ent=%d polyFlip=%d modal=%s pause=%d frame=%d", g_mark, kModeNames[g_mode], g_entMirror ? 1 : 0,
            g_polyFlip ? 1 : 0, ModalName().c_str(), (ogame && ogame->IsOnPause()) ? 1 : 0, g_frame));
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

    // Сигналы карты: пишем только изменения (телеметрия включена)
    static int lastSig = -1;
    if (g_teleOn) {
        int sig = (ogame && screen) ? MapSignals() : 0;
        if (sig != lastSig) {
            Log(SFmt("MAPSIG bits=0x%X (mapView=%d docIsMap=%d dlgOnScreen=%d mgrEnabled=%d) det=%s onScreenNow=%d modal=%s mark=%d frame=%d", sig,
                sig & 1, (sig >> 1) & 1, (sig >> 2) & 1, (sig >> 3) & 1, kMapDetNames[g_mapDet], MapOnScreenNow() ? 1 : 0,
                ModalName().c_str(), g_mark, g_frame));
            lastSig = sig;
        }
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
