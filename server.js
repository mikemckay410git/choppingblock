import express from "express";
import http from "http";
import { Server } from "socket.io";
import cors from "cors";
import { SerialPort } from "serialport";
import { ReadlineParser } from "@serialport/parser-readline";

const app = express();
const server = http.createServer(app);
const io = new Server(server, {
  cors: {
    origin: "*",
    methods: ["GET", "POST"]
  }
});

app.use(cors());
app.use(express.static(".")); // serve files from home directory

// ESP32 Serial Communication
class ESP32Bridge {
  constructor(serialPort = '/dev/ttyUSB0', baudRate = 115200) {
    this.serialPort = serialPort;
    this.baudRate = baudRate;
    this.serialConnection = null;
    this.io = io;
    this.enabled = false; // Track if serial is enabled
    // Debounce for server-side awarding
    this.lastAwardAtMs = 0;
    this.lastAwardPlayer = null;
  }

  async startSerialCommunication() {
    try {
      // List available ports for debugging
      const ports = await SerialPort.list();
      console.log('Available serial ports:', ports.map(p => p.path));
      
      // Check if the specified port exists
      const portExists = ports.some(port => port.path === this.serialPort);
      if (!portExists) {
        console.log(`Serial port ${this.serialPort} not found. ESP32 communication disabled.`);
        console.log('Server will run without ESP32 connection.');
        this.io.emit('esp32_status', { connected: false, enabled: false });
        return;
      }
      
      this.enabled = true;
      this.serialConnection = new SerialPort({
        path: this.serialPort,
        baudRate: this.baudRate,
        autoOpen: false
      });

      const parser = this.serialConnection.pipe(new ReadlineParser({ delimiter: '\n' }));
      
      this.serialConnection.on('open', () => {
        console.log(`Connected to ESP32 on ${this.serialPort}`);
        // Notify all clients that ESP32 is connected
        this.io.emit('esp32_status', { connected: true, enabled: true });
      });

      this.serialConnection.on('error', (err) => {
        console.warn(`Serial connection error: ${err.message}`);
        this.serialConnection = null;
        this.enabled = false;
        // Notify all clients that ESP32 is disconnected
        this.io.emit('esp32_status', { connected: false, enabled: false });
        // Retry connection after 5 seconds (longer delay for optional connection)
        setTimeout(() => this.startSerialCommunication(), 5000);
      });

      parser.on('data', (data) => {
        this.handleSerialMessage(data.trim());
      });

      // Open the connection
      await new Promise((resolve, reject) => {
        this.serialConnection.open((err) => {
          if (err) reject(err);
          else resolve();
        });
      });

    } catch (error) {
      console.warn(`Failed to connect to serial port ${this.serialPort}:`, error.message);
      console.log('Server will continue running without ESP32 connection.');
      this.enabled = false;
      // Notify all clients that ESP32 is disconnected
      this.io.emit('esp32_status', { connected: false, enabled: false });
      // Retry connection after 5 seconds (longer delay for optional connection)
      setTimeout(() => this.startSerialCommunication(), 5000);
    }
  }

  handleSerialMessage(message) {
    try {
      // Parse JSON message from ESP32
      const data = JSON.parse(message);
      console.log('Received from ESP32:', data);
      
      // Forward to all Socket.IO clients
      this.io.emit('esp32_data', data);
      
    } catch (error) {
      // Handle non-JSON messages (like debug output)
      console.log('Non-JSON message from ESP32:', message);

      // Parse common hit/winner lines and emit structured events
      // Examples:
      // "Player 2 hit detected at 1913756204 (adjusted from 2166662304) with strength 2"
      // "Winner declared: Player 2"
      const hitMatch = message.match(/^Player\s+(2|3)\s+hit detected.*strength\s+(\d+)/i);
      if (hitMatch) {
        const playerNum = parseInt(hitMatch[1], 10);
        const strength = parseInt(hitMatch[2], 10);
        this.io.emit('esp32_data', { type: 'hit', player: playerNum, strength });

        // Auto-award on hit to ensure lightboard updates even without frontend action
        const now = Date.now();
        if (this.lastAwardPlayer !== playerNum || (now - this.lastAwardAtMs) > 250) {
          const cmd = { cmd: 'awardPoint', player: playerNum, multiplier: 1 };
          this.sendToESP32(cmd);
          console.log('Auto-award on hit -> ESP32', cmd);
          this.lastAwardPlayer = playerNum;
          this.lastAwardAtMs = now;
        }
      }

      const winnerMatch = message.match(/^Winner declared:\s*(Player\s*[23]|Tie)/i);
      if (winnerMatch) {
        const winner = winnerMatch[1].replace(/\s+/, ' ');
        this.io.emit('esp32_data', { type: 'winner', winner });
      }
      
      // Forward important status messages to frontend
      if (message.includes('heartbeat') || 
          message.includes('Clock sync') || 
          message.includes('lightboard') ||
          message.includes('Player') ||
          message.includes('Status')) {
        
        // Send status message to frontend
        this.io.emit('esp32_status_message', {
          type: 'status',
          message: message,
          timestamp: new Date().toISOString()
        });
      }
    }
  }

  sendToESP32(command) {
    if (!this.enabled || !this.serialConnection || !this.serialConnection.isOpen) {
      console.warn('ESP32 serial connection not available - command ignored:', command);
      return;
    }
    
    try {
      const message = JSON.stringify(command) + '\n';
      this.serialConnection.write(message);
      console.log('Sent to ESP32:', command);
      // Surface to clients for debugging
      this.io.emit('esp32_status_message', {
        type: 'status',
        message: `Sent to ESP32: ${JSON.stringify(command)}`,
        timestamp: new Date().toISOString()
      });
    } catch (error) {
      console.error('Error sending to ESP32:', error);
    }
  }

  stop() {
    if (this.serialConnection && this.serialConnection.isOpen) {
      this.serialConnection.close();
    }
    console.log('ESP32 Bridge stopped');
  }
}

// Create ESP32 bridge instance
const esp32Bridge = new ESP32Bridge('/dev/ttyUSB0', 115200);

// Example endpoint
app.get("/api", (req, res) => {
  res.json({ message: "Quizboard backend running" });
});

// Health check endpoint
app.get("/health", (req, res) => {
  res.json({ 
    status: 'ok', 
    esp32Enabled: esp32Bridge.enabled,
    esp32Connected: esp32Bridge.serialConnection ? esp32Bridge.serialConnection.isOpen : false,
    socketClients: io.engine.clientsCount 
  });
});

// Socket.IO connection handling
io.on("connection", (socket) => {
  console.log("Client connected:", socket.id);
  
  // Send current ESP32 status to new client
  socket.emit('esp32_status', { 
    connected: esp32Bridge.serialConnection ? esp32Bridge.serialConnection.isOpen : false,
    enabled: esp32Bridge.enabled
  });
  
  // Handle commands from client to ESP32
  socket.on("esp32_command", (command) => {
    console.log('Received command from client:', command);
    esp32Bridge.sendToESP32(command);
  });
  
  socket.on("disconnect", () => {
    console.log("Client disconnected:", socket.id);
  });
});

// Start ESP32 communication
esp32Bridge.startSerialCommunication();

const PORT = 3000;
server.listen(PORT, '0.0.0.0', () => {
  console.log(`Server running on http://0.0.0.0:${PORT}`);
  console.log('ESP32 Bridge server started!');
  console.log('Web interface: http://localhost:3000');
  console.log('Network access: http://[PI_IP]:3000');
  console.log('Socket.IO server: ws://localhost:3000');
  console.log('ESP32 connection is optional - server will run with or without ESP32');
  console.log('Press Ctrl+C to stop');
});

// Handle graceful shutdown
process.on('SIGINT', () => {
  console.log('\nShutting down...');
  esp32Bridge.stop();
  process.exit(0);
});

process.on('SIGTERM', () => {
  console.log('\nShutting down...');
  esp32Bridge.stop();
  process.exit(0);
});
