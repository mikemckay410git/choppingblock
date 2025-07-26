# Piezo Vibration Sensor Optimization

## Overview
The shock sensor has been optimized specifically for a 32mm piezo vibration sensor with enhanced signal processing and peak detection.

## Key Features

### 1. Baseline Calibration
- **Automatic calibration** during startup
- **100 samples** averaged for stable baseline
- **Adaptive threshold** based on ambient conditions

### 2. Peak Detection
- **Real-time peak detection** for vibration events
- **Threshold-based triggering** (default: 50 units)
- **Peak decay** over 100ms for natural response

### 3. Signal Processing
- **Delta calculation** from baseline
- **Peak tracking** for maximum impact detection
- **Gradual decay** for realistic vibration response

## Technical Implementation

### Pin Configuration
```cpp
const int shockPin = 39;  // Analog pin for piezo sensor
```

### Key Variables
```cpp
static int shockBaseline = 0;      // Calibrated baseline
static int shockPeak = 0;          // Current peak value
static int shockThreshold = 50;    // Trigger threshold
static unsigned long lastPeakTime = 0;  // Peak timing
```

### Signal Processing Flow
1. **Read raw analog value** from piezo
2. **Calculate delta** from baseline
3. **Check threshold** for triggering
4. **Update peak** if higher than current
5. **Decay peak** over time
6. **Output peak value** for responsive detection

## Usage

### Wiring
- **Red wire** → 3.3V or 5V (check piezo specifications)
- **Black wire** → GPIO 39 (analog input)
- **Optional:** Add 1MΩ resistor across piezo for better signal

### Sensitivity Adjustment
```cpp
setShockThreshold(30);  // More sensitive
setShockThreshold(100); // Less sensitive
```

## Expected Behavior

### Normal Operation
- **Baseline:** ~0-20 units (ambient)
- **Light tap:** 50-200 units
- **Medium impact:** 200-500 units
- **Hard impact:** 500+ units

### Response Characteristics
- **Trigger threshold:** 50 units (adjustable)
- **Peak hold time:** 100ms
- **Decay rate:** 10% per 100ms
- **Update rate:** ~13 Hz (matches system)

## Troubleshooting

### Low Sensitivity
1. Check wiring connections
2. Verify power supply voltage
3. Reduce threshold: `setShockThreshold(30)`
4. Check piezo mounting (should be firmly attached)

### High Noise
1. Increase threshold: `setShockThreshold(100)`
2. Check for electrical interference
3. Verify proper grounding
4. Consider adding capacitor across piezo

### No Response
1. Verify pin assignment (GPIO 39)
2. Check power supply
3. Test piezo with multimeter
4. Verify baseline calibration in serial output

## Integration Notes

- **Compatible** with existing Player1/Player2 code
- **No changes** required to main game logic
- **Automatic calibration** on startup
- **Real-time updates** via WebSocket interface

The piezo sensor is now optimized for responsive vibration detection in your tool target game! 