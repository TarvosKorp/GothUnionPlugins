META
{
  Parser = Menu;
  After = zUnionMenu.d;
  Namespace = zDynaMap
};

// File encoding: UTF-8 (without BOM).
const string SECTION = "zDynaMap";

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

instance MENUITEM_UNION_AUTO_ALTERNATIVE_MENU(C_MENU_ITEM_UNION_DEF)
{
  text[0]          = "zDynaMap";
  text[1]          = "Configure zDynaMap settings";
  onselaction[0]   = SEL_ACTION_STARTMENU;
  onselaction_s[0] = "zDynaMap:MENU_DYNA_MAP_SETTINGS";
};

instance MENU_DYNA_MAP_SETTINGS(C_MENU_DEF)
{
  backpic   = MENU_BACK_PIC;
  items[0]  = "zDynaMap:MENUITEM_HEADLINE";
  items[1]  = "zDynaMap:MENUITEM_DYNAMAP";
  items[2]  = "zDynaMap:MENUITEM_DYNAMAP_CHOICE";
  items[3]  = "zDynaMap:MENUITEM_ALPHA";
  items[4]  = "zDynaMap:MENUITEM_ALPHA_CHOICE";
  items[5]  = "zDynaMap:MENUITEM_TRANSPARENCY";
  items[6]  = "zDynaMap:MENUITEM_TRANSPARENCY_SLIDER";
  items[7]  = "UNION_MENUITEM_BACK";
  flags     = flags | MENU_SHOW_INFO;
};

instance MENUITEM_HEADLINE(C_MENU_ITEM_DEF)
{
  type    = MENU_ITEM_TEXT;
  posx    = 0;
  posy    = MENU_TITLE_Y;
  dimx    = 8100;
  flags   = flags & ~IT_SELECTABLE;
  flags   = flags | IT_TXT_CENTER;
  text[0] = Str_GetLocalizedString(
    "zDynaMap - Настройки",
    "zDynaMap - Settings",
    "zDynaMap - Einstellungen",
    "zDynaMap - Ustawienia"
  );
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
  posx     = MENU_CHOICE_X;
  posy     = MENU_START_Y + MENU_CHOICE_YPLUS;
  dimx     = MENU_CHOICE_DX;
  dimy     = MENU_CHOICE_DY;
  flags    = flags & ~IT_SELECTABLE;
  flags    = flags | IT_TXT_CENTER;
};

instance MENUITEM_DYNAMAP(C_MENU_ITEM)
{
  C_MENU_ITEM_TEXT_BASE();
  posy += MENU_DY;
  text[0] = Str_GetLocalizedString(
    "Динамическая карта",
    "Dynamic map",
    "Dynamische Karte",
    "Mapa dynamiczna"
  );
};

instance MENUITEM_DYNAMAP_CHOICE(C_MENU_ITEM_DEF)
{
  C_MENUITEM_CHOICE_BASE();
  posy += MENU_DY;
  onchgsetoption        = "DynaMap";
  onchgsetoptionsection = SECTION;
  text[0] = Str_GetLocalizedString(
    "выкл|вкл",
    "off|on",
    "aus|an",
    "wył.|wł."
  );
};

instance MENUITEM_ALPHA(C_MENU_ITEM)
{
  C_MENU_ITEM_TEXT_BASE();
  posy += MENU_DY * 2;
  text[0] = Str_GetLocalizedString(
    "Альфа-канал",
    "Alpha channel",
    "Alpha-Kanal",
    "Kanał alfa"
  );
};

instance MENUITEM_ALPHA_CHOICE(C_MENU_ITEM_DEF)
{
  C_MENUITEM_CHOICE_BASE();
  posy += MENU_DY * 2;
  onchgsetoption        = "DynaMapAlpha";
  onchgsetoptionsection = SECTION;
  text[0] = Str_GetLocalizedString(
    "выкл|вкл",
    "off|on",
    "aus|an",
    "wył.|wł."
  );
};

instance MENUITEM_TRANSPARENCY(C_MENU_ITEM)
{
  C_MENU_ITEM_TEXT_BASE();
  posy += MENU_DY * 3;
  text[0] = Str_GetLocalizedString(
    "Прозрачность",
    "Transparency",
    "Transparenz",
    "Przezroczystość"
  );
};

instance MENUITEM_TRANSPARENCY_SLIDER(C_MENU_ITEM_DEF)
{
  backpic = MENU_SLIDER_BACK_PIC;
  type = MENU_ITEM_SLIDER;
  text[0] = "";
  fontname = MENU_FONT_SMALL;
  posx = 5500;
  posy = MENU_START_Y + MENU_DY * 3 + MENU_SLIDER_YPLUS;
  dimx = 2000;
  dimy = MENU_SLIDER_DY;
  onchgsetoption = "DynaMapTransparency";
  onchgsetoptionsection = SECTION;
  // 13 positions map to 20,25,...,80 percent transparency.
  userFloat[0] = 13;
  userString[0] = MENU_SLIDER_POS_PIC;
  flags = flags & ~IT_SELECTABLE;
  flags = flags | IT_TXT_CENTER;
};
