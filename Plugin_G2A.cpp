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
    g_mapView = self;
    if (g_docShowDepth > 0) g_mapUpdInShow = true;
}

void __fastcall Hook_MapView_Dtor(zCView* self, void* edx);
CInvoke<void(__fastcall*)(zCView*, void*)> Ivk_MapView_Dtor(
    0x0068D470, &Hook_MapView_Dtor, IvkEnabled(Engine_G2A));

void __fastcall Hook_MapView_Dtor(zCView* self, void* edx) {
    if (g_mapView == self) {
        g_mapView = NULL;
        g_mapDocIsMap = false;
    }
    Ivk_MapView_Dtor(self, edx);
}

// Show() позволяет отличить карту от обычного текстового документа.
void __fastcall Hook_DocMan_Show(void* self, void* edx);
CInvoke<void(__fastcall*)(void*, void*)> Ivk_DocMan_Show(
    0x0065F300, &Hook_DocMan_Show, IvkEnabled(Engine_G2A));

void __fastcall Hook_DocMan_Show(void* self, void* edx) {
    const bool outer = (g_docShowDepth == 0);
    if (outer) g_mapUpdInShow = false;

    ++g_docShowDepth;
    Ivk_DocMan_Show(self, edx);
    --g_docShowDepth;

    if (!outer) return;

    const int documentId = (int)(INT_PTR)edx;
    oCDocumentManager* dm = (oCDocumentManager*)self;
    oCViewDocument* document = dm->GetDocumentView(documentId);

    g_mapDocIsMap = g_mapUpdInShow ||
        (document && g_mapView && (void*)document == (void*)g_mapView);
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
void Game_Loop() {}
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
