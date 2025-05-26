#pragma once

#include "defines.h"

extern GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> *dis;

void initDisplay();
void deInitScreen();
extern bool dUChange; // Display update change boolean, to simplify the code
void disUp(bool reallyUpdate = false, bool ignoreCounter = false, bool ignoreSleep = false);
void resetHoldManage();
void updateDisplay(bool mode);

#if DEBUG
void initDisplayDebug();
#endif

