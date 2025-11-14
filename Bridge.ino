#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>

#define LED_PIN 2

// ===================== USER CONFIG =====================
// ESP-NOW Bridge: communicates between Raspberry Pi and other ESP32s
static const unsigned long HEARTBEAT_INTERVAL_MS = 1000;   // heartbeat to players
static const unsigned long SERIAL_TIMEOUT_MS = 100;        // serial read timeout
// =======================================================

// ===================== ESP-NOW Configuration =====================
// Player 1 MAC address (hardcoded for reliability)
uint8_t player1Address[] = {0x6C, 0xC8, 0x40, 0x4E, 0xEC, 0x2C}; // Player 1 STA MAC
// Player 2 MAC address (hardcoded for reliability)
uint8_t player2Address[] = {0x80, 0xF3, 0xDA, 0x5E, 0x14, 0xC8}; // Player 2 STA MAC
// Lightboard MAC address (will be learned dynamically)
uint8_t lightboardAddress[] = {0x78, 0x1C, 0x3C, 0xB8, 0xD5, 0xA8}; // Lightboard STA MAC
const uint8_t ESPNOW_BROADCAST_ADDR[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

typedef struct struct_message {
  uint8_t  playerId;   // 1=Player1, 2=Player2
  uint8_t  action;     // 1=heartbeat, 2=hit-detected, 3=reset-request, 4=clock-sync
  uint32_t hitTime;    // micros() timestamp of hit
  uint16_t hitStrength; // impact strength
  uint32_t syncTime;   // for clock synchronization
  uint32_t roundTripTime; // for latency measurement
} struct_message;

typedef struct struct_lightboard_message {
  uint8_t  deviceId;     // 1=Bridge, 3=Lightboard
  uint8_t  action;       // 1=heartbeat, 2=game-state, 3=score-update, 4=mode-change, 5=reset, 6=state-restore, 7=state-request
  uint8_t  gameMode;     // 1-6 (Territory, Swap Sides, Split Scoring, Score Order, Race, Tug O War)
  uint8_t  p1ColorIndex; // Player 1 color index
  uint8_t  p2ColorIndex; // Player 2 color index
  int8_t   p1Pos;        // Player 1 position (-1 to NUM_LEDS)
  int8_t   p2Pos;        // Player 2 position (-1 to NUM_LEDS)
  uint8_t  nextLedPos;   // For Score Order mode
  uint8_t  tugBoundary;  // For Tug O War mode
  uint8_t  p1RacePos;    // For Race mode
  uint8_t  p2RacePos;    // For Race mode
  uint8_t  celebrating;  // Celebration state
  uint8_t  winner;       // 0=none, 1=Player1, 2=Player2
} struct_lightboard_message;

struct_message myData;
struct_message player1Data;
struct_message player2Data;
struct_lightboard_message lightboardData;

// Connection tracking
bool player1Connected = false;
bool player2Connected = false;
unsigned long lastHeartbeat = 0;
unsigned long lastPlayer2Heartbeat = 0;
const unsigned long heartbeatTimeout = 2000; // 2 seconds
bool player1MacLearned = false;
bool player2MacLearned = false;

// Lightboard connection tracking
bool lightboardConnected = false;
bool lightboardWasConnected = false; // Track previous connection state for reconnection detection
unsigned long lastLightboardHeartbeat = 0;
bool lightboardMacLearned = false;

// Clock synchronization
bool clockSynced = false;
bool player2ClockSynced = false;
int32_t clockOffset = 0; // Player1 time - Bridge time
int32_t player2ClockOffset = 0; // Player2 time - Bridge time
uint32_t lastSyncTime = 0;
uint32_t lastPlayer2SyncTime = 0;
const unsigned long SYNC_INTERVAL = 1000; // Sync at most once per second
// =======================================================

// ===================== Game State =====================
// Serial communication with Raspberry Pi
String serialBuffer = "";
bool piConnected = false;
unsigned long lastPiHeartbeat = 0;
const unsigned long PI_HEARTBEAT_TIMEOUT = 5000; // 5 seconds

// Function declarations
void resetGame();
void resetGameForQuiz();
void determineWinner();
void syncClock();
void sendLightboardUpdate(uint8_t action);
void updateLightboardGameState();
void sendLightboardPointUpdate(uint8_t scoringPlayer);
void sendLightboardStateRestore();
void awardPointToPlayer(uint8_t playerId);
void awardMultiplePointsToPlayer(uint8_t playerId, int multiplier);
void sendToPi(String message);
void processPiCommand(String command);

// Local result tracking removed (host-only)
unsigned long g_lastBroadcastMs = 0;

// HitResult struct removed (host-only, no local hit detection)

// Game state
bool gameActive = true;  // Start with game active
String winner = "none";
// Bridge is host-only, no local hit time needed
uint32_t player1HitTime = 0;
uint32_t player2HitTime = 0;

// Lightboard game state (for LED strip display)
int lightboardGameMode = 1; // Default to Territory mode
int lightboardP1ColorIndex = 0; // Red
int lightboardP2ColorIndex = 1; // Blue
int lightboardP1Pos = -1;
int lightboardP2Pos = 38; // NUM_LEDS
int lightboardNextLedPos = 0;
int lightboardTugBoundary = 18; // CENTER_LEFT
int lightboardP1RacePos = -1;
int lightboardP2RacePos = -1;
bool lightboardCelebrating = false;
uint8_t lightboardWinner = 0; // 0=none, 1=Player1, 2=Player2

// Quiz action debouncing
unsigned long lastQuizActionTime = 0;
const unsigned long QUIZ_ACTION_DEBOUNCE_MS = 500; // 500ms debounce period

// Hit deduplication to prevent processing duplicate ESP-NOW messages
uint32_t lastProcessedHitTime = 0;
uint8_t lastProcessedHitPlayer = 0;
const unsigned long HIT_DEBOUNCE_MS = 100; // 100ms debounce period
unsigned long lastHitProcessTime = 0;
// =======================================================

// ===================== Serial Protocol =====================
// Commands from Pi to ESP32 Bridge:
// {"cmd":"heartbeat"} - Pi heartbeat
// {"cmd":"reset"} - Reset game
// {"cmd":"awardPoint","player":1,"multiplier":3} - Award points
// {"cmd":"lightboardSettings","mode":1,"p1Color":0,"p2Color":1} - Update lightboard
// {"cmd":"quizAction","action":"next"} - Quiz navigation
//
// Messages from ESP32 Bridge to Pi:
// {"type":"status","player1Connected":true,"player2Connected":true,"lightboardConnected":true}
// {"type":"winner","winner":"Player 1"}
// {"type":"hit","player":1,"time":1234567890,"strength":100}
// {"type":"error","message":"Player 1 disconnected"}
// =======================================================

// ===================== ESP-NOW Callbacks =====================
void OnDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  // Debug: Serial.print("Send Status: ");
  // Debug: Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  // Handle different message types based on length
  if (len == sizeof(struct_message)) {
    // Player 1 or Player 2 message
    memcpy(&player1Data, data, sizeof(player1Data));
    
    if (player1Data.playerId == 1) {
    // Learn Player 1 MAC dynamically to avoid manual entry issues
    if (info && !player1MacLearned) {
      memcpy(player1Address, info->src_addr, 6);
      char macStr[18];
      sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", player1Address[0],player1Address[1],player1Address[2],player1Address[3],player1Address[4],player1Address[5]);
      // Debug: Serial.printf("Discovered Player 1 MAC: %s\r\n", macStr);
      esp_now_del_peer(player1Address); // ignore result
      esp_now_peer_info_t p = {};
      memcpy(p.peer_addr, player1Address, 6);
      p.channel = 0;
      p.encrypt = false;
      if (esp_now_add_peer(&p) == ESP_OK) {
        player1MacLearned = true;
        // Debug: Serial.println("Player 1 peer added after discovery");
      } else {
        // Debug: Serial.println("Failed to add discovered Player 1 peer");
      }
    }
    player1Connected = true;
    lastHeartbeat = millis();

    if (player1Data.action == 1) {
      // Heartbeat - just update connection status (no need to send to Pi)
    } else if (player1Data.action == 2) {
      // Hit detected by Player 1
      // Convert Player 1's timestamp to our time reference
      uint32_t adjustedTime = player1Data.hitTime + clockOffset;
      
      // Deduplication: check if this is a duplicate hit
      unsigned long now = millis();
      if (adjustedTime == lastProcessedHitTime && player1Data.playerId == lastProcessedHitPlayer && 
          (now - lastHitProcessTime) < HIT_DEBOUNCE_MS) {
        Serial.printf("Duplicate hit from Player 1 ignored (time: %lu)\n", adjustedTime);
        return;
      }
      
      // Update deduplication tracking
      lastProcessedHitTime = adjustedTime;
      lastProcessedHitPlayer = player1Data.playerId;
      lastHitProcessTime = now;
      
      player1HitTime = adjustedTime;
      Serial.printf("Player 1 hit detected at %lu (adjusted from %lu) with strength %d\n", 
                   adjustedTime, player1Data.hitTime, player1Data.hitStrength);
      
      // Send hit notification to Pi
      sendToPi("{\"type\":\"hit\",\"player\":1,\"time\":" + String(adjustedTime) + ",\"strength\":" + String(player1Data.hitStrength) + "}");
      
      // Declare winner immediately if round active
      if (gameActive) {
        winner = "Player 1";
        gameActive = false;
        sendToPi("{\"type\":\"winner\",\"winner\":\"Player 1\"}");
        // Debug: Serial.println("Winner declared: Player 1");
      }
    } else if (player1Data.action == 3) {
      // Reset request from Player 1
      resetGame();
    } else if (player1Data.action == 4) {
      // Clock synchronization response
      uint32_t now = micros();
      uint32_t roundTrip = now - player1Data.syncTime;
      uint32_t player1Time = player1Data.roundTripTime;
      
      // Calculate clock offset: (Player1 time + roundTrip/2) - Bridge time
      clockOffset = (player1Time + roundTrip/2) - now;
      clockSynced = true;
      lastSyncTime = millis();
      
      // Clock sync complete - no need to send to Pi
      // Debug: Serial.printf("Clock sync: offset=%ld us, roundTrip=%lu us\n", clockOffset, roundTrip);
    }
    } else if (player1Data.playerId == 2) {
    // Learn Player 2 MAC dynamically to avoid manual entry issues
    if (info && !player2MacLearned) {
      memcpy(player2Address, info->src_addr, 6);
      char macStr[18];
      sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", player2Address[0],player2Address[1],player2Address[2],player2Address[3],player2Address[4],player2Address[5]);
      // Debug: Serial.printf("Discovered Player 2 MAC: %s\r\n", macStr);
      esp_now_del_peer(player2Address); // ignore result
      esp_now_peer_info_t p = {};
      memcpy(p.peer_addr, player2Address, 6);
      p.channel = 0;
      p.encrypt = false;
      if (esp_now_add_peer(&p) == ESP_OK) {
        player2MacLearned = true;
        // Debug: Serial.println("Player 2 peer added after discovery");
      } else {
        // Debug: Serial.println("Failed to add discovered Player 2 peer");
      }
    }
    player2Connected = true;
    lastPlayer2Heartbeat = millis();
    
    // Copy to player2Data for consistency
    memcpy(&player2Data, &player1Data, sizeof(player1Data));

    if (player2Data.action == 1) {
      // Heartbeat - just update connection status (no need to send to Pi)
    } else if (player2Data.action == 2) {
      // Hit detected by Player 2
      // Convert Player 2's timestamp to our time reference
      uint32_t adjustedTime = player2Data.hitTime + player2ClockOffset;
      
      // Deduplication: check if this is a duplicate hit
      unsigned long now = millis();
      if (adjustedTime == lastProcessedHitTime && player2Data.playerId == lastProcessedHitPlayer && 
          (now - lastHitProcessTime) < HIT_DEBOUNCE_MS) {
        Serial.printf("Duplicate hit from Player 2 ignored (time: %lu)\n", adjustedTime);
        return;
      }
      
      // Update deduplication tracking
      lastProcessedHitTime = adjustedTime;
      lastProcessedHitPlayer = player2Data.playerId;
      lastHitProcessTime = now;
      
      player2HitTime = adjustedTime;
      Serial.printf("Player 2 hit detected at %lu (adjusted from %lu) with strength %d\n", 
                   adjustedTime, player2Data.hitTime, player2Data.hitStrength);
      
      // Send hit notification to Pi
      sendToPi("{\"type\":\"hit\",\"player\":2,\"time\":" + String(adjustedTime) + ",\"strength\":" + String(player2Data.hitStrength) + "}");
      
      // Declare winner immediately if round active
      if (gameActive) {
        winner = "Player 2";
        gameActive = false;
        sendToPi("{\"type\":\"winner\",\"winner\":\"Player 2\"}");
        // Debug: Serial.println("Winner declared: Player 2");
      }
    } else if (player2Data.action == 3) {
      // Reset request from Player 2
      resetGame();
    } else if (player2Data.action == 4) {
      // Clock synchronization response
      uint32_t now = micros();
      uint32_t roundTrip = now - player2Data.syncTime;
      uint32_t player2Time = player2Data.roundTripTime;
      
      // Calculate clock offset: (Player2 time + roundTrip/2) - Bridge time
      player2ClockOffset = (player2Time + roundTrip/2) - now;
      player2ClockSynced = true;
      lastPlayer2SyncTime = millis();
      
      // Clock sync complete - no need to send to Pi
      // Debug: Serial.printf("Player 2 Clock sync: offset=%ld us, roundTrip=%lu us\n", player2ClockOffset, roundTrip);
    }
    }
  } else if (len == sizeof(struct_lightboard_message)) {
    // Lightboard message
    memcpy(&lightboardData, data, sizeof(lightboardData));
    
    if (lightboardData.deviceId == 3) { // Lightboard
      // Check if this is a reconnection (was disconnected, now connected)
      bool wasDisconnected = !lightboardConnected;
      
      // Lightboard MAC is already known and peer is already added
      lightboardWasConnected = lightboardConnected; // Store previous state
      lightboardConnected = true;
      lastLightboardHeartbeat = millis();
      // Debug: Serial.println("Lightboard message received - connection confirmed");

      if (lightboardData.action == 1) {
        // Heartbeat - just update connection status (no need to send to Pi)
        
        // If this is a reconnection or first connection, request state from Pi
        if (wasDisconnected || !lightboardWasConnected) {
          Serial.println("Lightboard connected - requesting state from Pi");
          // Request lightboard state from Pi
          sendToPi("{\"type\":\"lightboardStateRequest\"}");
        }
      } else if (lightboardData.action == 7) {
        // State request from lightboard - request state from Pi
        Serial.println("Lightboard requested state - requesting from Pi");
        sendToPi("{\"type\":\"lightboardStateRequest\"}");
      }
    }
  }
}

// ===================== Lightboard Communication =====================
void sendLightboardUpdate(uint8_t action) {
  // Always send heartbeats (action 1) regardless of connection status
  // Other actions require connection
  if (!lightboardConnected && action != 1) return;
  
  lightboardData.deviceId = 1; // Bridge
  lightboardData.action = action;
  lightboardData.gameMode = lightboardGameMode;
  lightboardData.p1ColorIndex = lightboardP1ColorIndex;
  lightboardData.p2ColorIndex = lightboardP2ColorIndex;
  lightboardData.p1Pos = lightboardP1Pos;
  lightboardData.p2Pos = lightboardP2Pos;
  lightboardData.nextLedPos = lightboardNextLedPos;
  lightboardData.tugBoundary = lightboardTugBoundary;
  lightboardData.p1RacePos = lightboardP1RacePos;
  lightboardData.p2RacePos = lightboardP2RacePos;
  
  // Only send winner information for specific actions
  if (action == 2) {
    // Game state update - no winner information, just settings
    lightboardData.celebrating = false;
    lightboardData.winner = 0;
  } else {
    // Other actions (heartbeat, mode change, reset) - include winner info
    lightboardData.celebrating = lightboardCelebrating;
    lightboardData.winner = lightboardWinner;
  }
  
  esp_now_send(lightboardAddress, (uint8_t*)&lightboardData, sizeof(lightboardData));
  // Debug: Serial.printf("Sent lightboard update: action=%d, mode=%d\n", action, lightboardGameMode);
}

void updateLightboardGameState() {
  // Send game state update to lightboard (mode, colors only)
  // Individual points are handled by sendLightboardPointUpdate()
  sendLightboardUpdate(2); // game-state action
}

void sendLightboardPointUpdate(uint8_t scoringPlayer) {
  // Send point update to lightboard with which player scored
  if (!lightboardConnected) return;
  
  lightboardData.deviceId = 1; // Bridge
  lightboardData.action = 3; // point update
  lightboardData.winner = scoringPlayer; // Send Player 1 or Player 2 directly
  
  esp_now_send(lightboardAddress, (uint8_t*)&lightboardData, sizeof(lightboardData));
  // Debug: Serial.printf("Sent lightboard point update: Player %d scored\n", scoringPlayer);
}

void sendLightboardStateRestore() {
  // Send full state restore to lightboard (action 6 = state restore)
  if (!lightboardConnected) return;
  
  lightboardData.deviceId = 1; // Bridge
  lightboardData.action = 6; // state restore
  lightboardData.gameMode = lightboardGameMode;
  lightboardData.p1ColorIndex = lightboardP1ColorIndex;
  lightboardData.p2ColorIndex = lightboardP2ColorIndex;
  lightboardData.p1Pos = lightboardP1Pos;
  lightboardData.p2Pos = lightboardP2Pos;
  lightboardData.nextLedPos = lightboardNextLedPos;
  lightboardData.tugBoundary = lightboardTugBoundary;
  lightboardData.p1RacePos = lightboardP1RacePos;
  lightboardData.p2RacePos = lightboardP2RacePos;
  lightboardData.celebrating = lightboardCelebrating;
  lightboardData.winner = lightboardWinner;
  
  esp_now_send(lightboardAddress, (uint8_t*)&lightboardData, sizeof(lightboardData));
  Serial.printf("Sent lightboard state restore: mode=%d, p1Pos=%d, p2Pos=%d\n", 
               lightboardGameMode, lightboardP1Pos, lightboardP2Pos);
}

void awardPointToPlayer(uint8_t playerId) {
  // Award a point to the specified player on the lightboard
  // playerId: 1 = Player 1, 2 = Player 2 (Bridge is host only)
  
  if (playerId != 1 && playerId != 2) {
    Serial.printf("Invalid player ID: %d. Must be 1 or 2 (Bridge is host only).\n", playerId);
    return;
  }
  
  if (!lightboardConnected) {
    // Debug: Serial.println("Lightboard not connected - cannot award point");
    return;
  }
  
  // Send point update to lightboard
  sendLightboardPointUpdate(playerId);
  
  // Update local game state
  if (playerId == 1) {
    winner = "Player 1";
    player1HitTime = micros(); // Use current time as hit time
  } else if (playerId == 2) {
    winner = "Player 2";
    player2HitTime = micros(); // Use current time as hit time
  }
  
  // Send winner notification to Pi
  sendToPi("{\"type\":\"winner\",\"winner\":\"" + winner + "\"}");
  
  // Update lightboard game state
  updateLightboardGameState();
  
  Serial.printf("Awarded point to Player %d\n", playerId);
}

void awardMultiplePointsToPlayer(uint8_t playerId, int multiplier) {
  // Award multiple points to the specified player on the lightboard
  // playerId: 1 = Player 1, 2 = Player 2 (Bridge is host only)
  // multiplier: number of points to award
  
  
  if (playerId != 1 && playerId != 2) {
    Serial.printf("Invalid player ID: %d. Must be 1 or 2 (Bridge is host only).\n", playerId);
    return;
  }
  
  if (multiplier < 1 || multiplier > 10) {
    Serial.printf("Invalid multiplier: %d. Must be 1-10.\n", multiplier);
    return;
  }
  
  if (!lightboardConnected) {
    // Debug: Serial.println("Lightboard not connected - cannot award points");
    return;
  }
  
  
  // Send multiple point updates to lightboard
  for (int i = 0; i < multiplier; i++) {
    sendLightboardPointUpdate(playerId);
    delay(100); // Small delay between points for visual effect
  }
  
  // Update lightboard game state (needed for proper LED progression)
  updateLightboardGameState();
  
  Serial.printf("Awarded %d points to Player %d\n", multiplier, playerId);
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
    
    // Send winner notification to Pi
    sendToPi("{\"type\":\"winner\",\"winner\":\"" + winner + "\"}");
  }
}

void resetGame() {
  winner = "none";
  // Bridge is host only - no need to reset bridge hit time
  player1HitTime = 0;
  player2HitTime = 0;
  gameActive = true;
  
  // Reset hit deduplication tracking
  lastProcessedHitTime = 0;
  lastProcessedHitPlayer = 0;
  lastHitProcessTime = 0;
  
  // Reset lightboard state
  lightboardWinner = 0;
  lightboardCelebrating = false;
  lightboardP1Pos = -1;
  lightboardP2Pos = 38;
  lightboardNextLedPos = 0;
  lightboardTugBoundary = 18;
  lightboardP1RacePos = -1;
  lightboardP2RacePos = -1;
  
  // Send reset to Player 1
  myData.action = 3; // reset request
  esp_now_send(player1Address, (uint8_t*)&myData, sizeof(myData));
  
  // Send reset to Player 2
  esp_now_send(player2Address, (uint8_t*)&myData, sizeof(myData));
  
  // Send reset to lightboard
  sendLightboardUpdate(5); // reset action
  
  // Send reset notification to Pi
  sendToPi("{\"type\":\"reset\"}");
  
  // Debug: Serial.println("Game reset");
}

void resetGameForQuiz() {
  // Light version of reset for quiz navigation - doesn't reset lightboard state
  winner = "none";
  // Bridge is host only - no need to reset bridge hit time
  player1HitTime = 0;
  player2HitTime = 0;
  gameActive = true;
  
  // Send reset notification to Pi
  sendToPi("{\"type\":\"reset\"}");
  
  // Debug: Serial.println("Game reset for quiz navigation");
}

// ===================== Clock Synchronization =====================
void syncClock() {
  if (player1Connected && (millis() - lastSyncTime >= SYNC_INTERVAL)) {
    myData.action = 4; // clock sync
    myData.syncTime = micros();
    myData.roundTripTime = 0;
    esp_now_send(player1Address, (uint8_t*)&myData, sizeof(myData));
    // Debug: Serial.println("Clock sync request sent to Player 1");
  }
  
  if (player2Connected && (millis() - lastPlayer2SyncTime >= SYNC_INTERVAL)) {
    myData.action = 4; // clock sync
    myData.syncTime = micros();
    myData.roundTripTime = 0;
    esp_now_send(player2Address, (uint8_t*)&myData, sizeof(myData));
    // Debug: Serial.println("Clock sync request sent to Player 2");
  }
}

// ===================== Serial Communication with Pi =====================
void sendToPi(String message) {
  Serial.println(message);
}

void processPiCommand(String command) {
  DynamicJsonDocument doc(1024);
  deserializeJson(doc, command);
  
  if (doc.containsKey("cmd")) {
    String cmd = doc["cmd"];
    
    if (cmd == "heartbeat") {
      piConnected = true;
      lastPiHeartbeat = millis();
      // Debug: Serial.println("Pi heartbeat received");
      
      // Send status back to Pi
      sendToPi("{\"type\":\"status\",\"player1Connected\":" + String(player1Connected ? "true" : "false") + 
               ",\"player2Connected\":" + String(player2Connected ? "true" : "false") + 
               ",\"lightboardConnected\":" + String(lightboardConnected ? "true" : "false") + 
               ",\"clockSynced\":" + String(clockSynced ? "true" : "false") + 
               ",\"player2ClockSynced\":" + String(player2ClockSynced ? "true" : "false") + "}");
      
    } else if (cmd == "reset") {
      resetGame();
      
    } else if (cmd == "awardPoint") {
      if (doc.containsKey("player") && doc.containsKey("multiplier")) {
        int player = doc["player"];
        int multiplier = doc["multiplier"];
        
        if (player == 1 || player == 2) {
          awardMultiplePointsToPlayer(player, multiplier);
        }
      }
      
    } else if (cmd == "lightboardSettings") {
      if (doc.containsKey("mode") && doc.containsKey("p1Color") && doc.containsKey("p2Color")) {
        int newMode = doc["mode"];
        int newP1Color = doc["p1Color"];
        int newP2Color = doc["p2Color"];
        
        if (newMode >= 1 && newMode <= 6 && newP1Color >= 0 && newP1Color <= 4 && newP2Color >= 0 && newP2Color <= 4) {
          bool modeChanged = (lightboardGameMode != newMode);
          lightboardGameMode = newMode;
          lightboardP1ColorIndex = newP1Color;
          lightboardP2ColorIndex = newP2Color;
          Serial.printf("Lightboard settings updated: mode=%d, p1Color=%d, p2Color=%d\n", newMode, newP1Color, newP2Color);
          
          if (modeChanged) {
            // Mode changed - reset game positions
            sendLightboardUpdate(4); // mode-change action
          } else {
            // Only colors changed - preserve game positions
            sendLightboardUpdate(2); // game-state action (preserves positions)
          }
        }
      }
      
    } else if (cmd == "lightboardState") {
      // Received lightboard state from Pi - restore full game state
      if (doc.containsKey("gameState")) {
        JsonObject gameState = doc["gameState"];
        
        if (gameState.containsKey("mode")) lightboardGameMode = gameState["mode"];
        if (gameState.containsKey("p1ColorIndex")) lightboardP1ColorIndex = gameState["p1ColorIndex"];
        if (gameState.containsKey("p2ColorIndex")) lightboardP2ColorIndex = gameState["p2ColorIndex"];
        if (gameState.containsKey("p1Pos")) lightboardP1Pos = gameState["p1Pos"];
        if (gameState.containsKey("p2Pos")) lightboardP2Pos = gameState["p2Pos"];
        if (gameState.containsKey("nextLedPos")) lightboardNextLedPos = gameState["nextLedPos"];
        if (gameState.containsKey("tugBoundary")) lightboardTugBoundary = gameState["tugBoundary"];
        if (gameState.containsKey("p1RacePos")) lightboardP1RacePos = gameState["p1RacePos"];
        if (gameState.containsKey("p2RacePos")) lightboardP2RacePos = gameState["p2RacePos"];
        if (gameState.containsKey("celebrating")) lightboardCelebrating = gameState["celebrating"];
        if (gameState.containsKey("winner")) lightboardWinner = gameState["winner"];
        
        Serial.printf("Lightboard state restored: mode=%d, p1Pos=%d, p2Pos=%d\n", 
                     lightboardGameMode, lightboardP1Pos, lightboardP2Pos);
        
        // Send full state restore to lightboard (action 6 = state restore)
        sendLightboardStateRestore();
      }
      
    } else if (cmd == "quizAction") {
      if (doc.containsKey("action")) {
        String action = doc["action"];
        if (action == "next" || action == "prev" || action == "toggle") {
          // Send quiz action notification to Pi
          sendToPi("{\"type\":\"quizAction\",\"action\":\"" + action + "\"}");
        }
      }
    }
  }
}

// ===================== Setup =====================
void setup(){
  Serial.begin(115200);
  delay(50);
  Serial.println();
  Serial.println(F("=== ESP-NOW Bridge - Player 1 (Host) ==="));
  Serial.println(F("Bridge mode: communicates between Raspberry Pi and ESP32s"));

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // No sensor pin init in host-only mode

  // No local calibration/preferences in host-only mode

  // Wi‑Fi AP + web UI (keep STA active for ESP-NOW reliability)
  WiFi.mode(WIFI_AP_STA);
  if (WiFi.softAP("ESP32Bridge", "12345678", 1 /*channel*/, 0 /*hidden*/, 2 /*max conn*/)){
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("SoftAP started: SSID=ESP32Bridge PASS=12345678 IP=%s\r\n", ip.toString().c_str());
      Serial.printf("AP MAC: %s | STA MAC: %s | CH: %d\r\n",
                WiFi.softAPmacAddress().c_str(),
                WiFi.macAddress().c_str(),
                WiFi.channel());
  Serial.println("=== Player 1 MAC Addresses ===");
  Serial.printf("AP MAC: %s\r\n", WiFi.softAPmacAddress().c_str());
  Serial.printf("STA MAC: %s\r\n", WiFi.macAddress().c_str());
  Serial.println("===============================");
  } else {
    Serial.println(F("SoftAP start FAILED"));
  }

  // ESP-NOW setup
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    while(true);
  }
  
  // Clear any existing peers to ensure clean state
  esp_now_del_peer(ESPNOW_BROADCAST_ADDR);
  Serial.println("Cleared existing ESP-NOW peers");
  
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  // Add Player 1 peer with known MAC address
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, player1Address, 6);
  peerInfo.channel = 0; // follow current channel
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) == ESP_OK) {
    Serial.println("Player 1 peer added successfully");
  } else {
    Serial.println("Failed to add Player 1 peer");
  }

  // Add Player 2 peer with known MAC address
  esp_now_peer_info_t player2PeerInfo = {};
  memcpy(player2PeerInfo.peer_addr, player2Address, 6);
  player2PeerInfo.channel = 0; // follow current channel
  player2PeerInfo.encrypt = false;
  if (esp_now_add_peer(&player2PeerInfo) == ESP_OK) {
    Serial.println("Player 2 peer added successfully");
  } else {
    Serial.println("Failed to add Player 2 peer");
  }

  // Add Lightboard peer with known MAC address
  esp_now_peer_info_t lightboardPeerInfo = {};
  memcpy(lightboardPeerInfo.peer_addr, lightboardAddress, 6);
  lightboardPeerInfo.channel = 1; // follow current channel
  lightboardPeerInfo.encrypt = false;
  
  lightboardPeerInfo.ifidx = WIFI_IF_AP;
if (esp_now_add_peer(&lightboardPeerInfo) == ESP_OK) {
    Serial.println("Lightboard peer added successfully");
    lightboardMacLearned = true; // We already know the MAC address
  } else {
    Serial.println("Failed to add Lightboard peer");
  }
  // AP is already on channel 1 via softAP() call above

  myData.playerId = 1;

  // Local hit detection disabled: Player 1 is host-only
  Serial.println(F("Local hit detection disabled (host-only)."));

  // Initialize game state
  gameActive = true;  // Ensure game starts active
  winner = "none";
  // Bridge is host only - no need to initialize bridge hit time
  player1HitTime = 0;
  player2HitTime = 0;
  clockSynced = false;
  player2ClockSynced = false;
  clockOffset = 0;
  player2ClockOffset = 0;
  
  Serial.println("ESP-NOW Bridge ready. Waiting for Pi connection...");
}

// ===================== Loop =====================
void loop(){
  const unsigned long nowUs = micros();
  const unsigned long nowMs = millis();

  // Clock synchronization
  syncClock();

  // Local capture disabled: skip start/capture processing
  // if (g_startPending) { ... }

  // if (g_capturing && (nowUs - g_t0) >= CAPTURE_WINDOW_US) { ... }

  // Process serial commands from Pi
  if (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBuffer.length() > 0) {
        processPiCommand(serialBuffer);
        serialBuffer = "";
      }
    } else {
      serialBuffer += c;
    }
  }

  // Check for Pi connection timeout
  if (piConnected && (millis() - lastPiHeartbeat > PI_HEARTBEAT_TIMEOUT)) {
    piConnected = false;
    // Debug: Serial.println("Pi connection lost");
  }

  // Check for connection timeout
  if (player1Connected && (millis() - lastHeartbeat > heartbeatTimeout)) {
    player1Connected = false;
    clockSynced = false; // Reset sync when connection lost
    player1MacLearned = false; // Reset MAC learning to force rediscovery
    // Debug: Serial.println("Player 1 connection lost - resetting discovery");
    sendToPi("{\"type\":\"error\",\"message\":\"Player 1 disconnected\"}");
  }
  
  if (player2Connected && (millis() - lastPlayer2Heartbeat > heartbeatTimeout)) {
    player2Connected = false;
    player2ClockSynced = false; // Reset sync when connection lost
    player2MacLearned = false; // Reset MAC learning to force rediscovery
    // Debug: Serial.println("Player 2 connection lost - resetting discovery");
    sendToPi("{\"type\":\"error\",\"message\":\"Player 2 disconnected\"}");
  }
  
  // Check for lightboard connection timeout
  if (lightboardConnected && (millis() - lastLightboardHeartbeat > heartbeatTimeout)) {
    lightboardWasConnected = lightboardConnected; // Store previous state
    lightboardConnected = false;
    lightboardMacLearned = false; // Reset MAC learning to force rediscovery
    // Debug: Serial.println("Lightboard connection lost - resetting discovery");
    sendToPi("{\"type\":\"error\",\"message\":\"Lightboard disconnected\"}");
  }

  // Send heartbeat to Player 1
  static unsigned long lastHeartbeatSend = 0;
  if (millis() - lastHeartbeatSend >= HEARTBEAT_INTERVAL_MS) {
    myData.action = 1; // heartbeat
    esp_now_send(player1Address, (uint8_t*)&myData, sizeof(myData));
    // Debug: Serial.println("Sent heartbeat to Player 1");
    lastHeartbeatSend = millis();
  }
  
  // Send heartbeat to Player 2
  static unsigned long lastPlayer2HeartbeatSend = 0;
  if (millis() - lastPlayer2HeartbeatSend >= HEARTBEAT_INTERVAL_MS) {
    myData.action = 1; // heartbeat
    esp_now_send(player2Address, (uint8_t*)&myData, sizeof(myData));
    // Debug: Serial.println("Sent heartbeat to Player 2");
    lastPlayer2HeartbeatSend = millis();
  }
  
  // Send heartbeat to lightboard (always try, even if MAC not learned yet)
  static unsigned long lastLightboardHeartbeatSend = 0;
  if (millis() - lastLightboardHeartbeatSend >= HEARTBEAT_INTERVAL_MS) {
    sendLightboardUpdate(1); // heartbeat action
    // Debug: Serial.println("Sent heartbeat to lightboard");
    lastLightboardHeartbeatSend = millis();
  }
}
