#!/usr/bin/env python3
"""
ESP32 Bridge Web Server
Bridges between web interface and ESP32 via serial communication
"""

import asyncio
import websockets
import json
import serial
import threading
import time
import logging
from pathlib import Path

# Configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

class ESP32Bridge:
    def __init__(self, serial_port='/dev/ttyUSB0', baud_rate=115200):
        self.serial_port = serial_port
        self.baud_rate = baud_rate
        self.serial_conn = None
        self.websocket_clients = set()
        self.running = False
        
    def start(self):
        """Start the bridge server"""
        self.running = True
        
        # Start serial communication in a separate thread
        serial_thread = threading.Thread(target=self.serial_worker)
        serial_thread.daemon = True
        serial_thread.start()
        
        # Start WebSocket server
        logger.info("Starting ESP32 Bridge WebSocket server on localhost:8080")
        return websockets.serve(self.handle_websocket, "localhost", 8080)
    
    def stop(self):
        """Stop the bridge server"""
        self.running = False
        if self.serial_conn:
            self.serial_conn.close()
    
    def serial_worker(self):
        """Worker thread for serial communication"""
        while self.running:
            try:
                if not self.serial_conn:
                    self.serial_conn = serial.Serial(
                        self.serial_port, 
                        self.baud_rate, 
                        timeout=1
                    )
                    logger.info(f"Connected to ESP32 on {self.serial_port}")
                
                # Read from serial
                if self.serial_conn.in_waiting > 0:
                    line = self.serial_conn.readline().decode('utf-8').strip()
                    if line:
                        self.handle_serial_message(line)
                
                time.sleep(0.01)  # Small delay to prevent CPU spinning
                
            except serial.SerialException as e:
                logger.warning(f"Serial connection error: {e}")
                self.serial_conn = None
                time.sleep(1)  # Wait before retrying
            except Exception as e:
                logger.error(f"Serial worker error: {e}")
                time.sleep(1)
    
    def handle_serial_message(self, message):
        """Handle messages received from ESP32"""
        try:
            # Parse JSON message from ESP32
            data = json.loads(message)
            logger.info(f"Received from ESP32: {data}")
            
            # Forward to all WebSocket clients
            self.broadcast_to_websockets(data)
            
        except json.JSONDecodeError:
            # Handle non-JSON messages (like debug output)
            logger.debug(f"Non-JSON message from ESP32: {message}")
        except Exception as e:
            logger.error(f"Error handling serial message: {e}")
    
    def send_to_esp32(self, command):
        """Send command to ESP32"""
        try:
            if self.serial_conn and self.serial_conn.is_open:
                message = json.dumps(command) + '\n'
                self.serial_conn.write(message.encode('utf-8'))
                logger.info(f"Sent to ESP32: {command}")
            else:
                logger.warning("Serial connection not available")
        except Exception as e:
            logger.error(f"Error sending to ESP32: {e}")
    
    async def handle_websocket(self, websocket, path):
        """Handle WebSocket connections"""
        self.websocket_clients.add(websocket)
        logger.info(f"WebSocket client connected. Total clients: {len(self.websocket_clients)}")
        
        try:
            async for message in websocket:
                try:
                    command = json.loads(message)
                    logger.info(f"Received from WebSocket: {command}")
                    
                    # Forward command to ESP32
                    self.send_to_esp32(command)
                    
                except json.JSONDecodeError:
                    logger.error(f"Invalid JSON from WebSocket: {message}")
                except Exception as e:
                    logger.error(f"Error handling WebSocket message: {e}")
                    
        except websockets.exceptions.ConnectionClosed:
            logger.info("WebSocket client disconnected")
        finally:
            self.websocket_clients.remove(websocket)
            logger.info(f"WebSocket client removed. Total clients: {len(self.websocket_clients)}")
    
    def broadcast_to_websockets(self, data):
        """Broadcast data to all WebSocket clients"""
        if not self.websocket_clients:
            return
            
        message = json.dumps(data)
        
        # Send to all clients asynchronously
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        
        async def send_to_all():
            tasks = []
            for websocket in self.websocket_clients.copy():
                try:
                    tasks.append(websocket.send(message))
                except:
                    # Remove disconnected clients
                    self.websocket_clients.discard(websocket)
            
            if tasks:
                await asyncio.gather(*tasks, return_exceptions=True)
        
        try:
            loop.run_until_complete(send_to_all())
        except Exception as e:
            logger.error(f"Error broadcasting to WebSockets: {e}")
        finally:
            loop.close()

def serve_static_files():
    """Simple HTTP server for static files"""
    import http.server
    import socketserver
    from pathlib import Path
    
    PORT = 3000
    
    class CustomHTTPRequestHandler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(Path(__file__).parent), **kwargs)
        
        def end_headers(self):
            # Add CORS headers
            self.send_header('Access-Control-Allow-Origin', '*')
            self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS')
            self.send_header('Access-Control-Allow-Headers', 'Content-Type')
            super().end_headers()
    
    with socketserver.TCPServer(("", PORT), CustomHTTPRequestHandler) as httpd:
        logger.info(f"Serving static files on http://localhost:{PORT}")
        httpd.serve_forever()

def main():
    """Main function"""
    import argparse
    
    parser = argparse.ArgumentParser(description='ESP32 Bridge Web Server')
    parser.add_argument('--serial-port', default='/dev/ttyUSB0', 
                       help='Serial port for ESP32 communication (default: /dev/ttyUSB0)')
    parser.add_argument('--baud-rate', type=int, default=115200,
                       help='Serial baud rate (default: 115200)')
    parser.add_argument('--no-static', action='store_true',
                       help='Disable static file server')
    
    args = parser.parse_args()
    
    # Create bridge instance
    bridge = ESP32Bridge(args.serial_port, args.baud_rate)
    
    try:
        # Start static file server in a separate thread if not disabled
        if not args.no_static:
            static_thread = threading.Thread(target=serve_static_files)
            static_thread.daemon = True
            static_thread.start()
        
        # Start WebSocket server
        start_server = bridge.start()
        
        logger.info("ESP32 Bridge server started!")
        logger.info("Web interface: http://localhost:3000")
        logger.info("WebSocket server: ws://localhost:8080")
        logger.info("Press Ctrl+C to stop")
        
        # Run the WebSocket server
        asyncio.get_event_loop().run_until_complete(start_server)
        asyncio.get_event_loop().run_forever()
        
    except KeyboardInterrupt:
        logger.info("Shutting down...")
    finally:
        bridge.stop()

if __name__ == "__main__":
    main()
