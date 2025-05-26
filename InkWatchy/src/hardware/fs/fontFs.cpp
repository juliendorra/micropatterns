#include "littlefs.h"

char loadedFontNames[FONT_COUNT][RESOURCES_NAME_LENGTH] = {0};
GFXfont *loadedFont[FONT_COUNT];
// Todo garbage collecting here too

const GFXfont *getFont(String name)
{
    if (fsSetup() == false)
    {
        debugLog("Failed to setup fs");
        return &FreeSansBold9pt7b;
    }
    uint8_t emptyListIndex = 0;
    for (int i = 0; i < FONT_COUNT; i++)
    {
        if (strcmp(loadedFontNames[i], name.c_str()) == 0)
        {
            return loadedFont[i];
        }
        else
        {
            if (strlen(loadedFontNames[i]) == 0)
            {
                emptyListIndex = i;
                break;
            }
        }
    }
    // Bitmap
    File fileFont = LittleFS.open("/font/" + name);
    if (fileFont == false)
    {
        debugLog("File is not available: " + name);
        return &FreeSansBold9pt7b;
    }
    int fileFontSize = fileFont.size();
    //debugLog("file size: " + String(fileFontSize));
    if (fileFontSize <= 0)
    {
        debugLog("This file has size 0: " + name);
    }
    uint8_t *fileBuf = (uint8_t *)malloc(fileFontSize * sizeof(uint8_t));
    if (fileFont.read(fileBuf, fileFontSize) == 0)
    {
        debugLog("Failed to read the file: " + name);
        return &FreeSansBold9pt7b;
    }
    fileFont.close();

    uint16_t fontBitmapSize = fileBuf[0] | (fileBuf[1] << 8);
    //debugLog("Bitmap size: " + String(fontBitmapSize) + " for font: " + name);
    //GFXfont *newFont = (GFXfont *)malloc(sizeof(GFXfont));
    GFXfont *newFont = new GFXfont();

    newFont->bitmap = fileBuf + 2; // To skip the uint16_t
    /*
    {
        debugLog("Dumping bitmap");
        const uint8_t *data = (const uint8_t *)newFont->bitmap;
        for (size_t i = 0; i < 12; ++i)
        {
            printf("0x%02x ", data[i]);
        }
        printf("\n");
    }
    */
    newFont->glyph = (GFXglyph *)(fileBuf + 2 + fontBitmapSize);
    /*
    {
        debugLog("Dumping glyph");
        debugLog("glyph bitmapOffset: " + String(newFont->glyph[0].bitmapOffset));
        debugLog("glyph width: " + String(newFont->glyph[0].width));
        debugLog("glyph height: " + String(newFont->glyph[0].height));
        debugLog("glyph xAdvance: " + String(newFont->glyph[0].xAdvance));
        debugLog("glyph xOffset: " + String(newFont->glyph[0].xOffset));
        debugLog("glyph yOffset: " + String(newFont->glyph[0].yOffset));
        printf("\n");
    }
    */

    size_t metaDataOffset = 2 + fontBitmapSize + 95 * sizeof(GFXglyph); // There are exactly 95 glyphs, always
    newFont->first = *reinterpret_cast<uint16_t *>(fileBuf + metaDataOffset);
    newFont->last = *reinterpret_cast<uint16_t *>(fileBuf + metaDataOffset + sizeof(uint16_t));
    newFont->yAdvance = *(fileBuf + metaDataOffset + 2 * sizeof(uint16_t));

    //debugLog("Value first: " + String(newFont->first) + " for font: " + name);
    //debugLog("Value last: " + String(newFont->last) + " for font: " + name);
    //debugLog("Value yAdvance: " + String(newFont->yAdvance) + " for font: " + name);

    int nameLength = name.length();
#if DEBUG
    if (nameLength > RESOURCES_NAME_LENGTH)
    {
        debugLog("Resource name: " + name + " is too big because RESOURCES_NAME_LENGTH. Buffer overflow.");
    }
#endif
    memset(loadedFontNames[emptyListIndex], '\0', RESOURCES_NAME_LENGTH); // To be sure comparison works
    strncpy(loadedFontNames[emptyListIndex], name.c_str(), RESOURCES_NAME_LENGTH);
    loadedFont[emptyListIndex] = newFont;
    return loadedFont[emptyListIndex];
}