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
uint8_t player1Address[] = {0x78, 0x1C, 0x3C, 0xB8, 0xD5, 0xA9}; // Player 1 AP MAC

typedef struct struct_lightboard_message {
  uint8_t  deviceId;     // 3=Lightboard
  uint8_t  action;       // 1=heartbeat, 2=game-state, 3=score-update, 4=mode-change, 5=reset
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

struct_lightboard_message myData;
struct_lightboard_message player1Data;

// Connection tracking
bool player1Connected = false;
unsigned long lastHeartbeat = 0;
const unsigned long heartbeatTimeout = 2000; // 2 seconds
bool player1MacLearned = false;

// ---- Game state ----
int p1Pos = -1;
int p2Pos = NUM_LEDS;
bool celebrating = false;

// Mode 4: Score Order tracking
int nextLedPosition = 0;
int scoringSequence[NUM_LEDS];

// Mode 5: Race tracking
int p1RacePos = -1;
int p2RacePos = -1;

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
int p1ColorIndex = 0; // Red (default)
int p2ColorIndex = 1; // Blue (default)

// Prototypes to placate Arduino preprocessor with custom return types
PlayerColor getP1Color();
PlayerColor getP2Color();
uint32_t getP1ColorValue();
uint32_t getP2ColorValue();

// ==================== Celebration Manager ====================
enum CelebrationType : uint8_t {
  CEL_WINNER_CHASE = 0,
  CEL_CENTER_RIPPLE,
  CEL_CONFETTI,
  CEL_BREATHE
};

bool celActive = false;
bool celP1Wins = false;
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

void startCelebration(bool player1Wins) {
  celActive = true;
  celP1Wins = player1Wins;
  if (celP1Wins) { 
    PlayerColor c = getP1Color();
    winnerR=c.r; winnerG=c.g; winnerB=c.b; 
  }
  else { 
    PlayerColor c = getP2Color();
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
PlayerColor getP1Color() { return availableColors[p1ColorIndex]; }
PlayerColor getP2Color() { return availableColors[p2ColorIndex]; }

// Get color as uint32_t for strip.setPixelColor
uint32_t getP1ColorValue() { 
  PlayerColor c = getP1Color(); 
  return col(c.r, c.g, c.b); 
}
uint32_t getP2ColorValue() { 
  PlayerColor c = getP2Color(); 
  return col(c.r, c.g, c.b); 
}

void clearStrip(){ for (int i=0;i<NUM_LEDS;i++) strip.setPixelColor(i,0); strip.show(); }

void paintProgress() {
  for (int i=0;i<NUM_LEDS;i++) strip.setPixelColor(i,0);
  if (gameMode==2) { if(p1Pos>=0&&p1Pos<NUM_LEDS) strip.setPixelColor(p1Pos,getP1ColorValue()); if(p2Pos>=0&&p2Pos<NUM_LEDS) strip.setPixelColor(p2Pos,getP2ColorValue()); }
  else if (gameMode==3) { if(p1Pos<=CENTER_LEFT) for(int i=CENTER_LEFT;i>=p1Pos&&i>=0;i--) strip.setPixelColor(i,getP1ColorValue()); if(p2Pos>=CENTER_RIGHT) for(int i=CENTER_RIGHT;i<=p2Pos&&i<NUM_LEDS;i++) strip.setPixelColor(i,getP2ColorValue()); }
  else if (gameMode==4) { for(int i=0;i<nextLedPosition;i++){ if(scoringSequence[i]==1) strip.setPixelColor(i,getP1ColorValue()); else if(scoringSequence[i]==2) strip.setPixelColor(i,getP2ColorValue()); }}
  else if (gameMode==5) { bool p1On=(p1RacePos>=0),p2On=(p2RacePos>=0); if(p1On&&p2On&&p1RacePos==p2RacePos) { PlayerColor c1=getP1Color(),c2=getP2Color(); strip.setPixelColor(p1RacePos,col((c1.r+c2.r)/2,(c1.g+c2.g)/2,(c1.b+c2.b)/2)); } else { if(p1On) strip.setPixelColor(p1RacePos,getP1ColorValue()); if(p2On) strip.setPixelColor(p2RacePos,getP2ColorValue()); }}
  else if (gameMode==6) { for(int i=0;i<=tugBoundary&&i<NUM_LEDS;i++) strip.setPixelColor(i,getP1ColorValue()); for(int i=tugBoundary+1;i<NUM_LEDS;i++) strip.setPixelColor(i,getP2ColorValue()); }
  else { for(int i=0;i<=p1Pos&&i<NUM_LEDS;i++) strip.setPixelColor(i,getP1ColorValue()); for(int i=NUM_LEDS-1;i>=p2Pos&&i>=0;i--) strip.setPixelColor(i,getP2ColorValue()); }
  strip.show();
}

void resetGame(){ 
  switch(gameMode){
    case 1:case 2:p1Pos=-1;p2Pos=NUM_LEDS;break;
    case 3:p1Pos=CENTER_LEFT+1;p2Pos=CENTER_RIGHT-1;break;
    case 4:nextLedPosition=0;for(int i=0;i<NUM_LEDS;i++)scoringSequence[i]=0;break;
    case 5:p1RacePos=-1;p2RacePos=-1;break;
    case 6:tugBoundary=CENTER_LEFT;break;
  } 
  if(gameMode==6) paintProgress(); else clearStrip(); 
}

// ===================== ESP-NOW Callbacks =====================
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
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
    player1Connected = true;
    lastHeartbeat = millis();
    
    // Clear demo mode and go black when connected
    clearStrip();
    Serial.println("Connection established - demo mode cleared");

    if (player1Data.action == 1) {
      // Heartbeat - just update connection status
      Serial.println("Player 1 heartbeat received");
    } else if (player1Data.action == 2) {
      // Game state update
      gameMode = player1Data.gameMode;
      p1ColorIndex = player1Data.p1ColorIndex;
      p2ColorIndex = player1Data.p2ColorIndex;
      p1Pos = player1Data.p1Pos;
      p2Pos = player1Data.p2Pos;
      nextLedPosition = player1Data.nextLedPos;
      tugBoundary = player1Data.tugBoundary;
      p1RacePos = player1Data.p1RacePos;
      p2RacePos = player1Data.p2RacePos;
      celebrating = player1Data.celebrating;
      
      // Handle winner celebration
      if (player1Data.winner == 1) {
        startCelebration(true); // Player 1 wins
      } else if (player1Data.winner == 2) {
        startCelebration(false); // Player 2 wins
      }
      
      Serial.printf("Game state update: mode=%d, p1Pos=%d, p2Pos=%d, winner=%d\n", 
                   gameMode, p1Pos, p2Pos, player1Data.winner);
      paintProgress();
      
    } else if (player1Data.action == 3) {
      // Score update
      Serial.println("Score update received");
      paintProgress();
      
    } else if (player1Data.action == 4) {
      // Mode change
      gameMode = player1Data.gameMode;
      p1ColorIndex = player1Data.p1ColorIndex;
      p2ColorIndex = player1Data.p2ColorIndex;
      Serial.printf("Mode changed to: %d\n", gameMode);
      resetGame();
      
    } else if (player1Data.action == 5) {
      // Reset
      Serial.println("Reset received");
      resetGame();
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

// ===================== Demo Mode =====================
void runDemoMode() {
  static unsigned long lastDemoUpdate = 0;
  static unsigned long lastFirework = 0;
  static int rainbowOffset = 0;
  static int fireworkCount = 0;
  static int fireworkPositions[5] = {0};
  static int fireworkAges[5] = {0};
  static bool fireworkActive[5] = {false};
  
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
      
      // Add fireworks every 2-4 seconds
      if (millis() - lastFirework >= random(2000, 4000)) {
        lastFirework = millis();
        
        // Find an inactive firework slot
        for (int i = 0; i < 5; i++) {
          if (!fireworkActive[i]) {
            fireworkPositions[i] = random(0, NUM_LEDS);
            fireworkAges[i] = 0;
            fireworkActive[i] = true;
            break;
          }
        }
      }
      
      // Update and draw fireworks
      for (int i = 0; i < 5; i++) {
        if (fireworkActive[i]) {
          fireworkAges[i]++;
          
          if (fireworkAges[i] > 30) {
            fireworkActive[i] = false; // Remove old firework
          } else {
            // Draw firework explosion
            int pos = fireworkPositions[i];
            int age = fireworkAges[i];
            
            // Main firework center
            if (age < 10) {
              // Growing explosion
              int radius = age;
              for (int j = max(0, pos - radius); j <= min(NUM_LEDS-1, pos + radius); j++) {
                int distance = abs(j - pos);
                if (distance <= radius) {
                  float intensity = 1.0f - (float)distance / (float)radius;
                  intensity *= (1.0f - (float)age / 10.0f); // Fade out
                  
                  // Random colors for fireworks
                  uint8_t r = random(200, 255);
                  uint8_t g = random(100, 255);
                  uint8_t b = random(100, 255);
                  
                  uint32_t fireworkColor = strip.Color(
                    (uint8_t)(r * intensity),
                    (uint8_t)(g * intensity),
                    (uint8_t)(b * intensity)
                  );
                  
                  strip.setPixelColor(j, fireworkColor);
                }
              }
            } else {
              // Sparkle trail
              int sparkleCount = 20 - age;
              for (int s = 0; s < sparkleCount; s++) {
                int sparklePos = pos + random(-5, 6);
                if (sparklePos >= 0 && sparklePos < NUM_LEDS) {
                  float intensity = (float)(20 - age) / 20.0f;
                  uint32_t sparkleColor = strip.Color(
                    (uint8_t)(255 * intensity),
                    (uint8_t)(255 * intensity),
                    (uint8_t)(255 * intensity)
                  );
                  strip.setPixelColor(sparklePos, sparkleColor);
                }
              }
            }
          }
        }
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
  
  // Run demo mode when not connected
  runDemoMode();

  // Check for connection timeout
  if (player1Connected && (millis() - lastHeartbeat > heartbeatTimeout)) {
    player1Connected = false;
    player1MacLearned = false; // Reset MAC learning to force rediscovery
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

  // Update LED based on connection status
  if (!player1Connected) {
    // Show connection status with a slow blink
    static unsigned long lastBlink = 0;
    if (millis() - lastBlink >= 1000) {
      static bool blinkState = false;
      blinkState = !blinkState;
      if (blinkState) {
        strip.setPixelColor(0, strip.Color(255, 0, 0)); // Red for disconnected
        strip.setPixelColor(NUM_LEDS-1, strip.Color(255, 0, 0));
      } else {
        strip.setPixelColor(0, 0);
        strip.setPixelColor(NUM_LEDS-1, 0);
      }
      strip.show();
      lastBlink = millis();
    }
  }
}
