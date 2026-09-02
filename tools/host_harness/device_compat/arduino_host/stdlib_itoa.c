// ESP-IDF's newlib provides these two non-standard symbols. Emscripten's libc
// does not, so bridge them to the versioned Arduino conversion implementation.
#include "stdlib_noniso.h"

char *itoa(int value, char *result, int base) {
  return ltoa((long)value, result, base);
}

char *utoa(unsigned int value, char *result, int base) {
  return ultoa((unsigned long)value, result, base);
}
