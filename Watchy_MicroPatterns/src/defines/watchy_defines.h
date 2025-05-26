#pragma once

// Pin definitions for Watchy
// Display (standard Watchy connections)
#define EPD_CS 5
#define EPD_DC 10
#define EPD_RST 9
#define EPD_BUSY 19

// Buttons (standard Watchy 4-button layout)
#define MENU_BTN_PIN 26 // Typically top-left
#define BACK_BTN_PIN 25 // Typically bottom-left
#define UP_BTN_PIN 32   // Typically top-right
#define DOWN_BTN_PIN 4  // Typically bottom-right

// RTC (standard Watchy I2C)
#define RTC_SDA 21
#define RTC_SCL 22

// Define logical inputs based on issue description
// M5Paper UP -> Watchy "LEFT UP" (e.g., MENU_BTN_PIN)
#define LOGICAL_INPUT_UP_PIN MENU_BTN_PIN 
// M5Paper DOWN -> Watchy "LEFT DOWN" (e.g., BACK_BTN_PIN)
#define LOGICAL_INPUT_DOWN_PIN BACK_BTN_PIN
// M5Paper PUSH -> Watchy "RIGHT UP" (e.g., UP_BTN_PIN) or "RIGHT DOWN" (e.g., DOWN_BTN_PIN)
// The InputManager will handle the OR logic for these two physical pins.
#define LOGICAL_INPUT_CONFIRM_PIN_1 UP_BTN_PIN
#define LOGICAL_INPUT_CONFIRM_PIN_2 DOWN_BTN_PIN

// GxEPD2 constructor arguments for Watchy
// For GxEPD2_154_D67 (Watchy V1.0, V1.5, V2.0 E-Paper)
// GxEPD2_DISPLAY_CLASS<GxEPD2_DRIVER_CLASS, MAX_HEIGHT(GxEPD2_DRIVER_CLASS)>
// display(GxEPD2_DRIVER_CLASS(/*CS=5*/ EPD_CS, /*DC=10*/ EPD_DC, /*RST=9*/ EPD_RST, /*BUSY=19*/ EPD_BUSY))
// For Watchy S3 (ESP32-S3) which might use a different panel or pins, this would need adjustment.
// The platformio.ini already defines ATCHY_VER, which could be used for conditional compilation here if needed.
// For now, these are generic.
