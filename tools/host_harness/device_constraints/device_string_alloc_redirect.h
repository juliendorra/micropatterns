// Force-included only while compiling upstream WString.cpp. Include the libc
// declarations before defining the names so the macros cannot rewrite them.
#ifndef MP_DEVICE_STRING_ALLOC_REDIRECT_H
#define MP_DEVICE_STRING_ALLOC_REDIRECT_H

#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif
void* mpArduinoStringMalloc(size_t size);
void* mpArduinoStringRealloc(void* ptr, size_t size);
void mpArduinoStringFree(void* ptr);
#ifdef __cplusplus
}
#endif

#define malloc mpArduinoStringMalloc
#define realloc mpArduinoStringRealloc
#define free mpArduinoStringFree

#endif
