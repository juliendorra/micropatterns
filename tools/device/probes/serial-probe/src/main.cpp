// Does the app run, and does its serial reach the host? Two channels, one build.
//   motor buzzes  -> the app IS running
//   serial silent  -> UART0 output does not reach the host
#include <Arduino.h>
#define VIB_MOTOR_PIN 13
void setup() {
    pinMode(VIB_MOTOR_PIN, OUTPUT);
    digitalWrite(VIB_MOTOR_PIN, LOW);
    Serial.begin(115200);
    delay(300);
    Serial.println("SERIALTEST: setup reached");
}
void loop() {
    Serial.printf("SERIALTEST alive %lu\n", millis() / 1000);
    Serial.flush();
    digitalWrite(VIB_MOTOR_PIN, HIGH); delay(120);
    digitalWrite(VIB_MOTOR_PIN, LOW);  delay(1500);
}
