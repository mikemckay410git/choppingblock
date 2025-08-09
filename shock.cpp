#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <math.h>
#include <Preferences.h>

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
static const unsigned long BROADCAST_INTERVAL_MS = 25;   // periodic lightweight WS

// Wi‑Fi AP creds
static const char* AP_SSID = "ToolBoard";
static const char* AP_PASS = "12345678";
// =======================================================


// ===================== Globals =====================
WebServer server(80);
WebSocketsServer ws(81);
Preferences g_prefs;

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

unsigned long g_lastResultMs = 0;
unsigned long g_lastBroadcastMs = 0;

struct HitResult {
  bool   valid = false;
  float  x = 0, y = 0;  // meters in same top-left frame
  int    haveTimes = 0;
  String mode = "none";
} g_lastHit;


// ===================== UI (canvas + raw table) =====================
const char HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>Axe Board TDoA</title>
<style>
body{font-family:Arial;margin:16px}
#c{border:1px solid #333;background:#fafafa}
table{border-collapse:collapse;margin-top:10px}
th,td{border:1px solid #999;padding:6px 10px;text-align:right}
th:first-child,td:first-child{text-align:left}
small{color:#666}
.badge{display:inline-block;padding:2px 6px;border-radius:4px;background:#eee;margin-right:8px}
.toast{position:fixed;left:50%;transform:translateX(-50%);bottom:16px;background:#222;color:#fff;padding:8px 12px;border-radius:6px;opacity:0;transition:opacity .2s}
.toast.show{opacity:0.92}
</style>
</head><body>
<h2>Axe Hit Localizer</h2>
<small>AP SSID: ToolBoard, pass: 12345678</small>
<div>WS: <span id=s>warming…</span></div>

<canvas id=c width=320 height=320></canvas>
<p><b>Estimate:</b> <span id=est>(waiting)</span></p>
<p><b>Wave speed:</b> <span id=vs>—</span> m/s</p>
<p>
  <span class="badge">mask: <span id=mask>-</span></span>
  <span class="badge">t0: <span id=t0>-</span></span>
</p>

<table>
  <thead>
    <tr><th>Sensor</th><th>First&nbsp;µs</th><th>Last&nbsp;µs</th><th>Edges</th></tr>
  </thead>
  <tbody>
    <tr><td>S0 Top</td><td id=f0>-</td><td id=l0>-</td><td id=c0>-</td></tr>
    <tr><td>S1 Bottom</td><td id=f1>-</td><td id=l1>-</td><td id=c1>-</td></tr>
    <tr><td>S2 Right</td><td id=f2>-</td><td id=l2>-</td><td id=c2>-</td></tr>
    <tr><td>S3 Left</td><td id=f3>-</td><td id=l3>-</td><td id=c3>-</td></tr>
  </tbody>
</table>

<script>
const ws=new WebSocket('ws://'+location.hostname+':81');
const c=document.getElementById('c'), ctx=c.getContext('2d');

// Board is 0..0.4 m in both axes, origin top-left.
const BOARD_SIZE_M = 0.4;

// Sensor markers (must match firmware index order & coords)
  const pts=[
  [0.200, 0.100], // S0 Top
  [0.200, 0.300], // S1 Bottom
  [0.300, 0.200], // S2 Right
  [0.100, 0.200]  // S3 Left
  ];

// Calibration targets (meters)
const targets=[
  {name:'Center', x:0.200, y:0.200},
  {name:'Top',    x:0.200, y:0.100},
  {name:'Bottom', x:0.200, y:0.300},
  {name:'Left',   x:0.100, y:0.200},
  {name:'Right',  x:0.300, y:0.200}
];

let calibActive=false;
let targetIdx=0;
let sum_ab=0, sum_bb=0, sampleCount=0;
let vsEst=null;
let waitingApply=false;

// toast helper
const toast=document.createElement('div');
toast.id='toast'; toast.className='toast'; document.body.appendChild(toast);
function showToast(msg,bg){
  toast.textContent=msg; if(bg) toast.style.background=bg; else toast.style.background='#222';
  toast.classList.add('show');
  setTimeout(()=>toast.classList.remove('show'), 1500);
}

function draw(x,y){
  ctx.clearRect(0,0,c.width,c.height);
  ctx.strokeRect(0,0,c.width,c.height);

  // draw sensors
  for(const p of pts){
    const mx=(p[0]/BOARD_SIZE_M)*c.width;
    const my=(p[1]/BOARD_SIZE_M)*c.height;
    ctx.beginPath(); ctx.arc(mx,my,4,0,Math.PI*2); ctx.stroke();
  }

  // draw calibration target if active
  if (calibActive && targets[targetIdx]){
    const tx=(targets[targetIdx].x/BOARD_SIZE_M)*c.width;
    const ty=(targets[targetIdx].y/BOARD_SIZE_M)*c.height;
    ctx.save();
    ctx.strokeStyle='#d00';
    ctx.beginPath(); ctx.moveTo(tx-8,ty); ctx.lineTo(tx+8,ty); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(tx,ty-8); ctx.lineTo(tx,ty+8); ctx.stroke();
    ctx.restore();
  }

  if (typeof x === 'number' && typeof y === 'number'){
    const hx=(x/BOARD_SIZE_M)*c.width;
    const hy=(y/BOARD_SIZE_M)*c.height;
    ctx.beginPath(); ctx.arc(hx,hy,6,0,Math.PI*2); ctx.fill();
  }
}

ws.onopen=()=>document.getElementById('s').textContent='connected';
ws.onclose=()=>document.getElementById('s').textContent='disconnected';
ws.onmessage=e=>{
  const d=JSON.parse(e.data);

  if(d.mode){
    document.getElementById('est').textContent=
      `x=${(d.x*1000).toFixed(0)} mm, y=${(d.y*1000).toFixed(0)} mm (mode: ${d.mode}, sensors: ${d.n})`;
    draw(d.x,d.y);
  }

  if(typeof d.vs === 'number'){
    document.getElementById('vs').textContent = d.vs.toFixed(1);
    if (d.ack === 'set_vs'){
      waitingApply=false;
      document.getElementById('btnApply').disabled=false;
      showToast(`Calibration applied: vs=${d.vs.toFixed(1)} m/s`, '#2a8f2a');
    }
  }

  if (Array.isArray(d.t) && Array.isArray(d.last) && Array.isArray(d.cnt)) {
    document.getElementById('mask').textContent = '0b' + (d.mask>>>0).toString(2);
    document.getElementById('t0').textContent = d.t0 || 0;
    for (let i=0;i<4;i++){
      document.getElementById('f'+i).textContent   = d.t[i]   !== null ? d.t[i]   : '-';
      document.getElementById('l'+i).textContent   = d.last[i]!== null ? d.last[i]: '-';
      document.getElementById('c'+i).textContent   = d.cnt[i];
    }

    // Calibration accumulation
    if (calibActive && targets[targetIdx]){
      const t = d.t;
      // find reference (earliest -> 0µs)
      let ref = t.findIndex(v=>v===0);
      if (ref < 0){
        let min=Infinity, idx=-1; for(let i=0;i<4;i++){ if (t[i]!==null && t[i]<min){min=t[i]; idx=i;} } ref=idx;
      }
      if (ref>=0){
        const tx = targets[targetIdx].x, ty = targets[targetIdx].y;
        const dxr = tx - pts[ref][0], dyr = ty - pts[ref][1];
        const Dr = Math.hypot(dxr, dyr) || 1e-9;
        let used=false;
        for (let i=0;i<4;i++){
          if (i===ref || t[i]===null) continue;
          const dxi = tx - pts[i][0], dyi = ty - pts[i][1];
          const Di = Math.hypot(dxi, dyi) || 1e-9;
          const a = (Di - Dr);                // meters
          const b = (t[i]) * 1e-6;            // seconds
          sum_ab += a * b;
          sum_bb += b * b;
          used = true;
        }
        if (used){
          sampleCount++;
          vsEst = sum_ab / Math.max(sum_bb, 1e-12);
          document.getElementById('sampleCount').textContent = String(sampleCount);
          document.getElementById('vsEst').textContent = isFinite(vsEst)? vsEst.toFixed(1): '—';
          document.getElementById('btnApply').disabled = !(vsEst && isFinite(vsEst) && sampleCount>=5);
          document.getElementById('btnNext').disabled = (targetIdx >= targets.length - 1);
        }
      }
    }
  }
};

// Calibration controls UI
const controls = document.createElement('div');
controls.innerHTML = `
  <h3>Calibration</h3>
  <p>Target: <span id=calTargetName>-</span> (<span id=calTargetX>-</span> m, <span id=calTargetY>-</span> m)</p>
  <button id=btnStart>Start</button>
  <button id=btnNext disabled>Next target</button>
  <button id=btnReset>Reset</button>
  <button id=btnApply disabled>Apply vs</button>
  <div>Samples: <span id=sampleCount>0</span>, vs est: <span id=vsEst>—</span> m/s</div>
`;
document.body.appendChild(controls);

function updateTargetUI(){
  const t = targets[targetIdx]||{name:'-',x:0,y:0};
  document.getElementById('calTargetName').textContent = t.name;
  document.getElementById('calTargetX').textContent = t.x.toFixed(3);
  document.getElementById('calTargetY').textContent = t.y.toFixed(3);
  draw();
}

document.getElementById('btnStart').onclick = ()=>{
  calibActive = true;
  targetIdx = 0;
  sum_ab=0; sum_bb=0; sampleCount=0; vsEst=null;
  document.getElementById('sampleCount').textContent='0';
  document.getElementById('vsEst').textContent='—';
  document.getElementById('btnApply').disabled = true;
  document.getElementById('btnNext').disabled = (targets.length<=1);
  updateTargetUI();
};
document.getElementById('btnNext').onclick = ()=>{
  if (targetIdx < targets.length-1){ targetIdx++; updateTargetUI(); }
  document.getElementById('btnNext').disabled = (targetIdx >= targets.length-1);
};
document.getElementById('btnReset').onclick = ()=>{
  sum_ab=0; sum_bb=0; sampleCount=0; vsEst=null;
  document.getElementById('sampleCount').textContent='0';
  document.getElementById('vsEst').textContent='—';
  document.getElementById('btnApply').disabled = true;
};
document.getElementById('btnApply').onclick = ()=>{
  if (vsEst && isFinite(vsEst)){
    waitingApply=true;
    document.getElementById('btnApply').disabled=true;
    showToast('Applying calibration…', '#444');
    ws.send(JSON.stringify({set_vs: vsEst}));
  }
};
updateTargetUI();
</script>
</body></html>
)HTML";


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


// ===================== Web handler =====================
void handleRoot(){ server.send(200,"text/html",HTML); }

static const char* edgeModeName(int m){
  switch(m){
    case RISING:  return "RISING";
    case FALLING: return "FALLING";
    case CHANGE:  return "CHANGE";
    default:      return "UNKNOWN";
  }
}


// ===================== Setup =====================
void setup(){
  Serial.begin(115200);
  delay(50);
  Serial.println();
  Serial.println(F("=== Axe Hit Localizer (ISR-min + TDoA + Raw UI) ==="));
  Serial.printf("GPIOs: [%d, %d, %d, %d]\r\n", SENSOR_PINS[0],SENSOR_PINS[1],SENSOR_PINS[2],SENSOR_PINS[3]);
  Serial.printf("Edge mode: %s\r\n", edgeModeName(EDGE_MODE));
  Serial.printf("V_SOUND: %.1f m/s, CAPTURE_WINDOW_US: %lu, DEADTIME_MS: %lu\r\n",
                V_SOUND, CAPTURE_WINDOW_US, DEADTIME_MS);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  for(int i=0;i<SENSOR_COUNT;i++){
    pinMode(SENSOR_PINS[i], INPUT); // comparator push-pull output (for open-collector use external pull-ups)
  }

  // Load persisted calibration
  g_prefs.begin("shock", false);
  float vsSaved = g_prefs.getFloat("vs", V_SOUND);
  if (vsSaved > 0.0f && vsSaved < 20000.0f) {
    V_SOUND = vsSaved;
  }
  Serial.printf("V_SOUND (loaded): %.1f m/s\r\n", V_SOUND);

  // Wi‑Fi AP + web UI
  WiFi.mode(WIFI_AP);
  if (WiFi.softAP(AP_SSID, AP_PASS)){
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("SoftAP started: SSID=%s PASS=%s IP=%s\r\n", AP_SSID, AP_PASS, ip.toString().c_str());
  } else {
    Serial.println(F("SoftAP start FAILED"));
  }

  server.on("/", handleRoot);
  server.begin();
  ws.begin();
  ws.onEvent([](uint8_t num, WStype_t type, uint8_t * payload, size_t length){
    if (type == WStype_TEXT && payload && length > 0) {
      String s((const char*)payload, length);
      // very small JSON parser for {"set_vs": number}
      int k = s.indexOf("\"set_vs\"");
      if (k >= 0) {
        int colon = s.indexOf(':', k);
        if (colon > 0) {
          // extract number starting at colon+1
          int i = colon + 1;
          while (i < (int)length && isspace((unsigned char)s[i])) i++;
          // read until non-number char (supports decimal and exponent minimal)
          int j = i;
          while (j < (int)length && (isdigit((unsigned char)s[j]) || s[j]=='+' || s[j]=='-' || s[j]=='.' || s[j]=='e' || s[j]=='E')) j++;
          float vsNew = s.substring(i, j).toFloat();
          if (vsNew > 500.0f && vsNew < 20000.0f) {
            V_SOUND = vsNew;
            g_prefs.putFloat("vs", V_SOUND);
            String ack = String("{\"ack\":\"set_vs\",\"vs\":") + String(V_SOUND,1) + "}";
            ws.sendTXT(num, ack);
          }
        }
      }
    }
  });

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
}


// ===================== Loop =====================
void loop(){
  server.handleClient();
  ws.loop();

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

    // ===== Build one-shot WS JSON with raw data =====
    String j = "{";
    j += "\"mode\":\""+String(r.mode)+"\",";
    j += "\"x\":"+String(r.valid? r.x:0,4)+",\"y\":"+String(r.valid? r.y:0,4)+",\"n\":"+String(r.haveTimes)+",";
    j += "\"mask\":"+String(mask)+",\"t0\":"+String(t0copy)+",";
    j += "\"vs\":"+String(V_SOUND,1)+",";

    j += "\"t\":[";
    for(int i=0;i<SENSOR_COUNT;i++){ if (tcopy[i]) j += String((long)(tcopy[i]-t0copy)); else j += "null"; if(i<SENSOR_COUNT-1) j+=','; }
    j += "],\"last\":[";
    for(int i=0;i<SENSOR_COUNT;i++){ if (lastEdgeCopy[i]) j += String((long)(lastEdgeCopy[i]-t0copy)); else j += "null"; if(i<SENSOR_COUNT-1) j+=','; }
    j += "],\"cnt\":[";
    for(int i=0;i<SENSOR_COUNT;i++){ j += String(cntCopy[i]); if(i<SENSOR_COUNT-1) j+=','; }
    j += "]}";
    ws.broadcastTXT(j);

    // Store for periodic/lightweight broadcast
    if (r.valid) { g_lastHit = r; g_lastResultMs = nowMs; }
    else { g_lastHit.valid = false; }

    // Deadtime (keep servicing web so UI stays alive)
    unsigned long start = nowMs;
    while (millis()-start < DEADTIME_MS) { server.handleClient(); ws.loop(); delay(1); }

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

  // (Optional) periodic lightweight broadcast; not needed since we send per-hit details
  if (nowMs - g_lastBroadcastMs >= BROADCAST_INTERVAL_MS){
    String j = "{\"vs\":"+String(V_SOUND,1);
    if (g_lastHit.valid){
      j += ",\"mode\":\""+g_lastHit.mode+"\",\"x\":"+String(g_lastHit.x,4)+",\"y\":"+String(g_lastHit.y,4)+",\"n\":"+String(g_lastHit.haveTimes);
    }
    j += "}";
    ws.broadcastTXT(j);
    g_lastBroadcastMs = nowMs;
  }
}
