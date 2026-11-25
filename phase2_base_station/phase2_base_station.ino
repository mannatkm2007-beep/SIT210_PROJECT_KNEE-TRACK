// KneeTrack - Phase 2 base station (Arduino Uno R3)
// MQ-2 on A0, active buzzer on D8, green LED D6, red LED D7.
// Sends {"gas_level": N, "hazard": 0/1} at 1 Hz over USB serial (115200 baud),
// plus an immediate packet whenever the hazard state changes.

const int PIN_MQ2    = A0;
const int PIN_BUZZER = 11;
const int PIN_LED_G  = 6;   // green status LED (via 220 ohm to GND)
const int PIN_LED_R  = 7;   // red status LED (via 220 ohm to GND)

const unsigned long WARMUP_MS      = 20000UL;  // MQ-2 needs warm-up (longer is better)
const unsigned long SAMPLE_MS      = 100;      // 10 Hz sampling keeps alert latency well under 1 s
const unsigned long REPORT_MS      = 1000;     // 1 Hz JSON report
const unsigned long BEEP_ON_MS     = 200;      // hazard beep pattern: 200 ms on / 200 ms off
const unsigned long BEEP_OFF_MS    = 200;

const int HAZARD_MARGIN_ON  = 150;  // hazard when reading > baseline + this
const int HAZARD_MARGIN_OFF = 100;  // clear when reading < baseline + this (hysteresis)
const int HAZARD_ABS        = 40;  // or when reading exceeds this absolute value

int  baseline = 0;
int  gasLevel = 0;
bool hazard   = false;
bool lastReportedHazard = false;

unsigned long tSample = 0, tReport = 0, tBeep = 0;
bool buzzerOn = false;

void sendJson() {
  Serial.print("{\"gas_level\": ");
  Serial.print(gasLevel);
  Serial.print(", \"hazard\": ");
  Serial.print(hazard ? 1 : 0);
  Serial.println("}");
}

int readAveraged() {
  long sum = 0;
  for (int i = 0; i < 8; i++) { sum += analogRead(PIN_MQ2); delay(2); }
  return (int)(sum / 8);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_R, OUTPUT);
  digitalWrite(PIN_LED_G, LOW);
  digitalWrite(PIN_LED_R, LOW);

  // Warm-up (red and green blink alternately), then take a clean-air baseline
  unsigned long start = millis();
  while (millis() - start < WARMUP_MS) {
    bool phase = ((millis() - start) / 500) % 2 == 0;
    digitalWrite(PIN_LED_G, phase ? HIGH : LOW);
    digitalWrite(PIN_LED_R, phase ? LOW : HIGH);
    delay(50);
  }
  long sum = 0;
  for (int i = 0; i < 20; i++) { sum += readAveraged(); delay(50); }
  baseline = (int)(sum / 20);

  // Short start-up beep so you know the board is ready
  digitalWrite(PIN_BUZZER, HIGH); delay(100); digitalWrite(PIN_BUZZER, LOW);
}

void loop() {
  unsigned long now = millis();

  // Sample and evaluate hazard (non-blocking)
  if (now - tSample >= SAMPLE_MS) {
    tSample = now;
    gasLevel = readAveraged();

    if (!hazard && (gasLevel > baseline + HAZARD_MARGIN_ON || gasLevel > HAZARD_ABS)) {
      hazard = true;
    } else if (hazard && gasLevel < baseline + HAZARD_MARGIN_OFF && gasLevel < HAZARD_ABS) {
      hazard = false;
    }

    // Immediate report on state change so the Pi hears about hazards fast
    if (hazard != lastReportedHazard) {
      sendJson();
      lastReportedHazard = hazard;
      tReport = now;
    }
  }

  // Regular 1 Hz report
  if (now - tReport >= REPORT_MS) {
    tReport = now;
    sendJson();
  }

  // Buzzer pattern: beeping while hazard, silent otherwise
  if (hazard) {
    unsigned long interval = buzzerOn ? BEEP_ON_MS : BEEP_OFF_MS;
    if (now - tBeep >= interval) {
      tBeep = now;
      buzzerOn = !buzzerOn;
      digitalWrite(PIN_BUZZER, buzzerOn ? HIGH : LOW);
      digitalWrite(PIN_LED_R, buzzerOn ? HIGH : LOW);  // red blinks with the buzzer
    }
    digitalWrite(PIN_LED_G, LOW);
  } else {
    if (buzzerOn) {
      buzzerOn = false;
      digitalWrite(PIN_BUZZER, LOW);
    }
    digitalWrite(PIN_LED_R, LOW);
    digitalWrite(PIN_LED_G, HIGH);                    // solid green = normal
  }
}
