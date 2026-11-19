META
{
  Parser = Menu;
  After = zUnionMenu.d;
  Namespace = zDanglingWeapons;
};

// Namespace = zDanglingWeapons
// Important !!!
// Supporting Unicode symbols.
// File encoding: UTF-8 (without BOM).

const int MENU_TEXT_X       = 450;
const int MENU_TEXT_DX      = 8000;
const int MENU_TEXT_DY      = 750;
const int MENU_SLIDER_X     = 5500;
const int MENU_SLIDER_DX    = 2000;
const int MENU_SLIDER_DY    = 600;
const int MENU_SLIDER_YPLUS = 0;
const int MENU_CHOICE_DX    = 2000;
const int MENU_CHOICE_DY    = 350;
const int MENU_CHOICE_YPLUS = 120;
const int MENU_TITLE_Y      = 450;
const int MENU_START_Y      = 1400;
const int MENU_BACK_Y       = 6500;
const int MENU_DY           = 550;
const int MENU_INFO_X       = 300;
const int MENU_INFO_Y       = 7780;

const string MENU_ITEM_BACK_PIC   = "";
const string MENU_BACK_PIC        = "MENU_INGAME.TGA";
const string MENU_CHOICE_BACK_PIC = "MENU_CHOICE_BACK.TGA";
const string MENU_SLIDER_POS_PIC  = "MENU_SLIDER_POS.TGA";
const string MENU_SLIDER_BACK_PIC = "MENU_SLIDER_BACK.TGA";


prototype C_EMPTY_MENU_DEF(C_MENU)
{
  C_MENU_DEF();
  backpic    = MENU_BACK_PIC;
  items[0]   = "";
  items[100] = "Union_menuitem_back";
  flags      = flags | MENU_SHOW_INFO;
};


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


instance C_MENUITEM_CHOICE_BASE(C_MENU_ITEM_DEF)
{
  backpic  = MENU_CHOICE_BACK_PIC;
  type     = MENU_ITEM_CHOICEBOX;
  fontname = MENU_FONT_SMALL;
  posx     = MENU_SLIDER_X;
  posy     = MENU_START_Y + MENU_CHOICE_YPLUS;
  dimx     = MENU_SLIDER_DX;
  dimy     = MENU_CHOICE_DY;
  flags    = flags & ~IT_SELECTABLE;
  flags    = flags | IT_TXT_CENTER;
};


instance C_MENUITEM_SLIDER_BASE(C_MENU_ITEM_DEF)
{
  backpic       = MENU_SLIDER_BACK_PIC;
  type          = MENU_ITEM_SLIDER;
  fontname      = MENU_FONT_SMALL;
  posx          = MENU_SLIDER_X;
  posy          = MENU_START_Y + MENU_CHOICE_YPLUS;
  dimx          = MENU_SLIDER_DX;
  dimy          = MENU_SLIDER_DY;
  flags         = flags & ~IT_SELECTABLE;
  flags         = flags | IT_TXT_CENTER;
  userstring[0] = MENU_SLIDER_POS_PIC;
};


instance MenuItem_Union_Auto_zDanglingWeapons(C_MENU_ITEM_UNION_DEF)
{
  text[0]          = "zDanglingWeapons";
  text[1]          = "Configure zDanglingWeapons settings";
  onselaction[0]   = SEL_ACTION_STARTMENU;
  onselaction_s[0] = "zDanglingWeapons:Menu_Opt";
};


instance Menu_Opt(C_EMPTY_MENU_DEF)
{
  Menu_SearchItems("ZDANGLINGWEAPONS:MENUITEM_OPT_*");
};


instance MenuItem_Opt_Headline(C_MENU_ITEM_DEF)
{
  type    = MENU_ITEM_TEXT;
  posx    = 0;
  posy    = MENU_TITLE_Y;
  dimx    = 8100;
  flags   = flags & ~IT_SELECTABLE;
  flags   = flags | IT_TXT_CENTER;
  text[0] = Str_GetLocalizedString(
    "ZDANGLINGWEAPONS НАСТРОЙКИ",
    "ZDANGLINGWEAPONS SETTINGS",
    "ZDANGLINGWEAPONS EINSTELLUNGEN",
    "USTAWIENIA ZDANGLINGWEAPONS"
  );
};


instance MenuItem_Opt_Speed(C_MENU_ITEM)
{
  C_MENU_ITEM_TEXT_BASE();
  posy += MENU_DY * 2;
  
  text[0] = Str_GetLocalizedString(
    "Скорость",
    "Stones",
    "Steine",
    "Kamieńy"
  );
};


instance MenuItem_Opt_Speed_Slider(C_MENU_ITEM_DEF)
{
  C_MENUITEM_SLIDER_BASE();
  posy += MENU_DY * 2;

  onchgsetoption        = "Speed";
  onchgsetoptionsection = "zDanglingWeapons";
  userfloat[0]          = 14;
};


instance MenuItem_Opt_Amplitude(C_MENU_ITEM)
{
  C_MENU_ITEM_TEXT_BASE();
  posy += MENU_DY * 3;
  
  text[0] = Str_GetLocalizedString(
    "Амплитуда",
    "Amplitude",
    "Amplitude",
    "Amplituda"
  );
};


instance MenuItem_Opt_Amplitude_Slider(C_MENU_ITEM_DEF)
{
  C_MENUITEM_SLIDER_BASE();
  posy += MENU_DY * 3;

  onchgsetoption        = "Amplitude";
  onchgsetoptionsection = "zDanglingWeapons";
  userfloat[0]          = 14;
};


instance MenuItem_Opt_LongSwordMode(C_MENU_ITEM)
{
  C_MENU_ITEM_TEXT_BASE();
  posy += MENU_DY * 4;
  
  text[0] = Str_GetLocalizedString(
    "Амп. двуруч. меча",
    "Amp. Two-Handed",
    "Amp. Zweihänder",
    "Amp. Br. dwuręczna"
  );
};


instance MenuItem_Opt_LongSwordMode_Choice(C_MENU_ITEM_DEF)
{
  C_MENUITEM_CHOICE_BASE();
  posy += MENU_DY * 4;
  
  onchgsetoption        = "LongSwordMode";
  onchgsetoptionsection = "zDanglingWeapons";
  text[0]               = Str_GetLocalizedString(
    "Очень слабо|Слабо|Нормально",
    "Very weak|Weak|Normally",
    "Sehr schwach|Schwach|Normal",
    "Bardzo słabo|słabo|normalnie"
  );
};


instance MenuItem_Opt_SoundsVolume(C_MENU_ITEM)
{
  C_MENU_ITEM_TEXT_BASE();
  posy += MENU_DY * 5;
  
  text[0] = Str_GetLocalizedString(
    "Громкость звуков",
    "Sound volume",
    "Lautstärke",
    "Głośność dźwięków"
  );
};


instance MenuItem_Opt_SoundsVolume_Slider(C_MENU_ITEM_DEF)
{
  C_MENUITEM_SLIDER_BASE();
  posy += MENU_DY * 5;

  onchgsetoption        = "SoundsVolume";
  onchgsetoptionsection = "zDanglingWeapons";
  userfloat[0]          = 14;
};


func int Act_OpenWebLink()
{
  Open_Link("https://worldofplayers.ru/threads/42219/");
  return 0;
};


instance MenuItem_Opt_Open_Link(C_MENU_ITEM_DEF)
{
  C_MENU_ITEM_TEXT_BASE();
  posy += MENU_DY * 8;

  posx             = 64;
  onselaction[0]   = SEL_ACTION_UNDEF;
  oneventaction[1] = Act_OpenWebLink;
  flags            = flags | IT_TXT_CENTER;
  text[0]          = Str_GetLocalizedString(
    "Открыть страницу проекта",
    "Open project page",
    "Projektseite öffnen",
    "Otwórz stronę projektu"
  );
};
