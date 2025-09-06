#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_NeoPixel.h>

// ---- LED strip config ----
#define LED_PIN      13
#define NUM_LEDS     38
#define BRIGHTNESS   50   // keep modest for USB power
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// ---- WiFi config ----
const char* ssid     = "McKays";
const char* password = "muffin4444";

// ---- Web server ----
WebServer server(80);

// ---- Game state ----
// p1 starts off the strip at -1 and moves +1 (right)
// p2 starts off the strip at NUM_LEDS and moves -1 (left)
int p1Pos = -1;
int p2Pos = NUM_LEDS;
bool celebrating = false;

// Game modes: 1=Head to Head, 2=Race to the End, 3=Get Home
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
      <option value="1">Head to Head</option>
      <option value="2">Race to the End</option>
      <option value="3">Get Home</option>
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

// ---- helpers ----
inline uint32_t col(uint8_t r, uint8_t g, uint8_t b){ return strip.Color(r,g,b); }

void clearStrip() {
  for (int i=0;i<NUM_LEDS;i++) strip.setPixelColor(i, 0);
  strip.show();
}

void paintProgress() {
  // Clear all LEDs first
  for (int i=0;i<NUM_LEDS;i++) strip.setPixelColor(i, 0);
  
  if (gameMode == 2) {
    // Race to the End: only show single LED per player
    if (p1Pos >= 0 && p1Pos < NUM_LEDS) strip.setPixelColor(p1Pos, col(255,0,0));
    if (p2Pos >= 0 && p2Pos < NUM_LEDS) strip.setPixelColor(p2Pos, col(0,80,255));
  } else if (gameMode == 3) {
    // Get Home: show progress from center toward home ends
    // Player 1: light from center (19) down to p1Pos (toward left end)
    for (int i=NUM_LEDS/2; i>=p1Pos && i>=0; i--) strip.setPixelColor(i, col(255,0,0));
    // Player 2: light from center (19) up to p2Pos (toward right end)
    for (int i=NUM_LEDS/2; i<=p2Pos && i<NUM_LEDS; i++) strip.setPixelColor(i, col(0,80,255));
  } else {
    // Head to Head: show progress from starting position
    for (int i=0;i<=p1Pos && i<NUM_LEDS; i++) strip.setPixelColor(i, col(255,0,0));
    for (int i=NUM_LEDS-1; i>=p2Pos && i>=0; i--) strip.setPixelColor(i, col(0,80,255));
  }
  strip.show();
}

void resetGame() {
  switch(gameMode) {
    case 1: // Head to Head - start at opposite ends
    case 2: // Race to the End - start at opposite ends
      p1Pos = -1;
      p2Pos = NUM_LEDS;
      break;
    case 3: // Get Home - start at center
      p1Pos = NUM_LEDS / 2;
      p2Pos = NUM_LEDS / 2;
      break;
  }
  clearStrip();
}

// celebration in winner's color: a few chases + full flashes
void celebrate(bool player1Wins) {
  celebrating = true;
  
  // Set colors based on winner
  uint8_t r, g, b;
  if (player1Wins) {
    // Player 1 wins - red celebration
    r = 255; g = 0; b = 0;
  } else {
    // Player 2 wins - blue celebration  
    r = 0; g = 80; b = 255;
  }

  // chase 2 rounds
  for (int round=0; round<2; round++) {
    for (int i=0; i<NUM_LEDS; i++) {
      // trailing dots
      for (int j=0; j<NUM_LEDS; j++) strip.setPixelColor(j, 0);
      strip.setPixelColor(i, col(r, g, b));
      if (i-2 >= 0) strip.setPixelColor(i-2, col(r/3, g/3, b/3));
      if (i-4 >= 0) strip.setPixelColor(i-4, col(r/6, g/6, b/6));
      strip.show();
      delay(15);
    }
  }

  // 3 winner color flashes
  for (int k=0;k<3;k++) {
    for (int i=0;i<NUM_LEDS;i++) strip.setPixelColor(i, col(r, g, b));
    strip.show();
    delay(120);
    for (int i=0;i<NUM_LEDS;i++) strip.setPixelColor(i, 0);
    strip.show();
    delay(120);
  }

  resetGame();
  celebrating = false;
}

void checkWinConditions() {
  bool gameOver = false;
  bool player1Wins = false;
  
  switch(gameMode) {
    case 1: // Head to Head - who has more LEDs when they meet
      if (p1Pos >= p2Pos) {
        int p1Leds = p1Pos + 1;
        int p2Leds = NUM_LEDS - p2Pos;
        gameOver = true;
        player1Wins = (p1Leds >= p2Leds); // Player 1 wins on tie
      }
      break;
      
    case 2: // Race to the End - first to reach opposite end wins
      if (p1Pos >= NUM_LEDS - 1) {
        gameOver = true;
        player1Wins = true; // Player 1 reached the end
      } else if (p2Pos <= 0) {
        gameOver = true;
        player1Wins = false; // Player 2 reached the end
      }
      break;
      
    case 3: // Get Home - first to reach their home end wins
      if (p1Pos <= 0) {
        gameOver = true;
        player1Wins = true; // Player 1 reached home (left end)
      } else if (p2Pos >= NUM_LEDS - 1) {
        gameOver = true;
        player1Wins = false; // Player 2 reached home (right end)
      }
      break;
  }
  
  if (gameOver) {
    celebrate(player1Wins);
  }
}

// ---- HTTP handlers ----
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleP1() {
  if (celebrating) { server.send(200, "text/plain", "Celebrating…"); return; }
  
  if (gameMode == 2) {
    // Race to the End: check for collision and jump over
    if (p1Pos + 1 == p2Pos) {
      // Collision detected - jump over Player 2
      p1Pos = p2Pos + 1;
    } else if (p1Pos < NUM_LEDS-1) {
      p1Pos++;
    }
  } else if (gameMode == 3) {
    // Get Home mode: Player 1 moves left (toward home)
    if (p1Pos > 0) p1Pos--;
  } else {
    // Head to Head mode: Player 1 moves right
    if (p1Pos < NUM_LEDS-1) p1Pos++;
  }
  
  paintProgress();
  checkWinConditions();
  server.send(200, "text/plain", String("P1 at ") + p1Pos);
}

void handleP2() {
  if (celebrating) { server.send(200, "text/plain", "Celebrating…"); return; }
  
  if (gameMode == 2) {
    // Race to the End: check for collision and jump over
    if (p2Pos - 1 == p1Pos) {
      // Collision detected - jump over Player 1
      p2Pos = p1Pos - 1;
    } else if (p2Pos > 0) {
      p2Pos--;
    }
  } else if (gameMode == 3) {
    // Get Home mode: Player 2 moves right (toward home)
    if (p2Pos < NUM_LEDS-1) p2Pos++;
  } else {
    // Head to Head mode: Player 2 moves left
    if (p2Pos > 0) p2Pos--;
  }
  
  paintProgress();
  checkWinConditions();
  server.send(200, "text/plain", String("P2 at ") + p2Pos);
}

void handleMode() {
  if (server.hasArg("mode")) {
    int newMode = server.arg("mode").toInt();
    if (newMode >= 1 && newMode <= 3) {
      gameMode = newMode;
      resetGame(); // Reset game when changing modes
      server.send(200, "text/plain", "Mode " + String(gameMode) + " set");
    } else {
      server.send(400, "text/plain", "Invalid mode");
    }
  } else {
    server.send(400, "text/plain", "Missing mode parameter");
  }
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ---- setup / loop ----
void setup() {
  Serial.begin(115200);
  delay(200);

  strip.begin();
  strip.setBrightness(BRIGHTNESS);
  clearStrip();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  uint8_t dots=0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
    dots++; if (dots>40) { Serial.println("\nStill trying…"); dots=0; }
  }
  Serial.println("\nWiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/p1", HTTP_POST, handleP1);
  server.on("/p2", HTTP_POST, handleP2);
  server.on("/mode", HTTP_POST, handleMode);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();
}
