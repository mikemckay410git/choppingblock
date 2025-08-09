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

// Local sensor update tracking
unsigned long lastLocalUpdateTime = 0;
const unsigned long localUpdateInterval = 100; // 100ms for local sensor updates
uint16_t lastLocalCap[5] = {0};
uint16_t lastLocalShock = 0;

// Calibration and baseline tracking
bool calibrationMode = true;
bool baselineEstablished = false;
uint16_t baselineCap[5] = {0};
uint16_t baselineShock = 0;
const int BASELINE_SAMPLES = 50; // Number of samples to establish baseline
int baselineSampleCount = 0;

// Calibration state
int currentCalibrationSensor = 0;
const int CALIBRATION_SENSORS = 5;
bool calibrationComplete = false;
unsigned long calibrationStartTime = 0;
const unsigned long CALIBRATION_TIMEOUT = 30000; // 30 seconds timeout

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
    .calibrating-cell { background: #ff9800 !important; color: white; font-weight: bold; animation: pulse 1s infinite; }
    .latency-info { font-size: 0.9em; color: #666; margin: 10px 0; }
    .update-counter { font-size: 0.8em; color: #999; }
    
    @keyframes pulse {
      0% { opacity: 1; }
      50% { opacity: 0.7; }
      100% { opacity: 1; }
    }
  </style>
</head>
<body>
  <h1>Tool Target Game</h1>
  <div id="status" class="status-light"></div>
  <div class="latency-info">
    <div>Balanced performance: ~13 Hz updates</div>
    <div>Updates: <span id="updateCounter">0</span></div>
    <button id="launchCalibrationBtn" onclick="launchCalibration()" style="background: #2196F3; color: white; border: none; padding: 10px 20px; border-radius: 5px; cursor: pointer; margin: 10px 0;">🔧 Launch Calibration Wizard</button>
  </div>
  
  <!-- Calibration Wizard -->
  <div id="calibrationWizard" style="display: none; background: #f0f0f0; padding: 20px; margin: 20px 0; border-radius: 10px;">
    <h3>🔧 Sensor Calibration Wizard</h3>
    <div id="calibrationStatus" style="color: orange; font-weight: bold; font-size: 1.2em; margin: 10px 0;">Starting calibration...</div>
    <div id="calibrationProgress" style="margin: 10px 0;">
      <div style="background: #ddd; height: 20px; border-radius: 10px; overflow: hidden;">
        <div id="progressBar" style="background: #4CAF50; height: 100%; width: 0%; transition: width 0.3s;"></div>
      </div>
      <div id="progressText" style="margin-top: 5px; font-size: 0.9em;">0/5 sensors calibrated</div>
    </div>
    <div id="currentSensorDisplay" style="font-size: 1.1em; margin: 10px 0; padding: 10px; background: #fff; border-radius: 5px;">
      <strong>Current Sensor:</strong> <span id="currentSensorName">Cap 0</span>
    </div>
    <div id="sensorInstructions" style="margin: 10px 0; padding: 10px; background: #e3f2fd; border-radius: 5px;">
      <strong>Instructions:</strong> Press and hold the highlighted sensor for 2 seconds
    </div>
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
  
  // Global calibration state
  window.calibrationActive = false;
  
  socket.onopen = () => console.log('WS open - Low latency mode active');
  socket.onmessage = evt => {
    const data = JSON.parse(evt.data);
    updateCount++;
    
    // Update counter
    document.getElementById('updateCounter').textContent = updateCount;
    
    // Update calibration wizard
    const calibrationWizard = document.getElementById('calibrationWizard');
    const calibrationDiv = document.getElementById('calibrationStatus');
    const progressBar = document.getElementById('progressBar');
    const progressText = document.getElementById('progressText');
    const currentSensorName = document.getElementById('currentSensorName');
    const sensorInstructions = document.getElementById('sensorInstructions');
    
    // Update calibration wizard only if user initiated it
    if (window.calibrationActive) {
      if (data.calibrating) {
        // Update calibration progress
        calibrationDiv.textContent = `Calibrating sensor ${data.currentSensor + 1} of 5...`;
        calibrationDiv.style.color = 'orange';
        
        // Update progress
        const progress = (data.currentSensor / 5) * 100;
        progressBar.style.width = progress + '%';
        progressText.textContent = `${data.currentSensor}/5 sensors calibrated`;
        
        // Update current sensor
        currentSensorName.textContent = `Cap ${data.currentSensor}`;
        currentSensorName.style.color = '#2196F3';
        currentSensorName.style.fontWeight = 'bold';
        
        // Update instructions
        sensorInstructions.innerHTML = '<strong>Instructions:</strong> Press and hold <strong>Cap ' + data.currentSensor + '</strong> for 2 seconds';
        sensorInstructions.style.background = '#fff3cd';
        sensorInstructions.style.border = '2px solid #ffc107';
      } else {
        // Calibration complete
        calibrationDiv.textContent = '✅ Calibration complete - Ready for use';
        calibrationDiv.style.color = 'green';
        
        // Hide wizard after a short delay
        setTimeout(() => {
          calibrationWizard.style.display = 'none';
          window.calibrationActive = false;
          resetCalibrationButton();
        }, 2000);
      }
    }
    
    // status
    const statusDiv = document.getElementById('status');
    data.connected ? statusDiv.classList.add('connected') : statusDiv.classList.remove('connected');

    // Baseline-aware smart detection for subtle changes and crosstalk compensation
    const shockThreshold = 20;
    
    // Collect all sensor values and calculate deviations from baseline
    let sensorValues = [];
    let deviations = [];
    let maxDeviation = 0;
    let minDeviation = 999;
    
    // Estimated baseline values (these would ideally come from the server)
    // For now, we'll use the first few values as baseline
    const estimatedBaselines = [2, 2, 2, 2, 2]; // Will be updated by server
    
    for (let i = 0; i < 5; i++) {
      const val1 = data[`cap${i}_1`] || 0;
      const val2 = data[`cap${i}_2`] || 0;
      const maxVal = Math.max(val1, val2);
      sensorValues.push(maxVal);
      
      // Calculate deviation from baseline
      const deviation = Math.max(0, maxVal - estimatedBaselines[i]);
      deviations.push(deviation);
      maxDeviation = Math.max(maxDeviation, deviation);
      minDeviation = Math.min(minDeviation, deviation);
    }
    
    // Calculate dynamic thresholds based on deviations
    const deviationRange = maxDeviation - minDeviation;
    const averageDeviation = deviations.reduce((a, b) => a + b, 0) / deviations.length;
    
    // Adaptive threshold system based on deviations
    let dynamicThresholds = [];
    for (let i = 0; i < 5; i++) {
      const deviation = deviations[i];
      
      // If there's a clear leader (much higher deviation than others), use relative detection
      if (maxDeviation > averageDeviation * 2 && maxDeviation > 3) {
        dynamicThresholds.push(maxDeviation * 0.4); // 40% of max deviation for clear leaders
      } else {
        // For subtle changes, use relative to average deviation
        const relativeThreshold = averageDeviation + (deviationRange * 0.3); // 30% above average
        dynamicThresholds.push(Math.max(1, relativeThreshold));
      }
    }
    
    // update cells with baseline-aware detection and calibration highlighting
    for (let i = 0; i < 5; i++) {
      ['1','2'].forEach(p => {
        const key = `cap${i}_${p}`;
        const cell = document.getElementById(key);
        const v = data[key];
        cell.innerText = v;
        
        // Clear previous highlighting
        cell.classList.remove('triggered-cell', 'calibrating-cell');
        
        if (data.calibrating && i === data.currentSensor) {
          // Highlight current sensor being calibrated
          cell.classList.add('calibrating-cell');
        } else {
          // Use deviation-based threshold for each sensor
          const deviation = deviations[i];
          const threshold = dynamicThresholds[i];
          deviation > threshold ? cell.classList.add('triggered-cell') : cell.classList.remove('triggered-cell');
        }
      });
    }
    ['1','2'].forEach(p => {
      const key = `shock_${p}`;
      const cell = document.getElementById(key);
      const v = data[key];
      cell.innerText = v;
      v > shockThreshold ? cell.classList.add('triggered-cell') : cell.classList.remove('triggered-cell');
    });
  };
  
  socket.onerror = (error) => console.log('WebSocket error:', error);
  socket.onclose = () => console.log('WebSocket closed');
  
  // Calibration functions
  function launchCalibration() {
    const wizard = document.getElementById('calibrationWizard');
    const button = document.getElementById('launchCalibrationBtn');
    
    // Set calibration active flag
    window.calibrationActive = true;
    
    // Show wizard and disable button
    wizard.style.display = 'block';
    button.disabled = true;
    button.textContent = 'Calibrating...';
    button.style.background = '#ccc';
    
    // Reset wizard state
    const calibrationDiv = document.getElementById('calibrationStatus');
    const progressBar = document.getElementById('progressBar');
    const progressText = document.getElementById('progressText');
    const currentSensorName = document.getElementById('currentSensorName');
    const sensorInstructions = document.getElementById('sensorInstructions');
    
    calibrationDiv.textContent = 'Starting calibration...';
    calibrationDiv.style.color = 'orange';
    progressBar.style.width = '0%';
    progressText.textContent = '0/5 sensors calibrated';
    currentSensorName.textContent = 'Waiting...';
    sensorInstructions.innerHTML = '<strong>Instructions:</strong> Waiting for server response...';
    
    // Send calibration request to server
    if (socket.readyState === WebSocket.OPEN) {
      socket.send(JSON.stringify({action: 'startCalibration'}));
    }
  }
  
  function resetCalibrationButton() {
    const button = document.getElementById('launchCalibrationBtn');
    button.disabled = false;
    button.textContent = '🔧 Launch Calibration Wizard';
    button.style.background = '#2196F3';
  }
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
      // Sensor update - always update remote data
      for (int i = 0; i < 5; i++) {
        lastRemoteCap[i] = msg.cap[i];
        remoteCap[i] = msg.cap[i];
      }
      lastRemoteShock = msg.shock;
      remoteShock = msg.shock;
      
      // Always broadcast when Player2 data arrives (if rate limit allows)
      if (millis() - lastBroadcastTime >= broadcastInterval) {
        // build JSON with both local & remote
              String j = "{";
      j += "\"connected\":" + String(player2Connected ? "true" : "false") + ",";
      j += "\"calibrating\":" + String(calibrationMode ? "true" : "false") + ",";
      j += "\"currentSensor\":" + String(currentCalibrationSensor) + ",";
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

void handleWebSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.printf("[%u] Disconnected!\n", num);
      break;
    case WStype_CONNECTED:
      Serial.printf("[%u] Connected!\n", num);
      break;
    case WStype_TEXT:
      // Handle calibration request
      if (length > 0) {
        String message = String((char*)payload);
        if (message.indexOf("startCalibration") != -1) {
          Serial.println("Calibration requested from web interface");
          // Restart calibration mode
          calibrationMode = true;
          calibrationComplete = false;
          currentCalibrationSensor = 0;
          calibrationStartTime = millis();
          baselineEstablished = false;
          Serial.println("=== CALIBRATION MODE RESTARTED ===");
          Serial.println("Please press each sensor in order (Cap 0, 1, 2, 3, 4)");
          Serial.println("Starting with Cap 0...");
        }
      }
      break;
  }
}

void handleCalibration() {
  static unsigned long lastCalibrationCheck = 0;
  static bool sensorDetected = false;
  static unsigned long sensorStartTime = 0;
  const unsigned long SENSOR_HOLD_TIME = 2000; // 2 seconds hold time
  
  unsigned long now = millis();
  
  // Check for timeout
  if (now - calibrationStartTime > CALIBRATION_TIMEOUT) {
    Serial.println("Calibration timeout! Starting normal operation...");
    calibrationMode = false;
    return;
  }
  
  // Check current sensor value
  int currentValue = getCapacitiveValue(currentCalibrationSensor);
  
  if (!sensorDetected && currentValue > 5) {
    // Sensor pressed
    sensorDetected = true;
    sensorStartTime = now;
    Serial.print("Cap "); Serial.print(currentCalibrationSensor); Serial.println(" pressed - hold for 2 seconds...");
  } else if (sensorDetected && currentValue <= 5) {
    // Sensor released too early
    sensorDetected = false;
    Serial.print("Cap "); Serial.print(currentCalibrationSensor); Serial.println(" released too early - try again");
  } else if (sensorDetected && (now - sensorStartTime) >= SENSOR_HOLD_TIME) {
    // Sensor held long enough - move to next
    Serial.print("Cap "); Serial.print(currentCalibrationSensor); Serial.println(" calibrated!");
    
    currentCalibrationSensor++;
    sensorDetected = false;
    
    if (currentCalibrationSensor >= CALIBRATION_SENSORS) {
      // All sensors calibrated
      calibrationComplete = true;
      calibrationMode = false;
      Serial.println("=== CALIBRATION COMPLETE ===");
      Serial.println("All sensors calibrated successfully!");
      Serial.println("Starting baseline collection...");
    } else {
      Serial.print("Now press Cap "); Serial.print(currentCalibrationSensor); Serial.println("...");
    }
  }
}

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
  ws.onEvent(handleWebSocketEvent);

  // ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    while(true);
  }
  esp_now_register_recv_cb(onDataRecv);

  sensorsInit();
  Serial.println("Player 1 ready - AP and ESP-NOW active");
  
  // Initialize local sensor tracking
  for (int i = 0; i < 5; i++) {
    lastLocalCap[i] = getCapacitiveValue(i);
  }
  lastLocalShock = getShockValue();
  
  // Start calibration mode
  calibrationMode = true;
  calibrationComplete = false;
  currentCalibrationSensor = 0;
  calibrationStartTime = millis();
  baselineEstablished = false;
  Serial.println("=== CALIBRATION MODE ===");
  Serial.println("Please press each sensor in order (Cap 0, 1, 2, 3, 4)");
  Serial.println("Hold each sensor for 2-3 seconds, then release");
  Serial.println("Starting with Cap 0...");
  
  // Send initial data to web interface
  String j = "{";
  j += "\"connected\":false,";
  for (int i = 0; i < 5; i++) {
    j += "\"cap" + String(i) + "_1\":" + String(getCapacitiveValue(i)) + ",";
    j += "\"cap" + String(i) + "_2\":0,";
  }
  j += "\"shock_1\":" + String(getShockValue()) + ",";
  j += "\"shock_2\":0}";
  ws.broadcastTXT(j);
}

void loop() {
  server.handleClient(); // handle web server requests
  ws.loop();           // handle WS
  sensorsLoop();       // update local sensors
  
  // Handle calibration mode or broadcast sensor data
  unsigned long now = millis();
  
  if (now - lastLocalUpdateTime >= localUpdateInterval) {
    if (calibrationMode && !calibrationComplete) {
      // Calibration mode - guide user through pressing each sensor
      handleCalibration();
    } else if (!baselineEstablished) {
      // Collect baseline samples after calibration
      for (int i = 0; i < 5; i++) {
        baselineCap[i] += getCapacitiveValue(i);
      }
      baselineShock += getShockValue();
      baselineSampleCount++;
      
      if (baselineSampleCount >= BASELINE_SAMPLES) {
        // Calculate average baseline
        for (int i = 0; i < 5; i++) {
          baselineCap[i] /= BASELINE_SAMPLES;
        }
        baselineShock /= BASELINE_SAMPLES;
        baselineEstablished = true;
        Serial.println("Baseline established!");
        Serial.print("Cap baselines: ");
        for (int i = 0; i < 5; i++) {
          Serial.print(baselineCap[i]); Serial.print(" ");
        }
        Serial.println();
        Serial.print("Shock baseline: "); Serial.println(baselineShock);
      }
    }
    
    // Always broadcast current sensor data (not just on changes)
    if (now - lastBroadcastTime >= broadcastInterval) {
      String j = "{";
      j += "\"connected\":" + String(player2Connected ? "true" : "false") + ",";
      for (int i = 0; i < 5; i++) {
        j += "\"cap" + String(i) + "_1\":" + String(getCapacitiveValue(i)) + ",";
        j += "\"cap" + String(i) + "_2\":" + String(remoteCap[i]) + ",";
      }
      j += "\"shock_1\":" + String(getShockValue()) + ",";
      j += "\"shock_2\":" + String(remoteShock) + "}";
      
      ws.broadcastTXT(j);
      lastBroadcastTime = now;
    }
    
    lastLocalUpdateTime = now;
  }
  
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
