// ═══════════════════════════════════════════════════════════════════
// zMirrorTest V1.7.6
// Зеркальный режим Gothic II NotR (Union SDK).
// HOME — включить/выключить зеркалирование.
// Рендер: GPU-FLIP для штатного D3D7, FLIP11 для GD3D11.
// FLIP11 зеркалит 3D-сцену до HUD/UI; проверено в игре.
// ═══════════════════════════════════════════════════════════════════

#include "resource.h"
#include <string>
#include <vector>
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
int  g_failStreak = 0;
int  g_frame = 0;
bool g_focusLockable = false;
bool g_uiPhase = false;
int  g_worldDepth = 0;

inline bool MirrorOn() { return g_mode != MODE_OFF; }

// ───────────────────────── Бэкенды GPU-режима ─────────────────────────
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

// true: штатный D3D7 GPU-FLIP; false: D3D11 FLIP11
inline bool GpuUsesFlip() {
    DetectWrapper();
    return !g_wrapperDetected;
}
// true: активный режим переворачивает готовый кадр (GPU-FLIP, ванилла)
inline bool FlipModeActive() { return g_mode == MODE_GPU && GpuUsesFlip(); }

// GD3D11 всегда использует FLIP11.
inline bool IsScreenFlipActive() {
    return g_mode == MODE_GPU && !GpuUsesFlip();
}

// Карта: выборочный режим (квады карты в DrawPolySimple) — всегда включён
bool g_frameMapOpen = false;    // карта открыта в этом кадре
bool g_mapPageSeen = false;     // картинка карты уже встречена в этом кадре
float g_mapL = 0, g_mapR = 0;   // рамка картинки карты по X (пиксели)


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
//   CopyResource=47.
// ID3D11Device: CreateTexture2D=5, CreateShaderResourceView=7,
//   CreateVertexShader=12, CreatePixelShader=15, CreateSamplerState=23.
// ID3D11DeviceChild: GetDevice=3.
// ID3D11Texture2D::GetDesc=10; IDXGISwapChain::Present=8, GetBuffer=9.
// IDXGIFactory2::CreateSwapChainForHwnd=15 (IDXGIFactory1::IsCurrent=13).

typedef HRESULT (STDMETHODCALLTYPE *PFN_GetDev)(void*, void**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetImmCtx)(void*, void**);
typedef ULONG (STDMETHODCALLTYPE *PFN_Rel)(void*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present_T)(void*, UINT, UINT);

void ZmCaptureDevice(void** ppDev, void** ppCtx) {
    if (!ppDev || !ppCtx || g_d11.ok) return;
    if (g_probeDev && g_probeDev == *ppDev) return;   // пробное устройство — не настоящее
    g_d11.dev = *ppDev; g_d11.ctx = *ppCtx;
    g_d11.ok = true;
    Log("D3D11: device captured via creation hook");
}

// --- пробный свопчейн (fallback) ---
void* g_probeSC = NULL;
void* g_vtProbeF = NULL;   // vtable FLIP-пробника
void* g_vtProbeL = NULL;   // vtable legacy-пробника
PFN_Present_T g_oPresentF = NULL;
PFN_Present_T g_oPresentL = NULL;


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

// ───────────────────────── FLIP11: D3D11 scene/backbuffer resources ─────────────────────────
// Рабочий scene pass использует активный RTV в точке перехода к HUD; Present-pass удалён.

struct ZmFlipSc {
    bool shaderTried;
    void* tex;    // ID3D11Texture2D (scratch)
    void* srv;    // ID3D11ShaderResourceView
    void* sampler; // ID3D11SamplerState
    void* vs;     // ID3D11VertexShader
    void* ps;     // ID3D11PixelShader
    void* rs;     // ID3D11RasterizerState (CullMode=NONE для fullscreen triangle)
    UINT w, h, format, sampleCount, sampleQuality, mipLevels, arraySize;
    int frames;
};
ZmFlipSc g_fs = {};

typedef HRESULT (WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, void**, void**);
inline void ZmRelease(void*& p);

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
        "  o.p=float4(p*float2(2.0,-2.0)+float2(-1.0,1.0),0,1);\n"
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
    if (err) { ((PFN_Rel)Vt(err)[2])(err); err = NULL; }
    if (FAILED(hr) || !vsCode) { Log(SFmt("FLIP11: VS compile failed hr=0x%08lX", (unsigned long)hr)); return; }
    hr = compile(pssrc, strlen(pssrc), "ps.hlsl", NULL, NULL, "main", "ps_4_0", 0, 0, &psCode, &err);
    if (err) { ((PFN_Rel)Vt(err)[2])(err); err = NULL; }
    if (FAILED(hr) || !psCode) {
        ((PFN_Rel)Vt(vsCode)[2])(vsCode);
        Log(SFmt("FLIP11: PS compile failed hr=0x%08lX", (unsigned long)hr)); return;
    }

    // ID3DBlob: GetBufferPointer = 3, GetBufferSize = 4
    typedef void* (STDMETHODCALLTYPE *PFN_GetBufPtr)(void*);
    typedef SIZE_T (STDMETHODCALLTYPE *PFN_GetBufSize)(void*);
    const void* vptr = ((PFN_GetBufPtr)Vt(vsCode)[3])(vsCode);
    SIZE_T vsize = ((PFN_GetBufSize)Vt(vsCode)[4])(vsCode);
    const void* pptr = ((PFN_GetBufPtr)Vt(psCode)[3])(psCode);
    SIZE_T psize = ((PFN_GetBufSize)Vt(psCode)[4])(psCode);
    HRESULT vhr = ((PFN_CreateVS)Vt(g_d11.dev)[12])(g_d11.dev, vptr, vsize, NULL, &g_fs.vs);
    HRESULT phr = ((PFN_CreatePS)Vt(g_d11.dev)[15])(g_d11.dev, pptr, psize, NULL, &g_fs.ps);
    ((PFN_Rel)Vt(vsCode)[2])(vsCode);
    ((PFN_Rel)Vt(psCode)[2])(psCode);
    if (FAILED(vhr) || FAILED(phr) || !g_fs.vs || !g_fs.ps) {
        Log(SFmt("FLIP11: shader object creation failed VS=0x%08lX PS=0x%08lX", (unsigned long)vhr, (unsigned long)phr));
        if (g_fs.vs) ZmRelease(g_fs.vs);
        if (g_fs.ps) ZmRelease(g_fs.ps);
        return;
    }
    Log("FLIP11: shaders compiled");
}

// ───────────────────────── FLIP11: прямой проход по активному D3D11 RTV ─────────────────────────
// GD3D11 не предоставляет свой HDR/scene target через IDirect3DDevice7::GetRenderTarget.
// На границе zCWorld::Render -> HUD у GD3D11 уже выбран Backbuffer RTV. Получаем его
// непосредственно из immediate context, копируем ресурс в scratch и рисуем обратно.
// Это сохраняет фазу «мир уже готов, интерфейс ещё не начат» и не требует D3D7 Lock/Blt.

struct ZmD3D11TexDesc {
    UINT Width, Height, MipLevels, ArraySize;
    UINT Format;
    UINT SampleCount, SampleQuality;
    UINT Usage, BindFlags, CPUAccessFlags, MiscFlags;
};
struct ZmD3D11Viewport {
    float TopLeftX, TopLeftY, Width, Height, MinDepth, MaxDepth;
};

inline void ZmRelease(void*& p) {
    if (p) { ((PFN_Rel)Vt(p)[2])(p); p = NULL; }
}

bool ZmEnsureSceneFlipResources(const ZmD3D11TexDesc& src) {
    typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateTex)(void*, const void*, const void*, void**);
    typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSRV)(void*, void*, const void*, void**);
    typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSampler)(void*, const void*, void**);
    typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateRS)(void*, const void*, void**);

    if (!g_fs.shaderTried) {
        g_fs.shaderTried = true;
        ZmCompileShaders();
    }
    if (!g_fs.vs || !g_fs.ps) return false;

    if (!g_fs.rs) {
        struct RastDesc { UINT FillMode, CullMode; BOOL FrontCCW; INT DepthBias; float DepthBiasClamp, SlopeScaledDepthBias; BOOL DepthClipEnable, ScissorEnable, MultisampleEnable, AALineEnable; };
        RastDesc rd; ZeroMemory(&rd, sizeof(rd));
        rd.FillMode = 3;       // D3D11_FILL_SOLID
        rd.CullMode = 1;       // D3D11_CULL_NONE
        rd.DepthClipEnable = TRUE;
        HRESULT hr = ((PFN_CreateRS)Vt(g_d11.dev)[22])(g_d11.dev, &rd, &g_fs.rs);
        if (FAILED(hr)) { Log(SFmt("FLIP11: scene CreateRasterizerState failed hr=0x%08lX", (unsigned long)hr)); return false; }
    }
    if (!g_fs.sampler) {
        struct SmpDesc { UINT Filter, AddressU, AddressV, AddressW; float MipLODBias; UINT MaxAnisotropy, ComparisonFunc; float BorderColor[4]; float MinLOD, MaxLOD; };
        SmpDesc sm; ZeroMemory(&sm, sizeof(sm));
        sm.AddressU = sm.AddressV = sm.AddressW = 3; // CLAMP
        sm.MaxLOD = 3.402823466e+38F;
        HRESULT hr = ((PFN_CreateSampler)Vt(g_d11.dev)[23])(g_d11.dev, &sm, &g_fs.sampler);
        if (FAILED(hr)) { Log(SFmt("FLIP11: scene CreateSamplerState failed hr=0x%08lX", (unsigned long)hr)); return false; }
    }

    bool same = g_fs.tex && g_fs.srv && g_fs.w == src.Width && g_fs.h == src.Height &&
        g_fs.format == src.Format && g_fs.sampleCount == src.SampleCount &&
        g_fs.sampleQuality == src.SampleQuality && g_fs.mipLevels == src.MipLevels &&
        g_fs.arraySize == src.ArraySize;
    if (same) return true;

    ZmRelease(g_fs.srv);
    ZmRelease(g_fs.tex);
    g_fs.w = g_fs.h = g_fs.format = g_fs.sampleCount = g_fs.sampleQuality = g_fs.mipLevels = g_fs.arraySize = 0;

    // Для обычного single-sample backbuffer. MSAA требует отдельного Texture2DMS shader/resolve path.
    if (src.SampleCount != 1 || src.ArraySize != 1) {
        static UINT lastBadSamples = 0, lastBadArray = 0;
        if (lastBadSamples != src.SampleCount || lastBadArray != src.ArraySize) {
            lastBadSamples = src.SampleCount; lastBadArray = src.ArraySize;
            Log(SFmt("FLIP11: unsupported scene target sampleCount=%u arraySize=%u", src.SampleCount, src.ArraySize));
        }
        return false;
    }

    ZmD3D11TexDesc sd = src;
    sd.Usage = 0;             // D3D11_USAGE_DEFAULT
    sd.BindFlags = 0x8;       // D3D11_BIND_SHADER_RESOURCE
    sd.CPUAccessFlags = 0;
    sd.MiscFlags = 0;
    HRESULT hr = ((PFN_CreateTex)Vt(g_d11.dev)[5])(g_d11.dev, &sd, NULL, &g_fs.tex);
    if (FAILED(hr) || !g_fs.tex) {
        Log(SFmt("FLIP11: scene scratch CreateTexture2D failed hr=0x%08lX format=%u %ux%u", (unsigned long)hr, src.Format, src.Width, src.Height));
        return false;
    }
    hr = ((PFN_CreateSRV)Vt(g_d11.dev)[7])(g_d11.dev, g_fs.tex, NULL, &g_fs.srv);
    if (FAILED(hr) || !g_fs.srv) {
        ZmRelease(g_fs.tex);
        Log(SFmt("FLIP11: scene scratch CreateSRV failed hr=0x%08lX format=%u", (unsigned long)hr, src.Format));
        return false;
    }
    g_fs.w = src.Width; g_fs.h = src.Height; g_fs.format = src.Format;
    g_fs.sampleCount = src.SampleCount; g_fs.sampleQuality = src.SampleQuality;
    g_fs.mipLevels = src.MipLevels; g_fs.arraySize = src.ArraySize;
    Log(SFmt("FLIP11: D3D11 scene target ready (%ux%u format=%u samples=%u)", src.Width, src.Height, src.Format, src.SampleCount));
    return true;
}

bool ZmFlipSceneD3D11() {
    if (!g_d11.ctx || !g_d11.dev) return false;
    void* ctx = g_d11.ctx;
    void* dev = g_d11.dev;

    typedef void (STDMETHODCALLTYPE *PFN_OMGetRTs)(void*, UINT, void**, void**);
    typedef void (STDMETHODCALLTYPE *PFN_GetResource)(void*, void**);
    typedef HRESULT (STDMETHODCALLTYPE *PFN_QueryInterface)(void*, const GUID&, void**);
    typedef void (STDMETHODCALLTYPE *PFN_GetDescTex)(void*, void*);
    typedef void (STDMETHODCALLTYPE *PFN_CopyResource)(void*, void*, void*);
    typedef void (STDMETHODCALLTYPE *PFN_Draw)(void*, UINT, UINT);
    typedef void (STDMETHODCALLTYPE *PFN_OMSetRTs)(void*, UINT, void**, void*);
    typedef void (STDMETHODCALLTYPE *PFN_RSSetVP)(void*, UINT, const ZmD3D11Viewport*);
    typedef void (STDMETHODCALLTYPE *PFN_RSGetVP)(void*, UINT*, ZmD3D11Viewport*);
    typedef void (STDMETHODCALLTYPE *PFN_IASetTopo)(void*, UINT);
    typedef void (STDMETHODCALLTYPE *PFN_IAGetTopo)(void*, UINT*);
    typedef void (STDMETHODCALLTYPE *PFN_SetShader)(void*, void*, void*, UINT);
    typedef void (STDMETHODCALLTYPE *PFN_GetShader)(void*, void**, void**, UINT*);
    typedef void (STDMETHODCALLTYPE *PFN_SetInputLayout)(void*, void*);
    typedef void (STDMETHODCALLTYPE *PFN_GetInputLayout)(void*, void**);
    typedef void (STDMETHODCALLTYPE *PFN_SetPSResources)(void*, UINT, UINT, void**);
    typedef void (STDMETHODCALLTYPE *PFN_GetPSResources)(void*, UINT, UINT, void**);
    typedef void (STDMETHODCALLTYPE *PFN_SetPSSamplers)(void*, UINT, UINT, void**);
    typedef void (STDMETHODCALLTYPE *PFN_GetPSSamplers)(void*, UINT, UINT, void**);
    typedef void (STDMETHODCALLTYPE *PFN_SetBlend)(void*, void*, const float*, UINT);
    typedef void (STDMETHODCALLTYPE *PFN_GetBlend)(void*, void**, float*, UINT*);
    typedef void (STDMETHODCALLTYPE *PFN_SetDepth)(void*, void*, UINT);
    typedef void (STDMETHODCALLTYPE *PFN_GetDepth)(void*, void**, UINT*);
    typedef void (STDMETHODCALLTYPE *PFN_RSSetState2)(void*, void*);
    typedef void (STDMETHODCALLTYPE *PFN_RSGetState)(void*, void**);
    typedef void (STDMETHODCALLTYPE *PFN_SetPredication)(void*, void*, BOOL);
    typedef void (STDMETHODCALLTYPE *PFN_GetPredication)(void*, void**, BOOL*);
    typedef void (STDMETHODCALLTYPE *PFN_SOSetTargets)(void*, UINT, void**, const UINT*);
    typedef void (STDMETHODCALLTYPE *PFN_SOGetTargets)(void*, UINT, void**);

    static const GUID IID_T2D = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };
    void* rtv = NULL; void* dsv = NULL; void* resource = NULL; void* texture = NULL;
    ((PFN_OMGetRTs)Vt(ctx)[89])(ctx, 1, &rtv, &dsv); // ID3D11DeviceContext::OMGetRenderTargets
    if (!rtv) {
        static bool onceNoRT = false;
        if (!onceNoRT) { onceNoRT = true; Log("FLIP11: OMGetRenderTargets returned no scene RTV at world/UI boundary"); }
        ZmRelease(dsv);
        return false;
    }
    ((PFN_GetResource)Vt(rtv)[7])(rtv, &resource); // ID3D11View::GetResource
    HRESULT qhr = resource ? ((PFN_QueryInterface)Vt(resource)[0])(resource, IID_T2D, &texture) : E_NOINTERFACE;
    if (resource) ZmRelease(resource);
    if (FAILED(qhr) || !texture) {
        Log(SFmt("FLIP11: scene RTV resource is not Texture2D hr=0x%08lX", (unsigned long)qhr));
        ZmRelease(rtv); ZmRelease(dsv); return false;
    }
    ZmD3D11TexDesc td; ZeroMemory(&td, sizeof(td));
    ((PFN_GetDescTex)Vt(texture)[10])(texture, &td);
    if (!td.Width || !td.Height || !ZmEnsureSceneFlipResources(td)) {
        ZmRelease(texture); ZmRelease(rtv); ZmRelease(dsv); return false;
    }

    // Сохраняем все затрагиваемые части pipeline. Контекст общий с GD3D11;
    // после прохода возвращаем его состояние до начала HUD/UI.
    void* oldRT[8] = {}; void* oldDSV = NULL;
    ((PFN_OMGetRTs)Vt(ctx)[89])(ctx, 8, oldRT, &oldDSV);
    void* oldRS = NULL; ((PFN_RSGetState)Vt(ctx)[94])(ctx, &oldRS);
    ZmD3D11Viewport oldVP[16]; UINT oldVPN = 16; ZeroMemory(oldVP, sizeof(oldVP));
    ((PFN_RSGetVP)Vt(ctx)[95])(ctx, &oldVPN, oldVP);
    if (oldVPN > 16) oldVPN = 16;
    void* oldBlend = NULL; float oldBlendFactor[4] = {}; UINT oldSampleMask = 0xFFFFFFFF;
    ((PFN_GetBlend)Vt(ctx)[91])(ctx, &oldBlend, oldBlendFactor, &oldSampleMask);
    void* oldDepth = NULL; UINT oldStencilRef = 0;
    ((PFN_GetDepth)Vt(ctx)[92])(ctx, &oldDepth, &oldStencilRef);
    void* oldVS = NULL; void* oldPS = NULL; void* oldGS = NULL; void* oldHS = NULL; void* oldDS = NULL;
    ((PFN_GetShader)Vt(ctx)[76])(ctx, &oldVS, NULL, NULL);
    ((PFN_GetShader)Vt(ctx)[74])(ctx, &oldPS, NULL, NULL);
    ((PFN_GetShader)Vt(ctx)[82])(ctx, &oldGS, NULL, NULL);
    ((PFN_GetShader)Vt(ctx)[98])(ctx, &oldHS, NULL, NULL);
    ((PFN_GetShader)Vt(ctx)[102])(ctx, &oldDS, NULL, NULL);
    void* oldLayout = NULL; ((PFN_GetInputLayout)Vt(ctx)[78])(ctx, &oldLayout);
    UINT oldTopo = 0; ((PFN_IAGetTopo)Vt(ctx)[83])(ctx, &oldTopo);
    void* oldSRV[128] = {}; void* oldSamplers[16] = {};
    ((PFN_GetPSResources)Vt(ctx)[73])(ctx, 0, 128, oldSRV);
    ((PFN_GetPSSamplers)Vt(ctx)[75])(ctx, 0, 16, oldSamplers);
    void* oldPredication = NULL; BOOL oldPredicateValue = FALSE;
    ((PFN_GetPredication)Vt(ctx)[86])(ctx, &oldPredication, &oldPredicateValue);
    void* oldSO[4] = {}; ((PFN_SOGetTargets)Vt(ctx)[93])(ctx, 4, oldSO);

    // Отвязываем исходный target и PS SRV, чтобы CopyResource не пересекался
    // с активным output binding. Затем копируем в отдельную текстуру.
    ((PFN_OMSetRTs)Vt(ctx)[33])(ctx, 0, NULL, NULL);
    void* nullSRVs[128] = {};
    ((PFN_SetPSResources)Vt(ctx)[8])(ctx, 0, 128, nullSRVs);
    ((PFN_CopyResource)Vt(ctx)[47])(ctx, g_fs.tex, texture);

    float vp[6] = { 0.0f, 0.0f, (float)td.Width, (float)td.Height, 0.0f, 1.0f };
    ((PFN_RSSetVP)Vt(ctx)[44])(ctx, 1, (const ZmD3D11Viewport*)vp);
    ((PFN_OMSetRTs)Vt(ctx)[33])(ctx, 1, &rtv, NULL);
    ((PFN_RSSetState2)Vt(ctx)[43])(ctx, g_fs.rs);
    ((PFN_IASetTopo)Vt(ctx)[24])(ctx, 4); // TRIANGLELIST
    ((PFN_SetInputLayout)Vt(ctx)[17])(ctx, NULL);
    ((PFN_SetShader)Vt(ctx)[11])(ctx, g_fs.vs, NULL, 0);
    ((PFN_SetShader)Vt(ctx)[9])(ctx, g_fs.ps, NULL, 0);
    ((PFN_SetShader)Vt(ctx)[23])(ctx, NULL, NULL, 0); // GS
    ((PFN_SetShader)Vt(ctx)[60])(ctx, NULL, NULL, 0); // HS
    ((PFN_SetShader)Vt(ctx)[64])(ctx, NULL, NULL, 0); // DS
    ((PFN_SetPSResources)Vt(ctx)[8])(ctx, 0, 1, &g_fs.srv);
    ((PFN_SetPSSamplers)Vt(ctx)[10])(ctx, 0, 1, &g_fs.sampler);
    ((PFN_SetBlend)Vt(ctx)[35])(ctx, NULL, NULL, 0xFFFFFFFF);
    ((PFN_SetDepth)Vt(ctx)[36])(ctx, NULL, 0);
    ((PFN_SetPredication)Vt(ctx)[30])(ctx, NULL, FALSE);
    ((PFN_SOSetTargets)Vt(ctx)[37])(ctx, 0, NULL, NULL);
    ((PFN_Draw)Vt(ctx)[13])(ctx, 3, 0);

    // Unbind scratch SRV before restoring the original RTV to avoid D3D11 hazards.
    ((PFN_SetPSResources)Vt(ctx)[8])(ctx, 0, 128, nullSRVs);
    ((PFN_OMSetRTs)Vt(ctx)[33])(ctx, 8, oldRT, oldDSV);
    ((PFN_RSSetState2)Vt(ctx)[43])(ctx, oldRS);
    if (oldVPN) ((PFN_RSSetVP)Vt(ctx)[44])(ctx, oldVPN, oldVP);
    ((PFN_SetBlend)Vt(ctx)[35])(ctx, oldBlend, oldBlendFactor, oldSampleMask);
    ((PFN_SetDepth)Vt(ctx)[36])(ctx, oldDepth, oldStencilRef);
    ((PFN_SetShader)Vt(ctx)[11])(ctx, oldVS, NULL, 0);
    ((PFN_SetShader)Vt(ctx)[9])(ctx, oldPS, NULL, 0);
    ((PFN_SetShader)Vt(ctx)[23])(ctx, oldGS, NULL, 0);
    ((PFN_SetShader)Vt(ctx)[60])(ctx, oldHS, NULL, 0);
    ((PFN_SetShader)Vt(ctx)[64])(ctx, oldDS, NULL, 0);
    ((PFN_SetInputLayout)Vt(ctx)[17])(ctx, oldLayout);
    ((PFN_IASetTopo)Vt(ctx)[24])(ctx, oldTopo);
    ((PFN_SetPSResources)Vt(ctx)[8])(ctx, 0, 128, oldSRV);
    ((PFN_SetPSSamplers)Vt(ctx)[10])(ctx, 0, 16, oldSamplers);
    ((PFN_SetPredication)Vt(ctx)[30])(ctx, oldPredication, oldPredicateValue);
    UINT soOffsets[4] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
    ((PFN_SOSetTargets)Vt(ctx)[37])(ctx, 4, oldSO, soOffsets);

    for (int i = 0; i < 8; ++i) ZmRelease(oldRT[i]);
    ZmRelease(oldDSV); ZmRelease(oldRS); ZmRelease(oldBlend); ZmRelease(oldDepth);
    ZmRelease(oldVS); ZmRelease(oldPS); ZmRelease(oldGS); ZmRelease(oldHS); ZmRelease(oldDS);
    ZmRelease(oldLayout); ZmRelease(oldPredication);
    for (int i = 0; i < 128; ++i) ZmRelease(oldSRV[i]);
    for (int i = 0; i < 16; ++i) ZmRelease(oldSamplers[i]);
    for (int i = 0; i < 4; ++i) ZmRelease(oldSO[i]);
    ZmRelease(texture); ZmRelease(rtv); ZmRelease(dsv);
    g_fs.frames++;
    return true;
}

// ───────────────────────── GPU-FLIP: переворот готового кадра на видеокарте ─────────────────────────
inline void RsRaw(IDirect3DDevice7* dev, D3DRENDERSTATETYPE t, DWORD v) {
    dev->SetRenderState(t, v);
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
    dev->SetViewport(&vp);

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
    if (haveVp) dev->SetViewport(&oldVp);

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

    // Штатный D3D7 backend использует D3D7 render target и GPU Blt + TL-quad.
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
    return result;
}

void MirrorFrame(bool inScene) {
    zCRnd_D3D* rnd = (zCRnd_D3D*)zrenderer;
    if (!rnd) return;
    IDirectDrawSurface7* rt = AcquireTarget();
    if (!rt) return;

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

    if (ok) g_failStreak = 0;
    else if (++g_failStreak >= 30) { g_mode = MODE_OFF; SyncMouseFlip(); Log("mirror auto-disabled: 30 failures in a row"); }
}

// GD3D11 FLIP11: прямой D3D11 pass по активному render target перед HUD/UI.
void MirrorFrameFlip11Scene() {
    static bool announced = false;
    if (!announced) {
        announced = true;
        Log("FLIP11: direct D3D11 active-RTV mirror at world/UI boundary (horizontal)");
    }
    bool ok = ZmFlipSceneD3D11();
    if (ok) g_failStreak = 0;
    else {
        static bool warned = false;
        if (!warned) { warned = true; Log("FLIP11: direct scene RTV pass not ready; see preceding target diagnostic"); }
        if (++g_failStreak >= 30) {
            g_mode = MODE_OFF; SyncMouseFlip();
            Log("FLIP11: mirror auto-disabled after 30 scene RTV failures");
        }
    }
}

// ───────────────────────── Хук отрисовки карты ─────────────────────────
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
            static std::vector<zTRndSimpleVertex> mcopy;
            if (mcopy.size() < (size_t)count) mcopy.resize(count);
            memcpy(&mcopy[0], verts, sizeof(zTRndSimpleVertex) * (size_t)count);
            float cx = g_mapL + g_mapR;
            for (int i = 0; i < count; i++) mcopy[i].pos.n[0] = cx - mcopy[i].pos.n[0];
            Ivk_Rnd_DrawPolySimple(self, edx, tex, &mcopy[0], count);
            return;
        }
    }
    Ivk_Rnd_DrawPolySimple(self, edx, tex, verts, count);
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
    bool useFlip = FlipModeActive();                        // GPU-FLIP (ванилла)
    bool useScFlip = IsScreenFlipActive();                  // FLIP11 (GD3D11)

    if (useScFlip) AcquireD3D11();

    g_uiPhase = false;
    g_worldDepth++;
    if (main) {
        g_frame++;
        g_frameMapOpen = MirrorOn() && MapOnScreenNow();
    }

    Ivk_World_Render(self, edx, cam);

    // Как и ванильный GPU-FLIP, отражаем уже готовый мир до начала UI-фазы.
    // Не используем Present: там HUD/ImGui уже могут быть частью backbuffer.
    if (main && useScFlip) MirrorFrameFlip11Scene();

    g_worldDepth--;
    if (main) {
        if (useFlip) MirrorFrame(true);
        g_uiPhase = true;
    }
    else g_uiPhase = prevUi;
}

// ───────────────────────── События Union ─────────────────────────
void Game_Entry() {
    WriteText("zMirror_log.txt", "Game_Entry: zMirrorTest V1.7.6 loaded\n", false);
}

void Game_Init() { }

void Game_Exit() {
    GfRelease();
    ZmRelease(g_fs.srv); ZmRelease(g_fs.tex); ZmRelease(g_fs.sampler);
    ZmRelease(g_fs.vs); ZmRelease(g_fs.ps); ZmRelease(g_fs.rs);
}

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
        SyncMouseFlip();
        Log(std::string("mirror mode: ") + kModeNames[g_mode]);
        if (g_mode == MODE_GPU) {
            if (GpuUsesFlip()) Msg("MIRROR MODE: GPU [FLIP]");
            else { AcquireD3D11(); Msg("MIRROR MODE: GPU [FLIP11]"); }
        }
        else Msg("MIRROR MODE: OFF");
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
