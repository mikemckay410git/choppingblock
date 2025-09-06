#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_NeoPixel.h>

// ---- LED strip config ----
#define LED_PIN      13
#define NUM_LEDS     38
#define BRIGHTNESS   50
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// ---- WiFi config ----
const char* ssid     = "McKays";
const char* password = "muffin4444";

// ---- Web server ----
WebServer server(80);

// ---- Center indices ----
const int CENTER_LEFT  = (NUM_LEDS / 2) - 1; // 18
const int CENTER_RIGHT = (NUM_LEDS / 2);     // 19

// ---- Game state ----
int p1Pos = -1;
int p2Pos = NUM_LEDS;
bool celebrating = false;

// Mode 4: Score Order tracking
int nextLedPosition = 0;
int scoringSequence[NUM_LEDS]; // 0=empty, 1=Player1, 2=Player2

// Mode 5: Race tracking
int p1RacePos = -1;
int p2RacePos = -1;

// Mode 6: Tug O War tracking
int tugBoundary = CENTER_LEFT;

// Game modes
// 1=Territory, 2=Swap Sides, 3=Split Scoring, 4=Score Order, 5=Race, 6=Tug O War
int gameMode = 1;

// ---- HTML page ----
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 LED Duel</title>
<style>
  :root { font-family: system-ui, sans-serif; }
  body { margin:0; background:#0b1220; color:#eaf0ff; }
  .wrap { display:grid; grid-template-columns:1fr 1fr; gap:12px; padding:16px; height:100vh; box-sizing:border-box; }
  .card { display:flex; align-items:center; justify-content:center; border:1px solid #22315a; border-radius:16px; background:#101a33; box-shadow:0 10px 30px rgba(0,0,0,.35); }
  button { font-size: clamp(18px, 4vw, 28px); padding:18px 26px; border-radius:14px; border:0; cursor:pointer; color:white; }
  .p1 button { background:#e24343; }
  .p2 button { background:#3a7bf7; }
  button:active { transform:translateY(1px); }
  .status { position:fixed; left:50%; transform:translateX(-50%); bottom:12px; opacity:.8; font-size:14px; }
  .mode-selector { position:fixed; top:16px; left:50%; transform:translateX(-50%); z-index:10; }
  select { padding:8px 12px; border-radius:8px; border:1px solid #22315a; background:#101a33; color:#eaf0ff; font-size:14px; }
</style>
</head>
<body>
  <div class="mode-selector">
    <select id="gameMode">
      <option value="1">Territory</option>
      <option value="2">Swap Sides</option>
      <option value="3">Split Scoring</option>
      <option value="4">Score Order</option>
      <option value="5">Race</option>
      <option value="6">Tug O War</option>
    </select>
  </div>
  <div class="wrap">
    <div class="card p1">
      <button id="p1">Player 1</button>
    </div>
    <div class="card p2">
      <button id="p2">Player 2</button>
    </div>
  </div>
  <div class="status" id="status">Ready</div>
<script>
  const statusEl = document.getElementById('status');
  const gameModeSelect = document.getElementById('gameMode');

  async function hit(path) {
    try {
      const r = await fetch(path, { method:'POST' });
      const t = await r.text();
      statusEl.textContent = t || 'OK';
    } catch(e) {
      statusEl.textContent = 'Error';
    }
  }

  async function setGameMode() {
    try {
      const mode = gameModeSelect.value;
      const r = await fetch('/mode', {
        method:'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'mode=' + mode
      });
      const t = await r.text();
      statusEl.textContent = t || 'Mode set';
    } catch(e) {
      statusEl.textContent = 'Mode error';
    }
  }

  document.getElementById('p1').addEventListener('click', ()=>hit('/p1'));
  document.getElementById('p2').addEventListener('click', ()=>hit('/p2'));
  gameModeSelect.addEventListener('change', setGameMode);
</script>
</body>
</html>
)HTML";

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
uint8_t loserR=0,  loserG=0,  loserB=0;

// Clamp for floats
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

  if (celP1Wins) { winnerR=255; winnerG=0;   winnerB=0;   loserR=0; loserG=80; loserB=255; }
  else           { winnerR=0;   winnerG=80;  winnerB=255; loserR=255; loserG=0; loserB=0; }

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
        strip.setPixelColor(idx, scaleColor(winnerR, winnerG, winnerB, s));
      }
    } break;
    case CEL_CENTER_RIPPLE: {
      for (int i=0;i<NUM_LEDS;i++) {
        int d = min(abs(i - CENTER_LEFT), abs(i - CENTER_RIGHT));
        float phase = (d * 0.55f) - (t * 10.0f);
        float s = 0.5f + 0.5f * sinf(phase);
        strip.setPixelColor(i, scaleColor(winnerR, max<uint8_t>(winnerG,120), winnerB, s));
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
        bool pickWinner = (random(0,100) < 70);
        uint8_t r = pickWinner ? winnerR : loserR;
        uint8_t g = pickWinner ? winnerG : loserG;
        uint8_t b = pickWinner ? winnerB : loserB;
        confR[i] = max(confR[i], r);
        confG[i] = max(confG[i], g);
        confB[i] = max(confB[i], b);
      }
    } break;
    case CEL_BREATHE: {
      float s = 0.5f + 0.5f * sinf(t * 2.0f * 3.14159f * 2.0f);
      for (int i=0;i<NUM_LEDS;i++) {
        strip.setPixelColor(i, scaleColor(winnerR, winnerG, winnerB, s));
      }
    } break;
  }

  strip.show();
  return true;
}

// ---- helpers ----
inline uint32_t col(uint8_t r, uint8_t g, uint8_t b){ return strip.Color(r,g,b); }

void clearStrip() {
  for (int i=0;i<NUM_LEDS;i++) strip.setPixelColor(i, 0);
  strip.show();
}

void paintProgress() {
  for (int i=0;i<NUM_LEDS;i++) strip.setPixelColor(i, 0);

  if (gameMode == 2) {
    if (p1Pos >= 0 && p1Pos < NUM_LEDS) strip.setPixelColor(p1Pos, col(255,0,0));
    if (p2Pos >= 0 && p2Pos < NUM_LEDS) strip.setPixelColor(p2Pos, col(0,80,255));
  } else if (gameMode == 3) {
    if (p1Pos <= CENTER_LEFT) for (int i=CENTER_LEFT; i>=p1Pos && i>=0; i--) strip.setPixelColor(i, col(255,0,0));
    if (p2Pos >= CENTER_RIGHT) for (int i=CENTER_RIGHT; i<=p2Pos && i<NUM_LEDS; i++) strip.setPixelColor(i, col(0,80,255));
  } else if (gameMode == 4) {
    for (int i=0; i<nextLedPosition; i++) {
      if (scoringSequence[i]==1) strip.setPixelColor(i, col(255,0,0));
      else if (scoringSequence[i]==2) strip.setPixelColor(i, col(0,80,255));
    }
  } else if (gameMode == 5) {
    bool p1On=(p1RacePos>=0), p2On=(p2RacePos>=0);
    if (p1On && p2On && p1RacePos==p2RacePos) strip.setPixelColor(p1RacePos, col(180,0,180));
    else {
      if (p1On) strip.setPixelColor(p1RacePos, col(255,0,0));
      if (p2On) strip.setPixelColor(p2RacePos, col(0,80,255));
    }
  } else if (gameMode == 6) {
    for (int i=0; i<=tugBoundary && i<NUM_LEDS; i++) strip.setPixelColor(i, col(255,0,0));
    for (int i=tugBoundary+1; i<NUM_LEDS; i++) strip.setPixelColor(i, col(0,80,255));
  } else {
    for (int i=0;i<=p1Pos && i<NUM_LEDS;i++) strip.setPixelColor(i, col(255,0,0));
    for (int i=NUM_LEDS-1;i>=p2Pos && i>=0;i--) strip.setPixelColor(i, col(0,80,255));
  }
  strip.show();
}

void resetGame() {
  switch(gameMode) {
    case 1: case 2: p1Pos=-1; p2Pos=NUM_LEDS; break;
    case 3: p1Pos=CENTER_LEFT+1; p2Pos=CENTER_RIGHT-1; break;
    case 4: nextLedPosition=0; for(int i=0;i<NUM_LEDS;i++) scoringSequence[i]=0; break;
    case 5: p1RacePos=-1; p2RacePos=-1; break;
    case 6: tugBoundary=CENTER_LEFT; break;
  }
  if (gameMode==6) paintProgress(); else clearStrip();
}

void checkWinConditions() {
  bool gameOver=false, player1Wins=false;
  switch(gameMode) {
    case 1: if (p1Pos>=p2Pos){ gameOver=true; player1Wins=(p1Pos+1 >= NUM_LEDS-p2Pos);} break;
    case 2: if (p1Pos>=NUM_LEDS-1){gameOver=true;player1Wins=true;} else if (p2Pos<=0){gameOver=true;player1Wins=false;} break;
    case 3: if (p1Pos<=0){gameOver=true;player1Wins=true;} else if (p2Pos>=NUM_LEDS-1){gameOver=true;player1Wins=false;} break;
    case 4: if (nextLedPosition>=NUM_LEDS){ gameOver=true; int p1=0,p2=0; for(int i=0;i<NUM_LEDS;i++){if(scoringSequence[i]==1)p1++; else if(scoringSequence[i]==2)p2++;} player1Wins=(p1>p2);} break;
    case 5: if (p1RacePos>=NUM_LEDS-1){gameOver=true;player1Wins=true;} else if (p2RacePos>=NUM_LEDS-1){gameOver=true;player1Wins=false;} break;
    case 6: if (tugBoundary>=NUM_LEDS-1){gameOver=true;player1Wins=true;} else if (tugBoundary<0){gameOver=true;player1Wins=false;} break;
  }
  if (gameOver){ startCelebration(player1Wins); celebrating=true; }
}

// ---- HTTP handlers ----
void handleRoot(){ server.send_P(200,"text/html",INDEX_HTML); }

void handleP1(){
  if(celebrating){server.send(200,"text/plain","Celebrating…");return;}
  if(gameMode==2){ if(p1Pos+1==p2Pos)p1Pos=p2Pos+1; else if(p1Pos<NUM_LEDS-1)p1Pos++; }
  else if(gameMode==3){ if(p1Pos>0)p1Pos--; }
  else if(gameMode==4){ if(nextLedPosition<NUM_LEDS){scoringSequence[nextLedPosition]=1;nextLedPosition++;} }
  else if(gameMode==5){ if(p1RacePos<0)p1RacePos=0; else if(p1RacePos<NUM_LEDS-1)p1RacePos++; }
  else if(gameMode==6){ if(tugBoundary<NUM_LEDS-1)tugBoundary++; }
  else { if(p1Pos<NUM_LEDS-1)p1Pos++; }
  paintProgress(); checkWinConditions(); server.send(200,"text/plain","P1 moved");
}

void handleP2(){
  if(celebrating){server.send(200,"text/plain","Celebrating…");return;}
  if(gameMode==2){ if(p2Pos-1==p1Pos)p2Pos=p1Pos-1; else if(p2Pos>0)p2Pos--; }
  else if(gameMode==3){ if(p2Pos<NUM_LEDS-1)p2Pos++; }
  else if(gameMode==4){ if(nextLedPosition<NUM_LEDS){scoringSequence[nextLedPosition]=2;nextLedPosition++;} }
  else if(gameMode==5){ if(p2RacePos<0)p2RacePos=0; else if(p2RacePos<NUM_LEDS-1)p2RacePos++; }
  else if(gameMode==6){ if(tugBoundary>=0)tugBoundary--; }
  else { if(p2Pos>0)p2Pos--; }
  paintProgress(); checkWinConditions(); server.send(200,"text/plain","P2 moved");
}

void handleMode(){
  if(server.hasArg("mode")){
    int newMode=server.arg("mode").toInt();
    if(newMode>=1 && newMode<=6){ gameMode=newMode; resetGame(); server.send(200,"text/plain","Mode "+String(gameMode)+" set"); }
    else server.send(400,"text/plain","Invalid mode");
  } else server.send(400,"text/plain","Missing mode parameter");
}

void handleNotFound(){ server.send(404,"text/plain","Not found"); }

// ---- setup / loop ----
void setup(){
  Serial.begin(115200);
  strip.begin(); strip.setBrightness(BRIGHTNESS); clearStrip();
  randomSeed((uint32_t)esp_timer_get_time());
  WiFi.mode(WIFI_STA); WiFi.begin(ssid,password);
  while(WiFi.status()!=WL_CONNECTED){delay(400);Serial.print(".");}
  Serial.println("\nWiFi connected! IP: "+WiFi.localIP().toString());
  server.on("/",HTTP_GET,handleRoot);
  server.on("/p1",HTTP_POST,handleP1);
  server.on("/p2",HTTP_POST,handleP2);
  server.on("/mode",HTTP_POST,handleMode);
  server.onNotFound(handleNotFound);
  server.begin();
  resetGame();
}

void loop(){
  server.handleClient();
  if(celebrating){
    if(!updateCelebration()){ celebrating=false; resetGame(); paintProgress(); }
  }
}
