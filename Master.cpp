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
</style>
</head>
<body>
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
  async function hit(path) {
    try {
      const r = await fetch(path, { method:'POST' });
      const t = await r.text();
      statusEl.textContent = t || 'OK';
    } catch(e) {
      statusEl.textContent = 'Error';
    }
  }
  document.getElementById('p1').addEventListener('click', ()=>hit('/p1'));
  document.getElementById('p2').addEventListener('click', ()=>hit('/p2'));
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
  // Player 1 = red from left edge to p1Pos
  for (int i=0;i<NUM_LEDS;i++) strip.setPixelColor(i, 0);
  for (int i=0;i<=p1Pos && i<NUM_LEDS; i++) strip.setPixelColor(i, col(255,0,0));
  for (int i=NUM_LEDS-1; i>=p2Pos && i>=0; i--) strip.setPixelColor(i, col(0,80,255)); // Player 2 = blue from right edge down to p2Pos
  strip.show();
}

void resetGame() {
  p1Pos = -1;
  p2Pos = NUM_LEDS;
  clearStrip();
}

// simple green celebration: a few chases + full flashes
void celebrate() {
  celebrating = true;

  // chase 2 rounds
  for (int round=0; round<2; round++) {
    for (int i=0; i<NUM_LEDS; i++) {
      // trailing dots
      for (int j=0; j<NUM_LEDS; j++) strip.setPixelColor(j, 0);
      strip.setPixelColor(i, col(0,180,0));
      if (i-2 >= 0) strip.setPixelColor(i-2, col(0,60,0));
      if (i-4 >= 0) strip.setPixelColor(i-4, col(0,20,0));
      strip.show();
      delay(15);
    }
  }

  // 3 green flashes
  for (int k=0;k<3;k++) {
    for (int i=0;i<NUM_LEDS;i++) strip.setPixelColor(i, col(0,200,0));
    strip.show();
    delay(120);
    for (int i=0;i<NUM_LEDS;i++) strip.setPixelColor(i, 0);
    strip.show();
    delay(120);
  }

  resetGame();
  celebrating = false;
}

void checkMeetAndMaybeCelebrate() {
  if (p1Pos >= p2Pos) {
    celebrate();
  }
}

// ---- HTTP handlers ----
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleP1() {
  if (celebrating) { server.send(200, "text/plain", "Celebrating…"); return; }
  if (p1Pos < NUM_LEDS-1) p1Pos++;
  paintProgress();
  checkMeetAndMaybeCelebrate();
  server.send(200, "text/plain", String("P1 at ") + p1Pos);
}

void handleP2() {
  if (celebrating) { server.send(200, "text/plain", "Celebrating…"); return; }
  if (p2Pos > 0) p2Pos--;
  paintProgress();
  checkMeetAndMaybeCelebrate();
  server.send(200, "text/plain", String("P2 at ") + p2Pos);
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
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();
}
