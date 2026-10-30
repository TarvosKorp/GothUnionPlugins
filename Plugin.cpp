// This file added in headers queue
// File: "Sources.h"
#include "resource.h"

namespace GOTHIC_ENGINE {

// Ограничитель числа между мин. и макс. значениями
template <class T> inline void zClamp (T& x, const T min, const T max) 
{
	if		(x<min)	x=min;
	else if (x>max)	x=max;
}

// Функция применения настройки инверсии к оси Х
void Apply_FlipMouseX_ByValue(int value)
{
	// защита указателя
	if (!zinput) return;

	// ограничиваем входное значение между нулём и единицей
	zClamp(value, 0, 1);

	int mouseFlip[2];
	// получаем состояние инверсии обоих осей
	zinput->GetMouseFlipXY(mouseFlip[VX], mouseFlip[VY]);

	// если значение по оси х изменилось
	if (value != mouseFlip[VX])
		// применяем новые настройки инверсии по этой оси
		zinput->SetMouseFlipXY(value, mouseFlip[VY]);
}

// Функция-перехватчик на изменение нашего параметра "FlipMouseX" в Gothic.ini
int Change_FlipMouseX(zCOptionEntry& entry)
{
	// получаем актуальное, значение данной настройки (оно временное, внутриигровое)
	int value = entry.varValueTemp.ToInt32();

	// и применяем это значение к оси х
	Apply_FlipMouseX_ByValue(value);

	// выходим
	return 0;
}


// Событие инициализации главного меню
void Game_Init() 
{
	// Защита указателя
	if (!zoptions) return;

	// Если все ок, то устанавливаем функцию-перехватчик на изменение параметра.
	// Как это работает:
	// 1. Параметр меняется через внутриигровое меню настроек плагина.
	// 2. Затем, после изменения, вызывается функция "Change_FlipMouseX()",
	// в которой мы считываем текущее значение и применяем настройки к игре.
	zoptions->InsertChangeHandler(SECTION, ENTRY_FlipMouseX, Change_FlipMouseX);

	// считываем параметр из Gothic.ini, если записи нет, то создаём новую,
	// со значением по умолчанию = 0
	int value = zoptions->ReadInt(SECTION, ENTRY_FlipMouseX.ToChar(), 0);

	// применяем полученное значение к игре
	Apply_FlipMouseX_ByValue(value);
	
}


  void Game_Entry() {
  }

  void Game_Exit() {
  }

  void Game_PreLoop() {
  }

  void Game_Loop() {
  }

  void Game_PostLoop() {
  }

  void Game_MenuLoop() {
  }

  // Information about current saving or loading world
  TSaveLoadGameInfo& SaveLoadGameInfo = UnionCore::SaveLoadGameInfo;

  void Game_SaveBegin() {
  }

  void Game_SaveEnd() {
  }

  void LoadBegin() {
  }

  void LoadEnd() {
  }

  void Game_LoadBegin_NewGame() {
    LoadBegin();
  }

  void Game_LoadEnd_NewGame() {
    LoadEnd();
  }

  void Game_LoadBegin_SaveGame() {
    LoadBegin();
  }

  void Game_LoadEnd_SaveGame() {
    LoadEnd();
  }

  void Game_LoadBegin_ChangeLevel() {
    LoadBegin();
  }

  void Game_LoadEnd_ChangeLevel() {
    LoadEnd();
  }

  void Game_LoadBegin_Trigger() {
  }
  
  void Game_LoadEnd_Trigger() {
  }
  
  void Game_Pause() {
  }
  
  void Game_Unpause() {
  }
  
  void Game_DefineExternals() {
  }

  void Game_ApplyOptions()
  {
  }

  /*
  Functions call order on Game initialization:
    - Game_Entry           * Gothic entry point
    - Game_DefineExternals * Define external script functions
    - Game_Init            * After DAT files init
  
  Functions call order on Change level:
    - Game_LoadBegin_Trigger     * Entry in trigger
    - Game_LoadEnd_Trigger       *
    - Game_Loop                  * Frame call window
    - Game_LoadBegin_ChangeLevel * Load begin
    - Game_SaveBegin             * Save previous level information
    - Game_SaveEnd               *
    - Game_LoadEnd_ChangeLevel   *
  
  Functions call order on Save game:
    - Game_Pause     * Open menu
    - Game_Unpause   * Click on save
    - Game_Loop      * Frame call window
    - Game_SaveBegin * Save begin
    - Game_SaveEnd   *
  
  Functions call order on Load game:
    - Game_Pause              * Open menu
    - Game_Unpause            * Click on load
    - Game_LoadBegin_SaveGame * Load begin
    - Game_LoadEnd_SaveGame   *
  */

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