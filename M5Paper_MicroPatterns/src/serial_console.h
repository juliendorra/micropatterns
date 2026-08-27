#ifndef SERIAL_CONSOLE_H
#define SERIAL_CONSOLE_H

// Serial command console -- a programmatic channel for driving the device
// over USB, so a script can be selected and run without touching the buttons.
//
// The M5Paper is a classic ESP32-D0WDQ6 (no native USB), so this is plain
// UART over the CH9102 bridge at the monitor_speed in platformio.ini (115200).
//
// Commands (one per line, \n or \r\n terminated):
//
//   list            print the script list as "<index>\t<humanId>\t<name>"
//   run <id|index>  run a script, by human id ("eyes") or list index ("3")
//   next / prev     same as the UP/DOWN buttons
//   current         print the currently loaded script id
//   help            print this list
//
// Every reply is prefixed with "MPCON|" so it can be grepped out of the
// firmware's own log chatter (CORE_DEBUG_LEVEL=5 is noisy).
//
// Commands are injected into g_inputEventQueue, the same queue the buttons
// feed, so they interact correctly with sleep, render-interrupt and app state.

#include "event_defs.h"

#define SERIAL_CONSOLE_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define SERIAL_CONSOLE_TASK_STACK_SIZE (4096) // Words; loads list.json + JSON doc

// Longest accepted command line; anything beyond is discarded to end-of-line.
#define SERIAL_CONSOLE_MAX_LINE 128

void SerialConsoleTask_Function(void *pvParameters);

#endif // SERIAL_CONSOLE_H
