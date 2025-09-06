# Lightboard ESP-NOW Module

This document describes the new lightboard system that connects via ESP-NOW to the Player 1 host, replacing the standalone web server approach.

## Architecture Overview

The system now consists of three ESP32 modules:

1. **Player 1 (Host)** - `player1_host_sync.ino`
   - Web server with quiz interface
   - ESP-NOW communication with Player 2 and Lightboard
   - Game state management and winner determination

2. **Player 2 (Client)** - `player2_client_sync.ino`
   - Impact detection and ESP-NOW communication
   - Sends hit data to Player 1

3. **Lightboard (Display)** - `lightboard_espnow.cpp`
   - LED strip control and visual effects
   - ESP-NOW communication with Player 1
   - Receives game state updates and displays scores

## ESP-NOW Communication Protocol

### Message Structure

The system uses two different message structures:

#### Player-to-Player Messages (struct_message)
```cpp
typedef struct struct_message {
  uint8_t  playerId;   // 1=Player1, 2=Player2
  uint8_t  action;     // 1=heartbeat, 2=hit-detected, 3=reset-request, 4=clock-sync
  uint32_t hitTime;    // micros() timestamp of hit
  uint16_t hitStrength; // impact strength
  uint32_t syncTime;   // for clock synchronization
  uint32_t roundTripTime; // for latency measurement
} struct_message;
```

#### Lightboard Messages (struct_lightboard_message)
```cpp
typedef struct struct_lightboard_message {
  uint8_t  deviceId;     // 1=Player1, 3=Lightboard
  uint8_t  action;       // 1=heartbeat, 2=game-state, 3=score-update, 4=mode-change, 5=reset
  uint8_t  gameMode;     // 1-6 (Territory, Swap Sides, Split Scoring, Score Order, Race, Tug O War)
  uint8_t  p1ColorIndex; // Player 1 color index
  uint8_t  p2ColorIndex; // Player 2 color index
  int8_t   p1Pos;        // Player 1 position (-1 to NUM_LEDS)
  int8_t   p2Pos;        // Player 2 position (-1 to NUM_LEDS)
  uint8_t  nextLedPos;   // For Score Order mode
  uint8_t  tugBoundary;  // For Tug O War mode
  uint8_t  p1RacePos;    // For Race mode
  uint8_t  p2RacePos;    // For Race mode
  uint8_t  celebrating;  // Celebration state
  uint8_t  winner;       // 0=none, 1=Player1, 2=Player2
} struct_lightboard_message;
```

### Action Types

#### Player Messages
- `1` - Heartbeat (connection keep-alive)
- `2` - Hit detected (with timestamp and strength)
- `3` - Reset request
- `4` - Clock synchronization

#### Lightboard Messages
- `1` - Heartbeat (connection keep-alive)
- `2` - Game state update (full state sync)
- `3` - Score update (incremental update)
- `4` - Mode change (new game mode selected)
- `5` - Reset (clear all state)

## Game Modes

The lightboard supports 6 different game modes:

1. **Territory** - Players expand from opposite ends
2. **Swap Sides** - Players can jump over each other
3. **Split Scoring** - Players expand from center outward
4. **Score Order** - Sequential LED filling based on scoring order
5. **Race** - Players race to the end
6. **Tug O War** - Boundary line moves based on player actions

## Setup Instructions

### Hardware Requirements

- 3x ESP32 development boards
- LED strip (WS2812B/NeoPixel) with 38 LEDs
- Impact sensors (piezo or similar) for Player 1 and Player 2
- Power supply for LED strip

### Software Setup

1. **Player 1 (Host)**
   - Upload `player1_host_sync.ino`
   - Connect to WiFi AP "ToolBoard" with password "12345678"
   - Access web interface at the displayed IP address

2. **Player 2 (Client)**
   - Upload `player2_client_sync.ino`
   - No WiFi configuration needed (ESP-NOW only)

3. **Lightboard**
   - Upload `lightboard_espnow.cpp`
   - Connect LED strip to GPIO 13
   - No WiFi configuration needed (ESP-NOW only)

### MAC Address Configuration

Update the MAC addresses in the code:

**Player 1 (player1_host_sync.ino):**
```cpp
uint8_t player2Address[] = {0x6C, 0xC8, 0x40, 0x4E, 0xEC, 0x2C}; // Player 2 STA MAC
uint8_t lightboardAddress[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; // Placeholder
```

**Player 2 (player2_client_sync.ino):**
```cpp
uint8_t player1Address[] = {0x78, 0x1C, 0x3C, 0xB8, 0xD5, 0xA8}; // Player 1 STA MAC
```

**Lightboard (lightboard_espnow.cpp):**
```cpp
uint8_t player1Address[] = {0x78, 0x1C, 0x3C, 0xB8, 0xD5, 0xA8}; // Player 1 STA MAC
```

## Features

### Web Interface
- Quiz mode with CSV file upload
- Lightboard game mode selection
- Real-time connection status for Player 2 and Lightboard
- Player scoring and name editing

### Lightboard Display
- Real-time game state visualization
- Celebration animations for winners
- Demo mode when not connected
- Connection status indication

### ESP-NOW Communication
- Automatic MAC address discovery
- Heartbeat monitoring with timeout detection
- Clock synchronization between players
- Reliable message delivery

## Usage

1. **Start the system:**
   - Power on Player 1 first (creates WiFi AP)
   - Power on Player 2 and Lightboard
   - Wait for ESP-NOW connections to establish

2. **Configure game mode:**
   - Access Player 1 web interface
   - Select lightboard game mode from dropdown
   - Mode change is sent to lightboard immediately

3. **Play the game:**
   - Players hit their respective boards
   - Winner is determined by timing
   - Lightboard displays celebration animation
   - Game resets automatically

4. **Quiz mode:**
   - Upload CSV files with questions
   - Select categories
   - Use toolboard hits to determine quiz winners
   - Scores are tracked and displayed

## Troubleshooting

### Connection Issues
- Check MAC addresses are correct
- Ensure all devices are on same WiFi channel
- Monitor serial output for connection status

### Lightboard Issues
- Verify LED strip connection to GPIO 13
- Check power supply for LED strip
- Monitor serial output for game state updates

### Game Issues
- Verify impact sensors are working
- Check timing synchronization
- Monitor serial output for hit detection

## Demo Mode

When the lightboard is not connected to Player 1, it runs a demo mode that:
- Cycles through all 6 game modes every 60 seconds
- Simulates game progress for each mode
- Shows different LED patterns and animations
- Demonstrates the visual capabilities

## Future Enhancements

- Additional game modes
- Sound effects synchronization
- Multiple lightboard support
- Tournament mode with multiple rounds
- Custom color schemes
- Performance metrics and statistics
