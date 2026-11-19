META
{
  Parser = Menu;
  After = zUnionMenu.d;
  Namespace = zSamplePlugin
};

const string SECTION = "zSamplePlugin";

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
  text[0]          = "zSamplePlugin";
  text[1]          = "Configure zSamplePlugin settings";
  onselaction[0]   = SEL_ACTION_STARTMENU;
  onselaction_s[0] = "zSamplePlugin:MENU_SAMPLEPLUGIN_SETTINGS";
};

instance MENU_SAMPLEPLUGIN_SETTINGS(C_MENU_DEF)
{
  backpic   = MENU_BACK_PIC;
  items[0]  = "zSamplePlugin:MENUITEM_HEADLINE";
  items[1]  = "zSamplePlugin:MENUITEM_ENABLED";
  items[2]  = "zSamplePlugin:MENUITEM_ENABLED_CHOICE";
  items[3]  = "UNION_MENUITEM_BACK";
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
    "zSamplePlugin - Настройки",
    "zSamplePlugin - Settings",
    "zSamplePlugin - Einstellungen",
    "zSamplePlugin - Ustawienia"
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

instance MENUITEM_ENABLED(C_MENU_ITEM)
{
  C_MENU_ITEM_TEXT_BASE();
  posy += MENU_DY;
  text[0] = Str_GetLocalizedString(
    "Включение плагина",
    "Plugin enabled",
    "Plugin aktiviert",
    "Wtyczka włączona"
  );
};

instance MENUITEM_ENABLED_CHOICE(C_MENU_ITEM_DEF)
{
  C_MENUITEM_CHOICE_BASE();
  posy += MENU_DY;
  onchgsetoption        = "Enabled";
  onchgsetoptionsection = SECTION;
  text[0] = Str_GetLocalizedString(
    "выкл|вкл",
    "off|on",
    "aus|an",
    "wył.|wł."
  );
};
