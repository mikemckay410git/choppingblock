#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <esp_now.h>

#define LED_PIN 2

WebServer server(80);
WebSocketsServer ws(81);

// Connection tracking
bool        player2Connected = false;
unsigned long lastHeartbeat   = 0;
const unsigned long heartbeatTimeout = 2000; // Increased to 2 seconds for more stable connection

// Remote (Player 2) sensor storage
uint16_t remoteCap[5] = {0};
uint16_t remoteShock  = 0;

// Optimization: Track if data has changed to avoid unnecessary WebSocket broadcasts
bool dataChanged = false;
uint16_t lastRemoteCap[5] = {0};
uint16_t lastRemoteShock = 0;

// Rate limiting for WebSocket broadcasts
unsigned long lastBroadcastTime = 0;
const unsigned long broadcastInterval = 25; // 25ms minimum between broadcasts (40 Hz max)

// ─── ESP-NOW Payload ─────────────────────────────────────────────────
typedef struct struct_message {
  uint8_t  playerId;   // 1=Player1, 2=Player2
  uint8_t  action;     // 1=heartbeat, 2=sensor-update
  uint16_t cap[5];     // five capacitive readings
  uint16_t shock;      // shock reading
} struct_message;
// ───────────────────────────────────────────────────────────────────────

// HTML + WebSocket client
const char html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>Tool Target Game</title>
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <style>
    body { font-family: Arial, sans-serif; text-align: center; padding: 20px; }
    .status-light { width: 40px; height: 40px; margin: 0 auto 20px; background: red; transition: background 0.1s; }
    .connected    { background: green !important; }
    table { margin: 0 auto; border-collapse: collapse; width: 360px; font-size: 1.1em; }
    th, td { border: 1px solid #333; padding: 8px 12px; }
    th { background: #eee; }
    .triggered-cell { background: blue !important; color: white; }
    .latency-info { font-size: 0.9em; color: #666; margin: 10px 0; }
    .update-counter { font-size: 0.8em; color: #999; }
  </style>
</head>
<body>
  <h1>Tool Target Game</h1>
  <div id="status" class="status-light"></div>
  <div class="latency-info">
    <div>Balanced performance: ~13 Hz updates</div>
    <div>Updates: <span id="updateCounter">0</span></div>
  </div>
  <h2>Live Sensor Data</h2>
  <table>
    <thead>
      <tr><th>Sensor</th><th>Value 1</th><th>Value 2</th></tr>
    </thead>
    <tbody>
      <tr><td>Cap 0</td><td id="cap0_1">0</td><td id="cap0_2">0</td></tr>
      <tr><td>Cap 1</td><td id="cap1_1">0</td><td id="cap1_2">0</td></tr>
      <tr><td>Cap 2</td><td id="cap2_1">0</td><td id="cap2_2">0</td></tr>
      <tr><td>Cap 3</td><td id="cap3_1">0</td><td id="cap3_2">0</td></tr>
      <tr><td>Cap 4</td><td id="cap4_1">0</td><td id="cap4_2">0</td></tr>
      <tr><td>Shock</td><td id="shock_1">0</td><td id="shock_2">0</td></tr>
    </tbody>
  </table>
<script>
  const socket = new WebSocket('ws://' + location.hostname + ':81');
  let updateCount = 0;
  
  socket.onopen = () => console.log('WS open - Low latency mode active');
  socket.onmessage = evt => {
    const data = JSON.parse(evt.data);
    updateCount++;
    
    // Update counter
    document.getElementById('updateCounter').textContent = updateCount;
    
    // status
    const statusDiv = document.getElementById('status');
    data.connected ? statusDiv.classList.add('connected') : statusDiv.classList.remove('connected');

    // update cells
    for (let i = 0; i < 5; i++) {
      ['1','2'].forEach(p => {
        const key = `cap${i}_${p}`;
        const cell = document.getElementById(key);
        const v = data[key];
        cell.innerText = v;
        v > 20 ? cell.classList.add('triggered-cell') : cell.classList.remove('triggered-cell');
      });
    }
    ['1','2'].forEach(p => {
      const key = `shock_${p}`;
      const cell = document.getElementById(key);
      const v = data[key];
      cell.innerText = v;
      v > 20 ? cell.classList.add('triggered-cell') : cell.classList.remove('triggered-cell');
    });
  };
  
  socket.onerror = (error) => console.log('WebSocket error:', error);
  socket.onclose = () => console.log('WebSocket closed');
</script>
</body>
</html>
)rawliteral";

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(struct_message)) return;
  struct_message msg;
  memcpy(&msg, data, sizeof(msg));

  if (msg.playerId == 2) {
    player2Connected = true;
    lastHeartbeat    = millis();

    if (msg.action == 1) {
      // Heartbeat - just update connection status
      dataChanged = true;
    } else if (msg.action == 2) {
      // Sensor update - check if data actually changed
      dataChanged = false;
      for (int i = 0; i < 5; i++) {
        if (msg.cap[i] != lastRemoteCap[i]) {
          dataChanged = true;
          lastRemoteCap[i] = msg.cap[i];
          remoteCap[i] = msg.cap[i];
        }
      }
      if (msg.shock != lastRemoteShock) {
        dataChanged = true;
        lastRemoteShock = msg.shock;
        remoteShock = msg.shock;
      }
      
      // Only broadcast if data changed and rate limit allows
      if (dataChanged && (millis() - lastBroadcastTime >= broadcastInterval)) {
        // build JSON with both local & remote
        String j = "{";
        j += "\"connected\":" + String(player2Connected ? "true" : "false") + ",";
        for (int i = 0; i < 5; i++) {
          j += "\"cap" + String(i) + "_1\":" + String(getCapacitiveValue(i)) + ",";
          j += "\"cap" + String(i) + "_2\":" + String(remoteCap[i]) + ",";
        }
        j += "\"shock_1\":" + String(getShockValue()) + ",";
        j += "\"shock_2\":" + String(remoteShock) + "}";

        ws.broadcastTXT(j);
        lastBroadcastTime = millis();
      }
    }
  }
}

void handleRoot()    { server.send(200, "text/html", html); }

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // Set WiFi mode to AP+STA for both AP and ESP-NOW
  WiFi.mode(WIFI_AP_STA);
  
  // AP + web
  WiFi.softAP("ToolMaster", "12345678");
  Serial.println("AP IP: " + WiFi.softAPIP().toString());
  server.on("/", handleRoot);
  server.begin();

  // WebSockets
  ws.begin();

  // ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    while(true);
  }
  esp_now_register_recv_cb(onDataRecv);

  sensorsInit();
  Serial.println("Player 1 ready - AP and ESP-NOW active");
}

void loop() {
  server.handleClient(); // handle web server requests
  ws.loop();           // handle WS
  sensorsLoop();       // update local sensors
  
  // Check for connection timeout
  if (player2Connected && (millis() - lastHeartbeat > heartbeatTimeout)) {
    player2Connected = false;
    dataChanged = true; // Force update to show disconnected status
    // Force broadcast of disconnected status
    if (millis() - lastBroadcastTime >= broadcastInterval) {
      String j = "{";
      j += "\"connected\":false,";
      for (int i = 0; i < 5; i++) {
        j += "\"cap" + String(i) + "_1\":" + String(getCapacitiveValue(i)) + ",";
        j += "\"cap" + String(i) + "_2\":0,";
      }
      j += "\"shock_1\":" + String(getShockValue()) + ",";
      j += "\"shock_2\":0}";
      ws.broadcastTXT(j);
      lastBroadcastTime = millis();
    }
  }
  
  digitalWrite(LED_PIN, player2Connected ? HIGH : LOW);
}
