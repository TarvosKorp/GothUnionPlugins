#include <string.h>
#include <math.h>
#include <stdlib.h>

// zDynaMap — live map marker, movement unlock, mouse turning and configurable map fade.
// Engine-specific addresses and renderer offsets are taken from the matching Union SDK API.

namespace GOTHIC_ENGINE {

static const DWORD MAP_MARKER_UPDATE_MS = 40;
static const DWORD MAP_FADE_DURATION_MS = 300;
static const DWORD MAP_OPEN_GRACE_MS = 1200;
static const float MAP_MOUSE_DEGREES_PER_UNIT = 0.30f;

static const unsigned int MAP_RENDERER_WIDTH_OFFSET = 0x984;
static const unsigned int MAP_RENDERER_HEIGHT_OFFSET = 0x988;

static volatile bool g_docOpen = false;
static DWORD g_mapOpenedAt = 0;
static oCViewDocumentMap* g_mapView = NULL;
static float g_frameDx = 0.0f;

static bool g_pluginEnabled = true;
static bool g_mapAlphaEnabled = true;
static int g_mapTransparency = 50;
static int g_mapCurrentAlpha = 255;

// Smooth replacement for the engine's eight-direction map arrow.
static oCViewDocumentMap* g_arrowPreparedMap = NULL;
static zCTexture* g_smoothArrowTexture = NULL;
static int g_smoothArrowWidth = 0;
static int g_smoothArrowHeight = 0;
static float g_smoothArrowAngle = 1.57079632679f;
static DWORD g_lastSmoothArrowAngleUpdate = 0;
static bool g_smoothArrowTextureReady = false;

// Keep the first native arrow position as the anchor. Later engine refreshes
// can shift PixelPosition slightly, so retain the correction measured from
// the engine's own UpdatePosition calls.
static oCViewDocumentMap* g_nativeAnchorMap = NULL;
static bool g_nativeAnchorCaptured = false;
static bool g_nativeAnchorCorrectionReady = false;
static int g_nativeAnchorX = 0;
static int g_nativeAnchorY = 0;
static int g_nativeAnchorCorrectionX = 0;
static int g_nativeAnchorCorrectionY = 0;

// Match the engine's native multiple-inheritance vtables exactly.
// The document manager stores the zCObject-side pointer in ListDocuments.
static bool IsMapDocument(oCViewDocument* view) {
    if (!view) return false;
    const unsigned int vptr = *(const unsigned int*)view;
    return vptr == 0x7DF45C ||
        vptr == 0x7DF42C ||
        vptr == 0x7DF434;
}

static oCViewDocumentMap* FindMapView() {
    oCDocumentManager& manager = oCDocumentManager::GetDocumentManager();
    zCListSort<oCViewDocument>* node = manager.ListDocuments;
    oCViewDocumentMap* found = NULL;
    for (int guard = 0; node && guard < 32; node = node->next, ++guard) {
        oCViewDocument* view = node->data;
        if (IsMapDocument(view))
            found = (oCViewDocumentMap*)view;
    }
    return found;
}

static void PrepareSmoothArrow() {
    if (!g_pluginEnabled || !g_docOpen || !g_mapView || g_arrowPreparedMap == g_mapView)
        return;

    g_mapView->ViewArrow.SetTextureAlpha(0);
    g_arrowPreparedMap = g_mapView;

    // Keep one reference to U.TGA for the plugin lifetime and reuse it
    // whenever the map is reopened.
    if (!g_smoothArrowTexture) {
        zSTRING northName("U.TGA");
        g_smoothArrowTexture = zCTexture::Load(northName, 0);
        g_smoothArrowTextureReady = (g_smoothArrowTexture != NULL);
        if (g_smoothArrowTextureReady) {
            g_smoothArrowTexture->GetPixelSize(g_smoothArrowWidth, g_smoothArrowHeight);
        }
    }
}

void __fastcall Hook_MapView_UpdatePosition(void* self, void* edx);
CInvoke<void(__fastcall*)(void*, void*)> Ivk_MapView_UpdatePosition(
    0x00726030, &Hook_MapView_UpdatePosition, IvkEnabled(Engine_G1));

void __fastcall Hook_MapView_UpdatePosition(void* self, void* edx) {
    oCViewDocumentMap* map = (oCViewDocumentMap*)self;
    if (map && g_pluginEnabled)
        map->ViewArrow.SetTextureAlpha(0);

    Ivk_MapView_UpdatePosition(self, edx);

    if (!map || !g_pluginEnabled)
        return;

    if (g_nativeAnchorMap != map) {
        g_nativeAnchorMap = map;
        g_nativeAnchorCaptured = false;
        g_nativeAnchorCorrectionReady = false;
        g_nativeAnchorCorrectionX = 0;
        g_nativeAnchorCorrectionY = 0;
    }

    const int nativeX = map->ViewArrow.PixelPosition.X;
    const int nativeY = map->ViewArrow.PixelPosition.Y;
    const int nativeW = map->ViewArrow.PixelSize.X;
    const int nativeH = map->ViewArrow.PixelSize.Y;

    if (!g_nativeAnchorCaptured && nativeW > 0 && nativeH > 0) {
        g_nativeAnchorX = nativeX;
        g_nativeAnchorY = nativeY;
        g_nativeAnchorCaptured = true;
    } else if (g_nativeAnchorCaptured && !g_nativeAnchorCorrectionReady) {
        const int dx = g_nativeAnchorX - nativeX;
        const int dy = g_nativeAnchorY - nativeY;
        if ((dx != 0 || dy != 0) &&
            dx >= -128 && dx <= 128 && dy >= -128 && dy <= 128) {
            g_nativeAnchorCorrectionX = dx;
            g_nativeAnchorCorrectionY = dy;
            g_nativeAnchorCorrectionReady = true;
        }
    }
}

static void DrawSmoothArrow() {
    if (!g_pluginEnabled || !g_docOpen || !g_mapView)
        return;

    oCNpc* hero = GOTHIC_ENGINE::player;
    zCRenderer* renderer = GOTHIC_ENGINE::zrenderer;
    if (!hero || !renderer)
        return;

    oCViewDocument& arrow = g_mapView->ViewArrow;
    zCTexture* currentTexture = arrow.Texture;
    const int x = arrow.PixelPosition.X;
    const int y = arrow.PixelPosition.Y;
    const int currentW = arrow.PixelSize.X;
    const int currentH = arrow.PixelSize.Y;

    if (!currentTexture || currentW < 4 || currentH < 4)
        return;

    const DWORD now = GetTickCount();
    if (g_lastSmoothArrowAngleUpdate == 0 ||
        now - g_lastSmoothArrowAngleUpdate >= MAP_MARKER_UPDATE_MS) {
        zVEC3 heading = hero->GetAtVectorWorld();
        float dx = heading.n[0];
        float dy = -heading.n[2];
        const float length = sqrtf(dx * dx + dy * dy);
        if (length >= 0.001f) {
            dx /= length;
            dy /= length;
            g_smoothArrowAngle = atan2f(dy, dx);
            g_lastSmoothArrowAngleUpdate = now;
        }
    }

    if (!g_smoothArrowTextureReady || !g_smoothArrowTexture ||
        g_smoothArrowWidth < 4 || g_smoothArrowHeight < 4)
        return;

    const float rotation = g_smoothArrowAngle - 1.57079632679f;
    const float c = cosf(rotation);
    const float sn = sinf(rotation);

    const int screenWidth = *(int*)((char*)renderer + MAP_RENDERER_WIDTH_OFFSET);
    const int screenHeight = *(int*)((char*)renderer + MAP_RENDERER_HEIGHT_OFFSET);
    if (screenWidth < 320 || screenWidth > 16384 ||
        screenHeight < 200 || screenHeight > 16384)
        return;

    const int anchorOffsetX = g_nativeAnchorCorrectionReady
        ? g_nativeAnchorCorrectionX : currentW / 2;
    const int anchorOffsetY = g_nativeAnchorCorrectionReady
        ? g_nativeAnchorCorrectionY : currentH / 2;

    const float halfW = (float)currentW * 0.5f;
    const float halfH = (float)currentH * 0.5f;
    const float cx = (float)x + (float)anchorOffsetX + halfW;
    const float cy = (float)y + (float)anchorOffsetY + halfH;

    // The marker is always half as transparent as the map itself.
    const int markerAlpha = (255 + g_mapCurrentAlpha + 1) / 2;

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
        vertices[i].color = zCOLOR(255, 255, 255, markerAlpha);
    }

    const int oldZWrite = renderer->GetZBufferWriteEnabled();
    const zTRnd_ZBufferCmp oldZCmp = renderer->GetZBufferCompare();
    const zTRnd_AlphaBlendFunc oldAlpha = renderer->GetAlphaBlendFunc();
    const int oldBilerp = renderer->GetBilerpFilterEnabled();

    renderer->SetZBufferWriteEnabled(FALSE);
    renderer->SetZBufferCompare(zRND_ZBUFFER_CMP_ALWAYS);
    renderer->SetAlphaBlendFunc(zRND_ALPHA_FUNC_BLEND);
    renderer->SetBilerpFilterEnabled(TRUE);
    renderer->SetViewport(0, 0, screenWidth, screenHeight);
    renderer->DrawPolySimple(g_smoothArrowTexture, vertices, 4);
    renderer->SetBilerpFilterEnabled(oldBilerp);
    renderer->SetAlphaBlendFunc(oldAlpha);
    renderer->SetZBufferCompare(oldZCmp);
    renderer->SetZBufferWriteEnabled(oldZWrite);
}

// zCViewPrint::Blit() — hide the stock eight-way arrow before it is rendered.
void __fastcall Hook_ViewArrow_Blit(void* self, void* edx);
CInvoke<void(__fastcall*)(void*, void*)> Ivk_ViewArrow_Blit(
    0x00756970, &Hook_ViewArrow_Blit, IvkEnabled(Engine_G1));

void __fastcall Hook_ViewArrow_Blit(void* self, void* edx) {
    if (g_pluginEnabled && g_docOpen && g_mapView &&
        self == (void*)&g_mapView->ViewArrow) {
        ((oCViewDocument*)self)->SetTextureAlpha(0);
    }
    Ivk_ViewArrow_Blit(self, edx);
}

// oCGame::CheckObjectRoutines() — submit the replacement marker in the map render stage.
void __fastcall Hook_Game_CheckObjectRoutines(void* self, void* edx);
CInvoke<void(__fastcall*)(void*, void*)> Ivk_Game_CheckObjectRoutines(
    0x00640160, &Hook_Game_CheckObjectRoutines, IvkEnabled(Engine_G1));

void __fastcall Hook_Game_CheckObjectRoutines(void* self, void* edx) {
    if (self == GOTHIC_ENGINE::ogame)
        DrawSmoothArrow();
    Ivk_Game_CheckObjectRoutines(self, edx);
}

// Collect the horizontal mouse delta while the map is open.
void __fastcall Hook_Input_GetMousePos(void* self, void* edx, float& x, float& y, float& z);
CInvoke<void(__fastcall*)(void*, void*, float&, float&, float&)> Ivk_Input_GetMousePos(
    0x004C8BD0, &Hook_Input_GetMousePos, IvkEnabled(Engine_G1));

void __fastcall Hook_Input_GetMousePos(void* self, void* edx, float& x, float& y, float& z) {
    Ivk_Input_GetMousePos(self, edx, x, y, z);
    if (g_docOpen && fabsf(x) > fabsf(g_frameDx))
        g_frameDx = x;
}

static void RefreshMapState() {
    static DWORD lastCheck = 0;
    const DWORD now = GetTickCount();
    if (now - lastCheck < 50)
        return;
    lastCheck = now;

    oCViewDocumentMap* current = FindMapView();
    if (current == g_mapView)
        return;

    g_mapView = current;
    g_docOpen = (current != NULL);
    g_mapOpenedAt = g_docOpen ? now : 0;
    g_frameDx = 0.0f;

    if (!current) {
        g_arrowPreparedMap = NULL;
        g_nativeAnchorMap = NULL;
        g_nativeAnchorCaptured = false;
        g_nativeAnchorCorrectionReady = false;
        g_nativeAnchorCorrectionX = 0;
        g_nativeAnchorCorrectionY = 0;
    } else if (g_nativeAnchorMap != current) {
        g_nativeAnchorMap = current;
        g_nativeAnchorCaptured = false;
        g_nativeAnchorCorrectionReady = false;
        g_nativeAnchorCorrectionX = 0;
        g_nativeAnchorCorrectionY = 0;
    }

    g_smoothArrowAngle = 1.57079632679f;
    g_lastSmoothArrowAngleUpdate = 0;
}

static void UnlockMovement() {
    if (!g_pluginEnabled || !g_docOpen)
        return;

    oCNpc* hero = GOTHIC_ENGINE::player;
    if (hero && hero->IsMovLock())
        hero->SetMovLock(0);
}

// Refresh the map position at a steady cadence, after the opening transition.
static void UpdateMapMarker() {
    if (!g_pluginEnabled || !g_docOpen)
        return;

    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (g_mapOpenedAt != 0 && now - g_mapOpenedAt < 250)
        return;
    if (now - last < MAP_MARKER_UPDATE_MS)
        return;
    last = now;

    if (!g_mapView)
        return;

    g_mapView->UpdatePosition();
    PrepareSmoothArrow();
}

// The game's own mouse turn is gated while the map is open; apply the captured
// horizontal delta directly to the hero instead.
static void ApplyMouseTurn() {
    if (!g_pluginEnabled || !g_docOpen) {
        g_frameDx = 0.0f;
        return;
    }

    const float dx = g_frameDx;
    g_frameDx = 0.0f;
    if (dx == 0.0f)
        return;

    oCNpc* hero = GOTHIC_ENGINE::player;
    if (hero)
        hero->RotateWorldY(dx * MAP_MOUSE_DEGREES_PER_UNIT);
}

// Fade the map while a movement key is physically held. The marker's alpha
// follows the map's actual in-transition alpha at a fixed 1:2 transparency ratio.
static void FadeMap() {
    static oCViewDocumentMap* lastMap = NULL;
    static oCViewDocument* lastPage = NULL;
    static zTRnd_AlphaBlendFunc originalMapBlend = zRND_ALPHA_FUNC_MAT_DEFAULT;
    static zTRnd_AlphaBlendFunc originalPageBlend = zRND_ALPHA_FUNC_MAT_DEFAULT;
    static int alpha = 255;
    static DWORD lastTick = 0;

    const DWORD now = GetTickCount();
    if (g_mapView != lastMap) {
        lastMap = g_mapView;
        lastPage = NULL;
        alpha = 255;
        g_mapCurrentAlpha = 255;
        lastTick = now;
        if (g_mapView)
            originalMapBlend = g_mapView->TextureFuncAlpha;
    }
    if (!g_mapView)
        return;

    oCViewDocument* page = g_mapView->ViewPageMap;
    if (page && page != lastPage) {
        lastPage = page;
        originalPageBlend = page->TextureFuncAlpha;
    }

    const bool moving =
        (GetAsyncKeyState('W') & 0x8000) != 0 ||
        (GetAsyncKeyState('S') & 0x8000) != 0 ||
        (GetAsyncKeyState('A') & 0x8000) != 0 ||
        (GetAsyncKeyState('D') & 0x8000) != 0 ||
        (GetAsyncKeyState('Q') & 0x8000) != 0 ||
        (GetAsyncKeyState('E') & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_UP) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;

    const int movingAlpha = (255 * (100 - g_mapTransparency) + 50) / 100;
    const bool graceFinished = g_mapOpenedAt != 0 &&
        (DWORD)(now - g_mapOpenedAt) >= MAP_OPEN_GRACE_MS;
    const int target = (g_pluginEnabled && g_mapAlphaEnabled &&
        moving && graceFinished) ? movingAlpha : 255;

    DWORD elapsed = now - lastTick;
    lastTick = now;
    if (elapsed > 50)
        elapsed = 50;

    if (alpha != target && elapsed != 0) {
        int range = 255 - movingAlpha;
        if (range < 1)
            range = 255;
        int step = (range * (int)elapsed + MAP_FADE_DURATION_MS - 1) /
            MAP_FADE_DURATION_MS;
        if (step < 1)
            step = 1;

        if (alpha < target) {
            alpha += step;
            if (alpha > target)
                alpha = target;
        } else {
            alpha -= step;
            if (alpha < target)
                alpha = target;
        }
    }

    g_mapCurrentAlpha = alpha;

    const zTRnd_AlphaBlendFunc mapBlend = (alpha < 255)
        ? zRND_ALPHA_FUNC_BLEND : originalMapBlend;
    g_mapView->SetTextureAlphaBlendFunc(mapBlend);
    g_mapView->SetTextureAlpha(alpha);

    if (page) {
        const zTRnd_AlphaBlendFunc pageBlend = (alpha < 255)
            ? zRND_ALPHA_FUNC_BLEND : originalPageBlend;
        page->SetTextureAlphaBlendFunc(pageBlend);
        page->SetTextureAlpha(alpha);
    }
}

int Change_DynaMap(zCOptionEntry& entry) {
    g_pluginEnabled = entry.varValueTemp.ToInt32() != 0;
    if (g_mapView) {
        if (!g_pluginEnabled) {
            g_mapView->ViewArrow.SetTextureAlpha(255);
        } else {
            g_mapOpenedAt = GetTickCount();
            g_arrowPreparedMap = NULL;
        }
    }
    return 0;
}

int Change_DynaMapAlpha(zCOptionEntry& entry) {
    g_mapAlphaEnabled = entry.varValueTemp.ToInt32() != 0;
    return 0;
}

int Change_DynaMapTransparency(zCOptionEntry& entry) {
    // Gothic sliders store normalized real values from 0.0 to 1.0.
    // Thirteen positions map to 20,25,...,80 percent transparency.
    float sliderValue = (float)atof(entry.varValueTemp.ToChar());
    if (sliderValue < 0.0f) sliderValue = 0.0f;
    if (sliderValue > 1.0f) sliderValue = 1.0f;
    const int step = (int)(sliderValue * 12.0f + 0.5f);
    g_mapTransparency = 20 + step * 5;
    return 0;
}

void Game_Entry() {}

void Game_Init() {
    if (!zoptions)
        return;

    zoptions->InsertChangeHandler("zDynaMap", "DynaMap", Change_DynaMap);
    zoptions->InsertChangeHandler("zDynaMap", "DynaMapAlpha", Change_DynaMapAlpha);
    zoptions->InsertChangeHandler("zDynaMap", "DynaMapTransparency", Change_DynaMapTransparency);

    g_pluginEnabled = zoptions->ReadInt("zDynaMap", "DynaMap", 1) != 0;
    g_mapAlphaEnabled = zoptions->ReadInt("zDynaMap", "DynaMapAlpha", 1) != 0;

    float sliderValue = zoptions->ReadReal("zDynaMap", "DynaMapTransparency", 0.5f);
    // Migrate the previous 0-100 representation to Gothic's native 0.0-1.0 range.
    if (sliderValue > 1.0f)
        sliderValue /= 100.0f;
    if (sliderValue < 0.0f)
        sliderValue = 0.0f;
    if (sliderValue > 1.0f)
        sliderValue = 1.0f;

    zoptions->WriteReal("zDynaMap", "DynaMapTransparency", sliderValue, False);
    const int step = (int)(sliderValue * 12.0f + 0.5f);
    g_mapTransparency = 20 + step * 5;
}

void Game_Exit() {
    if (g_smoothArrowTexture) {
        g_smoothArrowTexture->Release();
        g_smoothArrowTexture = NULL;
    }
}

void Game_PreLoop() {
    RefreshMapState();
    UnlockMovement();
    ApplyMouseTurn();
    UpdateMapMarker();
    FadeMap();
}

void Game_Loop() {
    UnlockMovement();
}

void Game_PostLoop() {}
void Game_MenuLoop() {}

TSaveLoadGameInfo& SaveLoadGameInfo = UnionCore::SaveLoadGameInfo;

void Game_SaveBegin() {}
void Game_SaveEnd() {}

void LoadBegin() {}
void LoadEnd() {}

void Game_LoadBegin_NewGame() { LoadBegin(); g_docOpen = false; }
void Game_LoadEnd_NewGame() { LoadEnd(); }

void Game_LoadBegin_SaveGame() { LoadBegin(); g_docOpen = false; }
void Game_LoadEnd_SaveGame() { LoadEnd(); }

void Game_LoadBegin_ChangeLevel() { LoadBegin(); g_docOpen = false; }
void Game_LoadEnd_ChangeLevel() { LoadEnd(); }

void Game_LoadBegin_Trigger() {}
void Game_LoadEnd_Trigger() {}

void Game_Pause() {}
void Game_Unpause() {}
void Game_DefineExternals() {}
void Game_ApplyOptions() {}

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
