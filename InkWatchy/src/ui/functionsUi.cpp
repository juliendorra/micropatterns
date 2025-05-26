#include "functionsUi.h"

// Default values... are set in screen
const GFXfont *font = &FreeSansBold9pt7b;
uint16_t maxHeight = 0;
int textSize = 1;

void setTextSize(int i)
{
  textSize = i;
  dis->setTextSize(i);
}

void setFont(const GFXfont *fontTmp)
{
  font = fontTmp;
  dis->setFont(font);
}

void writeLine(String strToWrite, int cursorX, uint16_t *currentHeight)
{
  dis->setCursor(cursorX, *currentHeight);
  dis->print(strToWrite);
  *currentHeight = *currentHeight + maxHeight;
}

// Remember to reset maxHeight if you don't use it
void centerText(String str, uint16_t *currentHeight)
{
  uint16_t w;
  getTextBounds(str, NULL, NULL, &w, NULL);
  int x = (dis->width() - w) / 2;
  dis->setCursor(x, *currentHeight);
  dis->print(str);
  *currentHeight = *currentHeight + maxHeight;
}

void writeTextReplaceBack(String str, int16_t x, int16_t y, uint16_t frColor, uint16_t bgColor, bool manualWidth, uint8_t manualWidthAdd, uint8_t manualHeighAdd)
{
  // debugLog("Drawing bitmap with text: " + str + " at: " + String(x) + "x" + String(y));
  uint16_t w, h;
  getTextBounds(str, NULL, NULL, &w, &h);
  // debugLog("w: " + String(w) + " h: " + String(h));
  if (manualWidth == false) {
    w = w + 5;
  } else {
    w = w + manualWidthAdd;
  }
  if (containsBelowChar(str) == true)
  {
    GFXcanvas1 canvasTmp(w, h + manualHeighAdd);
    canvasTmp.setTextWrap(false);
    canvasTmp.setFont(font);
    canvasTmp.setTextSize(textSize);
    canvasTmp.setCursor(0, h - manualHeighAdd);
    if (manualWidth == true)
    {
      canvasTmp.setTextWrap(false);
    }
    canvasTmp.print(str);
    dis->drawBitmap(x, y - h + manualHeighAdd, canvasTmp.getBuffer(), w, h + manualHeighAdd, frColor, bgColor); // this is relative to the cursor.
#if DRAW_DEBUG_RECT
    dis->drawRect(x, y - h + manualHeighAdd, w, h + manualHeighAdd, frColor);
#endif
  }
  else
  {
    GFXcanvas1 canvasTmp(w, h + manualHeighAdd);
    canvasTmp.setFont(font);
    canvasTmp.setTextSize(textSize);
    canvasTmp.setCursor(0, h);
    if (manualWidth == true)
    {
      canvasTmp.setTextWrap(false);
    }
    canvasTmp.print(str);
    dis->drawBitmap(x, y - h, canvasTmp.getBuffer(), w, h + manualHeighAdd, frColor, bgColor); // this is relative to the cursor.
#if DRAW_DEBUG_RECT
    dis->drawRect(x, y - h, w, h + manualHeighAdd, frColor);
#endif
  }
}

void writeTextCenterReplaceBack(String str, uint16_t y, uint16_t frColor, uint16_t bgColor)
{
  uint16_t w, h;
  getTextBounds(str, NULL, NULL, &w, &h);
  // debugLog("w: " + String(w));
  // debugLog("h: " + String(h));
  w = w + 5;
  int16_t x = (dis->width() - w) / 2;
  if (containsBelowChar(str) == true)
  {
    GFXcanvas1 canvasTmp(w, h + 3);
    canvasTmp.setFont(font);
    canvasTmp.setTextSize(textSize);
    canvasTmp.setCursor(0, h - 3);
    canvasTmp.print(str);
    dis->fillRect(0, y - h, 200, h + 3, GxEPD_WHITE);
    dis->drawBitmap(x, y - h + 3, canvasTmp.getBuffer(), w, h + 3, frColor, bgColor); // this is relative to the cursor.
#if DRAW_DEBUG_RECT
    dis->drawRect(x, y - h + 3, w, h + 3, frColor);
#endif
  }
  else
  {
    GFXcanvas1 canvasTmp(w, h + 3);
    canvasTmp.setFont(font);
    canvasTmp.setTextSize(textSize);
    canvasTmp.setCursor(0, h);
    canvasTmp.print(str);
    dis->fillRect(0, y - h, 200, h + 3, GxEPD_WHITE);
    dis->drawBitmap(x, y - h, canvasTmp.getBuffer(), w, h + 3, frColor, bgColor); // this is relative to the cursor.
#if DRAW_DEBUG_RECT
    dis->drawRect(x, y - h, w, h + 3, frColor);
#endif
  }
}

int16_t xS;
int16_t yS;
uint16_t wS;
uint16_t hS;
void getTextBounds(String &str, int16_t *xa, int16_t *ya, uint16_t *wa, uint16_t *ha, int16_t cxa, int16_t cya)
{
  int16_t cx = dis->getCursorX();
  int16_t cy = dis->getCursorY();
  dis->setCursor(10, 100);
  xS = 0;
  yS = 0;
  wS = 0;
  hS = 0;
  dis->getTextBounds(str, cxa, cya, &xS, &yS, &wS, &hS);
  if (xa != NULL)
  {
    *xa = xS;
  }
  if (ya != NULL)
  {
    *ya = yS;
  }
  if (wa != NULL)
  {
    *wa = wS;
  }
  if (ha != NULL)
  {
    *ha = hS;
  }
  dis->setCursor(cx, cy);
}

// Use the image pack here to make it easier
// Like that:
// writeImageN(100, 100, testSomethiongImgPack);
void writeImageN(int16_t x, int16_t y, ImageDef *image, uint16_t frColor, uint16_t bgColor)
{
  int16_t cx = dis->getCursorX();
  int16_t cy = dis->getCursorY();
  dis->setCursor(0, 0);
  dis->drawBitmap(x, y, image->bitmap, image->bw, image->bh, frColor, bgColor);
  dis->setCursor(cx, cy);
}

sizeInfo drawButton(int16_t x, int16_t y, String str, ImageDef *image, bool invert, int tolerance, int borderWidth, uint16_t frColor, uint16_t bgColor, bool draw, const GFXfont *buttonFont)
{
  // debugLog("DRAWBUTTON CALLED");
  sizeInfo size = {};
  int toleranceSize = tolerance * 2 + borderWidth * 2;
  size.w = toleranceSize;
  size.h = toleranceSize;

  int16_t tx = 0;
  int16_t ty = 0;
  uint16_t tw = 0;
  uint16_t th = 0;
  if (str != "")
  {
    dis->setTextWrap(true);
    getTextBounds(str, &tx, &ty, &tw, &th);
    // debugLog("tw: " + String(tw) + " th: " + String(th));
    // debugLog("tx: " + String(tx) + " ty: " + String(ty));
    size.w += tw;
    size.w += 1;
    if (size.w > dis->width() - toleranceSize)
    {
      // th = th * 2;
      size.h = size.h + toleranceSize;
      // debugLog("Second line in print button...");
    }
  }

  if (image->bitmap != NULL)
  {
    size.w += image->bw;
  }

  if (image->bitmap != NULL && str != "")
  {
    size.w += tolerance;
  }

  if (image->bitmap != NULL && image->bh > th)
  {
    size.h += image->bh;
  }
  else
  {
    size.h += th + tolerance; // Not sure + tolerance
  }

  // debugLog("Final size.h is: " + String(size.h));
  // debugLog("Final size.w is: " + String(size.w));

  if (draw == false)
  {
    return size;
  }

  GFXcanvas1 canvasTmp(size.w, size.h);
  canvasTmp.setTextWrap(true);
  canvasTmp.setFont(buttonFont);
  canvasTmp.setTextSize(textSize);
  canvasTmp.setCursor(tolerance + borderWidth, tolerance + borderWidth);
  if (image->bitmap != NULL)
  {
    canvasTmp.drawBitmap(tolerance + borderWidth, tolerance + borderWidth, image->bitmap, image->bw, image->bh, frColor, bgColor);
    if (str != "")
    {
      canvasTmp.setCursor(canvasTmp.getCursorX() + tolerance + image->bw, canvasTmp.getCursorY());
    }
  }
  if (str != "")
  {
    int yCursorTmp = 0;

    dis->setTextWrap(false);
    uint16_t thTmp = 0;
    getTextBounds(str, NULL, NULL, NULL, &thTmp);
    dis->setTextWrap(true);
    // debugLog("thTmp: " + String(thTmp));

    // Center font if its a one line
    // debugLog("size.h: " + String(size.h));
    // debugLog("thTmp: " + String(thTmp));
    if((int32_t(size.h) - int32_t(thTmp)) <= thTmp) {
      yCursorTmp = ((size.h - thTmp) / 2) + thTmp;
    } else {
      yCursorTmp = thTmp + tolerance;
    }

    if (containsBelowChar(str) == true)
    {
      // debugLog("We are below char for string: " + str);
      yCursorTmp = yCursorTmp - 3;
    }
    else
    {
      // debugLog("Below char not applied for string: " + str);
    }

    canvasTmp.setCursor(canvasTmp.getCursorX(), yCursorTmp);
    // debugLog("Printing text to canvas: " + str + " at: " + String(canvasTmp.getCursorX()) + "x" + String(canvasTmp.getCursorY()) + " for size: " + String(canvasTmp.width()) + "x" + String(canvasTmp.height()));
    canvasTmp.print(str);
  }

  int16_t canvasCursorXTmp = canvasTmp.getCursorX();
  int16_t canvasCursorYTmp = canvasTmp.getCursorY();
  int16_t rectOffset = 0;
  canvasTmp.setCursor(0, 0);
  uint16_t borderColor = bgColor;
  if (invert == true)
  {
    borderColor = frColor;
  }
  for (int16_t i = 0; i < borderWidth; i++)
  {
    // debugLog("Drawing border in button");
    canvasTmp.drawRect(i + rectOffset, i + rectOffset, size.w - rectOffset, size.h - rectOffset, borderColor);
  }
  canvasTmp.setCursor(canvasCursorXTmp, canvasCursorYTmp);

  int16_t cx = dis->getCursorX();
  int16_t cy = dis->getCursorY();
  dis->setCursor(0, 0);
  if (invert == false)
  {
    // debugLog("Drawing non inverted button");
    dis->drawBitmap(x, y, canvasTmp.getBuffer(), size.w, size.h, frColor, bgColor); // why inverted?
  }
  else
  {
    dis->drawBitmap(x, y, canvasTmp.getBuffer(), size.w, size.h, bgColor, frColor);
    // dis->drawInvertedBitmap(x, y, canvasTmp.getBuffer(), size.w, size.h, frColor);
  }
  dis->setCursor(cx, cy);
  return size;
}

void simpleCenterText(String text)
{
  dis->fillScreen(GxEPD_WHITE);
  writeTextCenterReplaceBack(text, dis->height() / 2);
  disUp(true);
}

void textPage(String title, String *strList, int listCount, const GFXfont *customFont)
{
  dis->fillScreen(GxEPD_WHITE);

  uint16_t h;
  setFont(&FreeSansBold9pt7b);
  setTextSize(1);
  getTextBounds(title, NULL, NULL, NULL, &h);
  if (containsBelowChar(title) == true)
  {
    h = h - 3;
  }
  writeTextCenterReplaceBack(title, h);
  h = h + 1;
  dis->fillRect(0, h, dis->width(), 1, GxEPD_BLACK);
  h = h + 3;

  dis->setFont(customFont);

  uint16_t textHeight;
  getTextBounds(strList[0], NULL, NULL, NULL, &textHeight);
  h = h + textHeight;
  uint16_t firstHeight = h;

  for (int i = 0; i < listCount; i++)
  {
    dis->setCursor(0, h);
    dis->print(strList[i]);
    h = h + textHeight;
    h = h + 3;
  }

  // hacky work arround, I'm not sure how to handle this properly
  /*
  if (h > 200 && listCount > 1)
  {
    dis->fillRect(0, firstHeight, 200, 200, GxEPD_WHITE);
    dis->setTextWrap(true);
    dis->setCursor(0, firstHeight);
    for (int i = 0; i < listCount; i++)
    {
      dis->print(strList[i]);
      if (i != listCount - 1)
      {
        dis->print(", ");
      }
    }
  }
  */

  disUp(true);
  setFont(font);
}

void drawProgressBar(int x, int y, int width, int height, int progress)
{
  progress = constrain(progress, 0, 100);

  int filledWidth = map(progress, 0, 100, 0, width);

  dis->fillRect(x, y, width, height, GxEPD_WHITE); // clean
  dis->fillRect(x, y, filledWidth, height, GxEPD_BLACK);

  for (int16_t i = filledWidth; i < width; i++)  
  {  
    for (int16_t j = 0; j < height; j++)  
    {  
      if ((i + j) % 2 == 0)  
      {  
        dis->drawPixel(x + i, y + j, GxEPD_BLACK);  
      }  
    }  
  }  
}

sizeInfo drawTextSimple(String text, String font, int16_t x, int16_t y)
{
  uint16_t h = 0;
  uint16_t w = 0;
  setFont(getFont(font));
  setTextSize(1);
  getTextBounds(text, NULL, NULL, &w, &h);
  dis->setCursor(x, y + h);
  dis->print(text);
  dUChange = true;
  return {w, h}; // hm?
}