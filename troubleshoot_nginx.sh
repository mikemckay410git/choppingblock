#!/bin/bash

# Troubleshoot nginx bad gateway error

echo "=== Troubleshooting Bad Gateway Error ==="

# Check if ESP32 Bridge is running
echo "1. Checking ESP32 Bridge status..."
if pm2 list | grep -q "esp32-bridge.*online"; then
    echo "✅ ESP32 Bridge is running in PM2"
    pm2 list | grep esp32-bridge
else
    echo "❌ ESP32 Bridge is not running in PM2"
    echo "   Starting ESP32 Bridge..."
    pm2 start ecosystem.config.js
fi

# Check if ports are listening
echo ""
echo "2. Checking if ports are listening..."
if netstat -tlnp | grep -q ":3001"; then
    echo "✅ Port 3001 is listening"
    netstat -tlnp | grep ":3001"
else
    echo "❌ Port 3001 is not listening"
fi

if netstat -tlnp | grep -q ":8080"; then
    echo "✅ Port 8080 is listening"
    netstat -tlnp | grep ":8080"
else
    echo "❌ Port 8080 is not listening"
fi

# Test local connectivity
echo ""
echo "3. Testing local connectivity..."
if curl -s http://localhost:3001 > /dev/null; then
    echo "✅ ESP32 Bridge responds on localhost:3001"
else
    echo "❌ ESP32 Bridge does not respond on localhost:3001"
fi

if curl -s http://localhost:8080 > /dev/null; then
    echo "✅ ESP32 Bridge responds on localhost:8080"
else
    echo "❌ ESP32 Bridge does not respond on localhost:8080"
fi

# Check nginx configuration
echo ""
echo "4. Checking nginx configuration..."
if [ -f /etc/nginx/sites-enabled/quiz.local ]; then
    echo "✅ nginx site configuration exists"
    echo "   Current configuration:"
    cat /etc/nginx/sites-enabled/quiz.local
else
    echo "❌ nginx site configuration missing"
fi

# Check nginx status
echo ""
echo "5. Checking nginx status..."
if systemctl is-active --quiet nginx; then
    echo "✅ nginx is running"
else
    echo "❌ nginx is not running"
    echo "   Starting nginx..."
    sudo systemctl start nginx
fi

# Test nginx configuration
echo ""
echo "6. Testing nginx configuration..."
if sudo nginx -t; then
    echo "✅ nginx configuration is valid"
else
    echo "❌ nginx configuration has errors"
fi

# Check nginx error logs
echo ""
echo "7. Checking nginx error logs..."
if [ -f /var/log/nginx/error.log ]; then
    echo "   Recent nginx errors:"
    sudo tail -10 /var/log/nginx/error.log
else
    echo "   No nginx error log found"
fi

echo ""
echo "=== Quick Fixes ==="
echo "If ESP32 Bridge is not running:"
echo "  pm2 start ecosystem.config.js"
echo ""
echo "If nginx needs restart:"
echo "  sudo systemctl restart nginx"
echo ""
echo "If ports are not listening:"
echo "  pm2 restart esp32-bridge"
echo ""
echo "Check PM2 logs:"
echo "  pm2 logs esp32-bridge"

