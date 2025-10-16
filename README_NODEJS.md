# ESP32 Bridge - Node.js Implementation

A Node.js web server that bridges communication between web interfaces and ESP32 devices via serial communication and WebSockets.

## Features

- **Serial Communication**: Bidirectional communication with ESP32 via serial port
- **WebSocket Server**: Real-time communication with web clients
- **Static File Server**: Serves web interface files
- **Auto-reconnection**: Automatically reconnects to ESP32 if connection is lost
- **Cross-platform**: Works on Windows, macOS, and Linux
- **Health Check**: Built-in health endpoint for monitoring

## Installation

1. **Install Node.js** (version 14 or higher)
   - Download from [nodejs.org](https://nodejs.org/)

2. **Install dependencies**:
   ```bash
   npm install
   ```

## Usage

### Basic Usage
```bash
npm start
```

### With Custom Serial Port
```bash
node server.js --serial-port COM3 --baud-rate 115200
```

### Development Mode (with auto-restart)
```bash
npm run dev
```

## Command Line Options

- `--serial-port <port>`: Serial port for ESP32 communication (default: `/dev/ttyUSB0`)
- `--baud-rate <rate>`: Serial baud rate (default: 115200)
- `--help`: Show help message

## Ports

- **HTTP Server**: `http://localhost:3000` (serves static files)
- **WebSocket Server**: `ws://localhost:8080` (real-time communication)
- **Health Check**: `http://localhost:3000/health`

## API

### WebSocket Messages

**To ESP32**: Send JSON commands via WebSocket
```javascript
const ws = new WebSocket('ws://localhost:8080');
ws.send(JSON.stringify({ command: 'led_on', pin: 13 }));
```

**From ESP32**: Receive JSON data via WebSocket
```javascript
ws.onmessage = (event) => {
    const data = JSON.parse(event.data);
    console.log('Received from ESP32:', data);
};
```

### Health Check Endpoint

```bash
curl http://localhost:3000/health
```

Response:
```json
{
    "status": "ok",
    "serialConnected": true,
    "websocketClients": 2
}
```

## ESP32 Communication

The bridge expects JSON messages from the ESP32:

```cpp
// ESP32 Arduino code example
void sendData() {
    DynamicJsonDocument doc(1024);
    doc["sensor"] = "temperature";
    doc["value"] = 25.5;
    doc["timestamp"] = millis();
    
    serializeJson(doc, Serial);
    Serial.println();
}
```

## Troubleshooting

### Serial Port Issues

1. **Find available ports**:
   ```bash
   # Windows
   node server.js --help
   
   # Linux/macOS
   ls /dev/tty*
   ```

2. **Permission issues** (Linux/macOS):
   ```bash
   sudo chmod 666 /dev/ttyUSB0
   ```

3. **Port already in use**:
   - Close other applications using the serial port
   - Unplug and reconnect the ESP32

### WebSocket Connection Issues

1. **Check if server is running**:
   ```bash
   curl http://localhost:3000/health
   ```

2. **Test WebSocket connection**:
   ```javascript
   const ws = new WebSocket('ws://localhost:8080');
   ws.onopen = () => console.log('Connected');
   ws.onerror = (error) => console.error('Error:', error);
   ```

## Development

### Project Structure
```
├── server.js            # Main bridge server
├── package.json         # Node.js dependencies
├── index.html          # Web interface
├── script.js           # Frontend JavaScript
├── styles.css          # Frontend styles
└── README.md           # This file
```

### Dependencies
- `express`: HTTP server and static file serving
- `ws`: WebSocket server implementation
- `serialport`: Serial communication with ESP32
- `cors`: Cross-origin resource sharing
- `nodemon`: Development auto-restart (dev dependency)

## Migration from Python

This Node.js implementation provides equivalent functionality to the Python version:

| Feature | Python | Node.js |
|---------|--------|---------|
| Serial Communication | `pyserial` | `serialport` |
| WebSocket Server | `websockets` | `ws` |
| HTTP Server | `http.server` | `express` |
| Async Operations | `asyncio` | Native Promises/async-await |
| Command Line Args | `argparse` | Custom parsing |

## License

MIT License
