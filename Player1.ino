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
const unsigned long heartbeatTimeout = 600; // ms

// Remote (Player 2) sensor storage
uint16_t remoteCap[5] = {0};
uint16_t remoteShock  = 0;

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
  </style>
</head>
<body>
  <h1>Tool Target Game</h1>
  <div id="status" class="status-light"></div>
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
  socket.onopen = () => console.log('WS open');
  socket.onmessage = evt => {
    const data = JSON.parse(evt.data);
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

    if (msg.action == 2) {
      // update remote
      for (int i = 0; i < 5; i++) remoteCap[i] = msg.cap[i];
      remoteShock = msg.shock;

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
    }
  }
}

void handleRoot()    { server.send(200, "text/html", html); }

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // AP + web
  WiFi.softAP("ToolMaster", "12345678");
  Serial.println(WiFi.softAPIP());
  server.on("/", handleRoot);
  server.begin();

  // WebSockets
  ws.begin();

  // ESP-NOW
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) while(true);
  esp_now_register_recv_cb(onDataRecv);

  sensorsInit();
}

void loop() {
  ws.loop();           // handle WS
  sensorsLoop();       // update local sensors
  digitalWrite(LED_PIN, player2Connected ? HIGH : LOW);
}
