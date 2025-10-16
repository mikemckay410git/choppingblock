#!/bin/bash

# ESP32 Bridge Setup Script
# This script sets up the Raspberry Pi web server for the ESP32 bridge

echo "=== ESP32 Bridge Setup ==="

# Check if Python 3 is installed
if ! command -v python3 &> /dev/null; then
    echo "Python 3 is not installed. Please install Python 3 first."
    exit 1
fi

# Check if pip is installed
if ! command -v pip3 &> /dev/null; then
    echo "pip3 is not installed. Installing pip3..."
    sudo apt update
    sudo apt install -y python3-pip
fi

# Install required Python packages
echo "Installing Python packages..."
pip3 install -r requirements.txt

# Make the bridge script executable
chmod +x esp32_bridge.py

# Create a systemd service file for auto-start
echo "Creating systemd service..."
sudo tee /etc/systemd/system/esp32-bridge.service > /dev/null <<EOF
[Unit]
Description=ESP32 Bridge Web Server
After=network.target

[Service]
Type=simple
User=pi
WorkingDirectory=$(pwd)
ExecStart=/usr/bin/python3 $(pwd)/esp32_bridge.py --serial-port /dev/ttyUSB0
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

# Enable the service
sudo systemctl daemon-reload
sudo systemctl enable esp32-bridge.service

echo ""
echo "=== Setup Complete ==="
echo ""
echo "To start the server manually:"
echo "  python3 esp32_bridge.py"
echo ""
echo "To start the service:"
echo "  sudo systemctl start esp32-bridge.service"
echo ""
echo "To check service status:"
echo "  sudo systemctl status esp32-bridge.service"
echo ""
echo "To view logs:"
echo "  sudo journalctl -u esp32-bridge.service -f"
echo ""
echo "Web interface will be available at:"
echo "  http://localhost:3000"
echo ""
echo "Make sure your ESP32 is connected to /dev/ttyUSB0"
echo "If it's on a different port, edit the service file or run manually with --serial-port"
