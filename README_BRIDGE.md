# ESP32 Bridge Architecture

This project now uses a **Raspberry Pi + ESP32 Bridge** architecture instead of running everything on the ESP32.

## Architecture Overview

```
┌─────────────────┐    WebSocket    ┌─────────────────┐    Serial    ┌─────────────────┐
│   Web Browser   │◄──────────────►│  Raspberry Pi   │◄────────────►│   ESP32 Bridge  │
│                 │                 │  Web Server     │               │   (Player 1)    │
└─────────────────┘                 └─────────────────┘               └─────────────────┘
                                                                              │
                                                                              │ ESP-NOW
                                                                              ▼
                                                                     ┌─────────────────┐
                                                                     │   Player 2      │
                                                                     │   ESP32         │
                                                                     └─────────────────┘
                                                                              │
                                                                              │ ESP-NOW
                                                                              ▼
                                                                     ┌─────────────────┐
                                                                     │   Player 3      │
                                                                     │   ESP32         │
                                                                     └─────────────────┘
                                                                              │
                                                                              │ ESP-NOW
                                                                              ▼
                                                                     ┌─────────────────┐
                                                                     │   Lightboard    │
                                                                     │   ESP32         │
                                                                     └─────────────────┘
```

## Benefits

- **More Processing Power**: Raspberry Pi handles complex web interface
- **Better Scalability**: Easy to add more features on Pi side
- **Cleaner Separation**: ESP32 focuses on real-time communication
- **Easier Development**: Web interface can be developed independently
- **Better Performance**: No more massive HTML embedded in ESP32 code

## Files Created

### Web Interface Files
- `index.html` - Main web interface
- `styles.css` - Styling for the web interface
- `script.js` - JavaScript logic for quiz functionality

### Bridge Server
- `server.js` - Node.js web server that bridges web interface and ESP32
- `package.json` - Node.js dependencies
- `README_NODEJS.md` - Node.js setup and usage documentation

### ESP32 Code
- `Player1_Bridge.ino` - ESP32 code that acts as a pure communication bridge

## Setup Instructions

### 1. On Raspberry Pi

1. **Install Node.js** (version 14 or higher):
   ```bash
   curl -fsSL https://deb.nodesource.com/setup_18.x | sudo -E bash -
   sudo apt-get install -y nodejs
   ```

2. **Install dependencies**:
   ```bash
   npm install
   ```

3. **Connect ESP32** to Raspberry Pi via USB (usually `/dev/ttyUSB0`)

4. **Start the bridge server**:
   ```bash
   npm start
   ```

### 2. On ESP32

1. **Upload `Player1_Bridge.ino`** to your ESP32
2. **Connect ESP32** to Raspberry Pi via USB serial
3. **ESP32 will automatically**:
   - Create WiFi AP named "ESP32Bridge"
   - Listen for serial commands from Pi
   - Communicate with Player 2, Player 3, and Lightboard via ESP-NOW

### 3. Access Web Interface

- **Open browser** and go to `http://localhost:80` (or your Pi's IP address)
- **Web interface** provides the same functionality as before:
  - Load CSV quiz files
  - Configure lightboard settings
  - Run quizzes
  - Award points to players
  - Control game flow

## Communication Protocol

### Web Interface ↔ Raspberry Pi
- **WebSocket** connection on port 80
- **Commands from Web to Pi**:
  ```json
  {"cmd":"heartbeat"}
  {"cmd":"reset"}
  {"cmd":"awardPoint","player":2,"multiplier":3}
  {"cmd":"lightboardSettings","mode":1,"p2Color":0,"p3Color":1}
  {"cmd":"quizAction","action":"next"}
  ```

### Raspberry Pi ↔ ESP32 Bridge
- **Serial** communication (USB)
- **Messages from Pi to ESP32**:
  ```json
  {"cmd":"heartbeat"}
  {"cmd":"reset"}
  {"cmd":"awardPoint","player":2,"multiplier":3}
  {"cmd":"lightboardSettings","mode":1,"p2Color":0,"p3Color":1}
  {"cmd":"quizAction","action":"next"}
  ```

### ESP32 Bridge ↔ Other ESP32s
- **ESP-NOW** communication (unchanged)
- **Messages from ESP32 Bridge to Players**:
  ```json
  {"playerId":1,"action":1,"hitTime":0,"hitStrength":0,"syncTime":0,"roundTripTime":0}
  ```

## Troubleshooting

### ESP32 Not Connecting
- Check serial port: `ls /dev/ttyUSB*`
- Run manually: `node server.js --serial-port /dev/ttyUSB1`
- Check ESP32 serial output for errors

### Web Interface Not Loading
- Check if server is running: `ps aux | grep node`
- Check logs in terminal where you started the server
- Try accessing: `http://localhost:80`

### ESP-NOW Communication Issues
- Check ESP32 serial output for connection status
- Verify MAC addresses in `Player1_Bridge.ino`
- Check WiFi channel consistency

## Development

### Adding New Features
1. **Web Interface**: Modify `script.js` and `index.html`
2. **Bridge Logic**: Modify `server.js`
3. **ESP32 Communication**: Modify `Player1_Bridge.ino`

### Testing
- **Web Interface**: Test in browser with `http://localhost:80`
- **Serial Communication**: Monitor terminal output where server is running
- **ESP32**: Monitor serial output in Arduino IDE

## File Structure

```
project/
├── index.html              # Web interface
├── styles.css              # Web styling
├── script.js               # Web JavaScript
├── server.js              # Node.js bridge server
├── package.json            # Node.js dependencies
├── README_NODEJS.md        # Node.js documentation
├── Player1_Bridge.ino     # ESP32 bridge code
├── Player2.ino            # Player 2 ESP32 code
├── Player3.ino            # Player 3 ESP32 code
├── Lightboard.cpp         # Lightboard ESP32 code
└── README.md              # This file
```

## Migration from Old Architecture

The new architecture maintains **100% compatibility** with existing:
- Player 2 and Player 3 ESP32 code
- Lightboard ESP32 code
- Quiz CSV files
- Game modes and settings

Only Player 1 has changed from a web server to a pure bridge.
