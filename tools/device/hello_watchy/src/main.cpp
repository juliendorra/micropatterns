#include <Arduino.h>
void setup() { Serial.begin(115200); delay(300); Serial.println("HELLO-WATCHY setup"); }
void loop()  { Serial.printf("HELLO-WATCHY alive %lu\n", millis()/1000); delay(1000); }
