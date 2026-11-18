#include <stdio.h>
#include <stdarg.h>

// zMapUnlocker — экспериментальное снятие блокировки управления при открытой карте Gothic II Addon.
// Первый этап: пропускаем игровые события через обработчик DocumentManager,
// сохраняя штатную обработку Escape для закрытия карты.
// UpdatePosition намеренно НЕ вызывается повторно: на этом этапе проверяем,
// обновляет ли игра GPS-маячок самостоятельно после разблокировки управления.

namespace GOTHIC_ENGINE {

static zCView* g_mapView = NULL;
static bool g_mapDocIsMap = false;
static bool g_mapUpdInShow = false;
static int g_docShowDepth = 0;
static bool g_diagMapWasOpen = false;
static unsigned long g_diagLastTime = 0;
static float g_diagMouseDxSum = 0.0f;
static float g_diagMouseDxPeak = 0.0f;
static unsigned long g_diagMouseSamples = 0;
static unsigned long g_diagKeyW = 0, g_diagKeyA = 0, g_diagKeyS = 0, g_diagKeyD = 0;
static unsigned long g_diagKeyUp = 0, g_diagKeyLeft = 0, g_diagKeyDown = 0, g_diagKeyRight = 0;

void MapDiagLog(const char* format, ...) {
    FILE* file = fopen("zMapUnlocker.log", "a");
    if (!file) return;

    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fprintf(file, "\n");
    fclose(file);
}

bool IsMapInterfaceOpen() {
    if (!g_mapView) return false;

    oCDocumentManager& dm = oCDocumentManager::GetDocumentManager();
    zCViewDialog* dlg = dm.DlgDocumentMain;
    if (!dlg) return false;

    return DocDlgOnScreen() || dlg->IsActive();
}

void ResetMapDiagCounters() {
    g_diagMouseDxSum = 0.0f;
    g_diagMouseDxPeak = 0.0f;
    g_diagMouseSamples = 0;
    g_diagKeyW = g_diagKeyA = g_diagKeyS = g_diagKeyD = 0;
    g_diagKeyUp = g_diagKeyLeft = g_diagKeyDown = g_diagKeyRight = 0;
}

void MapDiagLoop() {
    const bool mapOpen = IsMapInterfaceOpen();

    if (mapOpen && !g_diagMapWasOpen) {
        g_diagLastTime = ztimer ? ztimer->totalTime : 0;
        ResetMapDiagCounters();
        MapDiagLog("[map] OPEN view=%p mapDetected=%d dialogOnScreen=%d",
            g_mapView, g_mapDocIsMap ? 1 : 0, DocDlgOnScreen() ? 1 : 0);
    }
    else if (!mapOpen && g_diagMapWasOpen) {
        MapDiagLog("[map] CLOSE");
    }
    g_diagMapWasOpen = mapOpen;

    if (!mapOpen || !zinput || !ztimer) return;

    const float dx = zinput->GetState(MOUSE_DX);
    g_diagMouseDxSum += dx;
    const float absDx = dx < 0.0f ? -dx : dx;
    if (absDx > g_diagMouseDxPeak) g_diagMouseDxPeak = absDx;
    ++g_diagMouseSamples;

    if (zinput->KeyPressed(KEY_W)) ++g_diagKeyW;
    if (zinput->KeyPressed(KEY_A)) ++g_diagKeyA;
    if (zinput->KeyPressed(KEY_S)) ++g_diagKeyS;
    if (zinput->KeyPressed(KEY_D)) ++g_diagKeyD;
    if (zinput->KeyPressed(KEY_UP)) ++g_diagKeyUp;
    if (zinput->KeyPressed(KEY_LEFT)) ++g_diagKeyLeft;
    if (zinput->KeyPressed(KEY_DOWN)) ++g_diagKeyDown;
    if (zinput->KeyPressed(KEY_RIGHT)) ++g_diagKeyRight;

    const unsigned long now = ztimer->totalTime;
    if ((unsigned long)(now - g_diagLastTime) < 5000) return;

    oCDocumentManager& dm = oCDocumentManager::GetDocumentManager();
    zCViewDialog* dlg = dm.DlgDocumentMain;

    MapDiagLog(
        "[sample] t=%lu ms devices(kbd=%d mouse=%d) callbacks(doc=%d dlg=%d map=%d) "
        "mouse_dx(sum=%.2f peak=%.2f samples=%lu) "
        "keys(W=%lu A=%lu S=%lu D=%lu Up=%lu Left=%lu Down=%lu Right=%lu)",
        now,
        zinput->GetDeviceEnabled(zINPUT_KEYBOARD),
        zinput->GetDeviceEnabled(zINPUT_MOUSE),
        dm.GetEnableHandleEvent(),
        dlg ? dlg->GetEnableHandleEvent() : -1,
        g_mapView ? g_mapView->GetEnableHandleEvent() : -1,
        g_diagMouseDxSum, g_diagMouseDxPeak, g_diagMouseSamples,
        g_diagKeyW, g_diagKeyA, g_diagKeyS, g_diagKeyD,
        g_diagKeyUp, g_diagKeyLeft, g_diagKeyDown, g_diagKeyRight
    );

    g_diagLastTime = now;
    ResetMapDiagCounters();
}

bool DocDlgOnScreen() {
    if (!screen) return false;

    oCDocumentManager& dm = oCDocumentManager::GetDocumentManager();
    zCViewDialog* dlg = dm.DlgDocumentMain;
    if (!dlg) return false;

    int guard = 0;
    for (zCView* v = screen->childs.root; v && guard < 512; v = v->next, guard++) {
        if ((void*)v == (void*)dlg) return true;
    }
    return false;
}

// Запоминаем объект карты и не вмешиваемся в расчёт маркера.
void __fastcall Hook_MapView_UpdatePosition(zCView* self, void* edx);
CInvoke<void(__fastcall*)(zCView*, void*)> Ivk_MapView_UpdatePosition(
    0x0068D7B0, &Hook_MapView_UpdatePosition, IvkEnabled(Engine_G2A));

void __fastcall Hook_MapView_UpdatePosition(zCView* self, void* edx) {
    Ivk_MapView_UpdatePosition(self, edx);
    if (g_mapView != self) {
        g_mapView = self;
        MapDiagLog("[map] UpdatePosition first-seen view=%p showDepth=%d", self, g_docShowDepth);
    }
    if (g_docShowDepth > 0) g_mapUpdInShow = true;
}

void __fastcall Hook_MapView_Dtor(zCView* self, void* edx);
CInvoke<void(__fastcall*)(zCView*, void*)> Ivk_MapView_Dtor(
    0x0068D470, &Hook_MapView_Dtor, IvkEnabled(Engine_G2A));

void __fastcall Hook_MapView_Dtor(zCView* self, void* edx) {
    if (g_mapView == self) {
        MapDiagLog("[map] map-view destructor view=%p", self);
        g_mapView = NULL;
        g_mapDocIsMap = false;
    }
    Ivk_MapView_Dtor(self, edx);
}

// Show() позволяет отличить карту от обычного текстового документа.
void __fastcall Hook_DocMan_Show(void* self, void* edx, int documentId);
CInvoke<void(__fastcall*)(void*, void*, int)> Ivk_DocMan_Show(
    0x0065F300, &Hook_DocMan_Show, IvkEnabled(Engine_G2A));

void __fastcall Hook_DocMan_Show(void* self, void* edx, int documentId) {
    const bool outer = (g_docShowDepth == 0);
    if (outer) g_mapUpdInShow = false;

    ++g_docShowDepth;
    Ivk_DocMan_Show(self, edx, documentId);
    --g_docShowDepth;

    if (!outer) return;

    oCDocumentManager* dm = (oCDocumentManager*)self;
    oCViewDocument* document = dm->GetDocumentView(documentId);

    g_mapDocIsMap = g_mapUpdInShow ||
        (document && g_mapView && (void*)document == (void*)g_mapView);
    MapDiagLog("[doc] Show id=%d doc=%p mapView=%p updateInsideShow=%d mapDetected=%d",
        documentId, document, g_mapView, g_mapUpdInShow ? 1 : 0, g_mapDocIsMap ? 1 : 0);
}

// Пока карта открыта, не даём её обработчику поглощать игровые клавиши.
// Escape оставляем штатному обработчику, чтобы карта закрывалась обычным способом.
int __fastcall Hook_DocMan_HandleEvent(oCDocumentManager* self, void* edx, int key);
CInvoke<int(__fastcall*)(oCDocumentManager*, void*, int)> Ivk_DocMan_HandleEvent(
    0x0065F450, &Hook_DocMan_HandleEvent, IvkEnabled(Engine_G2A));

int __fastcall Hook_DocMan_HandleEvent(oCDocumentManager* self, void* edx, int key) {
    if (g_mapDocIsMap && DocDlgOnScreen() && key != KEY_ESCAPE) {
        return 0;
    }
    return Ivk_DocMan_HandleEvent(self, edx, key);
}

TSaveLoadGameInfo& SaveLoadGameInfo = UnionCore::SaveLoadGameInfo;

void Game_Entry() {}
void Game_Init() {}
void Game_Exit() {}

void Game_PreLoop() {}
void Game_Loop() { MapDiagLoop(); }
void Game_PostLoop() {}
void Game_MenuLoop() {}

void Game_SaveBegin() {}
void Game_SaveEnd() {}

void LoadBegin() {}
void LoadEnd() {}

void Game_LoadBegin_NewGame() { LoadBegin(); }
void Game_LoadEnd_NewGame() { LoadEnd(); }

void Game_LoadBegin_SaveGame() { LoadBegin(); }
void Game_LoadEnd_SaveGame() { LoadEnd(); }

void Game_LoadBegin_ChangeLevel() { LoadBegin(); }
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
