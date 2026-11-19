META
{
  Parser = Menu;
  After = zUnionMenu.d;
  Namespace = zMirrorWorld
};

// File encoding: UTF-8 (without BOM).

const int MENU_TEXT_X       = 450;
const int MENU_TEXT_DX      = 8000;
const int MENU_TEXT_DY      = 750;
const int MENU_CHOICE_X     = 6200;
const int MENU_CHOICE_DX    = 1500;
const int MENU_CHOICE_DY    = 350;
const int MENU_CHOICE_YPLUS = 120;
const int MENU_TITLE_Y      = 450;
const int MENU_START_Y      = 1400;
const int MENU_DY           = 550;

// Секция конфига в INI (совпадает с C++: zoptions->ReadInt("zMirrorWorld", "Mirror", 0))
const string SECTION = "zMirrorWorld";

// Отображение плагина в общем списке плагинов
instance MENUITEM_UNION_AUTO_ALTERNATIVE_MENU(C_MENU_ITEM_UNION_DEF)
{
  text[0]          = SECTION;
  text[1]          = "Configure zMirrorWorld settings";
  onselaction[0]   = SEL_ACTION_STARTMENU;
  onselaction_s[0] = "zMirrorWorld:MENU_MIRROR_SETTINGS";
};

// Меню настроек плагина
instance MENU_MIRROR_SETTINGS(C_MENU_DEF)
{
  backpic   = MENU_BACK_PIC;
  items[0]  = "zMirrorWorld:MENUITEM_HEADLINE";
  items[1]  = "zMirrorWorld:MENUITEM_MIRROR";
  items[2]  = "zMirrorWorld:MENUITEM_MIRROR_CHOICE";
  items[3]  = "UNION_MENUITEM_BACK";
  flags     = flags | MENU_SHOW_INFO;
};

// Основной заголовок в окне настроек
instance MENUITEM_HEADLINE(C_MENU_ITEM_DEF)
{
  type    = MENU_ITEM_TEXT;
  posx    = 0;
  posy    = MENU_TITLE_Y;
  dimx    = 8100;
  flags   = flags & ~IT_SELECTABLE;
  flags   = flags | IT_TXT_CENTER;
  text[0] = Str_GetLocalizedString(
    "zMirrorWorld - Настройки",
    "zMirrorWorld - Settings",
    "zMirrorWorld - Einstellungen",
    "zMirrorWorld - Ustawienia"
  );
};

// Базовые параметры текстового элемента настроек
instance C_MENU_ITEM_TEXT_BASE(C_MENU_ITEM_DEF)
{
  backpic        = MENU_ITEM_BACK_PIC;
  posx           = MENU_TEXT_X;
  posy           = MENU_START_Y;
  dimx           = MENU_TEXT_DX;
  dimy           = MENU_TEXT_DY;
  flags          = flags | IT_EFFECTS_NEXT;
  onselaction[0] = SEL_ACTION_UNDEF;
};

// Базовые параметры элемента выбора настроек
instance C_MENUITEM_CHOICE_BASE(C_MENU_ITEM_DEF)
{
  backpic  = MENU_CHOICE_BACK_PIC;
  type     = MENU_ITEM_CHOICEBOX;
  fontname = MENU_FONT_SMALL;
  posx     = MENU_CHOICE_X;
  posy     = MENU_START_Y + MENU_CHOICE_YPLUS;
  dimx     = MENU_CHOICE_DX;
  dimy     = MENU_CHOICE_DY;
  flags    = flags & ~IT_SELECTABLE;
  flags    = flags | IT_TXT_CENTER;
};

// Текстовый элемент - "Отразить 3d"
instance MENUITEM_MIRROR(C_MENU_ITEM)
{
  C_MENU_ITEM_TEXT_BASE();
  posy += (MENU_DY * 1);

  text[0] = Str_GetLocalizedString(
    "Отразить 3d",
    "Mirror 3d",
    "Spiegel 3d",
    "Lustro 3d"
  );
};

// Переключатель зеркала (выкл/вкл)
instance MENUITEM_MIRROR_CHOICE(C_MENU_ITEM_DEF)
{
  C_MENUITEM_CHOICE_BASE();
  posy += (MENU_DY * 1);

  // INI -> [zMirrorWorld] -> Mirror
  onchgsetoption        = "Mirror";
  onchgsetoptionsection = SECTION;

  text[0] = Str_GetLocalizedString(
    "выкл|вкл",
    "off|on",
    "aus|an",
    "nie|tak"
  );
};
