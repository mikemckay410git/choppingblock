#!/usr/bin/env node
/**
 * ESP32 Bridge Web Server - Node.js Implementation
 * Bridges between web interface and ESP32 via serial communication
 */

const express = require('express');
const WebSocket = require('ws');
const SerialPort = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');
const cors = require('cors');
const path = require('path');
const fs = require('fs');

class ESP32Bridge {
    constructor(serialPort = '/dev/ttyUSB0', baudRate = 115200) {
        this.serialPort = serialPort;
        this.baudRate = baudRate;
        this.serialConnection = null;
        this.websocketClients = new Set();
        this.running = false;
        this.app = express();
        this.server = null;
        this.wss = null;
        
        this.setupExpress();
    }

    setupExpress() {
        // Enable CORS
        this.app.use(cors());
        
        // Serve static files
        this.app.use(express.static(path.join(__dirname)));
        
        // Health check endpoint
        this.app.get('/health', (req, res) => {
            res.json({ 
                status: 'ok', 
                serialConnected: this.serialConnection ? this.serialConnection.isOpen : false,
                websocketClients: this.websocketClients.size 
            });
        });
    }

    async start() {
        this.running = true;
        
        try {
            // Start serial communication
            await this.startSerialCommunication();
            
            // Start HTTP server
            this.server = this.app.listen(3000, () => {
                console.log('Static file server running on http://localhost:3000');
            });
            
            // Start WebSocket server
            this.wss = new WebSocket.Server({ port: 8080 });
            this.setupWebSocketServer();
            
            console.log('ESP32 Bridge server started!');
            console.log('Web interface: http://localhost:3000');
            console.log('WebSocket server: ws://localhost:8080');
            console.log('Press Ctrl+C to stop');
            
        } catch (error) {
            console.error('Error starting bridge:', error);
            this.stop();
        }
    }

    async startSerialCommunication() {
        try {
            // List available ports for debugging
            const ports = await SerialPort.list();
            console.log('Available serial ports:', ports.map(p => p.path));
            
            this.serialConnection = new SerialPort({
                path: this.serialPort,
                baudRate: this.baudRate,
                autoOpen: false
            });

            const parser = this.serialConnection.pipe(new ReadlineParser({ delimiter: '\n' }));
            
            this.serialConnection.on('open', () => {
                console.log(`Connected to ESP32 on ${this.serialPort}`);
            });

            this.serialConnection.on('error', (err) => {
                console.warn(`Serial connection error: ${err.message}`);
                this.serialConnection = null;
                // Retry connection after 1 second
                setTimeout(() => this.startSerialCommunication(), 1000);
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
            // Retry connection after 1 second
            setTimeout(() => this.startSerialCommunication(), 1000);
        }
    }

    handleSerialMessage(message) {
        try {
            // Parse JSON message from ESP32
            const data = JSON.parse(message);
            console.log('Received from ESP32:', data);
            
            // Forward to all WebSocket clients
            this.broadcastToWebSockets(data);
            
        } catch (error) {
            // Handle non-JSON messages (like debug output)
            console.debug('Non-JSON message from ESP32:', message);
        }
    }

    sendToESP32(command) {
        try {
            if (this.serialConnection && this.serialConnection.isOpen) {
                const message = JSON.stringify(command) + '\n';
                this.serialConnection.write(message);
                console.log('Sent to ESP32:', command);
            } else {
                console.warn('Serial connection not available');
            }
        } catch (error) {
            console.error('Error sending to ESP32:', error);
        }
    }

    setupWebSocketServer() {
        this.wss.on('connection', (ws) => {
            this.websocketClients.add(ws);
            console.log(`WebSocket client connected. Total clients: ${this.websocketClients.size}`);
            
            ws.on('message', (message) => {
                try {
                    const command = JSON.parse(message);
                    console.log('Received from WebSocket:', command);
                    
                    // Forward command to ESP32
                    this.sendToESP32(command);
                    
                } catch (error) {
                    console.error('Invalid JSON from WebSocket:', message);
                }
            });
            
            ws.on('close', () => {
                this.websocketClients.delete(ws);
                console.log(`WebSocket client disconnected. Total clients: ${this.websocketClients.size}`);
            });
            
            ws.on('error', (error) => {
                console.error('WebSocket error:', error);
                this.websocketClients.delete(ws);
            });
        });
    }

    broadcastToWebSockets(data) {
        if (this.websocketClients.size === 0) {
            return;
        }
        
        const message = JSON.stringify(data);
        
        // Send to all clients
        this.websocketClients.forEach((ws) => {
            try {
                if (ws.readyState === WebSocket.OPEN) {
                    ws.send(message);
                } else {
                    // Remove disconnected clients
                    this.websocketClients.delete(ws);
                }
            } catch (error) {
                console.error('Error sending to WebSocket client:', error);
                this.websocketClients.delete(ws);
            }
        });
    }

    stop() {
        this.running = false;
        
        if (this.serialConnection && this.serialConnection.isOpen) {
            this.serialConnection.close();
        }
        
        if (this.wss) {
            this.wss.close();
        }
        
        if (this.server) {
            this.server.close();
        }
        
        console.log('ESP32 Bridge server stopped');
    }
}

// Command line argument parsing
function parseArguments() {
    const args = process.argv.slice(2);
    const options = {
        serialPort: '/dev/ttyUSB0',
        baudRate: 115200
    };
    
    for (let i = 0; i < args.length; i++) {
        switch (args[i]) {
            case '--serial-port':
                options.serialPort = args[++i];
                break;
            case '--baud-rate':
                options.baudRate = parseInt(args[++i]);
                break;
            case '--help':
                console.log(`
ESP32 Bridge Web Server - Node.js Implementation

Usage: node esp32_bridge.js [options]

Options:
  --serial-port <port>    Serial port for ESP32 communication (default: /dev/ttyUSB0)
  --baud-rate <rate>      Serial baud rate (default: 115200)
  --help                  Show this help message

Examples:
  node esp32_bridge.js
  node esp32_bridge.js --serial-port COM3 --baud-rate 9600
                `);
                process.exit(0);
                break;
        }
    }
    
    return options;
}

// Main function
async function main() {
    const options = parseArguments();
    
    // Create bridge instance
    const bridge = new ESP32Bridge(options.serialPort, options.baudRate);
    
    // Handle graceful shutdown
    process.on('SIGINT', () => {
        console.log('\nShutting down...');
        bridge.stop();
        process.exit(0);
    });
    
    process.on('SIGTERM', () => {
        console.log('\nShutting down...');
        bridge.stop();
        process.exit(0);
    });
    
    // Start the bridge
    await bridge.start();
}

// Run if this file is executed directly
if (require.main === module) {
    main().catch((error) => {
        console.error('Fatal error:', error);
        process.exit(1);
    });
}

module.exports = ESP32Bridge;
