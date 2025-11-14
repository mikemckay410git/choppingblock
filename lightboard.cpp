#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Adafruit_NeoPixel.h>

// ---- LED strip config ----
#define LED_PIN      13
#define NUM_LEDS     38
#define BRIGHTNESS   50
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// ---- Center indices ----
const int CENTER_LEFT  = (NUM_LEDS / 2) - 1; // 18
const int CENTER_RIGHT = (NUM_LEDS / 2);     // 19

// ===================== ESP-NOW Configuration =====================

// Force STA interface to a specific channel so ESP-NOW matches the host AP
static void forceStaChannel(uint8_t ch){
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
}

// Player 1 MAC address (hardcoded for reliability)
uint8_t player1Address[] = {0x80, 0xF3, 0xDA, 0x4A, 0x2F, 0x98}; // Player 1 AP MAC

typedef struct struct_lightboard_message {
  uint8_t  deviceId;     // 3=Lightboard
  uint8_t  action;       // 1=heartbeat, 2=game-state, 3=score-update, 4=mode-change, 5=reset, 6=state-restore, 7=state-request
  uint8_t  gameMode;     // 1-6 (Territory, Swap Sides, Split Scoring, Score Order, Race, Tug O War)
  uint8_t  p2ColorIndex; // Player 2 color index
  uint8_t  p3ColorIndex; // Player 3 color index
  int8_t   p2Pos;        // Player 2 position (-1 to NUM_LEDS)
  int8_t   p3Pos;        // Player 3 position (-1 to NUM_LEDS)
  uint8_t  nextLedPos;   // For Score Order mode
  uint8_t  tugBoundary;  // For Tug O War mode
  uint8_t  p2RacePos;    // For Race mode
  uint8_t  p3RacePos;    // For Race mode
  uint8_t  celebrating;  // Celebration state
  uint8_t  winner;       // 0=none, 2=Player2, 3=Player3
} struct_lightboard_message;

struct_lightboard_message myData;
struct_lightboard_message player1Data;

// Connection tracking
bool player1Connected = false;
unsigned long lastHeartbeat = 0;
const unsigned long heartbeatTimeout = 2000; // 2 seconds
bool player1MacLearned = false;
static bool wasConnected = false; // Track if we've been connected before

// ---- Game state ----
int p2Pos = -1;
int p3Pos = NUM_LEDS;
bool celebrating = false;

// Mode 4: Score Order tracking
int nextLedPosition = 0;
int scoringSequence[NUM_LEDS];

// Mode 5: Race tracking
int p2RacePos = -1;
int p3RacePos = -1;

// Mode 6: Tug O War tracking
int tugBoundary = CENTER_LEFT;

// Game modes
int gameMode = 1;

// Player colors (RGB values)
struct PlayerColor {
  uint8_t r, g, b;
};

// Available colors
const PlayerColor availableColors[] = {
  {255, 0, 0},   // Red
  {0, 80, 255},  // Blue
  {0, 255, 0},   // Green
  {255, 0, 255}, // Magenta
  {255, 80, 0}   // Orange
};

const int NUM_COLORS = sizeof(availableColors) / sizeof(availableColors[0]);

// Current player colors (indices into availableColors array)
int p2ColorIndex = 0; // Red (default)
int p3ColorIndex = 1; // Blue (default)

// Prototypes to placate Arduino preprocessor with custom return types
PlayerColor getP2Color();
PlayerColor getP3Color();
uint32_t getP2ColorValue();
uint32_t getP3ColorValue();
void requestStateRestore();

// ==================== Celebration Manager ====================
enum CelebrationType : uint8_t {
  CEL_WINNER_CHASE = 0,
  CEL_CENTER_RIPPLE,
  CEL_CONFETTI,
  CEL_BREATHE
};

bool celActive = false;
bool celP2Wins = false;
CelebrationType celType = CEL_WINNER_CHASE;

uint32_t celStartMs = 0;
uint32_t celLastFrame = 0;
uint16_t celDurationMs = 3000;

uint8_t winnerR=0, winnerG=0, winnerB=0;

// Clamp
static inline float clampv(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static inline uint32_t scaleColor(uint8_t r,uint8_t g,uint8_t b,float s){
  s = clampv(s, 0.0f, 1.0f);
  return strip.Color((uint8_t)(r*s),(uint8_t)(g*s),(uint8_t)(b*s));
}

void startCelebration(bool player2Wins) {
  celActive = true;
  celP2Wins = player2Wins;
  if (celP2Wins) { 
    PlayerColor c = getP2Color();
    winnerR=c.r; winnerG=c.g; winnerB=c.b; 
  }
  else { 
    PlayerColor c = getP3Color();
    winnerR=c.r; winnerG=c.g; winnerB=c.b; 
  }

  static uint8_t nextPattern = 0;
  celType = (CelebrationType)(nextPattern++ % 4);

  switch (celType) {
    case CEL_WINNER_CHASE:  celDurationMs = 2500; break;
    case CEL_CENTER_RIPPLE: celDurationMs = 2500; break;
    case CEL_CONFETTI:      celDurationMs = 2000; break;
    case CEL_BREATHE:       celDurationMs = 3000; break;
  }
  celStartMs   = millis();
  celLastFrame = 0;
}

bool updateCelebration() {
  if (!celActive) return false;
  uint32_t now = millis();
  if (now - celLastFrame < 16) return true;
  celLastFrame = now;

  float t = (float)(now - celStartMs) / (float)celDurationMs;
  if (t >= 1.0f) { celActive = false; return false; }

  for (int i=0;i<NUM_LEDS;i++) strip.setPixelColor(i, 0);

  switch (celType) {
    case CEL_WINNER_CHASE: {
      int head = (int)((now / 20) % NUM_LEDS);
      for (int k=0; k<6; k++) {
        int idx = head - k;
        if (idx < 0) idx += NUM_LEDS;
        float s = powf(0.75f, k);

        if (k == 0 && (celLastFrame/200)%2==0) {
          strip.setPixelColor(idx, strip.Color(255,255,255));
        } else {
          strip.setPixelColor(idx, scaleColor(winnerR, winnerG, winnerB, s));
        }
      }
    } break;

    case CEL_CENTER_RIPPLE: {
      for (int i=0;i<NUM_LEDS;i++) {
        int d = min(abs(i - CENTER_LEFT), abs(i - CENTER_RIGHT));
        float phase = (d * 0.55f) - (t * 10.0f);
        float s = 0.5f + 0.5f * sinf(phase);

        uint8_t r = (winnerR * 0.7) + (255 * 0.3);
        uint8_t g = (winnerG * 0.7) + (255 * 0.3);
        uint8_t b = (winnerB * 0.7) + (255 * 0.3);
        strip.setPixelColor(i, scaleColor(r,g,b,s));
      }
    } break;

    case CEL_CONFETTI: {
      static uint8_t confR[NUM_LEDS]={0}, confG[NUM_LEDS]={0}, confB[NUM_LEDS]={0};
      for (int i=0;i<NUM_LEDS;i++) {
        confR[i] = (uint8_t)(confR[i] * 0.85f);
        confG[i] = (uint8_t)(confG[i] * 0.85f);
        confB[i] = (uint8_t)(confB[i] * 0.85f);
        strip.setPixelColor(i, strip.Color(confR[i], confG[i], confB[i]));
      }
      uint8_t sparks = 2 + (now % 3);
      for (uint8_t s=0; s<sparks; s++) {
        int i = random(0, NUM_LEDS);
        bool whiteSpark = (random(0,100) < 30);
        uint8_t r = whiteSpark ? 255 : winnerR;
        uint8_t g = whiteSpark ? 255 : winnerG;
        uint8_t b = whiteSpark ? 255 : winnerB;
        confR[i] = max(confR[i], r);
        confG[i] = max(confG[i], g);
        confB[i] = max(confB[i], b);
      }
    } break;

    case CEL_BREATHE: {
      float s = 0.5f + 0.5f * sinf(t * 2.0f * 3.14159f * 2.0f);
      for (int i=0;i<NUM_LEDS;i++) {
        uint8_t r = (uint8_t)(winnerR * (1.0f-s) + 255 * s);
        uint8_t g = (uint8_t)(winnerG * (1.0f-s) + 255 * s);
        uint8_t b = (uint8_t)(winnerB * (1.0f-s) + 255 * s);
        strip.setPixelColor(i, strip.Color(r,g,b));
      }
    } break;
  }

  strip.show();
  return true;
}

// ---- helpers ----
inline uint32_t col(uint8_t r, uint8_t g, uint8_t b){ return strip.Color(r,g,b); }

// Get current player colors
PlayerColor getP2Color() { return availableColors[p2ColorIndex]; }
PlayerColor getP3Color() { return availableColors[p3ColorIndex]; }

// Get color as uint32_t for strip.setPixelColor
uint32_t getP2ColorValue() { 
  PlayerColor c = getP2Color(); 
  return col(c.r, c.g, c.b); 
}
uint32_t getP3ColorValue() { 
  PlayerColor c = getP3Color(); 
  return col(c.r, c.g, c.b); 
}

void clearStrip(){ for (int i=0;i<NUM_LEDS;i++) strip.setPixelColor(i,0); strip.show(); }

void requestStateRestore() {
  // Send state request message to Bridge (action 7 = state request)
  if (!player1MacLearned) return; // Can't send if we don't know the MAC yet
  
  myData.deviceId = 3; // Lightboard
  myData.action = 7; // state request
  esp_now_send(player1Address, (uint8_t*)&myData, sizeof(myData));
  Serial.println("Sent state request to Bridge");
}

void paintProgress() {
  for (int i=0;i<NUM_LEDS;i++) strip.setPixelColor(i,0);
  if (gameMode==2) { if(p2Pos>=0&&p2Pos<NUM_LEDS) strip.setPixelColor(p2Pos,getP2ColorValue()); if(p3Pos>=0&&p3Pos<NUM_LEDS) strip.setPixelColor(p3Pos,getP3ColorValue()); }
  else if (gameMode==3) { if(p2Pos<=CENTER_LEFT) for(int i=CENTER_LEFT;i>=p2Pos&&i>=0;i--) strip.setPixelColor(i,getP2ColorValue()); if(p3Pos>=CENTER_RIGHT) for(int i=CENTER_RIGHT;i<=p3Pos&&i<NUM_LEDS;i++) strip.setPixelColor(i,getP3ColorValue()); }
  else if (gameMode==4) { for(int i=0;i<nextLedPosition;i++){ if(scoringSequence[i]==2) strip.setPixelColor(i,getP2ColorValue()); else if(scoringSequence[i]==3) strip.setPixelColor(i,getP3ColorValue()); }}
  else if (gameMode==5) { bool p2On=(p2RacePos>=0),p3On=(p3RacePos>=0); if(p2On&&p3On&&p2RacePos==p3RacePos) { PlayerColor c2=getP2Color(),c3=getP3Color(); strip.setPixelColor(p2RacePos,col((c2.r+c3.r)/2,(c2.g+c3.g)/2,(c2.b+c3.b)/2)); } else { if(p2On) strip.setPixelColor(p2RacePos,getP2ColorValue()); if(p3On) strip.setPixelColor(p3RacePos,getP3ColorValue()); }}
  else if (gameMode==6) { for(int i=0;i<=tugBoundary&&i<NUM_LEDS;i++) strip.setPixelColor(i,getP2ColorValue()); for(int i=tugBoundary+1;i<NUM_LEDS;i++) strip.setPixelColor(i,getP3ColorValue()); }
  else { for(int i=0;i<=p2Pos&&i<NUM_LEDS;i++) strip.setPixelColor(i,getP2ColorValue()); for(int i=NUM_LEDS-1;i>=p3Pos&&i>=0;i--) strip.setPixelColor(i,getP3ColorValue()); }
  strip.show();
}

void resetGame(){ 
  switch(gameMode){
    case 1:case 2:p2Pos=-1;p3Pos=NUM_LEDS;break;
    case 3:p2Pos=CENTER_LEFT+1;p3Pos=CENTER_RIGHT-1;break;
    case 4:nextLedPosition=0;for(int i=0;i<NUM_LEDS;i++)scoringSequence[i]=0;break;
    case 5:p2RacePos=-1;p3RacePos=-1;break;
    case 6:tugBoundary=CENTER_LEFT;break;
  } 
  if(gameMode==6) paintProgress(); else clearStrip(); 
}

// ===================== ESP-NOW Callbacks =====================
void OnDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  Serial.print("Lightboard Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  Serial.printf("ESP-NOW message received: len=%d, expected=%d\n", len, sizeof(struct_lightboard_message));
  if (len != sizeof(struct_lightboard_message)) return;
  
  memcpy(&player1Data, data, sizeof(player1Data));

  if (player1Data.deviceId == 1) { // Player 1
    // Learn Player 1 MAC dynamically
    if (info && !player1MacLearned) {
      memcpy(player1Address, info->src_addr, 6);
      char macStr[18];
      sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", player1Address[0],player1Address[1],player1Address[2],player1Address[3],player1Address[4],player1Address[5]);
      Serial.printf("Discovered Player 1 MAC: %s\r\n", macStr);
      esp_now_del_peer(player1Address);
      esp_now_peer_info_t p = {};
      memcpy(p.peer_addr, player1Address, 6);
      p.channel = 1;
      p.encrypt = false;
      if (esp_now_add_peer(&p) == ESP_OK) {
        player1MacLearned = true;
        Serial.println("Player 1 peer added after discovery");
        Serial.println("Connection established! Heartbeats will now be sent.");
      } else {
        Serial.println("Failed to add discovered Player 1 peer");
      }
    }
    // Check if this is a new connection (was disconnected, now connected)
    bool wasDisconnected = !player1Connected;
    player1Connected = true;
    lastHeartbeat = millis();
    
    // Clear demo mode and go black only on new connection
    if (wasDisconnected) {
      clearStrip();
      Serial.println("Connection established - demo mode cleared, requesting state");
      // Request state restore from Bridge
      requestStateRestore();
    }

    if (player1Data.action == 1) {
      // Heartbeat - just update connection status
      Serial.println("Player 1 heartbeat received");
    } else if (player1Data.action == 2) {
      // Game state update - only update essential settings (mode, colors)
      gameMode = player1Data.gameMode;
      p2ColorIndex = player1Data.p2ColorIndex;
      p3ColorIndex = player1Data.p3ColorIndex;
      
      Serial.printf("Game state update: mode=%d, p2Color=%d, p3Color=%d\n", 
                   gameMode, p2ColorIndex, p3ColorIndex);
      paintProgress();
      
    } else if (player1Data.action == 3) {
      // Point update - run our own game logic
      Serial.printf("Point update received - Player %d scored, running lightboard game logic\n", player1Data.winner);
      handlePointUpdate(player1Data.winner);
      
    } else if (player1Data.action == 4) {
      // Mode change
      gameMode = player1Data.gameMode;
      p2ColorIndex = player1Data.p2ColorIndex;
      p3ColorIndex = player1Data.p3ColorIndex;
      Serial.printf("Mode changed to: %d\n", gameMode);
      resetGame();
      
    } else if (player1Data.action == 5) {
      // Reset
      Serial.println("Reset received - resetting lightboard game state");
      resetGame();
      
    } else if (player1Data.action == 6) {
      // State restore - restore full game state from saved state
      Serial.println("State restore received - restoring lightboard game state");
      gameMode = player1Data.gameMode;
      p2ColorIndex = player1Data.p2ColorIndex;
      p3ColorIndex = player1Data.p3ColorIndex;
      p2Pos = player1Data.p2Pos;
      p3Pos = player1Data.p3Pos;
      nextLedPosition = player1Data.nextLedPos;
      tugBoundary = player1Data.tugBoundary;
      p2RacePos = player1Data.p2RacePos;
      p3RacePos = player1Data.p3RacePos;
      celebrating = player1Data.celebrating;
      
      // Restore scoring sequence for mode 4 (Score Order)
      if (gameMode == 4) {
        // Note: scoringSequence is not in the message, so we'll just restore positions
        // The sequence will be rebuilt as points are awarded
      }
      
      Serial.printf("State restored: mode=%d, p2Pos=%d, p3Pos=%d, p2Color=%d, p3Color=%d\n",
                   gameMode, p2Pos, p3Pos, p2ColorIndex, p3ColorIndex);
      
      // Update display with restored state
      paintProgress();
    }
  }
}

// ===================== Setup =====================
void setup(){
  Serial.begin(115200);
  delay(1000); // Give serial time to initialize
  
  Serial.println();
  Serial.println("==========================================");
  Serial.println("=== LIGHTBOARD ESP-NOW MODULE ===");
  Serial.println("==========================================");
  
  // Display MAC address FIRST and prominently
  WiFi.mode(WIFI_STA);
  delay(50);
  forceStaChannel(1);
  Serial.println("Forced STA channel to 1");
  String macStr = WiFi.macAddress();
  Serial.println();
  Serial.println("*** LIGHTBOARD MAC ADDRESS ***");
  Serial.printf("STA MAC: %s\r\n", macStr.c_str());
  Serial.printf("WiFi Channel: %d\r\n", WiFi.channel());
  Serial.println("===============================");
  
  // Convert MAC address to proper format for player1_host_sync.ino
  macStr.replace(":", "");
  Serial.println("COPY THIS LINE TO player1_host_sync.ino:");
  Serial.print("uint8_t lightboardAddress[] = {");
  for (int i = 0; i < 6; i++) {
    String byteStr = macStr.substring(i * 2, i * 2 + 2);
    Serial.print("0x" + byteStr);
    if (i < 5) Serial.print(", ");
  }
  Serial.println("}; // Lightboard STA MAC");
  Serial.println("===============================");
  Serial.println();
  
  Serial.printf("LED Strip: %d LEDs on pin %d\r\n", NUM_LEDS, LED_PIN);

  // Initialize LED strip
  strip.begin();
  strip.setBrightness(BRIGHTNESS);
  clearStrip();
  randomSeed((uint32_t)esp_timer_get_time());

  // Wi-Fi setup for ESP-NOW
  WiFi.disconnect(); // Ensure clean state
  delay(100);
  
    // (removed) rely on forceStaChannel(1) before esp_now_init
  Serial.printf("WiFi Channel set to: %d\r\n", WiFi.channel());

  // ESP-NOW setup
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    while(true);
  }
  
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  // Add Player 1 peer with known MAC address
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, player1Address, 6);
  peerInfo.channel = 1; // follow current channel
  peerInfo.encrypt = false;
  
  peerInfo.ifidx = WIFI_IF_STA;
if (esp_now_add_peer(&peerInfo) == ESP_OK) {
    Serial.println("Player 1 peer added successfully");
  
    player1MacLearned = true;
    Serial.println("Connection established! Heartbeats will now be sent.");
} else {
    Serial.println("Failed to add Player 1 peer");
  }

  myData.deviceId = 3; // Lightboard device ID

  // Initialize game state
  resetGame();
  
  Serial.println("Lightboard ready - waiting for Player 1 connection");
  Serial.println("Make sure Player 1 is running and has the correct lightboard MAC address");
  Serial.println("The lightboard will automatically discover Player 1 when it sends a message");
}

// ===================== Helper Functions =====================
// Input a value 0 to 255 to get a color value.
// The colours are a transition r - g - b - back to r.
uint32_t wheel(byte WheelPos) {
  WheelPos = 255 - WheelPos;
  if(WheelPos < 85) {
    return strip.Color(255 - WheelPos * 3, 0, WheelPos * 3);
  }
  if(WheelPos < 170) {
    WheelPos -= 85;
    return strip.Color(0, WheelPos * 3, 255 - WheelPos * 3);
  }
  WheelPos -= 170;
  return strip.Color(WheelPos * 3, 255 - WheelPos * 3, 0);
}

// ===================== Lightboard Game Logic =====================
void handlePointUpdate(uint8_t scoringPlayer) {
  // This function runs the lightboard's own game logic based on point updates
  // The lightboard determines how to update positions based on the current game mode and which player scored
  
  switch (gameMode) {
    case 1: // Territory
      // Move the scoring player toward center
      if (scoringPlayer == 2 && p2Pos < NUM_LEDS - 1) {
        p2Pos++;
      } else if (scoringPlayer == 3 && p3Pos > 0) {
        p3Pos--;
      }
      break;
      
    case 2: // Swap Sides
      // Move the scoring player toward center, but avoid collision
      if (scoringPlayer == 2) {
        if (p2Pos + 1 == p3Pos) {
          p2Pos = p3Pos + 1; // Jump over if about to collide
        } else if (p2Pos < NUM_LEDS - 1) {
          p2Pos++;
        }
      } else if (scoringPlayer == 3) {
        if (p3Pos - 1 == p2Pos) {
          p3Pos = p2Pos - 1; // Jump over if about to collide
        } else if (p3Pos > 0) {
          p3Pos--;
        }
      }
      break;
      
    case 3: // Split Scoring
      // Move the scoring player away from center
      if (scoringPlayer == 2 && p2Pos > 0) {
        p2Pos--;
      } else if (scoringPlayer == 3 && p3Pos < NUM_LEDS - 1) {
        p3Pos++;
      }
      break;
      
    case 4: // Score Order
      // Fill LEDs in sequence with the scoring player
      if (nextLedPosition < NUM_LEDS) {
        scoringSequence[nextLedPosition] = scoringPlayer;
        nextLedPosition++;
      }
      break;
      
    case 5: // Race
      // Move the scoring player forward
      if (scoringPlayer == 2 && p2RacePos < NUM_LEDS - 1) {
        p2RacePos++;
      } else if (scoringPlayer == 3 && p3RacePos < NUM_LEDS - 1) {
        p3RacePos++;
      }
      break;
      
    case 6: // Tug O War
      // Move boundary based on who scored
      if (scoringPlayer == 2 && tugBoundary < NUM_LEDS - 1) {
        tugBoundary++;
      } else if (scoringPlayer == 3 && tugBoundary >= 0) {
        tugBoundary--;
      }
      break;
  }
  
  // Check for win conditions
  checkWinConditions();
  
  // Update display
  paintProgress();
}

void checkWinConditions() {
  bool p2Wins = false;
  bool p3Wins = false;
  
  switch (gameMode) {
    case 1: // Territory
      if (p2Pos >= p3Pos) {
        p2Wins = (p2Pos + 1 >= NUM_LEDS - p3Pos);
        p3Wins = !p2Wins;
      }
      break;
      
    case 2: // Swap Sides
      p2Wins = (p2Pos >= NUM_LEDS - 1);
      p3Wins = (p3Pos <= 0);
      break;
      
    case 3: // Split Scoring
      p2Wins = (p2Pos <= 0);
      p3Wins = (p3Pos >= NUM_LEDS - 1);
      break;
      
    case 4: // Score Order
      if (nextLedPosition >= NUM_LEDS) {
        int p2Count = 0, p3Count = 0;
        for (int i = 0; i < NUM_LEDS; i++) {
          if (scoringSequence[i] == 2) p2Count++;
          else if (scoringSequence[i] == 3) p3Count++;
        }
        p2Wins = (p2Count > p3Count);
        p3Wins = !p2Wins;
      }
      break;
      
    case 5: // Race
      p2Wins = (p2RacePos >= NUM_LEDS - 1);
      p3Wins = (p3RacePos >= NUM_LEDS - 1);
      break;
      
    case 6: // Tug O War
      p2Wins = (tugBoundary >= NUM_LEDS - 1);
      p3Wins = (tugBoundary < 0);
      break;
  }
  
  if (p2Wins && !p3Wins) {
    Serial.println("Lightboard: Player 2 wins! Starting celebration...");
    startCelebration(true); // Player 2 wins
    celebrating = true;
    Serial.printf("Celebration started: celActive=%d, celebrating=%d\n", celActive, celebrating);
  } else if (p3Wins && !p2Wins) {
    Serial.println("Lightboard: Player 3 wins! Starting celebration...");
    startCelebration(false); // Player 3 wins
    celebrating = true;
    Serial.printf("Celebration started: celActive=%d, celebrating=%d\n", celActive, celebrating);
  }
}

// ===================== Demo Mode =====================
void runDemoMode() {
  static unsigned long lastDemoUpdate = 0;
  static int rainbowOffset = 0;
  
  if (millis() - lastDemoUpdate >= 50) { // Update every 50ms for smooth animation
    lastDemoUpdate = millis();
    
    if (!player1Connected) {
      // Rainbow chase effect
      rainbowOffset = (rainbowOffset + 1) % 256;
      
      // Clear strip
      for (int i = 0; i < NUM_LEDS; i++) {
        strip.setPixelColor(i, 0);
      }
      
      // Create rainbow chase using wheel function
      for (int i = 0; i < NUM_LEDS; i++) {
        int hue = (rainbowOffset + (i * 256 / NUM_LEDS)) % 256;
        strip.setPixelColor(i, wheel(hue));
      }
      
      strip.show();
    }
  }
}

// ===================== Loop =====================
void loop(){
  const unsigned long nowMs = millis();

  // Handle celebration animation
  if (celebrating) {
    if (!updateCelebration()) {
      celebrating = false;
      resetGame();
      paintProgress();
    }
  }
  
  // Track previous connection state to detect transitions
  static bool prevPlayer1Connected = false;
  bool justConnected = !prevPlayer1Connected && player1Connected;
  prevPlayer1Connected = player1Connected;
  
  // If we just left demo mode (transitioned from disconnected to connected), request state
  if (justConnected) {
    Serial.println("Left demo mode - requesting state restore");
    requestStateRestore();
  }
  
  // Run demo mode when not connected
  runDemoMode();

  // Check for connection timeout
  if (player1Connected && (millis() - lastHeartbeat > heartbeatTimeout)) {
    player1Connected = false;
    player1MacLearned = false; // Reset MAC learning to force rediscovery
    wasConnected = false; // Reset connection flag to allow demo mode again
    Serial.println("Player 1 connection lost - resetting discovery");
    clearStrip(); // Clear LEDs when disconnected
  }

  // Send heartbeat to Player 1 (start immediately after setup)
  static unsigned long lastHeartbeatSend = 0;
  if (millis() - lastHeartbeatSend >= 1000) {
    myData.action = 1; // heartbeat
    esp_now_send(player1Address, (uint8_t*)&myData, sizeof(myData));
    if (player1MacLearned) {
      Serial.println("Sent heartbeat to Player 1");
    } else {
      Serial.println("Sent heartbeat to Player 1 (waiting for connection)");
    }
    lastHeartbeatSend = millis();
  }

  // Demo mode handles LED display when not connected
}
