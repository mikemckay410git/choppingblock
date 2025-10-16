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
      console.debug('Non-JSON message from ESP32:', message);
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
const esp32Bridge = new ESP32Bridge();

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
server.listen(PORT, () => {
  console.log(`Server running on http://localhost:${PORT}`);
  console.log('ESP32 Bridge server started!');
  console.log('Web interface: http://localhost:3000');
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
