import express from "express";
import http from "http";
import { Server } from "socket.io";
import cors from "cors";
import { SerialPort } from "serialport";
import { ReadlineParser } from "@serialport/parser-readline";
import fs from "fs";
import path from "path";
import { lightboardState } from "./lightboard.js";

const app = express();
const server = http.createServer(app);
const io = new Server(server, {
  cors: {
    origin: "*",
    methods: ["GET", "POST"]
  }
});

app.use(cors());
app.use(express.json()); // Parse JSON request bodies
app.use(express.static(".")); // serve files from home directory

// ESP32 Serial Communication
class ESP32Bridge {
  constructor(serialPort = '/dev/ttyUSB0', baudRate = 115200) {
    this.serialPort = serialPort;
    this.baudRate = baudRate;
    this.serialConnection = null;
    this.parser = null;
    this.io = io;
    this.enabled = false; // Track if serial is enabled
    this.lightboardConnected = false; // Track if physical lightboard is connected via ESP-NOW
    this.player1Connected = false; // Track if Player 1 is connected via ESP-NOW
    this.player2Connected = false; // Track if Player 2 is connected via ESP-NOW
    this.retryTimeout = null; // Track retry timeout for cleanup
    this.heartbeatInterval = null; // Track heartbeat interval timer
    // Removed debounce variables - ESP32 handles awarding internally
  }

  async startSerialCommunication() {
    // Clear any existing retry timeout
    if (this.retryTimeout) {
      clearTimeout(this.retryTimeout);
      this.retryTimeout = null;
    }

    // Clean up existing connection
    this.cleanup();

    try {
      // List available ports for debugging
      const ports = await SerialPort.list();
      console.log('Available serial ports:', ports.map(p => p.path));
      
      // Check if the specified port exists
      const portExists = ports.some(port => port.path === this.serialPort);
      if (!portExists) {
        console.log(`Serial port ${this.serialPort} not found. ESP32 communication disabled.`);
        console.log('Server will run without ESP32 connection.');
        this.lightboardConnected = false;
        this.player1Connected = false;
        this.player2Connected = false;
        this.io.emit('esp32_status', { 
          connected: false, 
          enabled: false,
          lightboardConnected: false,
          player1Connected: false,
          player2Connected: false
        });
        return;
      }
      
      this.enabled = true;
      this.serialConnection = new SerialPort({
        path: this.serialPort,
        baudRate: this.baudRate,
        autoOpen: false
      });

      this.parser = this.serialConnection.pipe(new ReadlineParser({ delimiter: '\n' }));
      
      this.serialConnection.on('open', () => {
        console.log(`Connected to ESP32 on ${this.serialPort}`);
        // Notify all clients that ESP32 is connected
        this.io.emit('esp32_status', { 
          connected: true, 
          enabled: true,
          lightboardConnected: this.lightboardConnected,
          player1Connected: this.player1Connected,
          player2Connected: this.player2Connected
        });
        
        // Start sending periodic heartbeats to Bridge (every 2 seconds)
        // Bridge responds with status messages that include lightboard connection info
        this.startHeartbeat();
      });

      this.serialConnection.on('error', (err) => {
        console.warn(`Serial connection error: ${err.message}`);
        this.cleanup();
        this.enabled = false;
        this.lightboardConnected = false; // Reset lightboard connection status
        this.player1Connected = false; // Reset player connections
        this.player2Connected = false;
        // Notify all clients that ESP32 is disconnected
        this.io.emit('esp32_status', { 
          connected: false, 
          enabled: false,
          lightboardConnected: false,
          player1Connected: false,
          player2Connected: false
        });
        // Retry connection after 5 seconds (longer delay for optional connection)
        this.retryTimeout = setTimeout(() => this.startSerialCommunication(), 5000);
      });

      this.parser.on('data', (data) => {
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
      this.cleanup();
      this.enabled = false;
      this.lightboardConnected = false;
      this.player2Connected = false;
      this.player3Connected = false;
      // Notify all clients that ESP32 is disconnected
      this.io.emit('esp32_status', { 
        connected: false, 
        enabled: false,
        lightboardConnected: false,
        player2Connected: false,
        player3Connected: false
      });
      // Retry connection after 5 seconds (longer delay for optional connection)
      this.retryTimeout = setTimeout(() => this.startSerialCommunication(), 5000);
    }
  }

  handleSerialMessage(message) {
    try {
      // Parse JSON message from ESP32
      const data = JSON.parse(message);
      console.log('Received from ESP32:', data);
      
      // Check if this is a status message with connection info
      if (data.type === 'status') {
        // Update connection statuses from Bridge
        let statusChanged = false;
        
        if (data.lightboardConnected !== undefined) {
          const wasConnected = this.lightboardConnected;
          this.lightboardConnected = data.lightboardConnected;
          if (wasConnected !== this.lightboardConnected) {
            console.log(`Lightboard connection status changed: ${this.lightboardConnected ? 'connected' : 'disconnected'}`);
            statusChanged = true;
          }
        }
        
        if (data.player1Connected !== undefined) {
          const wasConnected = this.player1Connected;
          this.player1Connected = data.player1Connected;
          if (wasConnected !== this.player1Connected) {
            console.log(`Player 1 connection status changed: ${this.player1Connected ? 'connected' : 'disconnected'}`);
            statusChanged = true;
          }
        }
        
        if (data.player2Connected !== undefined) {
          const wasConnected = this.player2Connected;
          this.player2Connected = data.player2Connected;
          if (wasConnected !== this.player2Connected) {
            console.log(`Player 2 connection status changed: ${this.player2Connected ? 'connected' : 'disconnected'}`);
            statusChanged = true;
          }
        }
        
        // Always emit status update when we receive status from Bridge
        // This ensures initial state is set and clients are kept in sync
        this.io.emit('esp32_status', { 
          connected: this.serialConnection ? this.serialConnection.isOpen : false,
          enabled: this.enabled,
          lightboardConnected: this.lightboardConnected,
          player1Connected: this.player1Connected,
          player2Connected: this.player2Connected
        });
      }
      
      // Check if this is a request for lightboard state
      if (data.type === 'lightboardStateRequest') {
        // Get current state from state manager
        const gameState = lightboardState.getGameState();
        
        // Send state back to ESP32
        this.sendToESP32({
          cmd: 'lightboardState',
          gameState: gameState
        });
        console.log('Sent lightboard state to ESP32:', gameState);
        return; // Don't forward this request to clients
      }
      
      // Update state when receiving point awards from ESP32
      if (data.type === 'winner' && data.winner) {
        // Extract player number from winner string (e.g., "Player 1" -> 1)
        const playerMatch = data.winner.match(/Player (\d+)/);
        if (playerMatch) {
          const player = parseInt(playerMatch[1]);
          if (player === 1 || player === 2) {
            lightboardState.awardPoint(player, 1);
            console.log(`Updated lightboard state: Player ${player} scored`);
          }
        }
      }
      
      // Update state when receiving reset from ESP32
      if (data.type === 'reset') {
        lightboardState.resetGame();
        console.log('Reset lightboard state from ESP32');
      }
      
      // Forward to all Socket.IO clients
      this.io.emit('esp32_data', data);
      
    } catch (error) {
      // Handle non-JSON messages (like debug output)
      console.log('Non-JSON message from ESP32:', message);

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

  startHeartbeat() {
    // Stop any existing heartbeat
    this.stopHeartbeat();
    
    // Send initial heartbeat immediately
    this.sendHeartbeat();
    
    // Then send heartbeats every 2 seconds
    this.heartbeatInterval = setInterval(() => {
      this.sendHeartbeat();
    }, 2000);
  }
  
  stopHeartbeat() {
    if (this.heartbeatInterval) {
      clearInterval(this.heartbeatInterval);
      this.heartbeatInterval = null;
    }
  }
  
  sendHeartbeat() {
    if (!this.enabled || !this.serialConnection || !this.serialConnection.isOpen) {
      return;
    }
    
    try {
      const message = JSON.stringify({ cmd: 'heartbeat' }) + '\n';
      this.serialConnection.write(message);
    } catch (error) {
      console.error('Error sending heartbeat to Bridge:', error);
    }
  }

  cleanup() {
    // Stop heartbeat
    this.stopHeartbeat();
    
    // Clear retry timeout
    if (this.retryTimeout) {
      clearTimeout(this.retryTimeout);
      this.retryTimeout = null;
    }

    // Remove parser event listeners
    if (this.parser) {
      this.parser.removeAllListeners();
      this.parser = null;
    }

    // Close and cleanup serial connection
    if (this.serialConnection) {
      if (this.serialConnection.isOpen) {
        this.serialConnection.close();
      }
      this.serialConnection.removeAllListeners();
      this.serialConnection = null;
    }
  }

  stop() {
    this.cleanup();
    console.log('ESP32 Bridge stopped');
  }
}

// Create ESP32 bridge instance
const esp32Bridge = new ESP32Bridge('/dev/ttyUSB0', 115200);

// Example endpoint
app.get("/api", (req, res) => {
  res.json({ message: "Quizboard backend running" });
});

// Lightboard emulator endpoint
app.get("/lightboard", (req, res) => {
  res.sendFile(path.join(process.cwd(), 'lightboard.html'));
});

// Lightboard state endpoints
app.get("/api/lightboard-state", (req, res) => {
  try {
    const state = lightboardState.getState();
    res.json(state);
  } catch (error) {
    console.error('Error reading lightboard state:', error);
    res.status(500).json({ error: 'Failed to read lightboard state' });
  }
});

app.post("/api/lightboard-state", (req, res) => {
  try {
    lightboardState.updateState(req.body);
    res.json({ success: true });
  } catch (error) {
    console.error('Error saving lightboard state:', error);
    res.status(500).json({ error: 'Failed to save lightboard state' });
  }
});

// Quiz files listing endpoint
app.get("/api/quiz-files", (req, res) => {
  try {
    const quizesDir = path.join(process.cwd(), 'Quizes');
    
    // Check if Quizes directory exists
    if (!fs.existsSync(quizesDir)) {
      return res.json([]);
    }
    
    const quizItems = [];
    
    // Read directory contents
    const items = fs.readdirSync(quizesDir, { withFileTypes: true });
    
    for (const item of items) {
      if (item.isFile() && item.name.toLowerCase().endsWith('.csv')) {
        // Regular CSV quiz file
        quizItems.push({
          type: 'regular',
          name: item.name,
          path: `Quizes/${item.name}`
        });
      } else if (item.isDirectory()) {
        // Check if it's a music quiz folder
        const folderPath = path.join(quizesDir, item.name);
        const csvFile = path.join(folderPath, `${item.name}.csv`);
        
        if (fs.existsSync(csvFile)) {
          // Check if folder contains audio files
          const audioFiles = fs.readdirSync(folderPath)
            .filter(file => file.toLowerCase().endsWith('.mp3') || file.toLowerCase().endsWith('.wav'));
          
          if (audioFiles.length > 0) {
            quizItems.push({
              type: 'music',
              name: item.name,
              path: `Quizes/${item.name}/${item.name}.csv`,
              audioFiles: audioFiles.map(file => `Quizes/${item.name}/${file}`)
            });
          } else {
            // Folder with CSV but no audio files - treat as regular quiz
            quizItems.push({
              type: 'regular',
              name: `${item.name}.csv`,
              path: `Quizes/${item.name}/${item.name}.csv`
            });
          }
        }
      }
    }
    
    // Sort alphabetically by name
    quizItems.sort((a, b) => a.name.localeCompare(b.name));
    
    res.json(quizItems);
  } catch (error) {
    console.error('Error reading quiz files:', error);
    res.status(500).json({ error: 'Failed to read quiz files' });
  }
});

// Health check endpoint
app.get("/health", (req, res) => {
  const memUsage = process.memoryUsage();
  res.json({ 
    status: 'ok', 
    esp32Enabled: esp32Bridge.enabled,
    esp32Connected: esp32Bridge.serialConnection ? esp32Bridge.serialConnection.isOpen : false,
    socketClients: io.engine.clientsCount,
    memory: {
      rss: Math.round(memUsage.rss / 1024 / 1024) + ' MB',
      heapTotal: Math.round(memUsage.heapTotal / 1024 / 1024) + ' MB',
      heapUsed: Math.round(memUsage.heapUsed / 1024 / 1024) + ' MB',
      external: Math.round(memUsage.external / 1024 / 1024) + ' MB'
    },
    uptime: Math.round(process.uptime()) + ' seconds'
  });
});

// Socket.IO connection handling
io.on("connection", (socket) => {
  console.log("Client connected:", socket.id);
  
  // Send current ESP32 status to new client
  socket.emit('esp32_status', { 
    connected: esp32Bridge.serialConnection ? esp32Bridge.serialConnection.isOpen : false,
    enabled: esp32Bridge.enabled,
    lightboardConnected: esp32Bridge.lightboardConnected,
    player1Connected: esp32Bridge.player1Connected,
    player2Connected: esp32Bridge.player2Connected
  });
  
  // Handle commands from client to ESP32
  socket.on("esp32_command", (command) => {
    console.log('Received command from client:', command);
    
    // Update lightboard state based on command
    if (command.cmd === 'awardPoint' && command.player && command.multiplier) {
      lightboardState.awardPoint(command.player, command.multiplier);
      console.log(`Updated lightboard state: Player ${command.player} awarded ${command.multiplier} point(s)`);
    } else if (command.cmd === 'reset') {
      // Client-initiated reset (from "Reset All Data") - reset everything including settings
      lightboardState.resetAll();
      console.log('Reset all lightboard data including settings');
      
      // Get the reset state with default settings
      const resetState = lightboardState.getGameState();
      
      // Send the reset command to ESP32
      esp32Bridge.sendToESP32(command);
      
      // Also send the updated default settings to ESP32 and emulator
      const settingsCommand = {
        cmd: 'lightboardSettings',
        mode: resetState.mode,
        p1Color: resetState.p1ColorIndex,
        p2Color: resetState.p2ColorIndex
      };
      esp32Bridge.sendToESP32(settingsCommand);
      io.emit('esp32_command', settingsCommand);
      
      // Broadcast the reset command to all connected clients (including lightboard emulator)
      io.emit('esp32_command', command);
      return; // Don't send reset command again below
    } else if (command.cmd === 'lightboardSettings' && command.mode !== undefined) {
      lightboardState.updateSettings(command.mode, command.p1Color, command.p2Color);
      console.log(`Updated lightboard settings: mode=${command.mode}, p1Color=${command.p1Color}, p2Color=${command.p2Color}`);
    }
    
    // Send command to ESP32 and broadcast to all clients (unless already handled above)
    esp32Bridge.sendToESP32(command);
    io.emit('esp32_command', command);
  });
  
  socket.on("disconnect", () => {
    console.log("Client disconnected:", socket.id);
  });
});

// Start ESP32 communication
esp32Bridge.startSerialCommunication();

// Periodic cleanup routine to prevent memory leaks
setInterval(() => {
  // Force garbage collection if available
  if (global.gc) {
    global.gc();
  }
  
  // Log memory usage every 10 minutes
  const memUsage = process.memoryUsage();
  console.log(`Memory usage - RSS: ${Math.round(memUsage.rss / 1024 / 1024)}MB, Heap: ${Math.round(memUsage.heapUsed / 1024 / 1024)}MB`);
}, 10 * 60 * 1000); // Every 10 minutes

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
