#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <esp_now.h>
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

// ===================== ESP-NOW Configuration =====================
// Player 2 MAC address (hardcoded for reliability)
uint8_t player2Address[] = {0x6C, 0xC8, 0x40, 0x4E, 0xEC, 0x2C}; // Player 2 STA MAC
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
struct_message player2Data;

// Connection tracking
bool player2Connected = false;
unsigned long lastHeartbeat = 0;
const unsigned long heartbeatTimeout = 2000; // 2 seconds
bool player2MacLearned = false;

// Clock synchronization
bool clockSynced = false;
int32_t clockOffset = 0; // Player2 time - Player1 time
uint32_t lastSyncTime = 0;
const unsigned long SYNC_INTERVAL = 1000; // Sync at most once per second
// =======================================================

// ===================== Game State =====================
WebServer server(80);
WebSocketsServer ws(81);
Preferences g_prefs;
int32_t g_activeWsClient = -1; // enforce single WebSocket client

// Function declarations
void resetGame();
void determineWinner();
void syncClock();

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
  uint32_t hitTime = 0;
  uint16_t hitStrength = 0;
} g_lastHit;

// Game state
bool gameActive = true;  // Start with game active
String winner = "none";
uint32_t player1HitTime = 0;
uint32_t player2HitTime = 0;
// =======================================================

// ===================== UI =====================
const char HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no, viewport-fit=cover"><meta charset="utf-8">
<title>ToolBoard Quiz</title>
<style>
:root{ --text:#e5e7eb; --ok:#10b981; --bad:#ef4444; --accent:#6366f1; --accent2:#8b5cf6; --bg:#0b1220; --card:#101a33; --ink:#eaf0ff; --muted:#aab6d3; }
html{ -webkit-text-size-adjust:100%; text-size-adjust:100%; }
html,body{height:100%}
body{ margin:0; font-family:system-ui,Segoe UI,Roboto,Arial; color:var(--text);
  background:radial-gradient(1200px 600px at 50% -20%, #1e293b 0%, #0b1220 60%, #070b13 100%);
  display:flex; align-items:center; justify-content:center; padding:24px; }
.card{ background:linear-gradient(180deg, rgba(255,255,255,0.04), rgba(255,255,255,0.02));
  border:1px solid rgba(255,255,255,0.08); border-radius:16px; padding:28px 22px;
  box-shadow:0 10px 30px rgba(0,0,0,0.3), inset 0 1px 0 rgba(255,255,255,0.04); text-align:center; width:min(520px, 92vw) }
#connDot{ position:fixed; z-index:5; width:14px; height:14px; border-radius:50%;
  top:calc(env(safe-area-inset-top, 0px) + 12px); right:calc(env(safe-area-inset-right, 0px) + 12px);
  border:2px solid rgba(255,255,255,0.25); }
.ok{background:var(--ok); box-shadow:0 0 0 2px rgba(255,255,255,0.15), 0 0 14px rgba(16,185,129,.45)}
.bad{background:var(--bad); box-shadow:0 0 0 2px rgba(255,255,255,0.15), 0 0 14px rgba(239,68,68,.45)}

/* Quiz Styles */
.app { width: min(900px, 96vw); }
h1 { font-weight: 700; letter-spacing: .3px; margin: 0 0 14px; font-size: clamp(20px, 3.3vw, 32px); color: var(--accent2); }
.bar { display: flex; gap: 10px; align-items: center; justify-content: space-between; margin-bottom: 12px; flex-wrap: wrap; }
.pill { background: rgba(255,255,255,.06); border: 1px solid rgba(255,255,255,.08); color: var(--muted); padding: 8px 12px; border-radius: 999px; font-size: 13px; }

.quiz-card {
  background: linear-gradient(180deg, rgba(255,255,255,.04), rgba(255,255,255,.02));
  border: 1px solid rgba(255,255,255,.12);
  border-radius: 18px; padding: clamp(18px, 3.5vw, 28px);
  box-shadow: 0 10px 30px rgba(0,0,0,.35), 0 2px 8px rgba(0,0,0,.25);
  transition: transform .2s ease, box-shadow .2s ease;
  margin-bottom: 20px;
}
.quiz-card:active { transform: scale(.998); box-shadow: 0 6px 18px rgba(0,0,0,.35); }

.q { font-size: clamp(20px, 3vw, 28px); line-height: 1.3; margin: 0; }
.a { margin-top: 14px; font-size: clamp(18px, 2.3vw, 22px); color: #d6f2ff; display: none; }
.a.show { display: block; }

.category-badge {
  background: linear-gradient(180deg, rgba(155,225,255,.15), rgba(155,225,255,.08));
  border: 1px solid rgba(155,225,255,.2);
  border-radius: 8px;
  padding: 6px 12px;
  font-size: 12px;
  font-weight: 600;
  color: var(--accent-2);
  margin-bottom: 12px;
  display: inline-block;
  align-self: flex-start;
}

.controls { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 10px; margin-top: 16px; }
button {
  -webkit-tap-highlight-color: transparent;
  appearance: none; cursor: pointer; user-select: none;
  border: 1px solid rgba(255,255,255,.14); color: var(--ink);
  background: linear-gradient(180deg, rgba(106,161,255,.22), rgba(106,161,255,.12));
  border-radius: 14px; padding: 12px 14px; font-size: 15px; font-weight: 600;
  box-shadow: 0 10px 30px rgba(0,0,0,.35), 0 2px 8px rgba(0,0,0,.25); 
  transition: transform .1s ease, filter .2s ease, background .2s ease;
}
button:hover { filter: brightness(1.05); }
button:active { transform: translateY(1px) scale(.998); }
.ghost { background: rgba(255,255,255,.06); }

.progress { margin-top: 8px; font-size: 13px; color: var(--muted); display: flex; justify-content: space-between; align-items: center; }
.kbd { font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", monospace; font-size: 12px; color: #cfe1ff; opacity: .9; }

.hidden { 
  display: none !important; 
  visibility: hidden !important;
  opacity: 0 !important;
  height: 0 !important;
  overflow: hidden !important;
}

/* Game Mode Styles */
.game-card {
  background: linear-gradient(180deg, rgba(255,255,255,.04), rgba(255,255,255,.02));
  border: 1px solid rgba(255,255,255,.08); border-radius: 16px; padding: 28px 22px;
  box-shadow: 0 10px 30px rgba(0,0,0,0.3), inset 0 1px 0 rgba(255,255,255,0.04); text-align:center; width:min(520px, 92vw);
  margin-bottom: 20px;
}
#headline{font-size:clamp(18px, 6vw, 28px); margin:0 0 16px}
.winner{display:none; margin:8px auto 10px; padding:10px 14px; border-radius:12px; font-weight:800; font-size:clamp(18px,5.5vw,26px);
  background:linear-gradient(135deg, rgba(99,102,241,.25), rgba(139,92,246,.25)); border:1px solid rgba(99,102,241,.35);
  max-width:min(92vw, 520px); white-space:nowrap; overflow:hidden; text-overflow:ellipsis}
.btnRow{display:flex; justify-content:center; margin-top:12px}
.btn{background:linear-gradient(135deg, var(--accent), var(--accent2)); color:white; border:none; padding:12px 22px; border-radius:12px; cursor:pointer; font-size:16px; box-shadow:0 8px 20px rgba(99,102,241,.35)}
.btn:disabled{background:#374151; box-shadow:none; cursor:not-allowed}

/* Mode Toggle */
.mode-toggle {
  display: flex;
  gap: 10px;
  margin-bottom: 20px;
  justify-content: center;
}
.mode-btn {
  background: rgba(255,255,255,.06);
  border: 1px solid rgba(255,255,255,.08);
  color: var(--muted);
  padding: 8px 16px;
  border-radius: 8px;
  cursor: pointer;
  transition: all 0.2s ease;
}
.mode-btn.active {
  background: linear-gradient(180deg, rgba(106,161,255,.25), rgba(106,161,255,.15));
  border-color: var(--accent);
  color: var(--ink);
}

/* File Upload Styles */
.file-input {
  background: rgba(255,255,255,.02);
  border: 1px solid rgba(255,255,255,.06);
  border-radius: 12px;
  padding: 12px 16px;
  margin-bottom: 16px;
  display: flex;
  align-items: center;
  gap: 12px;
  flex-wrap: wrap;
}

.file-input input[type="file"] {
  display: none;
}

.file-input label {
  cursor: pointer;
  color: var(--accent);
  font-weight: 500;
  font-size: 14px;
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 6px 12px;
  background: rgba(106,161,255,.1);
  border: 1px solid rgba(106,161,255,.2);
  border-radius: 8px;
  transition: all 0.2s ease;
}

.file-input label:hover {
  background: rgba(106,161,255,.15);
  transform: translateY(-1px);
}

.file-input .hint {
  font-size: 11px;
  color: var(--muted);
  opacity: 0.8;
}

.loaded-files {
  margin-left: auto;
  font-size: 11px;
  color: var(--muted);
}

.loaded-files h3 {
  display: none;
}

.file-list {
  list-style: none;
  padding: 0;
  margin: 0;
  display: flex;
  gap: 8px;
  flex-wrap: wrap;
}

.file-list li {
  background: rgba(255,255,255,.03);
  border: 1px solid rgba(255,255,255,.06);
  border-radius: 6px;
  padding: 4px 8px;
  font-size: 11px;
  color: var(--muted);
  opacity: 0.8;
}

.file-list .success {
  color: #4ade80;
  opacity: 0.9;
}

.file-list .error {
  color: #f87171;
  opacity: 0.9;
}

/* Category Selector Styles */
.category-selector {
  background: linear-gradient(180deg, rgba(255,255,255,.04), rgba(255,255,255,.02));
  border: 1px solid rgba(255,255,255,.12);
  border-radius: 18px; padding: clamp(18px, 3.5vw, 28px);
  box-shadow: var(--shadow);
  margin-bottom: 20px;
}

.category-selector h2 {
  margin: 0 0 16px 0;
  font-size: clamp(18px, 2.5vw, 24px);
  color: var(--accent);
}

.category-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
  gap: 12px;
}

.category-btn {
  background: linear-gradient(180deg, rgba(106,161,255,.15), rgba(106,161,255,.08));
  border: 1px solid rgba(255,255,255,.12);
  border-radius: 12px;
  padding: 16px;
  text-align: center;
  cursor: pointer;
  transition: all 0.2s ease;
  font-weight: 600;
}

.category-btn:hover {
  background: linear-gradient(180deg, rgba(106,161,255,.25), rgba(106,161,255,.15));
  transform: translateY(-2px);
  box-shadow: 0 8px 25px rgba(0,0,0,.3);
}

 .category-btn.selected {
   background: linear-gradient(180deg, rgba(155,225,255,.25), rgba(155,225,255,.15));
   border-color: var(--accent-2);
 }

 /* Game Status in Quiz Mode */
 .game-status {
   background: linear-gradient(180deg, rgba(255,255,255,.04), rgba(255,255,255,.02));
   border: 1px solid rgba(255,255,255,.12);
   border-radius: 12px;
   padding: 16px;
   margin-bottom: 20px;
   text-align: center;
 }

 .status-indicator {
   display: flex;
   align-items: center;
   justify-content: center;
   gap: 8px;
   margin-bottom: 8px;
 }

 .status-dot {
   width: 12px;
   height: 12px;
   border-radius: 50%;
   background: var(--bad);
   box-shadow: 0 0 8px rgba(239,68,68,.5);
   transition: all 0.3s ease;
 }

 .status-dot.connected {
   background: var(--ok);
   box-shadow: 0 0 8px rgba(16,185,129,.5);
 }

 .status-text {
   font-size: 14px;
   color: var(--muted);
   font-weight: 500;
 }

 .winner-display {
   margin-top: 12px;
 }

 .winner-badge {
   background: linear-gradient(135deg, rgba(99,102,241,.25), rgba(139,92,246,.25));
   border: 1px solid rgba(99,102,241,.35);
   border-radius: 8px;
   padding: 8px 16px;
   font-weight: 600;
   font-size: 16px;
   color: var(--ink);
   display: inline-block;
 }

 @media (max-width: 520px) {
   .controls { grid-template-columns: 1fr; }
   .mode-toggle { flex-direction: column; }
   .category-grid { grid-template-columns: 1fr; }
 }
</style>
</head><body>
  <div id=connDot class="bad"></div>
  
  <div class="app">
    <!-- Mode Toggle -->
    <div class="mode-toggle">
      <button class="mode-btn active" id="quizModeBtn" onclick="switchMode('quiz')">📚 Quiz Mode</button>
      <button class="mode-btn" id="gameModeBtn" onclick="switchMode('game')">🎮 Game Mode</button>
    </div>

    <!-- Quiz Interface -->
    <div id="quizInterface">
      <!-- File Upload Section -->
      <div class="file-input" id="fileInputSection">
        <label for="csvFile">📁 Load CSV Files</label>
        <input type="file" id="csvFile" accept=".csv" multiple>
        <div class="hint">Hold Ctrl/Cmd for multiple files</div>
        
        <div class="loaded-files hidden" id="loadedFiles">
          <h3>Loaded Files:</h3>
          <ul class="file-list" id="fileList"></ul>
        </div>
      </div>

      <!-- Category Selector -->
      <div class="category-selector hidden" id="categorySelector">
        <h2>Choose a Quiz Category</h2>
        <div class="category-grid" id="categoryGrid">
          <div class="category-btn">Loading categories...</div>
        </div>
      </div>

             <!-- Quiz Display -->
       <div class="quiz-display hidden" id="quizDisplay">
         <div class="bar">
           <h1 id="quizTitle">Quick‑Fire Quiz</h1>
           <div class="pill"><span id="counter">Loading...</span></div>
         </div>

         <!-- Game Status in Quiz Mode -->
         <div class="game-status" id="gameStatus">
           <div class="status-indicator">
             <span class="status-dot" id="statusDot"></span>
             <span class="status-text" id="statusText">Waiting for players...</span>
           </div>
           <div class="winner-display hidden" id="winnerDisplay">
             <div class="winner-badge" id="winnerBadge">Winner: <span id="winnerName"></span></div>
           </div>
         </div>

         <div class="quiz-card" id="card" aria-live="polite">
           <div class="category-badge hidden" id="categoryBadge"></div>
           <p class="q" id="q">Loading…</p>
           <p class="a" id="a"><strong>Answer:</strong> <span id="answerText"></span></p>
         </div>

         <div class="controls" aria-label="Controls">
           <button id="prev" title="Previous (←)">◀ Prev</button>
           <button id="toggle" class="ghost" title="Show/Hide Answer (Space)">Show Answer</button>
           <button id="next" title="Next (→)">Next ▶</button>
         </div>

         <div class="progress">
           <div>Shortcuts: <span class="kbd">←/→</span> prev/next, <span class="kbd">Space</span> show</div>
           <div class="kbd">Tip: Click the card to toggle the answer.</div>
         </div>
       </div>
    </div>

    <!-- Game Interface -->
    <div id="gameInterface" class="hidden">
      <div class="game-card">
        <h2 id=headline>Game waiting...</h2>
        <div class=winner id=winnerBox style="display:none">Winner: <span id=winnerText></span></div>
        <div class=btnRow id=btnRow style="display:none"><button id=resetBtn class=btn onclick=resetGame() disabled>Reset Round</button></div>
      </div>
    </div>
  </div>

<script>
// WebSocket connection
const ws=new WebSocket('ws://'+location.hostname+':81');
const connDot=document.getElementById('connDot');
const headline=document.getElementById('headline');
const winnerBox=document.getElementById('winnerBox');
const winnerText=document.getElementById('winnerText');
const resetBtn=document.getElementById('resetBtn');
const btnRow=document.getElementById('btnRow');

// Quiz elements
const quizInterface = document.getElementById('quizInterface');
const gameInterface = document.getElementById('gameInterface');
const fileInputSection = document.getElementById('fileInputSection');
const loadedFiles = document.getElementById('loadedFiles');
const fileList = document.getElementById('fileList');
const categorySelector = document.getElementById('categorySelector');
const categoryGrid = document.getElementById('categoryGrid');
const quizDisplay = document.getElementById('quizDisplay');
const quizTitle = document.getElementById('quizTitle');
const qEl = document.getElementById('q');
const aEl = document.getElementById('a');
const answerText = document.getElementById('answerText');
const categoryBadge = document.getElementById('categoryBadge');
const counterEl = document.getElementById('counter');
const btnPrev = document.getElementById('prev');
const btnNext = document.getElementById('next');
const btnToggle = document.getElementById('toggle');
const card = document.getElementById('card');
const csvFileInput = document.getElementById('csvFile');

// Game status elements in quiz mode
const gameStatus = document.getElementById('gameStatus');
const statusDot = document.getElementById('statusDot');
const statusText = document.getElementById('statusText');
const winnerDisplay = document.getElementById('winnerDisplay');
const winnerName = document.getElementById('winnerName');

// Quiz state
let QA = [];
let order = [];
let idx = 0;
let currentMode = 'quiz';
let availableCategories = [];

// Sample quiz data (you can replace this with your CSV data)
const sampleQuestions = [
  { q: "What is the capital of France?", a: "Paris", category: "Geography" },
  { q: "What is 2 + 2?", a: "4", category: "Math" },
  { q: "What is the largest planet in our solar system?", a: "Jupiter", category: "Science" },
  { q: "Who wrote Romeo and Juliet?", a: "William Shakespeare", category: "Literature" },
  { q: "What is the chemical symbol for gold?", a: "Au", category: "Science" },
  { q: "What year did World War II end?", a: "1945", category: "History" },
  { q: "What is the main component of the sun?", a: "Hydrogen", category: "Science" },
  { q: "What is the largest ocean on Earth?", a: "Pacific Ocean", category: "Geography" },
  { q: "What is the square root of 144?", a: "12", category: "Math" },
  { q: "Who painted the Mona Lisa?", a: "Leonardo da Vinci", category: "Art" }
];

// Initialize quiz
function initQuiz() {
  // Start with sample questions
  availableCategories = [{
    filename: 'sample.csv',
    name: 'Sample Questions',
    questions: sampleQuestions
  }];
  showCategorySelector();
  createCategoryButtons(availableCategories);
}

function setOrder(randomize) {
  order = [...Array(QA.length).keys()];
  if (randomize) shuffle(order);
  idx = 0;
}

function shuffle(arr) {
  for (let i = arr.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1));
    [arr[i], arr[j]] = [arr[j], arr[i]];
  }
  return arr;
}

function render(hideAnswer = true) {
  if (QA.length === 0) return;
  
  const qa = QA[order[idx]];
  qEl.textContent = qa.q;
  answerText.textContent = qa.a;
  
  if (qa.category) {
    categoryBadge.textContent = qa.category;
    categoryBadge.classList.remove('hidden');
  } else {
    categoryBadge.classList.add('hidden');
  }
  
  if (hideAnswer) {
    aEl.classList.remove('show');
    btnToggle.textContent = 'Show Answer';
  }
  counterEl.textContent = `${idx+1} / ${QA.length}`;
}

function next() {
  if (idx < order.length - 1) { 
    idx++; 
    render(); 
  } else {
    idx = 0; 
    render(); 
  }
  
  // Rearm the game when moving to next question
  if (currentMode === 'quiz') {
    ws.send(JSON.stringify({action: 'reset'}));
    // Hide winner display and answer
    winnerDisplay.classList.add('hidden');
    aEl.classList.remove('show');
    btnToggle.textContent = 'Show Answer';
  }
}

function prev() {
  if (idx > 0) { 
    idx--; 
    render(); 
  } else {
    idx = order.length - 1; 
    render(); 
  }
}

function toggleAnswer() {
  aEl.classList.toggle('show');
  btnToggle.textContent = aEl.classList.contains('show') ? 'Hide Answer' : 'Show Answer';
}

// Mode switching
function switchMode(mode) {
  currentMode = mode;
  
  // Update button states
  document.getElementById('quizModeBtn').classList.toggle('active', mode === 'quiz');
  document.getElementById('gameModeBtn').classList.toggle('active', mode === 'game');
  
  // Show/hide interfaces
  quizInterface.classList.toggle('hidden', mode !== 'quiz');
  gameInterface.classList.toggle('hidden', mode !== 'game');
  
  // Initialize game status for quiz mode
  if (mode === 'quiz') {
    // Set initial status based on connection
    if (connDot.className === 'ok') {
      statusDot.className = 'status-dot connected';
      statusText.textContent = 'Ready to strike!';
    } else {
      statusDot.className = 'status-dot';
      statusText.textContent = 'Waiting for players...';
    }
    // Hide winner display initially
    winnerDisplay.classList.add('hidden');
  }
  
  // Send mode change to toolboard
  if (ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({action: 'mode', mode: mode}));
  }
}

// Quiz event listeners
btnNext.addEventListener('click', next);
btnPrev.addEventListener('click', prev);
btnToggle.addEventListener('click', toggleAnswer);
card.addEventListener('click', toggleAnswer);

// Keyboard shortcuts
window.addEventListener('keydown', (e) => {
  if (currentMode === 'quiz') {
    if (e.key === 'ArrowRight') { e.preventDefault(); next(); }
    else if (e.key === 'ArrowLeft') { e.preventDefault(); prev(); }
    else if (e.key === ' ' || e.code === 'Space') { e.preventDefault(); toggleAnswer(); }
  }
});

// Game functions
function showWinner(n){winnerText.textContent=n;winnerBox.style.display='inline-block';headline.textContent='Round finished';btnRow.style.display='flex';resetBtn.disabled=false}
function hideWinner(){winnerText.textContent='';winnerBox.style.display='none';btnRow.style.display='none';resetBtn.disabled=true}
function resetGame(){ws.send(JSON.stringify({action:'reset'}));hideWinner();headline.textContent=(connDot.className==='ok')?'Ready to strike!':'Game waiting...'}

// WebSocket event handling
ws.onmessage=e=>{
  const d=JSON.parse(e.data);
  if(d.connected!==undefined){
    if(d.connected){
      connDot.className='ok';
      if(winnerBox.style.display!=='inline-block')headline.textContent='Ready to strike!'
      
      // Update quiz mode status
      if(currentMode === 'quiz') {
        statusDot.className = 'status-dot connected';
        statusText.textContent = 'Ready to strike!';
      }
    }else{
      connDot.className='bad';
      hideWinner();
      headline.textContent='Game waiting...'
      
      // Update quiz mode status
      if(currentMode === 'quiz') {
        statusDot.className = 'status-dot';
        statusText.textContent = 'Waiting for players...';
      }
    }
  }
  if(d.winner!==undefined){
    if(d.winner&&d.winner!=='none'){
      showWinner(d.winner)
      
      // Update quiz mode winner display
      if(currentMode === 'quiz') {
        winnerName.textContent = d.winner;
        winnerDisplay.classList.remove('hidden');
        // Automatically reveal answer when someone wins
        aEl.classList.add('show');
        btnToggle.textContent = 'Hide Answer';
      }
    }else{
      hideWinner();
      headline.textContent=(connDot.className==='ok')?'Ready to strike!':'Game waiting...'
      
      // Update quiz mode status
      if(currentMode === 'quiz') {
        winnerDisplay.classList.add('hidden');
        // Hide answer when game resets
        aEl.classList.remove('show');
        btnToggle.textContent = 'Show Answer';
      }
    }
  }
  // Handle toolboard controls for quiz
  if(d.quizAction){
    if(currentMode === 'quiz') {
      switch(d.quizAction) {
        case 'next': next(); break;
        case 'prev': prev(); break;
        case 'toggle': toggleAnswer(); break;
      }
    }
  }
};

// === CSV PARSER ===
function parseCSV(csv) {
  const lines = csv.split('\n');
  const headers = lines[0].split(',').map(h => h.replace(/"/g, ''));
  const data = [];
  
  for (let i = 1; i < lines.length; i++) {
    if (lines[i].trim() === '') continue;
    
    const values = [];
    let current = '';
    let inQuotes = false;
    
    for (let j = 0; j < lines[i].length; j++) {
      const char = lines[i][j];
      
      if (char === '"') {
        inQuotes = !inQuotes;
      } else if (char === ',' && !inQuotes) {
        values.push(current.trim());
        current = '';
      } else {
        current += char;
      }
    }
    values.push(current.trim());
    
    const row = {};
    headers.forEach((header, index) => {
      row[header] = values[index] ? values[index].replace(/^"|"$/g, '') : '';
    });
    data.push(row);
  }
  
  return data;
}

// === FILE HANDLING ===
csvFileInput.addEventListener('change', handleFileSelect);

function handleFileSelect(event) {
  const files = event.target.files;
  if (files.length === 0) return;

  // Clear previous categories
  availableCategories = [];
  fileList.innerHTML = '';
  
  let loadedCount = 0;
  const totalFiles = files.length;

  Array.from(files).forEach((file, index) => {
    if (file.type === 'text/csv' || file.name.endsWith('.csv')) {
      const reader = new FileReader();
      
      reader.onload = function(e) {
        try {
          const csvText = e.target.result;
          const csvData = parseCSV(csvText);
          
          // Convert CSV data to the expected format
          const questions = csvData.map(row => {
            const question = row.Question || row.question || row.Q || row.q || Object.values(row)[0];
            const answer = row.Answer || row.answer || row.A || row.a || Object.values(row)[1];
            const category = row.Category || row.category || row.Cat || row.cat || 'General';
            return { q: question, a: answer, category: category };
          }).filter(qa => qa.q && qa.a);
          
          if (questions.length > 0) {
            const categoryName = file.name.replace('.csv', '').replace(/_/g, ' ').replace(/\b\w/g, l => l.toUpperCase());
            availableCategories.push({
              filename: file.name,
              name: categoryName,
              questions: questions
            });
            
            // Add success message to file list
            addFileToList(file.name, `${questions.length} questions`, 'success');
          } else {
            addFileToList(file.name, 'No questions', 'error');
          }
        } catch (error) {
          console.error('Error parsing CSV:', error);
          addFileToList(file.name, 'Error', 'error');
        }
        
        loadedCount++;
        
        // If all files are processed, show category selector
        if (loadedCount === totalFiles) {
          if (availableCategories.length > 0) {
            showCategorySelector();
            createCategoryButtons(availableCategories);
          }
        }
      };
      
      reader.onerror = function() {
        addFileToList(file.name, 'Read failed', 'error');
        loadedCount++;
        
        if (loadedCount === totalFiles) {
          if (availableCategories.length > 0) {
            showCategorySelector();
            createCategoryButtons(availableCategories);
          }
        }
      };
      
      reader.readAsText(file);
    } else {
      addFileToList(file.name, 'Not CSV', 'error');
      loadedCount++;
      
      if (loadedCount === totalFiles) {
        if (availableCategories.length > 0) {
          showCategorySelector();
          createCategoryButtons(availableCategories);
        }
      }
    }
  });
  
  // Show the loaded files section
  loadedFiles.classList.remove('hidden');
}

function addFileToList(filename, message, status) {
  const li = document.createElement('li');
  li.className = status;
  li.innerHTML = `${filename.replace('.csv', '')}: ${message}`;
  fileList.appendChild(li);
}

// === CATEGORY SELECTION ===
function showCategorySelector() {
  categorySelector.classList.remove('hidden');
  quizDisplay.classList.add('hidden');
  fileInputSection.classList.remove('hidden');
}

function showQuizDisplay() {
  categorySelector.classList.add('hidden');
  quizDisplay.classList.remove('hidden');
  fileInputSection.classList.add('hidden');
}

function createCategoryButtons(categories) {
  if (categories.length === 0) {
    categoryGrid.innerHTML = `
      <div class="category-btn" style="grid-column: 1 / -1; color: var(--muted);">
        No valid CSV files loaded. Please select CSV files with Question,Answer format.
      </div>
    `;
    return;
  }

  // Add "Combine All" button if there are multiple categories
  let buttonsHTML = '';
  if (categories.length > 1) {
    const totalQuestions = categories.reduce((sum, cat) => sum + cat.questions.length, 0);
    buttonsHTML += `
      <div class="category-btn combine-all" style="grid-column: 1 / -1; background: linear-gradient(180deg, rgba(155,225,255,.25), rgba(155,225,255,.15)); border-color: var(--accent-2);">
        <div style="font-size: 18px; margin-bottom: 4px;">🎯 Combine All Categories</div>
        <div style="font-size: 12px; color: var(--muted);">${totalQuestions} total questions from ${categories.length} categories</div>
      </div>
    `;
  }

  // Add individual category buttons
  buttonsHTML += categories.map(category => `
    <div class="category-btn" data-filename="${category.filename}">
      <div style="font-size: 18px; margin-bottom: 4px;">${category.name}</div>
      <div style="font-size: 12px; color: var(--muted);">${category.questions.length} questions</div>
    </div>
  `).join('');

  categoryGrid.innerHTML = buttonsHTML;

  // Add click handlers
  categoryGrid.querySelectorAll('.category-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      if (btn.classList.contains('combine-all')) {
        loadCombinedCategories(categories);
      } else {
        const filename = btn.dataset.filename;
        loadCategory(filename);
      }
    });
  });
}

function loadCategory(filename) {
  const category = availableCategories.find(cat => cat.filename === filename);
  if (!category) {
    alert('Category not found');
    return;
  }

  QA = category.questions;
  quizTitle.textContent = category.name;
  
  showQuizDisplay();
  setOrder(true);
  render(true);
}

function loadCombinedCategories(categories) {
  // Combine all questions from all categories
  QA = [];
  const categoryNames = [];
  
  categories.forEach(category => {
    // Add category information to each question
    const questionsWithCategory = category.questions.map(qa => ({
      ...qa,
      category: category.name
    }));
    QA = QA.concat(questionsWithCategory);
    categoryNames.push(category.name);
  });

  quizTitle.textContent = `Mixed: ${categoryNames.join(', ')}`;
  
  showQuizDisplay();
  setOrder(true);
  render(true);
}

// Initialize quiz on load
document.addEventListener('DOMContentLoaded', function() {
  initQuiz();
});
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

// ===================== ESP-NOW Callbacks =====================
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(struct_message)) return;
  
  memcpy(&player2Data, data, sizeof(player2Data));

  if (player2Data.playerId == 2) {
    // Learn Player 2 MAC dynamically to avoid manual entry issues
    if (info && !player2MacLearned) {
      memcpy(player2Address, info->src_addr, 6);
      char macStr[18];
      sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", player2Address[0],player2Address[1],player2Address[2],player2Address[3],player2Address[4],player2Address[5]);
      Serial.printf("Discovered Player 2 MAC: %s\r\n", macStr);
      esp_now_del_peer(player2Address); // ignore result
      esp_now_peer_info_t p = {};
      memcpy(p.peer_addr, player2Address, 6);
      p.channel = 0;
      p.encrypt = false;
      if (esp_now_add_peer(&p) == ESP_OK) {
        player2MacLearned = true;
        Serial.println("Player 2 peer added after discovery");
      } else {
        Serial.println("Failed to add discovered Player 2 peer");
      }
    }
    player2Connected = true;
    lastHeartbeat = millis();

    if (player2Data.action == 1) {
      // Heartbeat - just update connection status
      Serial.println("Player 2 heartbeat received");
    } else if (player2Data.action == 2) {
      // Hit detected by Player 2
      // Convert Player 2's timestamp to our time reference
      uint32_t adjustedTime = player2Data.hitTime + clockOffset;
      player2HitTime = adjustedTime;
      Serial.printf("Player 2 hit detected at %lu (adjusted from %lu) with strength %d\n", 
                   adjustedTime, player2Data.hitTime, player2Data.hitStrength);
      
      // Declare winner immediately if round active
      if (gameActive) {
        winner = "Player 2";
        gameActive = false;
        String j = "{\"winner\":\"" + winner + "\"}";
        ws.broadcastTXT(j);
        Serial.println("Winner declared: Player 2");
      }
    } else if (player2Data.action == 3) {
      // Reset request from Player 2
      resetGame();
    } else if (player2Data.action == 4) {
      // Clock synchronization response
      uint32_t now = micros();
      uint32_t roundTrip = now - player2Data.syncTime;
      uint32_t player2Time = player2Data.roundTripTime;
      
      // Calculate clock offset: (Player2 time + roundTrip/2) - Player1 time
      clockOffset = (player2Time + roundTrip/2) - now;
      clockSynced = true;
      lastSyncTime = millis();
      
      Serial.printf("Clock sync: offset=%ld us, roundTrip=%lu us\n", clockOffset, roundTrip);
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
    
    // Broadcast winner to web interface
    String j = "{\"winner\":\"" + winner + "\"}";
    ws.broadcastTXT(j);
  }
}

void resetGame() {
  winner = "none";
  player1HitTime = 0;
  player2HitTime = 0;
  gameActive = true;
  
  // Send reset to Player 2
  myData.action = 3; // reset request
  esp_now_send(player2Address, (uint8_t*)&myData, sizeof(myData));
  
  // Broadcast reset to web interface
  String j = "{\"winner\":\"none\"}";
  ws.broadcastTXT(j);
  
  Serial.println("Game reset");
}

// ===================== Clock Synchronization =====================
void syncClock() {
  if (player2Connected && (millis() - lastSyncTime >= SYNC_INTERVAL)) {
    myData.action = 4; // clock sync
    myData.syncTime = micros();
    myData.roundTripTime = 0;
    esp_now_send(player2Address, (uint8_t*)&myData, sizeof(myData));
    Serial.println("Clock sync request sent");
  }
}

// ===================== Web handler =====================
void handleRoot(){ server.send(200,"text/html",HTML); }

void handleWebSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  String j; // Declare outside switch to avoid scoping issues
  
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.printf("[%u] Disconnected!\n", num);
      if ((int32_t)num == g_activeWsClient) {
        g_activeWsClient = -1;
      }
      break;
    case WStype_CONNECTED:
      Serial.printf("[%u] Connected!\n", num);
      // allow only one UI client at a time to conserve resources
      if (g_activeWsClient >= 0 && g_activeWsClient != (int32_t)num) {
        ws.disconnect((uint8_t)g_activeWsClient);
      }
      g_activeWsClient = (int32_t)num;
      
      // Send initial state to new client
      j = "{";
      j += "\"connected\":" + String(player2Connected ? "true" : "false") + ",";
      j += "\"winner\":\"" + winner + "\"";
      j += "}";
      ws.sendTXT(num, j);
      break;
    case WStype_TEXT:
      if (length > 0) {
        String message = String((char*)payload);
        if (message.indexOf("reset") != -1) {
          resetGame();
        } else if (message.indexOf("mode") != -1) {
          // Handle mode change from web interface
          // This could be used to switch between quiz and game modes
          Serial.println("Mode change requested from web interface");
        }
      }
      break;
  }
}

// ===================== Setup =====================
void setup(){
  Serial.begin(115200);
  delay(50);
  Serial.println();
  Serial.println(F("=== Two Player Impact Game - Player 1 (Host) - SYNC VERSION ==="));
  Serial.printf("GPIOs: [%d, %d, %d, %d]\r\n", SENSOR_PINS[0],SENSOR_PINS[1],SENSOR_PINS[2],SENSOR_PINS[3]);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  for(int i=0;i<SENSOR_COUNT;i++){
    pinMode(SENSOR_PINS[i], INPUT);
  }

  // Load persisted calibration
  g_prefs.begin("shock", false);
  float vsSaved = g_prefs.getFloat("vs", V_SOUND);
  if (vsSaved > 0.0f && vsSaved < 20000.0f) {
    V_SOUND = vsSaved;
  }
  Serial.printf("V_SOUND (loaded): %.1f m/s\r\n", V_SOUND);
  
  // Clear any stored connection state to ensure clean startup
  g_prefs.clear();
  Serial.println("Cleared stored preferences for clean startup");

  // Wi‑Fi AP + web UI (keep STA active for ESP-NOW reliability)
  WiFi.mode(WIFI_AP_STA);
  if (WiFi.softAP(AP_SSID, AP_PASS, 1 /*channel*/, 0 /*hidden*/, 2 /*max conn*/)){
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("SoftAP started: SSID=%s PASS=%s IP=%s\r\n", AP_SSID, AP_PASS, ip.toString().c_str());
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

  server.on("/", handleRoot);
  server.onNotFound([](){ server.send(404, "text/plain", "Not found"); });
  server.begin();
  ws.begin();
  ws.onEvent(handleWebSocketEvent);

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

  // Add Player 2 peer with known MAC address
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, player2Address, 6);
  peerInfo.channel = 0; // follow current channel
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) == ESP_OK) {
    Serial.println("Player 2 peer added successfully");
  } else {
    Serial.println("Failed to add Player 2 peer");
  }
  // AP is already on channel 1 via softAP() call above

  myData.playerId = 1;

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
  gameActive = true;  // Ensure game starts active
  winner = "none";
  player1HitTime = 0;
  player2HitTime = 0;
  clockSynced = false;
  clockOffset = 0;
}

// ===================== Loop =====================
void loop(){
  server.handleClient();
  ws.loop();

  const unsigned long nowUs = micros();
  const unsigned long nowMs = millis();

  // Clock synchronization
  syncClock();

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

    // Count timestamps
    int have=0; for(int i=0;i<SENSOR_COUNT;i++) if (tcopy[i]) have++;

    // Minimal serial debug
    Serial.printf("Capture t0=%lu mask=0b%lu sensors=%d\r\n", t0copy, (unsigned long)mask, have);

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

      // Record Player 1 hit
  if (r.valid) {
    // Determine hit location for quiz controls
    String quizAction = "";
    if (r.x < 0.2f && r.y < 0.2f) {
      // Top-left: Previous question
      quizAction = "prev";
      Serial.println("Quiz: Previous question");
    } else if (r.x > 0.2f && r.y < 0.2f) {
      // Top-right: Next question
      quizAction = "next";
      Serial.println("Quiz: Next question");
    } else if (r.x < 0.2f && r.y > 0.2f) {
      // Bottom-left: Toggle answer
      quizAction = "toggle";
      Serial.println("Quiz: Toggle answer");
    } else if (r.x > 0.2f && r.y > 0.2f) {
      // Bottom-right: Show answer
      quizAction = "toggle";
      Serial.println("Quiz: Show answer");
    }
    
    // Send quiz action to web interface
    if (quizAction != "") {
      String quizMsg = "{\"quizAction\":\"" + quizAction + "\"}";
      ws.broadcastTXT(quizMsg);
    }
    
    // Handle game mode if active
    if (gameActive) {
      player1HitTime = r.hitTime;
      Serial.printf("Player 1 hit detected at %lu with strength %d\n", player1HitTime, r.hitStrength);
      
      // Send hit to Player 2
      myData.action = 2; // hit detected
      myData.hitTime = r.hitTime;
      myData.hitStrength = r.hitStrength;
      esp_now_send(player2Address, (uint8_t*)&myData, sizeof(myData));
      
      // Declare winner immediately
      winner = "Player 1";
      gameActive = false;
      String winMsg = "{\"winner\":\"" + winner + "\"}";
      ws.broadcastTXT(winMsg);
      Serial.println("Winner declared: Player 1");
    }
  } else {
    // Debug output to see what's happening
    Serial.println("Hit not valid - check sensor readings");
  }

    // No sensor/location broadcast in minimal UI

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

  // Periodic status broadcast
  if (nowMs - g_lastBroadcastMs >= BROADCAST_INTERVAL_MS){
    String j = "{\"connected\":" + String(player2Connected ? "true" : "false") + 
               ",\"clockSynced\":" + String(clockSynced ? "true" : "false") + "}";
    ws.broadcastTXT(j);
    g_lastBroadcastMs = nowMs;
  }

  // Check for connection timeout
  if (player2Connected && (millis() - lastHeartbeat > heartbeatTimeout)) {
    player2Connected = false;
    clockSynced = false; // Reset sync when connection lost
    player2MacLearned = false; // Reset MAC learning to force rediscovery
    Serial.println("Player 2 connection lost - resetting discovery");
  }

  // Send heartbeat to Player 2
  static unsigned long lastHeartbeatSend = 0;
  if (millis() - lastHeartbeatSend >= 1000) {
    myData.action = 1; // heartbeat
    esp_now_send(player2Address, (uint8_t*)&myData, sizeof(myData));
    Serial.println("Sent heartbeat to Player 2");
    lastHeartbeatSend = millis();
  }
}
