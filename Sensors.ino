// sensors.ino
// Shared sensor routines for ESP32 Tool Target Game
// Uses the ESP32's touchRead() API with baseline calibration,
// exponential smoothing, and delta-from-baseline reporting.
// Also smooths the shock sensor via analogRead().

#include <Arduino.h>
#include <esp32-hal-adc.h>   // for analogSetPinAttenuation()

// ── Pin definitions ────────────────────────────────────────────
const int touchPins[5] = { 32, 33, 27, 14, 12 };  // must be touch-capable GPIOs
const int shockPin     = 39;                     // analogRead()
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

// ── Config constants ────────────────────────────────────────────
const float α             = 0.05f; // More sensitive smoothing (was 0.1f)
const int   CALIB_SAMPLES = 200;   // More samples for better baseline
const int   CALIB_DELAY   =   2;   // Faster sampling

// Note: Thresholds are now handled in Player1.ino web interface
// Sensors just output raw amplified values
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

  // 3) Initialize shock smoothing
  shockSmooth = analogRead(shockPin);
  shockValue  = int(shockSmooth);
}

void sensorsLoop() {
  // — Touch channels —
  for (int i = 0; i < 5; i++) {
    float raw = touchRead(touchPins[i]);
    touchSmooth[i] = α * raw + (1.0f - α) * touchSmooth[i];
    float delta = float(baselineTouch[i]) - touchSmooth[i];
    
    // Always show the raw delta value (amplified)
    capacitiveValues[i] = int(delta * 2); // Amplify the signal
  }

  // — Shock channel —
  int rawShock = analogRead(shockPin);
  shockSmooth  = α * rawShock + (1.0f - α) * shockSmooth;
  shockValue   = int(shockSmooth);
}

int getCapacitiveValue(int index) {
  if (index < 0 || index >= 5) return -1;
  return capacitiveValues[index];
}

int getShockValue() {
  return shockValue;
}
