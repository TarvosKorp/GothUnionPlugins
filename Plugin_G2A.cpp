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
static float g_frameDx = 0.0f;

// Mouse turn state.
static float g_degPerUnit = MAPDIAG_MOUSE_DEG_PER_UNIT;
static float g_mouseSign = 1.0f;

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


// ---------------------------------------------------------------- smooth map arrow
// The stock map arrow is an eight-page document. Hide only that small arrow view
// and draw a continuously oriented pointer into the same view after the screen
// has rendered its normal contents.
static oCViewDocumentMap* g_arrowPreparedMap = NULL;
static oCViewDocumentMap* g_earlyMapView = NULL;
static zCTexture* g_smoothArrowTexture = NULL;
static int g_smoothArrowWidth = 0;
static int g_smoothArrowHeight = 0;
static float g_smoothArrowBaseAngle = 0.0f;
static float g_smoothArrowAngle = 1.57079632679f;
static DWORD g_lastSmoothArrowAngleUpdate = 0;
static bool g_smoothArrowTextureReady = false;
static bool g_captureNativeArrowBlit = false;
static bool g_drawingSmoothArrow = false;
static bool g_diagMapPageSeen = false;
static bool g_diagMapPageLogged = false;
static bool g_diagSmoothArrowLogged = false;
static float g_diagMapLeft = 0.0f;
static float g_diagMapRight = 0.0f;
static unsigned long g_diagDrawSequence = 0;
static int g_diagDrawHookDepth = 0;
static bool g_nativeArrowCenterValid = false;
static float g_nativeArrowCenterX = 0.0f;
static float g_nativeArrowCenterY = 0.0f;
static bool g_nativeArrowGeometryLogged = false;
static bool g_arrowDrawLogged = false;
static bool g_arrowBlitLogged = false;
static bool g_markerLogged = false;

static void MapDiagPrepareSmoothArrow() {
    if (!g_docOpen || !g_mapView || g_arrowPreparedMap == g_mapView) return;
    // Hide the engine's eight-page arrow and load its canonical north-facing
    // artwork directly. Never depend on whichever page the hero happened to
    // be facing when the map was opened.
    g_mapView->ViewArrow.SetTextureAlpha(0);
    g_arrowPreparedMap = g_mapView;
    g_nativeArrowGeometryLogged = false;
    g_diagMapPageSeen = false;
    g_diagMapPageLogged = false;
    g_diagSmoothArrowLogged = false;
    g_diagMapLeft = 0.0f;
    g_diagMapRight = 0.0f;
    g_arrowDrawLogged = false;
    g_arrowBlitLogged = false;
    g_markerLogged = false;

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
        g_earlyMapView = map;
        map->ViewArrow.SetTextureAlpha(0);
    }
    Ivk_MapView_UpdatePosition(self, edx);
}

static void MapDiagDrawSmoothArrow(oCViewDocumentMap* map) {
    // Use the exact map instance whose ViewArrow::Blit is currently executing.
    // g_earlyMapView may still point to a previous map document after close/reopen.
    if (!map) return;
    oCNpc* hero = Gothic_II_Addon::player;
    zCRenderer* renderer = Gothic_II_Addon::zrenderer;
    if (!hero || !renderer) return;

    oCViewDocument& arrow = map->ViewArrow;
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
    // Mark only our own submission so the renderer hook can distinguish it
    // from the native arrow and the map page.
    const bool wasDrawingSmoothArrow = g_drawingSmoothArrow;
    g_drawingSmoothArrow = true;
    renderer->DrawPolySimple(texture, vertices, 4);
    g_drawingSmoothArrow = wasDrawingSmoothArrow;
    renderer->SetBilerpFilterEnabled(oldBilerp);
    renderer->SetAlphaBlendFunc(oldAlpha);
    renderer->SetZBufferCompare(oldZCmp);
    renderer->SetZBufferWriteEnabled(oldZWrite);

    if (!g_arrowDrawLogged) {
        g_arrowDrawLogged = true;
        MapDiagLog("[arrow] first draw rect=%d,%d %dx%d center=%.2f,%.2f anchor=%s screen=%dx%d rotation=%.3f",
            x, y, w, h, cx, cy,
            g_nativeArrowCenterValid ? "native" : "view-fallback",
            sw, sh, rotation);
    }
}

// Capture the geometry submitted by the engine while ViewArrow::Blit is active.
// This gives us the real vanilla anchor rather than inferring it from view bounds.
void __fastcall Hook_MapDiag_DrawPolySimple(void* self, void* edx, zCTexture* tex, zTRndSimpleVertex* verts, int count);
CInvoke<void(__fastcall*)(void*, void*, zCTexture*, zTRndSimpleVertex*, int)> Ivk_MapDiag_DrawPolySimple(
    0x0064AC30, &Hook_MapDiag_DrawPolySimple, IvkEnabled(Engine_G2A));

void __fastcall Hook_MapDiag_DrawPolySimple(void* self, void* edx, zCTexture* tex, zTRndSimpleVertex* verts, int count) {
    const int hookDepth = ++g_diagDrawHookDepth;
    const unsigned long sequence = ++g_diagDrawSequence;

    // Observe the map-page and replacement-arrow calls at the exact shared
    // DrawPolySimple hook point. MirrorWorld uses the same target and applies
    // its X transform only after it has recognized the map page.
    if (g_docOpen && verts && count > 0 && count <= 64) {
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
        zSTRING textureName = tex ? tex->GetObjectName() : zSTRING("<null>");
        const char* textureText = textureName.ToChar();
        const bool namedMapPage = textureText && _strnicmp(textureText, "MAP", 3) == 0;
        const int screenW = *(int*)((char*)Gothic_II_Addon::zrenderer + 0x98C);
        const int screenH = *(int*)((char*)Gothic_II_Addon::zrenderer + 0x990);
        const bool largeMapQuad = !g_diagMapPageSeen && screenW > 0 && screenH > 0 &&
            (maxX - minX) >= 0.3f * (float)screenW && (maxY - minY) >= 0.4f * (float)screenH;
        if (!g_diagMapPageSeen && (namedMapPage || largeMapQuad)) {
            g_diagMapPageSeen = true;
            g_diagMapLeft = minX;
            g_diagMapRight = maxX;
            if (!g_diagMapPageLogged) {
                g_diagMapPageLogged = true;
                MapDiagLog("[draw-chain] map-page seq=%lu depth=%d tex='%s' bounds=%.2f,%.2f..%.2f,%.2f count=%d detect=%s",
                    sequence, hookDepth, textureText ? textureText : "<null>",
                    minX, minY, maxX, maxY, count, namedMapPage ? "name" : "large-quad");
            }
        }
        if (g_drawingSmoothArrow && !g_diagSmoothArrowLogged) {
            g_diagSmoothArrowLogged = true;
            const bool inside = g_diagMapPageSeen &&
                minX >= g_diagMapLeft - 1.0f && maxX <= g_diagMapRight + 1.0f;
            MapDiagLog("[draw-chain] smooth-arrow seq=%lu depth=%d tex='%s' bounds=%.2f,%.2f..%.2f,%.2f mapPageSeen=%d mapX=%.2f..%.2f inside=%d count=%d",
                sequence, hookDepth, textureText ? textureText : "<null>",
                minX, minY, maxX, maxY, (int)g_diagMapPageSeen,
                g_diagMapLeft, g_diagMapRight, (int)inside, count);
        }
    }

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
            if (!g_nativeArrowGeometryLogged) {
                g_nativeArrowGeometryLogged = true;
                zSTRING texName = tex ? tex->GetObjectName() : zSTRING("<null>");
                MapDiagLog("[arrow-anchor] native quad tex='%s' bounds=%.2f,%.2f..%.2f,%.2f center=%.2f,%.2f size=%.2fx%.2f count=%d",
                    texName.ToChar(), minX, minY, maxX, maxY,
                    g_nativeArrowCenterX, g_nativeArrowCenterY, width, height, count);
            }
        }
    }
    Ivk_MapDiag_DrawPolySimple(self, edx, tex, verts, count);
    --g_diagDrawHookDepth;
}

// zCViewPrint::Blit() — G2A 0x006934A0.
// ViewArrow inherits this exact Blit implementation. Draw the replacement
// immediately after its own native page has been blitted, not after root screen.
void __fastcall Hook_ViewArrow_Blit(void* self, void* edx);
CInvoke<void(__fastcall*)(void*, void*)> Ivk_ViewArrow_Blit(
    0x006934A0, &Hook_ViewArrow_Blit, IvkEnabled(Engine_G2A));

void __fastcall Hook_ViewArrow_Blit(void* self, void* edx) {
    oCViewDocumentMap* arrowMap = NULL;
    if (g_mapView && self == (void*)&g_mapView->ViewArrow)
        arrowMap = g_mapView;
    else if (g_earlyMapView && self == (void*)&g_earlyMapView->ViewArrow)
        arrowMap = g_earlyMapView;
    const bool isArrow = arrowMap != NULL;

    if (isArrow) {
        ((oCViewDocument*)self)->SetTextureAlpha(0);
        g_captureNativeArrowBlit = true;
        g_nativeArrowCenterValid = false;
    }

    Ivk_ViewArrow_Blit(self, edx);

    if (isArrow) {
        g_captureNativeArrowBlit = false;
        if (!g_arrowBlitLogged) {
            g_arrowBlitLogged = true;
            MapDiagLog("[arrow] first Blit self=%p map=%p anchor=%d center=%.2f,%.2f",
                self, g_mapView ? g_mapView : g_earlyMapView, (int)g_nativeArrowCenterValid,
                g_nativeArrowCenterX, g_nativeArrowCenterY);
        }
        // Submit the replacement at the exact point where the stock arrow is
        // rendered. MirrorWorld processes map quads in this UI draw phase;
        // CheckObjectRoutines is too late for its per-map-quad transform.
        MapDiagDrawSmoothArrow(arrowMap);
    }
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

#if MAPDIAG_FORCE_MOVE
    if (g_docOpen && raw == 0.0f) {
        float forced = MapDiagForcedValue(key);
        if (forced != 0.0f) value = forced;
    }
#endif
    return value;
}

// zCInput_Win32::GetToggled(unsigned short) — G2A 0x004D5020.
int __fastcall Hook_Input_GetToggled(void* self, void* edx, unsigned short key);
CInvoke<int(__fastcall*)(void*, void*, unsigned short)> Ivk_Input_GetToggled(
    0x004D5020, &Hook_Input_GetToggled, IvkEnabled(Engine_G2A));

int __fastcall Hook_Input_GetToggled(void* self, void* edx, unsigned short key) {
    return Ivk_Input_GetToggled(self, edx, key);
}

// zCInput_Win32::GetMousePos(float&, float&, float&) — G2A 0x004D5730.
void __fastcall Hook_Input_GetMousePos(void* self, void* edx, float& x, float& y, float& z);
CInvoke<void(__fastcall*)(void*, void*, float&, float&, float&)> Ivk_Input_GetMousePos(
    0x004D5730, &Hook_Input_GetMousePos, IvkEnabled(Engine_G2A));

void __fastcall Hook_Input_GetMousePos(void* self, void* edx, float& x, float& y, float& z) {
    Ivk_Input_GetMousePos(self, edx, x, y, z);
    if (g_docOpen && fabsf(x) > fabsf(g_frameDx)) g_frameDx = x;
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
        if (isMap) found = (oCViewDocumentMap*)view;
    }
    if (logIt) MapDiagLog("[docs] mapView=%p vptr=%08X", found, found ? *(unsigned int*)found : 0);
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
    g_earlyMapView = current;
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
        MapDiagLog("[doc] OPEN mapView=%p", current);
        MapDiagFindMapView(true);
        MapDiagHeroProbe("map-open");
    } else if (wasOpen) {
        MapDiagLog("[doc] CLOSE");
        MapDiagHeroProbe("map-close");
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

    if (((unsigned int)event & 0xFF) == 0x01)
        MapDiagLog("[doc] ESC close event original=%d", originalResult);

    bool passMovement = MAPDIAG_PASS_KEYS && MapDiagIsMovementKey(event);
    return passMovement ? 0 : originalResult;
}


// Clear the hero's movement lock while a document is open.
static void MapDiagUnlock() {
#if MAPDIAG_UNLOCK
    if (!g_docOpen) return;
    oCNpc* hero = Gothic_II_Addon::player;
    if (!hero) return;
    if (hero->IsMovLock()) {
        hero->SetMovLock(0);
        static bool logged = false;
        if (!logged) {
            logged = true;
            MapDiagLog("[unlock] movement lock cleared");
        }
    }
#endif
}

// 10 Hz: ask the engine to recompute the GPS marker (position + heading) of the open map.
static void MapDiagMarker() {
#if MAPDIAG_MARKER
    if (!g_docOpen) return;
    static DWORD last = 0;
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
    if (!g_markerLogged) {
        g_markerLogged = true;
        oCNpc* hero = Gothic_II_Addon::player;
        if (hero) {
            float px = 0, py = 0, pz = 0;
            hero->GetPositionWorld(px, py, pz);
            zVEC3 at = hero->GetAtVectorWorld();
            MapDiagLog("[marker] first update map=%p hero=(%.0f,%.0f,%.0f) heading=(%.2f,%.2f)",
                map, px, py, pz, at.n[0], at.n[2]);
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
    // No per-frame mouse logging: input deltas are high-frequency and not relevant
    // to the current marker-anchor investigation.
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
    MapDiagLog("[plugin] Game_Entry UNLOCK=%d MARKER=%d MOUSE=%d", (int)MAPDIAG_UNLOCK, (int)MAPDIAG_MARKER, (int)MAPDIAG_MOUSE_TURN);
    MapDiagHookState("Input::GetState", const_cast<THookInfo&>(Ivk_Input_GetState.GetHookInfo()));
    MapDiagHookState("Input::GetToggled", const_cast<THookInfo&>(Ivk_Input_GetToggled.GetHookInfo()));
    MapDiagHookState("Input::GetMousePos", const_cast<THookInfo&>(Ivk_Input_GetMousePos.GetHookInfo()));
    MapDiagHookState("DocumentManager::HandleEvent", const_cast<THookInfo&>(Ivk_DocMan_HandleEvent.GetHookInfo()));
    MapDiagHookState("ViewArrow::Blit", const_cast<THookInfo&>(Ivk_ViewArrow_Blit.GetHookInfo()));
    MapDiagHookState("Renderer::DrawPolySimple", const_cast<THookInfo&>(Ivk_MapDiag_DrawPolySimple.GetHookInfo()));
    MapDiagHookState("MapView::UpdatePosition", const_cast<THookInfo&>(Ivk_MapView_UpdatePosition.GetHookInfo()));
    MapDiagRuntimeProbe("Game_Entry");
}

void Game_Init() { }
void Game_Exit() { MapDiagLog("[plugin] Game_Exit"); }
void Game_PreLoop() { MapDiagRefreshMapState(); MapDiagUnlock(); MapDiagMouseTurn(); MapDiagMarker(); }
void Game_Loop() { MapDiagUnlock(); }
void Game_PostLoop() { }
void Game_MenuLoop() { }

void Game_SaveBegin() { }
void Game_SaveEnd() { }
void Game_LoadBegin_NewGame() { g_docOpen = false; }
void Game_LoadEnd_NewGame() { }
void Game_LoadBegin_SaveGame() { g_docOpen = false; }
void Game_LoadEnd_SaveGame() { }
void Game_LoadBegin_ChangeLevel() { g_docOpen = false; }
void Game_LoadEnd_ChangeLevel() { }
void Game_LoadBegin_Trigger() { }
void Game_LoadEnd_Trigger() { }
void Game_Pause() { }
void Game_Unpause() { }
void Game_DefineExternals() { }
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
