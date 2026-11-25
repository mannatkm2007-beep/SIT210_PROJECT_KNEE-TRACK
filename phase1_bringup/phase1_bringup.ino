// KneeTrack - Phase 1 hardware bring-up (Arduino Nano 33 IoT)
// Tests each part on its own. Open Serial Monitor at 115200 baud.
// Pins: FSR A0 | TRIG D2 | ECHO D3 (via divider) | BUZZER D5 | RGB D9, D10, D11

#include <Arduino_LSM6DS3.h>

const int PIN_FSR    = A0;
const int PIN_TRIG   = 2;
const int PIN_ECHO   = 3;
const int PIN_BUZZER = 5;
const int PIN_R      = 9;
const int PIN_G      = 10;
const int PIN_B      = 11;   // if blue does not dim, move it to D6 (see notes)

const bool BUZZER_ACTIVE = true;  // true = active buzzer, false = passive (tone)

void setRGB(int r, int g, int b) {
  analogWrite(PIN_R, r);
  analogWrite(PIN_G, g);
  analogWrite(PIN_B, b);
}

void beep(int ms) {
  if (BUZZER_ACTIVE) {
    digitalWrite(PIN_BUZZER, HIGH);
    delay(ms);
    digitalWrite(PIN_BUZZER, LOW);
  } else {
    tone(PIN_BUZZER, 2000, ms);
    delay(ms);
  }
}

float readDistanceCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long us = pulseIn(PIN_ECHO, HIGH, 30000UL);  // 30 ms timeout
  if (us == 0) return -1.0;                              // no echo
  return us * 0.0343 / 2.0;
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_R, OUTPUT);
  pinMode(PIN_G, OUTPUT);
  pinMode(PIN_B, OUTPUT);

  if (!IMU.begin()) {
    Serial.println("IMU FAILED to start");
  } else {
    Serial.println("IMU OK");
  }

  // LED colour test: red, green, blue, then white
  setRGB(255, 0, 0);   delay(500);
  setRGB(0, 255, 0);   delay(500);
  setRGB(0, 0, 255);   delay(500);
  setRGB(255, 255, 255); delay(500);
  setRGB(0, 0, 0);

  // Buzzer test
  beep(200);
  Serial.println("Setup done. Streaming sensor values...");
}

void loop() {
  float ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
  if (IMU.accelerationAvailable()) IMU.readAcceleration(ax, ay, az);
  if (IMU.gyroscopeAvailable())    IMU.readGyroscope(gx, gy, gz);

  int fsr = analogRead(PIN_FSR);
  float dist = readDistanceCm();

  Serial.print("acc(g): ");  Serial.print(ax, 2); Serial.print(", ");
  Serial.print(ay, 2);       Serial.print(", ");  Serial.print(az, 2);
  Serial.print(" | gyro(dps): "); Serial.print(gx, 1); Serial.print(", ");
  Serial.print(gy, 1);       Serial.print(", ");  Serial.print(gz, 1);
  Serial.print(" | FSR: ");  Serial.print(fsr);
  Serial.print(" | dist(cm): "); Serial.println(dist, 1);

  // Quick visual check: LED goes red when FSR is pressed hard, green otherwise
  if (fsr > 600) { setRGB(255, 0, 0); beep(50); }
  else           { setRGB(0, 255, 0); }

  delay(1000);
}
