// KneeTrack - Phase 3 wearable firmware (Arduino Nano 33 IoT)
// 50 Hz sampling, sensor validation + fallback, gait classification (rule-based
// placeholder for TinyML), RGB + buzzer alerts, BLE telemetry, RAM buffer, watchdog.
//
// Pins: TRIG D2 | ECHO D3 (divider) | BUZZER D5 (active) | RGB D9, D10, D11   (no FSR)
// Libraries: Arduino_LSM6DS3, ArduinoBLE, Adafruit SleepyDog Library
//
// Changes in this version:
//  - Ground reference is measured per person at power-on (retries if the foot is not still)
//    and the stance/swing thresholds are derived from the measured noise.
//  - Ground reference slowly follows drift while the foot is firmly on the ground.
//  - A single missed echo no longer creates a fake step (3 misses in a row = swing).
//  - Late calibration if the ultrasonic was dead at boot and recovers later.
//  - Starts in stance, so no phantom step is counted at power-on.
//  - STEP_TEST_MODE: LED shows stance (cyan) / swing (white) so steps can be checked by eye.
//  - Non-blocking LED: magenta blink = sensor fault, solid magenta = BLE failed.

#include <Arduino_LSM6DS3.h>
#include <ArduinoBLE.h>
#include <Adafruit_SleepyDog.h>
#include <math.h>

#define DEBUG_SERIAL   1   // 1 = print a status line once per second
#define STEP_TEST_MODE 1   // 1 = LED shows stance/swing (bench test), 0 = LED shows gait class

// ---------------- Pins ----------------
const int PIN_TRIG = 2, PIN_ECHO = 3, PIN_BUZZER = 5;
const int PIN_R = 9, PIN_G = 10, PIN_B = 11;   // if blue does not fade on D11, move it to D6
// Red is brighter than green/blue at 3.3 V, so dim it until white looks white.
// Tune these by eye (0.0 to 1.0). Use 1.0 for all three if you fit better resistors.
const float GAIN_R = 0.20f, GAIN_G = 1.0f, GAIN_B = 1.0f;

// ---------------- Timing ----------------
const unsigned long PERIOD_US = 20000UL;   // 50 Hz
const float DT = 0.02f;
const int US_EVERY_N = 3;                  // ultrasonic every 60 ms (about 16 Hz)
const unsigned long US_TIMEOUT_US = 6000;  // ~1 m max range, keeps the loop short
const int LIVE_EVERY_N = 1;                // BLE live packet every N samples (1 = 50 Hz, 2 = 25 Hz if packets drop)

// ---------------- Tunable thresholds ----------------
// Stance/swing thresholds are offsets ABOVE the per-person ground reference.
// They start at these defaults and are replaced after calibration (see calibrateGround()).
float stanceOnMm  = 20.0f;                 // foot at or below this height = stance
float stanceOffMm = 35.0f;                 // foot at or above this height = swing
const float STANCE_ON_MIN_MM  = 15.0f;     // lower and upper limit for the noise-based "on" threshold
const float STANCE_ON_MAX_MM  = 25.0f;
const float STANCE_GAP_MM     = 15.0f;     // off threshold = on threshold + this
const float CALIB_MAX_SD_CM   = 1.0f;      // calibration is rejected if the foot moved more than this
const int   CALIB_SAMPLES     = 15;
const int   CALIB_ATTEMPTS    = 5;
const float DRIFT_RATE        = 0.003f;    // how fast the ground reference follows slow drift
const int   LIFT_MISSES       = 3;         // missed echoes in a row that count as "foot lifted" (~180 ms)
const unsigned long MIN_PHASE_MS = 150;    // ignore stance/swing flips faster than this (debounce)

const float WALK_ROM_MIN = 15.0f;          // below this ROM = not walking
const float ROM_LOW      = 30.0f;          // deg, reduced range of motion
const float STANCE_HI    = 0.70f, STANCE_LO = 0.45f;  // stance ratio limits
const float CLEAR_LOW_ABOVE_OFF_MM = 15.0f;           // shuffling if max clearance < stanceOffMm + this
const unsigned long REALERT_MS = 5000;     // repeat beep while still deviating

// ---------------- Packet (17 bytes) ----------------
struct __attribute__((packed)) Packet {
  uint32_t t_ms;
  int16_t  angle_cdeg;     // angle x 100
  int16_t  gyro_dps10;     // gyro x 10
  uint16_t fsr;            // always 0 (FSR removed, field kept for compatibility)
  uint16_t clear_mm;
  uint16_t accmag_mg;
  uint8_t  gait;           // 0 normal, 1 mild, 2 significant
  uint8_t  flags;          // see FLAG_*
  uint8_t  seq;
};
const uint8_t FLAG_US_FAULT = 2, FLAG_IMU_FAULT = 4, FLAG_REPLAY = 8, FLAG_STANCE = 16;

// ---------------- BLE ----------------
BLEService svc("a7c10000-5d3e-4f6a-9b1c-2e8f4d6a1b00");
BLECharacteristic txChar("a7c10001-5d3e-4f6a-9b1c-2e8f4d6a1b00",
                         BLERead | BLENotify, sizeof(Packet));
bool wasConnected = false;
bool bleFailed = false;

// ---------------- Offline buffer: 30 s at 10 Hz in RAM ----------------
const int BUF_N = 300;
Packet buf[BUF_N];
int bufHead = 0, bufCount = 0;

void bufPush(const Packet &p) {
  buf[(bufHead + bufCount) % BUF_N] = p;
  if (bufCount < BUF_N) bufCount++;
  else bufHead = (bufHead + 1) % BUF_N;   // overwrite oldest
}
bool bufPop(Packet &p) {
  if (bufCount == 0) return false;
  p = buf[bufHead];
  bufHead = (bufHead + 1) % BUF_N;
  bufCount--;
  return true;
}

// ---------------- Sensor state ----------------
float g_ax = 0, g_ay = 0, g_az = 1, g_gx = 0, g_gy = 0, g_gz = 0;
float angle = 0;
float angleOffset = 0;           // zero reference, set at power-on while standing straight
float groundRefCm = 0, lastClearMm = 0;
bool  groundValid = false;       // true once a ground reference exists
int   imuStale = 0, usMiss = 0;
bool  usFault = false, imuFault = false;
bool  stance = true;             // starts in stance: the foot is on the ground at power-on
unsigned long lastPhaseChangeMs = 0;
uint32_t stepCount = 0;          // stance starts since power-on
uint32_t sampleCount = 0;
uint16_t maxClrSec = 0;          // highest clearance in the last second (debug print only)
uint8_t seq = 0;

// ---------------- Gait window (2 s) ----------------
const int WIN = 100;
float    angleBuf[WIN];
uint8_t  stanceBuf[WIN];
uint16_t clearBuf[WIN];
int winIdx = 0, winFilled = 0;
uint8_t gaitClass = 0;
unsigned long lastAlertMs = 0;

// ---------------- Outputs ----------------
bool beeping = false;
unsigned long beepStart = 0;
unsigned long tickUntil = 0;     // short step tick (step-test mode)
int curR = -1, curG = -1, curB = -1;

void setRGB(int r, int g, int b) {
  if (r == curR && g == curG && b == curB) return;   // avoid rewriting the PWM every loop
  curR = r; curG = g; curB = b;
  analogWrite(PIN_R, (int)(r * GAIN_R));
  analogWrite(PIN_G, (int)(g * GAIN_G));
  analogWrite(PIN_B, (int)(b * GAIN_B));
}

void startDoubleBeep() { beeping = true; beepStart = millis(); }

void updateBuzzer() {   // call every loop, never blocks
  unsigned long now = millis();
  if (now < tickUntil) { digitalWrite(PIN_BUZZER, HIGH); return; }
  if (!beeping) { digitalWrite(PIN_BUZZER, LOW); return; }
  unsigned long t = now - beepStart;
  bool on = (t < 100) || (t >= 200 && t < 300);
  digitalWrite(PIN_BUZZER, on ? HIGH : LOW);
  if (t >= 300) beeping = false;
}

// One place decides the LED colour, called every loop (non-blocking).
//   solid magenta        = BLE failed
//   blinking magenta     = ultrasonic or IMU fault
//   STEP_TEST_MODE = 1   : cyan = stance, white = swing
//   STEP_TEST_MODE = 0   : green = normal, amber = mild, red = significant
void updateLed() {
  if (bleFailed) { setRGB(255, 0, 255); return; }
  if (usFault || imuFault) {
    if ((millis() / 500) % 2 == 0) setRGB(255, 0, 255); else setRGB(0, 0, 0);
    return;
  }
#if STEP_TEST_MODE
  if (stance) setRGB(0, 255, 255);        // cyan = stance
  else        setRGB(255, 255, 255);      // white = swing
#else
  if (gaitClass == 0)      setRGB(0, 255, 0);
  else if (gaitClass == 1) setRGB(255, 120, 0);
  else                     setRGB(255, 0, 0);
#endif
}

// ---------------- Ultrasonic ----------------
float readDistCm() {
  digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long us = pulseIn(PIN_ECHO, HIGH, US_TIMEOUT_US);
  if (us == 0) return -1.0f;
  float cm = us * 0.0343f / 2.0f;
  if (cm < 2.0f || cm > 100.0f) return -1.0f;
  return cm;
}

// Measure the "foot on ground" distance for THIS wearer and strap height.
// Rejects the attempt if the readings vary too much (foot was moving) and retries.
// Also sets the stance thresholds from the measured noise.
bool calibrateGround() {
  for (int attempt = 0; attempt < CALIB_ATTEMPTS; attempt++) {
    float v[CALIB_SAMPLES];
    int n = 0;
    setRGB(0, 0, 255);                                // blue = measuring
    for (int i = 0; i < CALIB_SAMPLES; i++) {
      float d = readDistCm();
      if (d > 0) v[n++] = d;
      delay(60);
    }
    if (n >= CALIB_SAMPLES / 2) {
      float mean = 0;
      for (int i = 0; i < n; i++) mean += v[i];
      mean /= n;
      float var = 0;
      for (int i = 0; i < n; i++) var += (v[i] - mean) * (v[i] - mean);
      float sdCm = sqrtf(var / n);
      if (sdCm <= CALIB_MAX_SD_CM) {
        groundRefCm = mean;
        groundValid = true;
        float noiseMm = sdCm * 10.0f;
        stanceOnMm = 3.0f * noiseMm;
        if (stanceOnMm < STANCE_ON_MIN_MM) stanceOnMm = STANCE_ON_MIN_MM;
        if (stanceOnMm > STANCE_ON_MAX_MM) stanceOnMm = STANCE_ON_MAX_MM;
        stanceOffMm = stanceOnMm + STANCE_GAP_MM;
        return true;
      }
    }
    setRGB(0, 0, 0);                                  // brief off = "hold still, trying again"
    delay(300);
  }
  return false;
}

// ---------------- Classifier (placeholder for TinyML) ----------------
// Replace this function with the quantised 1D CNN once trained.
uint8_t classifyWindow() {
  float mn = angleBuf[0], mx = angleBuf[0];
  int stanceCnt = 0;
  uint16_t maxClear = 0;
  for (int i = 0; i < WIN; i++) {
    if (angleBuf[i] < mn) mn = angleBuf[i];
    if (angleBuf[i] > mx) mx = angleBuf[i];
    stanceCnt += stanceBuf[i];
    if (clearBuf[i] > maxClear) maxClear = clearBuf[i];
  }
  float rom = mx - mn;
  int steps = 0;                              // stance starts in chronological order
  uint8_t prev = stanceBuf[winIdx];
  for (int k = 1; k < WIN; k++) {
    uint8_t cur = stanceBuf[(winIdx + k) % WIN];
    if (cur && !prev) steps++;
    prev = cur;
  }
  if (steps < 1) return 0;                    // no step in 2 s: standing, not walking
  float stanceRatio = (float)stanceCnt / WIN;
  if (rom < WALK_ROM_MIN) return 0;           // not walking: no judgement

  int flags = 0;
  if (rom < ROM_LOW) flags++;
  if (stanceRatio > STANCE_HI || stanceRatio < STANCE_LO) flags++;
  if (!usFault && maxClear < stanceOffMm + CLEAR_LOW_ABOVE_OFF_MM) flags++;
  if (flags == 0) return 0;
  if (flags == 1) return 1;
  return 2;
}

void onNewClass(uint8_t c) {
  unsigned long now = millis();
  bool changed = (c != gaitClass);
  gaitClass = c;
  if (c == 2 && (changed || now - lastAlertMs >= REALERT_MS)) {
    startDoubleBeep();
    lastAlertMs = now;
  }
}

float wrap180(float a) {
  while (a > 180.0f) a -= 360.0f;
  while (a < -180.0f) a += 360.0f;
  return a;
}

// ---------------- Sampling at 50 Hz ----------------
Packet makePacket() {
  Packet p;
  p.t_ms = millis();
  p.angle_cdeg = (int16_t)(angle * 100.0f);
  p.gyro_dps10 = (int16_t)(g_gx * 10.0f);
  p.fsr = 0;                       // FSR removed; field kept so the packet format and database stay the same
  p.clear_mm = (uint16_t)lastClearMm;
  float am = sqrtf(g_ax * g_ax + g_ay * g_ay + g_az * g_az) * 1000.0f;
  p.accmag_mg = (uint16_t)am;
  p.gait = gaitClass;
  p.flags = (usFault ? FLAG_US_FAULT : 0) | (imuFault ? FLAG_IMU_FAULT : 0) |
            (stance ? FLAG_STANCE : 0);
  p.seq = seq++;
  return p;
}

void sampleOnce() {
  // --- IMU ---
  bool gotA = false, gotG = false;
  if (IMU.accelerationAvailable()) { IMU.readAcceleration(g_ax, g_ay, g_az); gotA = true; }
  if (IMU.gyroscopeAvailable())    { IMU.readGyroscope(g_gx, g_gy, g_gz);    gotG = true; }
  if (gotA && gotG) imuStale = 0; else imuStale++;
  float amag = sqrtf(g_ax * g_ax + g_ay * g_ay + g_az * g_az);
  imuFault = (imuStale > 5) || isnan(amag) || amag > 8.0f;

  // Angle: complementary filter. Axis choice depends on how you mount the board;
  // swap g_ay/g_az or flip the sign of g_gx if the angle moves the wrong way.
  float accAngle = wrap180(atan2f(g_ay, g_az) * 57.29578f - angleOffset);
  if (sampleCount == 0) angle = accAngle;
  if (!imuFault) angle = 0.98f * (angle + g_gx * DT) + 0.02f * accAngle;

  // --- Ultrasonic (about 16 Hz) ---
  if (sampleCount % US_EVERY_N == 0) {
    float d = readDistCm();
    if (d < 0) {
      usMiss++;
      if (usMiss >= 20) usFault = true;           // about 1.2 s with no valid echo
    } else {
      if (!groundValid) { groundRefCm = d; groundValid = true; }   // late calibration
      usMiss = 0; usFault = false;
      float c = (d - groundRefCm) * 10.0f;
      lastClearMm = c < 0 ? 0 : c;
      // Follow slow drift (strap slipping) only while the foot is firmly on the ground.
      if (stance && lastClearMm <= stanceOnMm) groundRefCm += DRIFT_RATE * (d - groundRefCm);
    }
  }
  if (usFault) lastClearMm = 0;
  if ((uint16_t)lastClearMm > maxClrSec) maxClrSec = (uint16_t)lastClearMm;

  // --- Stance detection: foot height from the ULTRASONIC sensor decides. ---
  bool newStance = stance;
  if (!usFault) {
    // Several missed echoes in a row usually mean the foot is lifted or tilted. One miss is ignored.
    bool lifted   = (usMiss >= LIFT_MISSES) || (lastClearMm >= stanceOffMm);
    bool onGround = (usMiss == 0) && (lastClearMm <= stanceOnMm);
    if (!stance && onGround) newStance = true;
    else if (stance && lifted) newStance = false;
  } else if (!imuFault) {
    // Ultrasonic is broken: fall back to the IMU (shin nearly still = foot on the ground).
    newStance = fabsf(g_gx) < 40.0f;
  }                                                // IMU and ultrasonic both broken: keep the last state
  if (newStance != stance && millis() - lastPhaseChangeMs >= MIN_PHASE_MS) {
    stance = newStance;
    lastPhaseChangeMs = millis();
    if (stance) {
      stepCount++;
#if STEP_TEST_MODE
      tickUntil = millis() + 20;                   // short non-blocking tick on every counted step
#endif
    }
  }

  // --- 2 s window, classify every 0.5 s ---
  angleBuf[winIdx] = angle;
  stanceBuf[winIdx] = stance ? 1 : 0;
  clearBuf[winIdx] = (uint16_t)lastClearMm;
  winIdx = (winIdx + 1) % WIN;
  if (winFilled < WIN) winFilled++;
  if (winFilled == WIN && sampleCount % 25 == 0) onNewClass(classifyWindow());

  // --- Telemetry ---
  Packet p = makePacket();
  if (BLE.connected()) {
    if (sampleCount % LIVE_EVERY_N == 0) txChar.writeValue((uint8_t *)&p, sizeof(p));
  } else if (sampleCount % 5 == 0) {
    bufPush(p);                                    // 10 Hz while offline
  }
  sampleCount++;
}

void flushBuffer() {            // replay buffered data after reconnect, 2 per loop
  for (int i = 0; i < 2; i++) {
    Packet p;
    if (!bufPop(p)) return;
    p.flags |= FLAG_REPLAY;
    txChar.writeValue((uint8_t *)&p, sizeof(p));
  }
}

void setup() {
  pinMode(PIN_TRIG, OUTPUT); pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_R, OUTPUT); pinMode(PIN_G, OUTPUT); pinMode(PIN_B, OUTPUT);
  setRGB(0, 0, 255);                               // blue = starting up

#if DEBUG_SERIAL
  Serial.begin(115200);
  while (!Serial && millis() < 2000) {}
#endif

  if (!IMU.begin()) imuFault = true;

  // Measure the foot-on-ground reference for this wearer (keep the foot flat and still).
  if (!calibrateGround()) usFault = true;          // blinks magenta; late calibration if the sensor recovers

  // Zero the knee angle: stand straight and still at power-on (about 1.5 s)
  setRGB(0, 0, 255);
  if (!imuFault) {
    float sy = 0, sz = 0; int n2 = 0; unsigned long t0 = millis();
    while (millis() - t0 < 1500) {
      float x, y, z;
      if (IMU.accelerationAvailable()) { IMU.readAcceleration(x, y, z); sy += y; sz += z; n2++; }
    }
    if (n2 > 0) angleOffset = atan2f(sy / n2, sz / n2) * 57.29578f;
  }

  if (!BLE.begin()) {
    bleFailed = true;                              // solid magenta
  } else {
    BLE.setLocalName("KneeTrack");
    BLE.setAdvertisedService(svc);
    svc.addCharacteristic(txChar);
    BLE.addService(svc);
    BLE.advertise();
  }

  stance = true;                                   // foot is on the ground now: no phantom step at boot
  lastPhaseChangeMs = millis();
  updateLed();
  Watchdog.enable(4000);                           // reset if loop hangs for 4 s
}

void loop() {
  static unsigned long tLast = micros();
  static unsigned long tPrint = 0;

  Watchdog.reset();
  BLE.poll();

  bool connected = BLE.connected();
  if (connected && !wasConnected) wasConnected = true;
  if (!connected && wasConnected) { wasConnected = false; BLE.advertise(); }
  if (connected) flushBuffer();

  unsigned long now = micros();
  if (now - tLast >= PERIOD_US) {
    tLast += PERIOD_US;
    if (now - tLast > 5 * PERIOD_US) tLast = now;  // fell far behind: resync
    sampleOnce();
  }
  updateBuzzer();
  updateLed();

#if DEBUG_SERIAL
  if (millis() - tPrint >= 1000) {
    tPrint = millis();
    Serial.print("angle="); Serial.print(angle, 1);
    Serial.print(" clr(mm)="); Serial.print((int)lastClearMm);
    Serial.print(" max1s="); Serial.print(maxClrSec); maxClrSec = 0;
    Serial.print(" ref(cm)="); Serial.print(groundRefCm, 1);
    Serial.print(" on/off="); Serial.print((int)stanceOnMm); Serial.print("/"); Serial.print((int)stanceOffMm);
    Serial.print(" stance="); Serial.print(stance);
    Serial.print(" steps="); Serial.print(stepCount);
    Serial.print(" gait="); Serial.print(gaitClass);
    Serial.print(" faults(U/I)="); Serial.print(usFault); Serial.print(imuFault);
    Serial.print(" ble="); Serial.print(connected);
    Serial.print(" buf="); Serial.println(bufCount);
  }
#endif
}
