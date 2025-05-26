#pragma once

#include "defines.h"

// Width and height for font size 1: 105x19. 8 buttons with 1 pixel offset
// Width and height for font size 2: 203x32  6 buttons with 1 pixel offset and 3 pixel offset between menu label and buttons

struct entryMenu {
    String text;
    ImageDef* image;
    void (*function)();
};

struct menuData
{
  int totalMenus;
  int textSize;
  String menuName;
  int linesThick;
  entryMenu entryList[MAX_MENU_ITEMS];
  int itemsOnPage;
  int currentButton;
};

extern String lastMenuSelected;
extern int currentMenuItem;

void initMenu(entryMenu entryList[MAX_MENU_ITEMS], int totalMenus, String menuName, int textSize = 1, int linesThick = 0);
void showMenu();
void loopMenu();

