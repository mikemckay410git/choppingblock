# Manual Installation Guide for Raspberry Pi

If the automated setup script doesn't work, follow these manual steps:

## Option 1: System Packages (Recommended)

```bash
# Update package list
sudo apt update

# Install Python packages via apt
sudo apt install -y python3-websockets python3-serial

# Run the bridge server
python3 esp32_bridge.py
```

## Option 2: Virtual Environment (If system packages fail)

```bash
# Create virtual environment
python3 -m venv esp32_bridge_env

# Activate virtual environment
source esp32_bridge_env/bin/activate

# Install packages
pip install websockets pyserial

# Run the bridge server
python3 esp32_bridge.py

# To run in background:
nohup python3 esp32_bridge.py > bridge.log 2>&1 &
```

## Option 3: Force Install (Not Recommended)

```bash
# Override system protection (risky)
pip3 install --break-system-packages websockets pyserial

# Run the bridge server
python3 esp32_bridge.py
```

## Quick Test

After installation, test the server:

```bash
# Start the server
python3 esp32_bridge.py

# In another terminal, test WebSocket connection
curl -i -N -H "Connection: Upgrade" -H "Upgrade: websocket" -H "Sec-WebSocket-Version: 13" -H "Sec-WebSocket-Key: x3JJHMbDL1EzLkh9GBhXDw==" http://localhost:8080/
```

## Troubleshooting

### Permission Issues
```bash
# Make sure you're in the right directory
cd /home/quiz

# Check file permissions
ls -la esp32_bridge.py
chmod +x esp32_bridge.py
```

### Serial Port Issues
```bash
# Check available serial ports
ls /dev/ttyUSB*
ls /dev/ttyACM*

# Run with specific port
python3 esp32_bridge.py --serial-port /dev/ttyUSB1
```

### Port Already in Use
```bash
# Check what's using port 8080
sudo netstat -tlnp | grep 8080

# Kill process if needed
sudo kill -9 <PID>
```

## Service Management

### Create Manual Service
```bash
# Create service file
sudo nano /etc/systemd/system/esp32-bridge.service

# Add this content:
[Unit]
Description=ESP32 Bridge Web Server
After=network.target

[Service]
Type=simple
User=quiz
WorkingDirectory=/home/quiz
ExecStart=/usr/bin/python3 /home/quiz/esp32_bridge.py --serial-port /dev/ttyUSB0
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target

# Enable and start service
sudo systemctl daemon-reload
sudo systemctl enable esp32-bridge.service
sudo systemctl start esp32-bridge.service

# Check status
sudo systemctl status esp32-bridge.service
```

### View Logs
```bash
# View service logs
sudo journalctl -u esp32-bridge.service -f

# View recent logs
sudo journalctl -u esp32-bridge.service --since "1 hour ago"
```
