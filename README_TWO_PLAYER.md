# Two Player Impact Detection Game

This system allows two players to compete by hitting their respective boards, with the system determining who hit first. Player 1 acts as the host with a web interface, while Player 2 connects via ESP-NOW.

## Hardware Setup

### Required Components
- 2x ESP32 development boards
- 2x sets of 4 piezoelectric sensors (or similar impact sensors)
- 2x impact boards (wood, metal, or other rigid material)
- Wiring and mounting hardware

### Sensor Connections
Both boards use the same sensor pin configuration:
- **Sensor 0 (Top)**: GPIO 35
- **Sensor 1 (Bottom)**: GPIO 33  
- **Sensor 2 (Right)**: GPIO 34
- **Sensor 3 (Left)**: GPIO 32
- **LED Indicator**: GPIO 2

## Software Setup

### 1. Get MAC Addresses
First, you need to find the MAC addresses of both ESP32 boards:

1. Upload this simple sketch to each board:
```cpp
#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  Serial.print("MAC Address: ");
  Serial.println(WiFi.macAddress());
}

void loop() {
  delay(1000);
}
```

2. Note the MAC addresses from the Serial Monitor for both boards.

### 2. Configure MAC Addresses
Update the MAC addresses in both files:

**In `player1_host.ino`:**
```cpp
// MAC of Player 2 (replace with your actual address)
uint8_t player2Address[] = { 0x78, 0x1C, 0x3C, 0xB8, 0xD5, 0xA9 };
```

**In `player2_client.ino`:**
```cpp
// MAC of Player 1 (replace with your actual address)
uint8_t player1Address[] = { 0x78, 0x1C, 0x3C, 0xB8, 0xD5, 0xA9 };
```

Replace the example MAC addresses with the actual addresses from your boards.

### 3. Upload Code
1. Upload `player1_host.ino` to the board that will be Player 1 (the host)
2. Upload `player2_client.ino` to the board that will be Player 2 (the client)

## How It Works

### Player 1 (Host)
- Creates a WiFi access point named "ToolBoard" with password "12345678"
- Provides a web interface at the board's IP address (typically 192.168.4.1)
- Detects impacts using Time Difference of Arrival (TDoA) localization
- Communicates with Player 2 via ESP-NOW
- Determines the winner and displays results

### Player 2 (Client)
- Connects to Player 1 via ESP-NOW
- Detects impacts using the same TDoA system
- Sends hit data to Player 1
- Receives game state updates

### Game Flow
1. Both boards power up and establish ESP-NOW connection
2. Player 1's web interface shows "Game Active" when Player 2 connects
3. Players hit their respective boards
4. The system determines who hit first based on microsecond timestamps
5. Results are displayed on Player 1's web interface
6. Use the "Reset Game" button to start a new round

## Web Interface

Access the web interface by:
1. Connecting to the "ToolBoard" WiFi network (password: 12345678)
2. Opening a web browser and navigating to `192.168.4.1`

### Interface Features
- **Connection Status**: Shows if Player 2 is connected
- **Game Status**: Displays current game state
- **Winner Display**: Shows who won the round
- **Reset Button**: Starts a new round
- **Hit Visualization**: Shows where impacts occurred on the board
- **Sensor Data**: Real-time sensor timing information

## Troubleshooting

### Connection Issues
- Ensure both boards are powered on
- Check that MAC addresses are correctly configured
- Verify both boards are in range for ESP-NOW communication
- LED indicators show connection status (solid = connected, blinking/off = disconnected)

### Impact Detection Issues
- Verify sensor connections are secure
- Check that sensors are properly mounted to the board
- Adjust `V_SOUND` value if impact localization is inaccurate
- Ensure sensors are not too sensitive (adjust thresholds if needed)

### Web Interface Issues
- Make sure you're connected to the "ToolBoard" WiFi network
- Try refreshing the page if the interface doesn't update
- Check that the ESP32 is powered and running

## Configuration Options

### Timing Parameters
```cpp
static const unsigned long CAPTURE_WINDOW_US = 8000;  // Impact detection window
static const unsigned long DEADTIME_MS = 120;         // Time between detections
```

### Sound Speed
```cpp
static float V_SOUND = 3000.0f; // m/s - adjust based on your board material
```

### Sensor Positions
```cpp
static const float SX[SENSOR_COUNT] = {0.200f, 0.200f, 0.300f, 0.100f};
static const float SY[SENSOR_COUNT] = {0.100f, 0.300f, 0.200f, 0.200f};
```

## Advanced Features

### TDoA Localization
The system uses Time Difference of Arrival (TDoA) to determine impact location:
- Measures arrival times at 4 sensors
- Uses mathematical optimization to find impact coordinates
- Provides fallback methods for partial detections

### Real-time Communication
- ESP-NOW provides low-latency communication between boards
- Heartbeat system ensures connection monitoring
- Automatic reconnection handling

### Persistent Calibration
- Wave speed calibration is saved to flash memory
- Survives power cycles and resets

## Safety Notes

- Ensure proper mounting of impact boards to prevent injury
- Use appropriate materials for your intended impact force
- Consider adding protective covers or barriers if needed
- Test thoroughly before use in competitive environments

## Future Enhancements

Potential improvements could include:
- Multiple round scoring
- Impact strength measurement
- Sound effects or visual feedback
- Mobile app interface
- Tournament mode with multiple players
- Impact pattern analysis
