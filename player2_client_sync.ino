#include <WiFi.h>
#include <esp_now.h>

#define LED_PIN 2

// ===================== USER CONFIG =====================
static const int SENSOR_COUNT = 4;
// Index order: 0=Top (GPIO35), 1=Bottom (GPIO33), 2=Right (GPIO34), 3=Left (GPIO32)
static const uint8_t SENSOR_PINS[SENSOR_COUNT] = {35, 33, 34, 32};

// Catch both polarities while debugging; later change to RISING or FALLING
static const int EDGE_MODE = CHANGE;

// Board coordinates: top-left origin (0,0) to (0.4,0.4) meters
// Sensor positions in METERS matching index order above
static const float SX[SENSOR_COUNT] = {0.200f, 0.200f, 0.300f, 0.100f};
static const float SY[SENSOR_COUNT] = {0.100f, 0.300f, 0.200f, 0.200f};

// Board bounds (meters) used to keep solutions on the board
static const float BOARD_SIZE_M = 0.4f;
static const float BOARD_MIN_X = 0.0f;
static const float BOARD_MAX_X = BOARD_SIZE_M;
static const float BOARD_MIN_Y = 0.0f;
static const float BOARD_MAX_Y = BOARD_SIZE_M;
// Solver acceptance threshold (meters RMS residual). Tune based on noise.
static const float SOLVER_RMS_THRESH_M = 0.02f; // 20 mm

// Estimated plate wave speed (tune this)
static float V_SOUND = 3000.0f; // m/s

// Timing
static const unsigned long CAPTURE_WINDOW_US     = 8000; // wide for debugging
static const unsigned long DEADTIME_MS           = 120;  // ms quiet before re‑arm
// =======================================================

// ===================== ESP-NOW Configuration =====================
// MAC of Player 1 (replace with your actual address)
uint8_t player1Address[] = { 0x78, 0x1C, 0x3C, 0xB8, 0xD5, 0xA9 };
const uint8_t ESPNOW_BROADCAST_ADDR[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

typedef struct struct_message {
  uint8_t  playerId;   // 1=Player1, 2=Player2
  uint8_t  action;     // 1=heartbeat, 2=hit-detected, 3=reset-request, 4=clock-sync
  uint32_t hitTime;    // micros() timestamp of hit
  uint16_t hitStrength; // impact strength
  uint32_t syncTime;   // for clock synchronization
  uint32_t roundTripTime; // for latency measurement
} struct_message;

struct_message myData;
struct_message player1Data;

// Connection tracking
bool player1Connected = false;
unsigned long lastHeartbeat = 0;
const unsigned long heartbeatTimeout = 2000; // 2 seconds
bool player1MacLearned = false;

// Clock synchronization (client only responds; host paces requests)
bool clockSynced = false;
int32_t clockOffset = 0; // Player1 time - Player2 time
uint32_t lastSyncTime = 0;
// =======================================================

// ===================== Game State =====================
volatile unsigned long g_firstTime[SENSOR_COUNT]; // first arrival micros() per sensor
volatile uint32_t      g_hitMask = 0;             // bit i set when sensor i latched first arrival
volatile bool          g_armed = true;            // ready for new hit
volatile bool          g_capturing = false;       // capture window open
volatile unsigned long g_t0 = 0;                  // first edge time (µs)

// ISR-light start handoff
volatile bool          g_startPending = false;    // main loop should start capture
volatile int           g_firstIndex   = -1;       // who triggered first (for debug)

// Edge debug counters (for UI table)
volatile uint16_t g_edgeCount[SENSOR_COUNT]      = {0,0,0,0};
volatile unsigned long g_lastEdgeUs[SENSOR_COUNT]= {0,0,0,0};

struct HitResult {
  bool   valid = false;
  float  x = 0, y = 0;  // meters in same top-left frame
  int    haveTimes = 0;
  String mode = "none";
  uint32_t hitTime = 0;
  uint16_t hitStrength = 0;
} g_lastHit;

// Game state
bool gameActive = false;
String winner = "none";
uint32_t player1HitTime = 0;
uint32_t player2HitTime = 0;
// =======================================================

// ===================== Math: TDoA solver =====================
static bool tdoaSolve(const float *sx, const float *sy,
                      const unsigned long *t, float vs,
                      float &x, float &y, int &nUsed)
{
  // Count available timestamps
  int have = 0;
  for (int i = 0; i < SENSOR_COUNT; i++) if (t[i] != 0) have++;
  nUsed = have;
  if (have < 3) return false;

  // Choose reference as earliest arrival
  int ref = 0; unsigned long tref = ~0u;
  for (int i = 0; i < SENSOR_COUNT; i++) {
    if (t[i] && t[i] < tref) { tref = t[i]; ref = i; }
  }

  // Precompute TDoA (meters) relative to reference for used sensors
  double dd[SENSOR_COUNT];
  bool use[SENSOR_COUNT];
  int rows = 0;
  for (int i = 0; i < SENSOR_COUNT; i++) {
    if (i == ref || t[i] == 0) { use[i] = false; continue; }
    double dt_us = double(t[i]) - double(tref);
    dd[i] = double(vs) * dt_us * 1e-6; // meters
    use[i] = true;
    rows++;
  }
  if (rows < 2) return false;

  // Initial guess: board center (or mean of sensor positions)
  double xg = 0.0, yg = 0.0; int npos = 0;
  for (int i = 0; i < SENSOR_COUNT; i++) { xg += sx[i]; yg += sy[i]; npos++; }
  xg /= (npos > 0 ? npos : 1);
  yg /= (npos > 0 ? npos : 1);

  // Keep iterations stable
  const double eps = 1e-9;
  const int maxIter = 15;
  const double damping = 1e-6; // Levenberg damping

  bool brokeSingular = false;
  for (int it = 0; it < maxIter; it++) {
    // Distances to reference and Jacobian accumulation
    double dxr = xg - sx[ref];
    double dyr = yg - sy[ref];
    double Dr = sqrt(dxr*dxr + dyr*dyr); if (Dr < eps) Dr = eps;

    double ATA00 = 0.0, ATA01 = 0.0, ATA11 = 0.0;
    double ATb0 = 0.0, ATb1 = 0.0;

    for (int i = 0; i < SENSOR_COUNT; i++) {
      if (!use[i]) continue;
      double dxi = xg - sx[i];
      double dyi = yg - sy[i];
      double Di = sqrt(dxi*dxi + dyi*dyi); if (Di < eps) Di = eps;

      // Residual: (Di - Dr) - dd[i] = 0
      double ri = (Di - Dr) - dd[i];

      // Jacobian wrt x and y
      double dFdx = (dxi/Di) - (dxr/Dr);
      double dFdy = (dyi/Di) - (dyr/Dr);

      // Accumulate normal equations
      ATA00 += dFdx * dFdx;
      ATA01 += dFdx * dFdy;
      ATA11 += dFdy * dFdy;
      ATb0  += dFdx * ri;
      ATb1  += dFdy * ri;
    }

    // Solve (J^T J + lambda I) delta = -J^T r
    ATA00 += damping; ATA11 += damping;
    double det = ATA00 * ATA11 - ATA01 * ATA01;
    if (fabs(det) < 1e-12) { brokeSingular = true; break; }

    double inv00 =  ATA11 / det;
    double inv01 = -ATA01 / det;
    double inv11 =  ATA00 / det;

    double dx = -(inv00 * ATb0 + inv01 * ATb1);
    double dy = -(inv01 * ATb0 + inv11 * ATb1);

    // Limit step size to keep stable
    double stepNorm = sqrt(dx*dx + dy*dy);
    const double maxStep = 0.05; // meters per iteration
    if (stepNorm > maxStep) {
      dx *= (maxStep / stepNorm);
      dy *= (maxStep / stepNorm);
    }

    xg += dx; yg += dy;

    if (sqrt(dx*dx + dy*dy) < 1e-4) break; // ~0.1 mm
  }

  if (brokeSingular) return false;

  // Compute RMS residual to assess fit quality
  {
    double dxr = xg - sx[ref];
    double dyr = yg - sy[ref];
    double Dr = sqrt(dxr*dxr + dyr*dyr);
    if (Dr < 1e-9) Dr = 1e-9;
    double rss = 0.0; int m = 0;
    for (int i = 0; i < SENSOR_COUNT; i++) {
      if (!use[i]) continue;
      double dxi = xg - sx[i];
      double dyi = yg - sy[i];
      double Di = sqrt(dxi*dxi + dyi*dyi); if (Di < 1e-9) Di = 1e-9;
      double ri = (Di - Dr) - dd[i];
      rss += ri * ri;
      m++;
    }
    if (m >= 2) {
      double rms = sqrt(rss / m);
      if (rms > SOLVER_RMS_THRESH_M) return false;
    }
  }

  // Clamp to board bounds to avoid off-board estimates due to noise
  if (xg < BOARD_MIN_X) xg = BOARD_MIN_X; else if (xg > BOARD_MAX_X) xg = BOARD_MAX_X;
  if (yg < BOARD_MIN_Y) yg = BOARD_MIN_Y; else if (yg > BOARD_MAX_Y) yg = BOARD_MAX_Y;

  x = float(xg);
  y = float(yg);
  return true;
}

// ===================== ISRs (ultra-minimal) =====================
static inline void IRAM_ATTR latch_time_min(int i) {
  unsigned long now = micros(); // safe on ESP32 core

  // count every edge for debug
  g_edgeCount[i]++;
  g_lastEdgeUs[i] = now;

  // First edge -> ask main loop to start capture
  if (g_armed && !g_capturing && !g_startPending) {
    g_t0 = now;
    g_firstIndex = i;
    g_startPending = true; // main loop will open window & light LED
  }

  // If capture already open (or about to open), latch first time for this sensor
  if (g_capturing || g_startPending) {
    if (!(g_hitMask & (1u << i))) {
      g_hitMask |= (1u << i);
      g_firstTime[i] = now;
    }
  }
}

void IRAM_ATTR edgeISR0(){ latch_time_min(0); }
void IRAM_ATTR edgeISR1(){ latch_time_min(1); }
void IRAM_ATTR edgeISR2(){ latch_time_min(2); }
void IRAM_ATTR edgeISR3(){ latch_time_min(3); }
void (*ISR_FUN[SENSOR_COUNT])() = { edgeISR0, edgeISR1, edgeISR2, edgeISR3 };

// ===================== ESP-NOW Callbacks =====================
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(struct_message)) return;
  
  memcpy(&player1Data, data, sizeof(player1Data));

  if (player1Data.playerId == 1) {
    // Learn Player 1 MAC dynamically
    if (info && !player1MacLearned) {
      memcpy(player1Address, info->src_addr, 6);
      char macStr[18];
      sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", player1Address[0],player1Address[1],player1Address[2],player1Address[3],player1Address[4],player1Address[5]);
      Serial.printf("Discovered Player 1 MAC: %s\r\n", macStr);
      esp_now_del_peer(player1Address);
      esp_now_peer_info_t p = {};
      memcpy(p.peer_addr, player1Address, 6);
      p.channel = 0;
      p.encrypt = false;
      if (esp_now_add_peer(&p) == ESP_OK) {
        player1MacLearned = true;
        Serial.println("Player 1 peer added after discovery");
      } else {
        Serial.println("Failed to add discovered Player 1 peer");
      }
    }
    player1Connected = true;
    lastHeartbeat = millis();

    if (player1Data.action == 1) {
      // Heartbeat - just update connection status
      Serial.println("Player 1 heartbeat received");
    } else if (player1Data.action == 2) {
      // Hit detected by Player 1
      // Convert Player 1's timestamp to our time reference
      uint32_t adjustedTime = player1Data.hitTime + clockOffset;
      player1HitTime = adjustedTime;
      Serial.printf("Player 1 hit detected at %lu (adjusted from %lu) with strength %d\n", 
                   adjustedTime, player1Data.hitTime, player1Data.hitStrength);
      
      // Determine winner if we have both hits
      if (player2HitTime > 0) {
        determineWinner();
      }
    } else if (player1Data.action == 3) {
      // Reset request from Player 1
      resetGame();
    } else if (player1Data.action == 4) {
      // Clock synchronization request
      uint32_t now = micros();
      uint32_t roundTrip = now - player1Data.syncTime;
      
      // Send back our current time and the round trip time
      myData.action = 4; // clock sync response
      myData.syncTime = player1Data.syncTime; // echo back the original sync time
      myData.roundTripTime = now; // our current time
      esp_now_send(player1Address, (uint8_t*)&myData, sizeof(myData));
      
      Serial.printf("Clock sync response sent, roundTrip=%lu us\n", roundTrip);
    }
  }
}

// ===================== Game Logic =====================
void determineWinner() {
  if (player1HitTime > 0 && player2HitTime > 0) {
    // Calculate time difference in microseconds
    int32_t timeDiff = (int32_t)player1HitTime - (int32_t)player2HitTime;
    
    if (timeDiff < -100) { // Player 2 hit first (with 100us tolerance)
      winner = "Player 2";
      Serial.printf("Player 2 wins! Time diff: %ld us\n", -timeDiff);
    } else if (timeDiff > 100) { // Player 1 hit first (with 100us tolerance)
      winner = "Player 1";
      Serial.printf("Player 1 wins! Time diff: %ld us\n", timeDiff);
    } else {
      winner = "Tie";
      Serial.printf("It's a tie! Time diff: %ld us\n", timeDiff);
    }
  }
}

void resetGame() {
  winner = "none";
  player1HitTime = 0;
  player2HitTime = 0;
  gameActive = true;
  
  Serial.println("Game reset");
}

// ===================== Setup =====================
void setup(){
  Serial.begin(115200);
  delay(50);
  Serial.println();
  Serial.println(F("=== Two Player Impact Game - Player 2 (Client) - SYNC VERSION ==="));
  Serial.printf("GPIOs: [%d, %d, %d, %d]\r\n", SENSOR_PINS[0],SENSOR_PINS[1],SENSOR_PINS[2],SENSOR_PINS[3]);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  for(int i=0;i<SENSOR_COUNT;i++){
    pinMode(SENSOR_PINS[i], INPUT);
  }

  // Wi-Fi setup for ESP-NOW
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(); // Ensure clean state
  Serial.printf("STA MAC: %s\r\n", WiFi.macAddress().c_str());

  // ESP-NOW setup
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    while(true);
  }
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, player1Address, 6);
  peerInfo.channel = 0; // follow current channel
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add Player 1 peer (check MAC)");
    Serial.printf("Configured peer MAC: %02X:%02X:%02X:%02X:%02X:%02X\r\n",
                  player1Address[0], player1Address[1], player1Address[2],
                  player1Address[3], player1Address[4], player1Address[5]);
  }

  myData.playerId = 2;

  // Attach interrupts
  for(int i=0;i<SENSOR_COUNT;i++){
    attachInterrupt(digitalPinToInterrupt(SENSOR_PINS[i]), ISR_FUN[i], EDGE_MODE);
  }
  Serial.println(F("Interrupts attached. Waiting for hits..."));

  // Arm
  g_armed = true;
  g_capturing = false;
  g_startPending = false;
  g_firstIndex = -1;
  g_hitMask = 0;
  for(int i=0;i<SENSOR_COUNT;i++){
    g_firstTime[i]=0; g_edgeCount[i]=0; g_lastEdgeUs[i]=0;
  }

  // Initialize game state
  gameActive = true;
  winner = "none";
  player1HitTime = 0;
  player2HitTime = 0;
  clockSynced = false;
  clockOffset = 0;
  
  Serial.println("Player 2 ready - waiting for Player 1 connection");
}

// ===================== Loop =====================
void loop(){
  const unsigned long nowUs = micros();
  const unsigned long nowMs = millis();

  // If ISR asked us to start, open the capture window here (NOT inside ISR)
  if (g_startPending) {
    noInterrupts();
    g_capturing = true;
    g_armed = false;
    // clear edge counters for this window
    for (int k=0;k<SENSOR_COUNT;k++){ g_edgeCount[k]=0; g_lastEdgeUs[k]=0; }
    // ensure the very first sensor time exists
    if (g_firstIndex >= 0 && !(g_hitMask & (1u << g_firstIndex))) {
      g_hitMask |= (1u << g_firstIndex);
      g_firstTime[g_firstIndex] = g_t0;
    }
    g_startPending = false;
    interrupts();

    digitalWrite(LED_PIN, HIGH);
    Serial.println(F(">> Capture started"));
    Serial.printf("t0=%lu, first sensor=%d\r\n", g_t0, g_firstIndex);
  }

  // If capturing, close window after CAPTURE_WINDOW_US and process
  if (g_capturing && (nowUs - g_t0) >= CAPTURE_WINDOW_US) {
    // Copy volatile data atomically
    noInterrupts();
    unsigned long tcopy[SENSOR_COUNT];
    uint32_t mask = g_hitMask;
    unsigned long t0copy = g_t0;
    unsigned long lastEdgeCopy[SENSOR_COUNT];
    uint16_t cntCopy[SENSOR_COUNT];
    for (int i=0;i<SENSOR_COUNT;i++){
      tcopy[i]        = g_firstTime[i];
      lastEdgeCopy[i] = g_lastEdgeUs[i];
      cntCopy[i]      = g_edgeCount[i];
    }
    // end capture
    g_capturing = false;
    interrupts();

    // ============ SERIAL DEBUG ============
    Serial.println(F("---- Capture ----"));
    Serial.print(F("t0=")); Serial.println(t0copy);
    Serial.print(F("mask=0b")); Serial.println(mask, BIN);
    for (int i=0;i<SENSOR_COUNT;i++){
      Serial.print(F("S")); Serial.print(i); Serial.print(F(": "));
      if (tcopy[i]) {
        long dt = (long)(tcopy[i] - t0copy);
        Serial.print(dt); Serial.print(F(" us"));
      } else {
        Serial.print(F("-"));
      }
      Serial.print(F("  | last="));
      if (lastEdgeCopy[i]) Serial.print((long)(lastEdgeCopy[i]-t0copy));
      else Serial.print(F("-"));
      Serial.print(F(" us, cnt=")); Serial.println(cntCopy[i]);
    }

    // Count timestamps
    int have=0; for(int i=0;i<SENSOR_COUNT;i++) if (tcopy[i]) have++;

    HitResult r;
    r.haveTimes = have;
    r.hitTime = t0copy;
    r.hitStrength = have; // Use number of sensors as strength indicator
    
    if (have >= 3){
      float x,y; int nUsed;
      if (tdoaSolve(SX,SY,tcopy,V_SOUND,x,y,nUsed)){
        r.valid = true; r.x=x; r.y=y; r.mode="tdoa";
      }
    }
    if (!r.valid){
      // Fallback: earliest sensor heuristic (cheap & dirty)
      int first=-1; unsigned long tf=~0u;
      for(int i=0;i<SENSOR_COUNT;i++) if(tcopy[i] && tcopy[i]<tf){ tf=tcopy[i]; first=i; }
      if (first>=0){ r.valid=true; r.x=SX[first]*0.8f; r.y=SY[first]*0.8f; r.mode = (have>=2) ? "partial" : "nearest"; }
    }

    // Record Player 2 hit
    if (r.valid && gameActive) {
      player2HitTime = r.hitTime;
      Serial.printf("Player 2 hit detected at %lu with strength %d\n", player2HitTime, r.hitStrength);
      
      // Send hit to Player 1
      myData.action = 2; // hit detected
      myData.hitTime = r.hitTime;
      myData.hitStrength = r.hitStrength;
      esp_now_send(player1Address, (uint8_t*)&myData, sizeof(myData));
      
      // Determine winner if we have both hits
      if (player1HitTime > 0) {
        determineWinner();
      }
    }

    // Store for reference
    if (r.valid) { g_lastHit = r; }
    else { g_lastHit.valid = false; }

    // Deadtime
    unsigned long start = nowMs;
    while (millis()-start < DEADTIME_MS) { delay(1); }

    // Re-arm
    noInterrupts();
    g_armed = true;
    g_hitMask = 0;
    g_firstIndex = -1;
    for(int i=0;i<SENSOR_COUNT;i++){
      g_firstTime[i]=0; g_edgeCount[i]=0; g_lastEdgeUs[i]=0;
    }
    interrupts();
    digitalWrite(LED_PIN, LOW);
    Serial.println(F("<< Re-armed"));
  }

  // Check for connection timeout
  if (player1Connected && (millis() - lastHeartbeat > heartbeatTimeout)) {
    player1Connected = false;
    clockSynced = false; // Reset sync when connection lost
    Serial.println("Player 1 connection lost");
  }

  // Send heartbeat to Player 1
  static unsigned long lastHeartbeatSend = 0;
  if (millis() - lastHeartbeatSend >= 1000) {
    myData.action = 1; // heartbeat
    if (player1MacLearned) {
      esp_now_send(player1Address, (uint8_t*)&myData, sizeof(myData));
    }
    // broadcast to help discovery by Player 1
    esp_now_send(ESPNOW_BROADCAST_ADDR, (uint8_t*)&myData, sizeof(myData));
    lastHeartbeatSend = millis();
  }

  // Update LED based on connection status
  digitalWrite(LED_PIN, player1Connected ? HIGH : LOW);
}
