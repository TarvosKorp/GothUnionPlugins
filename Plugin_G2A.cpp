#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <intrin.h>

// zMapUnlocker — iteration 5: live GPS marker + own horizontal mouse turn while a map is open.
//
// Results so far (logs 1-3):
//  * Log path bug fixed (absolute path). movlock=1 while a document is open; clearing it every
//    frame (iteration 4) restores walking and key turning (GAME_LEFT/RIGHT/STRAFE).
//  * Mouse deltas ARE delivered to GetMousePos while the map is open, but the hero ignores them
//    (some guard inside the hero/camera mouse code; not found yet).
//  * The GPS marker does not follow the hero: oCViewDocumentMap::UpdatePosition (0x0068D7B0) is
//    apparently called only once, when the map is shown.
//
// This build:
//  1) MARKER: ~10x per second, finds the open oCViewDocumentMap (oCDocumentManager::ListDocuments,
//     type checked by vtable pointer) and calls its UpdatePosition(). No hook on UpdatePosition/Show.
//  2) MOUSE: takes the horizontal mouse delta seen in the GetMousePos hook and rotates the hero with
//     RotateWorldY while the map is open. Tune live: NUMPAD+ / NUMPAD- = speed x1.25 / x0.8,
//     NUMPAD* = flip direction. Values are logged ([mouse]); tell me the final ones.
//  3) DIAG: [callers] lists return addresses of GetMousePos callers in the world vs. with the map
//     open, to find the real consumer later.
// NOTE: the "document open" flag is set by the first key event after opening (press any key once).
#ifndef MAPDIAG_UNLOCK
#define MAPDIAG_UNLOCK 1
#endif
#ifndef MAPDIAG_FORCE_MOVE
#define MAPDIAG_FORCE_MOVE 0
#endif
#ifndef MAPDIAG_PASS_KEYS
#define MAPDIAG_PASS_KEYS 0
#endif
#ifndef MAPDIAG_MARKER
#define MAPDIAG_MARKER 1
#endif
#ifndef MAPDIAG_MARKER_MS
#define MAPDIAG_MARKER_MS 40      // 25 updates per second; synchronized with arrow heading sampling
#endif
#ifndef MAPDIAG_MOUSE_TURN
#define MAPDIAG_MOUSE_TURN 1
#endif
#ifndef MAPDIAG_MOUSE_DEG_PER_UNIT
#define MAPDIAG_MOUSE_DEG_PER_UNIT 0.30f
#endif

namespace GOTHIC_ENGINE {

// ---------------------------------------------------------------- logging
static const char* MapDiagPath() {
    static char path[MAX_PATH] = { 0 };
    static bool ready = false;
    if (!ready) {
        ready = true;
        DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            lstrcpyA(path, "zMapUnlocker.log");
        } else {
            char* slash = strrchr(path, '\\');
            if (slash) *(slash + 1) = 0;
            lstrcatA(path, "zMapUnlocker.log");
        }
    }
    return path;
}

void MapDiagReset() {
    FILE* file = fopen(MapDiagPath(), "w");
    if (file) fclose(file);
}

void MapDiagLog(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    _vsnprintf(line, sizeof(line) - 1, format, args);
    va_end(args);
    line[sizeof(line) - 1] = 0;

    OutputDebugStringA("[zMapUnlocker] ");
    OutputDebugStringA(line);
    OutputDebugStringA("\n");

    FILE* file = fopen(MapDiagPath(), "a");
    if (!file) return;
    fprintf(file, "%8lu | %s\n", (unsigned long)GetTickCount(), line);
    fclose(file);
}

void MapDiagCwd(const char* stage) {
    char cwd[MAX_PATH] = { 0 };
    GetCurrentDirectoryA(MAX_PATH, cwd);
    MapDiagLog("[cwd] %s cwd='%s'", stage, cwd);
}

// ---------------------------------------------------------------- shared state
static volatile bool g_docOpen = false;          // synchronized from the live document list
static oCViewDocumentMap* g_mapView = NULL;
static unsigned long g_cLoop = 0, g_cPre = 0, g_cPost = 0, g_cMenu = 0;
static unsigned long g_cGetState = 0, g_cGetStateNZ = 0, g_cGetMouse = 0, g_cGetMouseNZ = 0, g_cToggled = 0;
static DWORD g_lastStats = 0, g_lastHero = 0;
static DWORD g_lastInputLog = 0;

// Keep all high-frequency input diagnostics under a shared ~5 lines/second limit.
static bool MapDiagAllowInputLog() {
    DWORD now = GetTickCount();
    if (now - g_lastInputLog < 200) return false;
    g_lastInputLog = now;
    return true;
}

// mouse turn state
static float g_frameDx = 0.0f;                         // largest |dx| seen this frame (from the GetMousePos hook)
static float g_degPerUnit = MAPDIAG_MOUSE_DEG_PER_UNIT;
static float g_mouseSign = 1.0f;

// GetMousePos caller histogram: [0] = world, [1] = document open
static void* g_callers[16] = { 0 };
static unsigned long g_callerCnt[2][16] = { { 0 } };
static void MapDiagNoteCaller(void* ret, int phase) {
    for (int i = 0; i < 16; ++i) {
        if (g_callers[i] == ret) { ++g_callerCnt[phase][i]; return; }
        if (!g_callers[i]) { g_callers[i] = ret; ++g_callerCnt[phase][i]; return; }
    }
}
static void MapDiagDumpCallers(const char* label, int phase) {
    char buf[600]; buf[0] = 0;
    for (int i = 0; i < 16 && g_callers[i]; ++i) {
        char one[48];
        _snprintf(one, sizeof(one) - 1, " %p x%lu", g_callers[i], g_callerCnt[phase][i]);
        one[sizeof(one) - 1] = 0;
        if (strlen(buf) + strlen(one) < sizeof(buf) - 1) strcat(buf, one);
    }
    MapDiagLog("[callers] GetMousePos %s:%s", label, buf);
    for (int i = 0; i < 16; ++i) g_callerCnt[phase][i] = 0;
}

static void MapDiagHeroProbe(const char* why) {
    oCNpc* hero = Gothic_II_Addon::player;
    zCInput* in = Gothic_II_Addon::zinput;
    int kbd = in ? in->GetDeviceEnabled(zINPUT_KEYBOARD) : -1;
    int mouse = in ? in->GetDeviceEnabled(zINPUT_MOUSE) : -1;
    if (!hero) {
        MapDiagLog("[hero] %s player=NULL kbdEnabled=%d mouseEnabled=%d", why, kbd, mouse);
        return;
    }
    MapDiagLog("[hero] %s player=%p interactItem=%p curState=%d tgtState=%d movlock=%d bodyState=%d weaponMode=%d kbdEnabled=%d mouseEnabled=%d",
        why, hero, hero->interactItem, hero->interactItemCurrentState, hero->interactItemTargetState,
        (int)hero->movlock, hero->GetBodyState(), hero->GetWeaponMode(), kbd, mouse);
}

// Called from several hooks/callbacks; rate-limited by tick count.
static void MapDiagTick() {
    DWORD now = GetTickCount();
    DWORD period = g_docOpen ? 1000 : 5000;
    if (g_lastStats == 0) g_lastStats = now;
    if (now - g_lastStats >= period) {
        MapDiagLog("[stats] doc=%d dt=%lu loop=%lu pre=%lu post=%lu menu=%lu GetState=%lu(nz=%lu) GetToggled=%lu GetMousePos=%lu(nz=%lu)",
            (int)g_docOpen, (unsigned long)(now - g_lastStats), g_cLoop, g_cPre, g_cPost, g_cMenu,
            g_cGetState, g_cGetStateNZ, g_cToggled, g_cGetMouse, g_cGetMouseNZ);
        g_cLoop = g_cPre = g_cPost = g_cMenu = 0;
        g_cGetState = g_cGetStateNZ = g_cGetMouse = g_cGetMouseNZ = g_cToggled = 0;
        g_lastStats = now;
    }
    if (g_docOpen && now - g_lastHero >= 1000) {
        g_lastHero = now;
        MapDiagHeroProbe("tick");
    }
}

// ---------------------------------------------------------------- smooth map arrow
// The stock map arrow is an eight-page document. Hide only that small arrow view
// and draw a continuously oriented pointer into the same view after the screen
// has rendered its normal contents.
static oCViewDocumentMap* g_arrowPreparedMap = NULL;
static unsigned long g_smoothArrowDraws = 0;
static DWORD g_lastArrowDrawLog = 0;
static zCTexture* g_smoothArrowTexture = NULL;
static int g_smoothArrowWidth = 0;
static int g_smoothArrowHeight = 0;
static float g_smoothArrowBaseAngle = 0.0f;
static float g_smoothArrowAngle = 1.57079632679f;
static DWORD g_lastSmoothArrowAngleUpdate = 0;
static bool g_smoothArrowTextureReady = false;
static unsigned long g_arrowBlitCalls = 0;
static DWORD g_lastArrowBlitLog = 0;
static bool g_captureNativeArrowBlit = false;
static bool g_nativeArrowCenterValid = false;
static float g_nativeArrowCenterX = 0.0f;
static float g_nativeArrowCenterY = 0.0f;
static DWORD g_lastNativeArrowGeometryLog = 0;

static void MapDiagPrepareSmoothArrow() {
    if (!g_docOpen || !g_mapView || g_arrowPreparedMap == g_mapView) return;
    // Hide the engine's eight-page arrow and load its canonical north-facing
    // artwork directly. Never depend on whichever page the hero happened to
    // be facing when the map was opened.
    g_mapView->ViewArrow.SetTextureAlpha(0);
    g_arrowPreparedMap = g_mapView;

    zSTRING northName("U.TGA");
    g_smoothArrowTexture = zCTexture::Load(northName, 0);
    g_smoothArrowTextureReady = (g_smoothArrowTexture != NULL);
    g_smoothArrowBaseAngle = 1.57079632679f;
    if (g_smoothArrowTextureReady) {
        g_smoothArrowTexture->GetPixelSize(g_smoothArrowWidth, g_smoothArrowHeight);
    } else {
        g_smoothArrowWidth = 0;
        g_smoothArrowHeight = 0;
    }

    int w = g_mapView->ViewArrow.VirtualSize.X;
    int h = g_mapView->ViewArrow.VirtualSize.Y;
    MapDiagLog("[arrow] native arrow hidden map=%p view=%p virtual=%dx%d canonical=U.TGA texture=%p textureSize=%dx%d ready=%d",
        g_mapView, &g_mapView->ViewArrow, w, h, g_smoothArrowTexture,
        g_smoothArrowWidth, g_smoothArrowHeight, (int)g_smoothArrowTextureReady);
}


// Catch the engine's first map-position update. It runs before the map's
// ViewArrow is blitted, so the stock arrow can be hidden before its first frame.
void __fastcall Hook_MapView_UpdatePosition(void* self, void* edx);
CInvoke<void(__fastcall*)(void*, void*)> Ivk_MapView_UpdatePosition(
    0x0068D7B0, &Hook_MapView_UpdatePosition, IvkEnabled(Engine_G2A));

void __fastcall Hook_MapView_UpdatePosition(void* self, void* edx) {
    oCViewDocumentMap* map = (oCViewDocumentMap*)self;
    if (map) {
        g_mapView = map;
        map->ViewArrow.SetTextureAlpha(0);
    }
    Ivk_MapView_UpdatePosition(self, edx);
}

static void MapDiagDrawSmoothArrow() {
    if (!g_docOpen || !g_mapView) return;
    oCNpc* hero = Gothic_II_Addon::player;
    zCRenderer* renderer = Gothic_II_Addon::zrenderer;
    if (!hero || !renderer) return;

    oCViewDocument& arrow = g_mapView->ViewArrow;
    zCTexture* currentTexture = arrow.Texture;
    const int x = arrow.PixelPosition.X;
    const int y = arrow.PixelPosition.Y;
    const int currentW = arrow.PixelSize.X;
    const int currentH = arrow.PixelSize.Y;
    if (!currentTexture || currentW < 4 || currentH < 4) {
        static bool missingLogged = false;
        if (!missingLogged) {
            missingLogged = true;
            MapDiagLog("[arrow] direct draw unavailable texture=%p pixel=%d,%d %dx%d virtual=%dx%d",
                currentTexture, x, y, currentW, currentH, arrow.VirtualSize.X, arrow.VirtualSize.Y);
        }
        return;
    }

    // Sample heading at the same 25 Hz cadence as UpdatePosition().
    // The artwork is always the explicitly loaded U.TGA (north/up).
    const DWORD now = GetTickCount();
    if (g_lastSmoothArrowAngleUpdate == 0 ||
        now - g_lastSmoothArrowAngleUpdate >= MAPDIAG_MARKER_MS) {
        zVEC3 at = hero->GetAtVectorWorld();
        float dx = at.n[0];
        float dy = -at.n[2];
        const float len = sqrtf(dx * dx + dy * dy);
        if (len >= 0.001f) {
            dx /= len;
            dy /= len;
            g_smoothArrowAngle = atan2f(dy, dx);
            g_lastSmoothArrowAngleUpdate = now;
        }
    }

    // U.TGA is the north/up cardinal artwork: +90 degrees in this
    // heading convention. Never fall back to the engine's changing 8-way page.
    if (!g_smoothArrowTextureReady || !g_smoothArrowTexture ||
        g_smoothArrowWidth < 4 || g_smoothArrowHeight < 4)
        return;
    zCTexture* texture = g_smoothArrowTexture;
    const int w = g_smoothArrowWidth;
    const int h = g_smoothArrowHeight;
    const float rotation = g_smoothArrowAngle - 1.57079632679f;
    const float c = cosf(rotation);
    const float sn = sinf(rotation);

    // Prefer the actual center submitted by the engine for its native arrow.
    // PixelPosition/PixelSize describe the view rectangle, which may not be the
    // exact quad center used by zCViewPrint::Blit.
    const float cx = g_nativeArrowCenterValid
        ? g_nativeArrowCenterX
        : (float)x + (float)currentW * 0.5f;
    const float cy = g_nativeArrowCenterValid
        ? g_nativeArrowCenterY
        : (float)y + (float)currentH * 0.5f;
    const float halfW = (float)w * 0.46f;
    const float halfH = (float)h * 0.46f;
    const float lx[4] = { -halfW, halfW, halfW, -halfW };
    const float ly[4] = { -halfH, -halfH, halfH, halfH };
    const float tu[4] = { 0.f, 1.f, 1.f, 0.f };
    const float tv[4] = { 0.f, 0.f, 1.f, 1.f };
    zTRndSimpleVertex vertices[4];
    for (int i = 0; i < 4; ++i) {
        vertices[i].pos.n[0] = cx + c * lx[i] - sn * ly[i];
        vertices[i].pos.n[1] = cy + sn * lx[i] + c * ly[i];
        vertices[i].z = 1.f;
        vertices[i].uv.n[0] = tu[i];
        vertices[i].uv.n[1] = tv[i];
        vertices[i].color = zCOLOR(255, 255, 255);
    }

    // CheckObjectRoutines is the immediate-render stage used by the Coop
    // marker implementation. View::Line() queues line primitives too late here;
    // DrawPolySimple submits the textured quad directly before EndFrame.
    // G2A layout differs from G1: zCRnd_D3D screen width/height are
    // at +0x98C/+0x990. +0x984/+0x988 are the G1 offsets.
    const int sw = *(int*)((char*)renderer + 0x98C);
    const int sh = *(int*)((char*)renderer + 0x990);
    if (sw < 320 || sw > 16384 || sh < 200 || sh > 16384) {
        static bool badScreenLogged = false;
        if (!badScreenLogged) {
            badScreenLogged = true;
            MapDiagLog("[arrow] invalid renderer dimensions screen=%dx%d", sw, sh);
        }
        return;
    }

    const int oldZWrite = renderer->GetZBufferWriteEnabled();
    const zTRnd_ZBufferCmp oldZCmp = renderer->GetZBufferCompare();
    const zTRnd_AlphaBlendFunc oldAlpha = renderer->GetAlphaBlendFunc();
    const int oldBilerp = renderer->GetBilerpFilterEnabled();
    renderer->SetZBufferWriteEnabled(FALSE);
    renderer->SetZBufferCompare(zRND_ZBUFFER_CMP_ALWAYS);
    renderer->SetAlphaBlendFunc(zRND_ALPHA_FUNC_BLEND);
    renderer->SetBilerpFilterEnabled(TRUE);
    renderer->SetViewport(0, 0, sw, sh);
    renderer->DrawPolySimple(texture, vertices, 4);
    renderer->SetBilerpFilterEnabled(oldBilerp);
    renderer->SetAlphaBlendFunc(oldAlpha);
    renderer->SetZBufferCompare(oldZCmp);
    renderer->SetZBufferWriteEnabled(oldZWrite);

    ++g_smoothArrowDraws;
    DWORD drawNow = GetTickCount();
    if (g_lastArrowDrawLog == 0 || drawNow - g_lastArrowDrawLog >= 1000) {
        g_lastArrowDrawLog = drawNow;
        MapDiagLog("[arrow] renderer draw #%lu rect=%d,%d %dx%d tex=%p screen=%dx%d rotation=%.3f cachedNorth=%d",
            g_smoothArrowDraws, x, y, w, h, texture, sw, sh, rotation,
            (int)g_smoothArrowTextureReady);
    }
}

// Capture the geometry submitted by the engine while ViewArrow::Blit is active.
// This gives us the real vanilla anchor rather than inferring it from view bounds.
void __fastcall Hook_MapDiag_DrawPolySimple(void* self, void* edx, zCTexture* tex, zTRndSimpleVertex* verts, int count);
CInvoke<void(__fastcall*)(void*, void*, zCTexture*, zTRndSimpleVertex*, int)> Ivk_MapDiag_DrawPolySimple(
    0x0064AC30, &Hook_MapDiag_DrawPolySimple, IvkEnabled(Engine_G2A));

void __fastcall Hook_MapDiag_DrawPolySimple(void* self, void* edx, zCTexture* tex, zTRndSimpleVertex* verts, int count) {
    if (g_captureNativeArrowBlit && verts && count >= 3 && count <= 8) {
        float minX = 1.0e30f, minY = 1.0e30f;
        float maxX = -1.0e30f, maxY = -1.0e30f;
        for (int i = 0; i < count; ++i) {
            const float vx = verts[i].pos.n[0];
            const float vy = verts[i].pos.n[1];
            if (vx < minX) minX = vx;
            if (vx > maxX) maxX = vx;
            if (vy < minY) minY = vy;
            if (vy > maxY) maxY = vy;
        }
        const float width = maxX - minX;
        const float height = maxY - minY;
        // Ignore borders/lines and any unexpected large primitives.
        if (width >= 4.0f && width <= 128.0f && height >= 4.0f && height <= 128.0f) {
            g_nativeArrowCenterX = (minX + maxX) * 0.5f;
            g_nativeArrowCenterY = (minY + maxY) * 0.5f;
            g_nativeArrowCenterValid = true;
            const DWORD now = GetTickCount();
            if (g_lastNativeArrowGeometryLog == 0 || now - g_lastNativeArrowGeometryLog >= 1000) {
                g_lastNativeArrowGeometryLog = now;
                zSTRING texName = tex ? tex->GetObjectName() : zSTRING("<null>");
                MapDiagLog("[arrow-anchor] native quad tex='%s' bounds=%.2f,%.2f..%.2f,%.2f center=%.2f,%.2f size=%.2fx%.2f count=%d",
                    texName.ToChar(), minX, minY, maxX, maxY,
                    g_nativeArrowCenterX, g_nativeArrowCenterY, width, height, count);
            }
        }
    }
    Ivk_MapDiag_DrawPolySimple(self, edx, tex, verts, count);
}

// zCViewPrint::Blit() — G2A 0x006934A0.
// ViewArrow inherits this exact Blit implementation. Draw the replacement
// immediately after its own native page has been blitted, not after root screen.
void __fastcall Hook_ViewArrow_Blit(void* self, void* edx);
CInvoke<void(__fastcall*)(void*, void*)> Ivk_ViewArrow_Blit(
    0x006934A0, &Hook_ViewArrow_Blit, IvkEnabled(Engine_G2A));

void __fastcall Hook_ViewArrow_Blit(void* self, void* edx) {
    const bool isArrow = g_mapView &&
        self == (void*)&g_mapView->ViewArrow;

    if (isArrow) {
        ((oCViewDocument*)self)->SetTextureAlpha(0);
        g_captureNativeArrowBlit = true;
        g_nativeArrowCenterValid = false;
    }

    Ivk_ViewArrow_Blit(self, edx);

    if (isArrow) {
        g_captureNativeArrowBlit = false;
        ++g_arrowBlitCalls;
        DWORD now = GetTickCount();
        if (g_lastArrowBlitLog == 0 || now - g_lastArrowBlitLog >= 1000) {
            g_lastArrowBlitLog = now;
            MapDiagLog("[arrow] ViewArrow::Blit #%lu self=%p map=%p",
                g_arrowBlitCalls, self, g_mapView);
        }
    }
}

// oCGame::CheckObjectRoutines() — G2A 0x006CABB0.
// oCGame::Render calls this after zCViewDraw::Render and before EndFrame.
// Coop's proven map-overlay implementation uses this exact stage.
void __fastcall Hook_Game_CheckObjectRoutines(void* self, void* edx);
CInvoke<void(__fastcall*)(void*, void*)> Ivk_Game_CheckObjectRoutines(
    0x006CABB0, &Hook_Game_CheckObjectRoutines, IvkEnabled(Engine_G2A));

void __fastcall Hook_Game_CheckObjectRoutines(void* self, void* edx) {
    if (self == Gothic_II_Addon::ogame)
        MapDiagDrawSmoothArrow();
    Ivk_Game_CheckObjectRoutines(self, edx);
}

// ---------------------------------------------------------------- input hooks
#if MAPDIAG_FORCE_MOVE
// Physically held key -> logical movement id (only used while a document is open).
static float MapDiagForcedValue(unsigned short logical) {
    int vk = 0;
    switch (logical) {
    case 3:  vk = 'W'; break;   // GAME_UP
    case 4:  vk = 'S'; break;   // GAME_DOWN
    case 1:  vk = 'A'; break;   // GAME_LEFT  (turn)
    case 2:  vk = 'D'; break;   // GAME_RIGHT (turn)
    case 18: vk = 'Q'; break;   // GAME_STRAFELEFT
    case 19: vk = 'E'; break;   // GAME_STRAFERIGHT
    default: return 0.0f;
    }
    return (GetAsyncKeyState(vk) & 0x8000) ? 1.0f : 0.0f;
}
#endif

// zCInput_Win32::GetState(unsigned short) — G2A 0x004D4D30.
float __fastcall Hook_Input_GetState(void* self, void* edx, unsigned short key);
CInvoke<float(__fastcall*)(void*, void*, unsigned short)> Ivk_Input_GetState(
    0x004D4D30, &Hook_Input_GetState, IvkEnabled(Engine_G2A));

float __fastcall Hook_Input_GetState(void* self, void* edx, unsigned short key) {
    float raw = Ivk_Input_GetState(self, edx, key);
    float value = raw;
    ++g_cGetState;
    if (raw != 0.0f) ++g_cGetStateNZ;

#if MAPDIAG_FORCE_MOVE
    if (g_docOpen && raw == 0.0f) {
        float forced = MapDiagForcedValue(key);
        if (forced != 0.0f) value = forced;
    }
#endif

    static DWORD lastLog[1024] = { 0 };
    static float lastValue[1024] = { 0.0f };
    static bool seen[1024] = { false };
    unsigned int slot = key < 1024 ? (unsigned int)key : 1023;
    DWORD now = GetTickCount();
    bool changed = !seen[slot] || value != lastValue[slot];
    if ((changed || (value != 0.0f && now - lastLog[slot] >= 200)) && MapDiagAllowInputLog()) {
        MapDiagLog("[input] GetState doc=%d key=%u raw=%.3f returned=%.3f",
            (int)g_docOpen, (unsigned)key, raw, value);
        lastLog[slot] = now;
        lastValue[slot] = value;
        seen[slot] = true;
    }
    MapDiagTick();
    return value;
}

// zCInput_Win32::GetToggled(unsigned short) — G2A 0x004D5020.
int __fastcall Hook_Input_GetToggled(void* self, void* edx, unsigned short key);
CInvoke<int(__fastcall*)(void*, void*, unsigned short)> Ivk_Input_GetToggled(
    0x004D5020, &Hook_Input_GetToggled, IvkEnabled(Engine_G2A));

int __fastcall Hook_Input_GetToggled(void* self, void* edx, unsigned short key) {
    int value = Ivk_Input_GetToggled(self, edx, key);
    ++g_cToggled;
    if (value != 0 && MapDiagAllowInputLog()) {
        MapDiagLog("[input] GetToggled doc=%d key=%u value=%d", (int)g_docOpen, (unsigned)key, value);
    }
    return value;
}

// zCInput_Win32::GetMousePos(float&, float&, float&) — G2A 0x004D5730.
void __fastcall Hook_Input_GetMousePos(void* self, void* edx, float& x, float& y, float& z);
CInvoke<void(__fastcall*)(void*, void*, float&, float&, float&)> Ivk_Input_GetMousePos(
    0x004D5730, &Hook_Input_GetMousePos, IvkEnabled(Engine_G2A));

void __fastcall Hook_Input_GetMousePos(void* self, void* edx, float& x, float& y, float& z) {
    void* retAddr = _ReturnAddress();
    Ivk_Input_GetMousePos(self, edx, x, y, z);
    ++g_cGetMouse;
    MapDiagNoteCaller(retAddr, g_docOpen ? 1 : 0);
    if (g_docOpen && fabsf(x) > fabsf(g_frameDx)) g_frameDx = x;
    static DWORD lastLog = 0;
    static bool wasMoving = false;
    DWORD now = GetTickCount();
    bool moving = x != 0.0f || y != 0.0f || z != 0.0f;
    if (moving) ++g_cGetMouseNZ;
    if (((moving && (!wasMoving || now - lastLog >= 200)) || (!moving && wasMoving)) && MapDiagAllowInputLog()) {
        MapDiagLog("[input] GetMousePos doc=%d x=%.3f y=%.3f z=%.3f", (int)g_docOpen, x, y, z);
        lastLog = now;
    }
    wasMoving = moving;
    MapDiagTick();
}

// ---------------------------------------------------------------- map view lookup
// Primary vtable pointers of oCViewDocumentMap (from the SDK vftable tables). A plain
// oCViewDocument has different ones, so this tells a map from a book/note.
static bool MapDiagIsMapVptr(unsigned int vptr) {
    // The ListDocuments pointer addresses the zCObject subobject's primary vtable.
    // G2A tables: oCViewDocumentMap_for_zCObject=0x0083BACC,
    //             oCViewDocumentMap (zCInputCallback)=0x0083BA9C,
    //             oCViewDocumentMap_for_zCViewBase=0x0083BAA4.
    return vptr == 0x0083BACC || vptr == 0x0083BA9C || vptr == 0x0083BAA4;
}

static oCViewDocumentMap* MapDiagFindMapView(bool logIt) {
    oCDocumentManager& mgr = oCDocumentManager::GetDocumentManager();
    zCListSort<oCViewDocument>* node = mgr.ListDocuments;
    oCViewDocumentMap* found = NULL;
    for (int guard = 0; node && guard < 32; node = node->next, ++guard) {
        oCViewDocument* view = node->data;
        if (!view) continue;
        unsigned int vptr = *(unsigned int*)view;
        bool isMap = MapDiagIsMapVptr(vptr);
        if (logIt)
            MapDiagLog("[docs] node view=%p vptr=%08X isMap=%d", view, vptr, (int)isMap);
        if (isMap) found = (oCViewDocumentMap*)view;
    }
    if (logIt) MapDiagLog("[docs] mapView=%p", found);
    return found;
}

// Refresh from the actual document list, not from the first key event. This
// also lets mouse turning and movement unlock activate immediately on map open.
static void MapDiagRefreshMapState() {
    static DWORD lastCheck = 0;
    DWORD now = GetTickCount();
    if (now - lastCheck < 50) return;
    lastCheck = now;

    oCViewDocumentMap* current = MapDiagFindMapView(false);
    if (current == g_mapView) return;

    bool wasOpen = g_docOpen;
    g_mapView = current;
    g_docOpen = (current != NULL);
    g_frameDx = 0.0f;
    if (!current) g_arrowPreparedMap = NULL;
    g_smoothArrowTexture = NULL;
    g_smoothArrowWidth = 0;
    g_smoothArrowHeight = 0;
    g_smoothArrowBaseAngle = 1.57079632679f;
    g_smoothArrowAngle = 1.57079632679f;
    g_lastSmoothArrowAngleUpdate = 0;
    g_smoothArrowTextureReady = false;

    if (g_docOpen) {
        g_lastStats = 0;
        MapDiagLog("[doc] OPEN detected from ListDocuments mapView=%p", current);
        MapDiagFindMapView(true);
        MapDiagHeroProbe("map-open");
        MapDiagDumpCallers("world", 0);
    } else if (wasOpen) {
        MapDiagLog("[doc] CLOSE detected from ListDocuments");
        MapDiagHeroProbe("map-close");
        MapDiagDumpCallers("map-open", 1);
    }
}

// ---------------------------------------------------------------- DocumentManager::HandleEvent
// oCDocumentManager::HandleEvent(int) — G2A 0x0065F450.
// Kept from iteration 2 (movement keys are not swallowed). Also drives g_docOpen.
static bool MapDiagIsMovementKey(int event) {
    switch ((unsigned int)event & 0xFF) {
    case 0x11: case 0x1F: case 0x1E: case 0x20: case 0x10: case 0x12:   // W S A D Q E
    case 0xC8: case 0xD0: case 0xCB: case 0xCD:                          // arrows
        return true;
    default:
        return false;
    }
}

int __fastcall Hook_DocMan_HandleEvent(void* self, void* edx, int event);
CInvoke<int(__fastcall*)(void*, void*, int)> Ivk_DocMan_HandleEvent(
    0x0065F450, &Hook_DocMan_HandleEvent, IvkEnabled(Engine_G2A));

int __fastcall Hook_DocMan_HandleEvent(void* self, void* edx, int event) {
    int originalResult = Ivk_DocMan_HandleEvent(self, edx, event);

    if (((unsigned int)event & 0xFF) == 0x01) {
        MapDiagLog("[doc] ESC event original=%d", originalResult);
    }

    static DWORD lastLog[256] = { 0 };
    unsigned int slot = (unsigned int)event & 0xFF;
    DWORD now = GetTickCount();
    bool passMovement = MAPDIAG_PASS_KEYS && MapDiagIsMovementKey(event);
    int result = passMovement ? 0 : originalResult;
    if (now - lastLog[slot] >= 200 && MapDiagAllowInputLog()) {
        MapDiagLog("[map-input] HandleEvent event=0x%08X original=%d returned=%d", (unsigned)event, originalResult, result);
        lastLog[slot] = now;
    }
    return result;
}


// Clear the hero's movement lock while a document is open.
static void MapDiagUnlock() {
#if MAPDIAG_UNLOCK
    if (!g_docOpen) return;
    oCNpc* hero = Gothic_II_Addon::player;
    if (!hero) return;
    if (hero->IsMovLock()) {
        hero->SetMovLock(0);
        static unsigned long cleared = 0;
        ++cleared;
        if (cleared <= 3 || cleared % 300 == 0)
            MapDiagLog("[unlock] SetMovLock(0) cleared=%lu now=%d", cleared, (int)hero->IsMovLock());
    }
#endif
}

// 10 Hz: ask the engine to recompute the GPS marker (position + heading) of the open map.
static void MapDiagMarker() {
#if MAPDIAG_MARKER
    if (!g_docOpen) return;
    static DWORD last = 0;
    static unsigned long n = 0;
    static bool warned = false;
    DWORD now = GetTickCount();
    if (now - last < MAPDIAG_MARKER_MS) return;
    last = now;
    oCViewDocumentMap* map = g_mapView;
    if (!map) {
        if (!warned) { warned = true; MapDiagLog("[marker] no map view in ListDocuments (open document is not a map, or type check failed)"); }
        return;
    }
    warned = false;
    map->UpdatePosition();
    MapDiagPrepareSmoothArrow();
    ++n;
    if (n <= 3 || n % 10 == 0) {
        oCNpc* hero = Gothic_II_Addon::player;
        if (hero) {
            float px = 0, py = 0, pz = 0;
            hero->GetPositionWorld(px, py, pz);
            zVEC3 at = hero->GetAtVectorWorld();
            MapDiagLog("[marker] UpdatePosition #%lu map=%p hero=(%.0f, %.0f, %.0f) at=(%.2f, %.2f)", n, map, px, py, pz, at.n[0], at.n[2]);
        }
    }
#endif
}

// Own horizontal mouse turn while the map is open (the game's own mouse turn is gated).
static void MapDiagMouseTurn() {
#if MAPDIAG_MOUSE_TURN
    if (!g_docOpen) { g_frameDx = 0.0f; return; }

    // live tuning (edge-triggered)
    static bool prevAdd = false, prevSub = false, prevMul = false;
    bool add = (GetAsyncKeyState(VK_ADD) & 0x8000) != 0;
    bool sub = (GetAsyncKeyState(VK_SUBTRACT) & 0x8000) != 0;
    bool mul = (GetAsyncKeyState(VK_MULTIPLY) & 0x8000) != 0;
    if (add && !prevAdd) g_degPerUnit *= 1.25f;
    if (sub && !prevSub) g_degPerUnit *= 0.8f;
    if (mul && !prevMul) g_mouseSign = -g_mouseSign;
    if ((add && !prevAdd) || (sub && !prevSub) || (mul && !prevMul))
        MapDiagLog("[mouse] tuning: degPerUnit=%.3f sign=%+.0f", g_degPerUnit, g_mouseSign);
    prevAdd = add; prevSub = sub; prevMul = mul;

    float dx = g_frameDx;
    g_frameDx = 0.0f;
    if (dx == 0.0f) return;
    oCNpc* hero = Gothic_II_Addon::player;
    if (!hero) return;
    hero->RotateWorldY(dx * g_degPerUnit * g_mouseSign);
    static unsigned long n = 0;
    ++n;
    if (n <= 5 || n % 100 == 0)
        MapDiagLog("[mouse] turn #%lu dx=%.3f deg=%.3f", n, dx, dx * g_degPerUnit * g_mouseSign);
#endif
}

// ---------------------------------------------------------------- hook state / runtime probes
void MapDiagHookState(const char* name, THookInfo& info) {
    MapDiagLog("[hook] %s hooked=%d exists=%d original=%p trampoline=%p detour=%p",
        name, (int)CCallBack::IsHooked(info), (int)CCallBack::IsExist(info),
        info.lpOriginal, info.lpPointer, info.lpDetour);
}

void MapDiagRuntimeProbe(const char* stage) {
    Gothic_II_Addon::zCInput* inputObject = Gothic_II_Addon::zinput;
    if (!inputObject) {
        MapDiagLog("[runtime] %s zinput=NULL", stage);
        return;
    }
    MapDiagLog("[runtime] %s zinput=%p", stage, inputObject);
}

// ---------------------------------------------------------------- Union application callbacks
void Game_Entry();
void Game_Init();
void Game_Exit();
void Game_PreLoop();
void Game_Loop();
void Game_PostLoop();
void Game_MenuLoop();
void Game_SaveBegin();
void Game_SaveEnd();
void Game_LoadBegin_NewGame();
void Game_LoadEnd_NewGame();
void Game_LoadBegin_SaveGame();
void Game_LoadEnd_SaveGame();
void Game_LoadBegin_ChangeLevel();
void Game_LoadEnd_ChangeLevel();
void Game_LoadBegin_Trigger();
void Game_LoadEnd_Trigger();
void Game_Pause();
void Game_Unpause();
void Game_DefineExternals();
void Game_ApplyOptions();

TSaveLoadGameInfo& SaveLoadGameInfo = UnionCore::SaveLoadGameInfo;

extern CApplication* lpApplication;

void Game_Entry() {
    MapDiagReset();
    MapDiagLog("[plugin] Game_Entry (iteration 5, UNLOCK=%d MARKER=%d MOUSE=%d)", (int)MAPDIAG_UNLOCK, (int)MAPDIAG_MARKER, (int)MAPDIAG_MOUSE_TURN);
    MapDiagCwd("Game_Entry");
    MapDiagHookState("Input::GetState", const_cast<THookInfo&>(Ivk_Input_GetState.GetHookInfo()));
    MapDiagHookState("Input::GetToggled", const_cast<THookInfo&>(Ivk_Input_GetToggled.GetHookInfo()));
    MapDiagHookState("Input::GetMousePos", const_cast<THookInfo&>(Ivk_Input_GetMousePos.GetHookInfo()));
    MapDiagHookState("DocumentManager::HandleEvent", const_cast<THookInfo&>(Ivk_DocMan_HandleEvent.GetHookInfo()));
    MapDiagHookState("ViewArrow::Blit", const_cast<THookInfo&>(Ivk_ViewArrow_Blit.GetHookInfo()));
    MapDiagHookState("Game::CheckObjectRoutines", const_cast<THookInfo&>(Ivk_Game_CheckObjectRoutines.GetHookInfo()));
    MapDiagRuntimeProbe("Game_Entry");
}

void Game_Init() { MapDiagLog("[callback] Game_Init"); MapDiagRuntimeProbe("Game_Init"); }
void Game_Exit() { MapDiagLog("[callback] Game_Exit"); }
void Game_PreLoop() { ++g_cPre; MapDiagRefreshMapState(); MapDiagUnlock(); MapDiagMouseTurn(); MapDiagMarker(); MapDiagTick(); }
void Game_Loop() {
    ++g_cLoop;
    MapDiagUnlock();
    MapDiagTick();
}
void Game_PostLoop() { ++g_cPost; }
void Game_MenuLoop() { ++g_cMenu; MapDiagTick(); }

void Game_SaveBegin() { MapDiagLog("[callback] Game_SaveBegin"); }
void Game_SaveEnd() { MapDiagLog("[callback] Game_SaveEnd"); }
void Game_LoadBegin_NewGame() { g_docOpen = false; MapDiagLog("[callback] Game_LoadBegin_NewGame"); }
void Game_LoadEnd_NewGame() { MapDiagLog("[callback] Game_LoadEnd_NewGame"); }
void Game_LoadBegin_SaveGame() { g_docOpen = false; MapDiagLog("[callback] Game_LoadBegin_SaveGame"); }
void Game_LoadEnd_SaveGame() { MapDiagLog("[callback] Game_LoadEnd_SaveGame"); }
void Game_LoadBegin_ChangeLevel() { g_docOpen = false; MapDiagLog("[callback] Game_LoadBegin_ChangeLevel"); }
void Game_LoadEnd_ChangeLevel() { MapDiagLog("[callback] Game_LoadEnd_ChangeLevel"); }
void Game_LoadBegin_Trigger() { }
void Game_LoadEnd_Trigger() { }
void Game_Pause() { MapDiagLog("[callback] Game_Pause doc=%d", (int)g_docOpen); }
void Game_Unpause() { MapDiagLog("[callback] Game_Unpause doc=%d", (int)g_docOpen); }
void Game_DefineExternals() { MapDiagLog("[callback] Game_DefineExternals"); }
void Game_ApplyOptions() { }

#define AppDefault True
CApplication* lpApplication = !CHECK_THIS_ENGINE ? Null : CApplication::CreateRefApplication(
    Enabled(AppDefault) Game_Entry,
    Enabled(AppDefault) Game_Init,
    Enabled(AppDefault) Game_Exit,
    Enabled(AppDefault) Game_PreLoop,
    Enabled(AppDefault) Game_Loop,
    Enabled(AppDefault) Game_PostLoop,
    Enabled(AppDefault) Game_MenuLoop,
    Enabled(AppDefault) Game_SaveBegin,
    Enabled(AppDefault) Game_SaveEnd,
    Enabled(AppDefault) Game_LoadBegin_NewGame,
    Enabled(AppDefault) Game_LoadEnd_NewGame,
    Enabled(AppDefault) Game_LoadBegin_SaveGame,
    Enabled(AppDefault) Game_LoadEnd_SaveGame,
    Enabled(AppDefault) Game_LoadBegin_ChangeLevel,
    Enabled(AppDefault) Game_LoadEnd_ChangeLevel,
    Enabled(AppDefault) Game_LoadBegin_Trigger,
    Enabled(AppDefault) Game_LoadEnd_Trigger,
    Enabled(AppDefault) Game_Pause,
    Enabled(AppDefault) Game_Unpause,
    Enabled(AppDefault) Game_DefineExternals,
    Enabled(AppDefault) Game_ApplyOptions
);

} // namespace GOTHIC_ENGINE
