// Absolute minimum: no display, no libraries, no globals with constructors.
// Buzzes the vibration motor forever. If this does not buzz, the fault is the
// build/flash/boot path for this board -- not any application code.
#include <Arduino.h>
#define VIB_MOTOR_PIN 13
void setup() {
    pinMode(VIB_MOTOR_PIN, OUTPUT);
    digitalWrite(VIB_MOTOR_PIN, LOW);
}
void loop() {
    digitalWrite(VIB_MOTOR_PIN, HIGH); delay(200);
    digitalWrite(VIB_MOTOR_PIN, LOW);  delay(800);
}
