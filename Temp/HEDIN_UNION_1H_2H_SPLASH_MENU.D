META
{
  Parser = Menu;
  After = zUnionMenu_HEDIN.d;
};

// Important !!!
// Supporting Unicode symbols.
// File encoding: UTF-8 (without BOM).

const int HEDIN_SPLASH_PLUGIN_MENU_TEXT_X       = 1500;
const int HEDIN_SPLASH_PLUGIN_MENU_TEXT_DX      = 8000;
const int HEDIN_SPLASH_PLUGIN_MENU_TEXT_DY      = 750;
const int HEDIN_SPLASH_PLUGIN_MENU_SLIDER_X     = 5500;
const int HEDIN_SPLASH_PLUGIN_MENU_SLIDER_DX    = 1400;
const int HEDIN_SPLASH_PLUGIN_MENU_SLIDER_DY    = 500;
const int HEDIN_SPLASH_PLUGIN_MENU_SLIDER_YPLUS = 0;
const int HEDIN_SPLASH_PLUGIN_MENU_CHOICE_DX    = 1000;
const int HEDIN_SPLASH_PLUGIN_MENU_CHOICE_DY    = 350;
const int HEDIN_SPLASH_PLUGIN_MENU_CHOICE_YPLUS = -50;
const int HEDIN_SPLASH_PLUGIN_MENU_TITLE_Y      = 900;
const int HEDIN_SPLASH_PLUGIN_MENU_START_Y      = 1400;
const int HEDIN_SPLASH_PLUGIN_MENU_BACK_Y       = 6500;
const int HEDIN_SPLASH_PLUGIN_MENU_DY           = 550;
const int HEDIN_SPLASH_PLUGIN_MENU_INFO_X       = 300;
const int HEDIN_SPLASH_PLUGIN_MENU_INFO_Y       = 7780;

const string HEDIN_SPLASH_PLUGIN_MENU_ITEM_BACK_PIC   = "";
const string HEDIN_SPLASH_PLUGIN_MENU_BACK_PIC        = "MENU_INGAME.TGA";
const string HEDIN_SPLASH_PLUGIN_MENU_CHOICE_BACK_PIC = "MENU_CHOICE_BACK.TGA";
const string HEDIN_SPLASH_PLUGIN_MENU_SLIDER_POS_PIC  = "MENU_SLIDER_POS.TGA";
const string HEDIN_SPLASH_PLUGIN_MENU_SLIDER_BACK_PIC = "MENU_SLIDER_BACK.TGA";


instance MENUITEM_UNION_HEDIN_AUTO_UNION_SPLASH_PLUGIN(C_MENU_ITEM_UNION_DEF_HEDIN)
{
  text[0]          = "New Splash 1H/2H Plugin";
  text[1]          = "Configure New Splash 1H/2H Plugin";
  onselaction[0]   = SEL_ACTION_STARTMENU;
  onselaction_s[0] = "HEDIN_SPLASH_PLUGIN_MENU_OPT";
};


instance HEDIN_SPLASH_PLUGIN_MENU_OPT(C_MENU_DEF)
{
  backpic   = HEDIN_SPLASH_PLUGIN_MENU_BACK_PIC;
  items[0]  = "HEDIN_SPLASH_PLUGIN_MENUITEM_HEADLINE";
  items[1]  = "HEDIN_SPLASH_PLUGIN_MENUITEM_1";
  items[2]  = "HEDIN_SPLASH_PLUGIN_MENUITEM_1_CHOICE";
  items[3]  = "HEDIN_SPLASH_PLUGIN_MENUITEM_2";
  items[4]  = "HEDIN_SPLASH_PLUGIN_MENUITEM_2_CHOICE";
  items[5]  = "HEDIN_SPLASH_PLUGIN_MENUITEM_3";
  items[6]  = "HEDIN_SPLASH_PLUGIN_MENUITEM_3_CHOICE";
  items[7]  = "HEDIN_SPLASH_PLUGIN_MENUITEM_4";
  items[8]  = "HEDIN_SPLASH_PLUGIN_MENUITEM_4_CHOICE";
  items[20] = "UNION_MENUITEM_BACK_HEDIN";
  flags     = flags | MENU_SHOW_INFO;
};


instance C_HEDIN_SPLASH_PLUGIN_MENU_ITEM_TEXT_BASE(C_MENU_ITEM_DEF)
{
  backpic        = HEDIN_SPLASH_PLUGIN_MENU_ITEM_BACK_PIC;
  posx           = HEDIN_SPLASH_PLUGIN_MENU_TEXT_X;
  posy           = HEDIN_SPLASH_PLUGIN_MENU_START_Y;
  dimx           = HEDIN_SPLASH_PLUGIN_MENU_TEXT_DX;
  dimy           = HEDIN_SPLASH_PLUGIN_MENU_TEXT_DY;
  flags          = flags | IT_EFFECTS_NEXT;
  fontname 	 	 = MENU_FONT_SMALL;
  onselaction[0] = SEL_ACTION_UNDEF;
};


instance C_HEDIN_SPLASH_PLUGIN_MENUITEM_CHOICE_BASE(C_MENU_ITEM_DEF)
{
  backpic  = HEDIN_SPLASH_PLUGIN_MENU_CHOICE_BACK_PIC;
  type     = MENU_ITEM_CHOICEBOX;
  fontname = MENU_FONT_SMALL;
  posx     = HEDIN_SPLASH_PLUGIN_MENU_SLIDER_X;
  posy     = HEDIN_SPLASH_PLUGIN_MENU_START_Y + HEDIN_SPLASH_PLUGIN_MENU_CHOICE_YPLUS;
  dimx     = HEDIN_SPLASH_PLUGIN_MENU_SLIDER_DX;
  dimy     = HEDIN_SPLASH_PLUGIN_MENU_CHOICE_DY;
  flags    = flags & ~IT_SELECTABLE;
  flags    = flags | IT_TXT_CENTER;
};


instance C_HEDIN_SPLASH_PLUGIN_MENUITEM_SLIDER_BASE(C_MENU_ITEM_DEF)
{
  backpic       = HEDIN_SPLASH_PLUGIN_MENU_SLIDER_BACK_PIC;
  type          = MENU_ITEM_SLIDER;
  fontname      = MENU_FONT_SMALL;
  posx          = HEDIN_SPLASH_PLUGIN_MENU_SLIDER_X;
  posy          = HEDIN_SPLASH_PLUGIN_MENU_START_Y + HEDIN_SPLASH_PLUGIN_MENU_CHOICE_YPLUS;
  dimx          = HEDIN_SPLASH_PLUGIN_MENU_SLIDER_DX;
  dimy          = HEDIN_SPLASH_PLUGIN_MENU_SLIDER_DY;
  flags         = flags & ~IT_SELECTABLE;
  flags         = flags | IT_TXT_CENTER;
  userstring[0] = MENU_SLIDER_POS_PIC;
};


instance HEDIN_SPLASH_PLUGIN_MENUITEM_HEADLINE(C_MENU_ITEM_DEF)
{
  type    = MENU_ITEM_TEXT;
  posx    = 0;
  posy    = HEDIN_SPLASH_PLUGIN_MENU_TITLE_Y;
  dimx    = 8100;
  flags   = flags & ~IT_SELECTABLE;
  flags   = flags | IT_TXT_CENTER;
  text[0] = Str_GetLocalizedString(
    "Splash 1H/2H Plugin НАСТРОЙКИ",
    "Splash 1H/2H Plugin SETTINGS",
    "Splash 1H/2H Plugin EINSTELLUNGEN",
    "Splash 1H/2H Plugin SETTINGS"
  );
};


instance HEDIN_SPLASH_PLUGIN_MENUITEM_1(C_MENU_ITEM_DEF)
{
  C_HEDIN_SPLASH_PLUGIN_MENU_ITEM_TEXT_BASE();
  posy += HEDIN_SPLASH_PLUGIN_MENU_DY * 1;
  text[0] = Str_GetLocalizedString("Хардкорный режим","Hardcore mode","Hardcore mode", "Hardcore mode" );
};
instance HEDIN_SPLASH_PLUGIN_MENUITEM_1_CHOICE(C_MENU_ITEM_DEF)
{
  C_HEDIN_SPLASH_PLUGIN_MENUITEM_CHOICE_BASE();
  posy += HEDIN_SPLASH_PLUGIN_MENU_DY * 1;
  onchgsetoption        = "needHardFightMode";
  onchgsetoptionsection = "HEDIN_UNION_SPLASH_NEW";
  text[0]               = Str_GetLocalizedString("выкл|вкл","off|on","Aus|Ein", "wyłączone|włączone");
};


instance HEDIN_SPLASH_PLUGIN_MENUITEM_2(C_MENU_ITEM_DEF)
{
  C_HEDIN_SPLASH_PLUGIN_MENU_ITEM_TEXT_BASE();
  posy += HEDIN_SPLASH_PLUGIN_MENU_DY * 2;
  text[0] = Str_GetLocalizedString("Кол-во врагов","Number of enemies","Number of enemies", "Number of enemies" );
};
instance HEDIN_SPLASH_PLUGIN_MENUITEM_2_CHOICE(C_MENU_ITEM_DEF)
{
  C_HEDIN_SPLASH_PLUGIN_MENUITEM_CHOICE_BASE();
  posy += HEDIN_SPLASH_PLUGIN_MENU_DY * 2;
  onchgsetoption        = "numberSplashEnemiesIdx";
  onchgsetoptionsection = "HEDIN_UNION_SPLASH_NEW";
  text[0]               = "1|2|3|4|5|6|7";
};


instance  HEDIN_SPLASH_PLUGIN_MENUITEM_3(C_MENU_ITEM_DEF)
{
  C_HEDIN_SPLASH_PLUGIN_MENU_ITEM_TEXT_BASE();
  posy += HEDIN_SPLASH_PLUGIN_MENU_DY * 3;
  text[0] = Str_GetLocalizedString("Дистанция","Distance","Distance", "Distance" );
};
instance  HEDIN_SPLASH_PLUGIN_MENUITEM_3_CHOICE(C_MENU_ITEM_DEF)
{
  C_HEDIN_SPLASH_PLUGIN_MENUITEM_CHOICE_BASE();
  posy += HEDIN_SPLASH_PLUGIN_MENU_DY * 3;
  onchgsetoption        = "numberSplashDistanceIdx";
  onchgsetoptionsection = "HEDIN_UNION_SPLASH_NEW";
  text[0]               = "200|300|400|500|600|700";
};

instance  HEDIN_SPLASH_PLUGIN_MENUITEM_4(C_MENU_ITEM_DEF)
{
  C_HEDIN_SPLASH_PLUGIN_MENU_ITEM_TEXT_BASE();
  posy += HEDIN_SPLASH_PLUGIN_MENU_DY * 4;
  text[0] = Str_GetLocalizedString("Показывать сообщения","Show messages","Show messages", "Show messages" );
};
instance  HEDIN_SPLASH_PLUGIN_MENUITEM_4_CHOICE(C_MENU_ITEM_DEF)
{
  C_HEDIN_SPLASH_PLUGIN_MENUITEM_CHOICE_BASE();
  posy += HEDIN_SPLASH_PLUGIN_MENU_DY * 4;
  onchgsetoption        = "needShowTexts";
  onchgsetoptionsection = "HEDIN_UNION_SPLASH_NEW";
  text[0]               = Str_GetLocalizedString("выкл|вкл","off|on","Aus|Ein", "wyłączone|włączone");
};


const string HEDIN_SPLASH_PLUGIN_MENU_SHADOW = "SPELLBOOK_AMOUNT.TGA";

