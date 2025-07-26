// sensors.ino
// Shared sensor routines for ESP32 Tool Target Game
// Uses the ESP32's touchRead() API with baseline calibration,
// exponential smoothing, and delta-from-baseline reporting.
// Optimized for 32mm piezo vibration sensor with peak detection.

#include <Arduino.h>
#include <esp32-hal-adc.h>   // for analogSetPinAttenuation()

// ── Pin definitions ────────────────────────────────────────────
const int touchPins[5] = { 32, 33, 27, 14, 12 };  // must be touch-capable GPIOs
const int shockPin     = 39;                     // analogRead() for piezo
// ────────────────────────────────────────────────────────────────

// ── Smoothed output values ─────────────────────────────────────
static int   capacitiveValues[5] = {0};
static int   shockValue          = 0;
// ────────────────────────────────────────────────────────────────

// ── Internal state for smoothing & calibration ─────────────────
static float    touchSmooth[5];
static float    shockSmooth;
static uint16_t baselineTouch[5];
// ────────────────────────────────────────────────────────────────

// ── Piezo-specific variables ───────────────────────────────────
static int    shockBaseline = 0;
static int    shockPeak     = 0;
static int    shockThreshold = 50;  // Adjustable threshold for piezo sensitivity
static unsigned long lastPeakTime = 0;
static const unsigned long peakDecayTime = 100; // ms for peak decay
// ────────────────────────────────────────────────────────────────

// ── Config constants ────────────────────────────────────────────
const float α             = 0.1f;  // smoothing factor (0 < α < 1)
const int   CALIB_SAMPLES = 100;   // samples for baseline
const int   CALIB_DELAY   =   5;   // ms between baseline samples
// ────────────────────────────────────────────────────────────────

void sensorsInit() {
  // 1) Set ADC attenuation to 11dB (full-range) on all pins
  for (int i = 0; i < 5; i++) {
    analogSetPinAttenuation(touchPins[i], ADC_11db);
  }
  analogSetPinAttenuation(shockPin, ADC_11db);

  // 2) Baseline-calibrate each touch channel
  for (int i = 0; i < 5; i++) {
    uint32_t sum = 0;
    for (int j = 0; j < CALIB_SAMPLES; j++) {
      sum += touchRead(touchPins[i]);
      delay(CALIB_DELAY);
    }
    baselineTouch[i]    = sum / CALIB_SAMPLES;
    touchSmooth[i]      = baselineTouch[i];
    capacitiveValues[i] = 0;  // start at zero delta
  }

  // 3) Initialize piezo shock sensor with baseline calibration
  uint32_t shockSum = 0;
  for (int j = 0; j < CALIB_SAMPLES; j++) {
    shockSum += analogRead(shockPin);
    delay(CALIB_DELAY);
  }
  shockBaseline = shockSum / CALIB_SAMPLES;
  shockSmooth   = shockBaseline;
  shockValue    = 0;
  shockPeak     = 0;
  
  Serial.println("Piezo sensor calibrated - Baseline: " + String(shockBaseline));
}

void sensorsLoop() {
  // — Touch channels —
  for (int i = 0; i < 5; i++) {
    float raw = touchRead(touchPins[i]);
    touchSmooth[i] = α * raw + (1.0f - α) * touchSmooth[i];
    float delta = float(baselineTouch[i]) - touchSmooth[i];
    capacitiveValues[i] = delta > 0 ? int(delta) : 0;
  }

  // — Piezo shock channel with peak detection —
  int rawShock = analogRead(shockPin);
  int shockDelta = abs(rawShock - shockBaseline);
  
  // Update smoothed value
  shockSmooth = α * rawShock + (1.0f - α) * shockSmooth;
  
  // Peak detection for piezo
  if (shockDelta > shockThreshold) {
    if (shockDelta > shockPeak) {
      shockPeak = shockDelta;
      lastPeakTime = millis();
    }
  }
  
  // Decay peak over time
  if (millis() - lastPeakTime > peakDecayTime) {
    shockPeak = shockPeak * 0.9; // Gradual decay
  }
  
  // Output the peak value for responsive vibration detection
  shockValue = shockPeak;
}

int getCapacitiveValue(int index) {
  if (index < 0 || index >= 5) return -1;
  return capacitiveValues[index];
}

int getShockValue() {
  return shockValue;
}

// Optional: Function to adjust piezo sensitivity
void setShockThreshold(int threshold) {
  shockThreshold = threshold;
}
