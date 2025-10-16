#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <ArduinoJson.h>

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
  uint8_t  deviceId;     // 1=Player1, 3=Lightboard
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

struct_message myData;
struct_message player2Data;
struct_lightboard_message lightboardData;

// Connection tracking
bool player2Connected = false;
unsigned long lastHeartbeat = 0;
const unsigned long heartbeatTimeout = 2000; // 2 seconds
bool player2MacLearned = false;

// Lightboard connection tracking
bool lightboardConnected = false;
bool lightboardWasConnected = false; // Track previous connection state for reconnection detection
unsigned long lastLightboardHeartbeat = 0;
bool lightboardMacLearned = false;

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
void resetGameForQuiz();
void determineWinner();
void syncClock();
void sendLightboardUpdate(uint8_t action);
void updateLightboardGameState();
void sendLightboardPointUpdate(uint8_t scoringPlayer);
void awardPointToPlayer(uint8_t playerId);
void awardMultiplePointsToPlayer(uint8_t playerId, int multiplier);

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
// =======================================================

// ===================== UI =====================
const char HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no, viewport-fit=cover"><meta charset="utf-8">
<title>ToolBoard Quiz</title>
<style>
:root{ --text:#e5e7eb; --ok:#10b981; --bad:#ef4444; --accent:#6366f1; --accent2:#8b5cf6; --bg:#0b1220; --card:#101a33; --ink:#eaf0ff; --muted:#aab6d3; }
html{ -webkit-text-size-adjust:100%; text-size-adjust:100%; }
html,body{min-height:100%}
body{ margin:0; font-family:system-ui,Segoe UI,Roboto,Arial; color:var(--text);
  background:radial-gradient(1200px 600px at 50% -20%, #1e293b 0%, #0b1220 60%, #070b13 100%);
  display:flex; align-items:flex-start; justify-content:center; padding:24px; }
.app-container{ margin-top:max(0px, calc(50vh - 300px)); }
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
    min-height: 200px;
    display: flex;
    flex-direction: column;
    justify-content: center;
    position: relative;
  }
.quiz-card:active { transform: scale(.998); box-shadow: 0 6px 18px rgba(0,0,0,.35); }

 .q { font-size: clamp(20px, 3vw, 28px); line-height: 1.3; margin: 0; min-height: 1.3em; }
 .a { position: absolute; bottom: 0; left: 0; right: 0; margin-top: 14px; font-size: clamp(18px, 2.3vw, 22px); color: #d6f2ff; opacity: 0; visibility: hidden; max-height: 0; overflow: hidden; padding: 0 clamp(18px, 3.5vw, 28px) clamp(18px, 3.5vw, 28px) clamp(18px, 3.5vw, 28px); }
 .a.show { opacity: 1; visibility: visible; max-height: 200px; }

 .category-badge {
   background: linear-gradient(180deg, rgba(155,225,255,.15), rgba(155,225,255,.08));
   border: 1px solid rgba(155,225,255,.2);
   border-radius: 8px;
   padding: 6px 12px;
   font-size: 12px;
   font-weight: 600;
   color: var(--accent-2);
   position: absolute;
   top: 12px;
   left: 12px;
   z-index: 1;
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





 /* Reset Button Styles */
 .reset-section {
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

 .reset-btn {
   background: linear-gradient(180deg, rgba(239,68,68,.25), rgba(239,68,68,.15)) !important;
   border: 1px solid rgba(239,68,68,.3) !important;
   color: #fca5a5 !important;
   font-size: 14px !important;
   padding: 6px 12px !important;
   border-radius: 8px !important;
   transition: all 0.2s ease;
   display: flex;
   align-items: center;
   gap: 6px;
 }

 .reset-btn:hover {
   background: linear-gradient(180deg, rgba(239,68,68,.35), rgba(239,68,68,.25)) !important;
   transform: translateY(-1px);
 }

 .reset-hint {
   font-size: 11px;
   color: var(--muted);
   opacity: 0.8;
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

/* Lightboard Settings Button - matches file-input label styling */
.lightboard-settings-btn {
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

.lightboard-settings-btn:hover {
  background: rgba(106,161,255,.15);
  transform: translateY(-1px);
}

 .file-input .hint {
   font-size: 11px;
   color: var(--muted);
   opacity: 0.8;
 }


 /* Settings Modal Styles */
 .settings-section {
   margin-bottom: 20px;
 }

 .settings-section label {
   display: block;
   font-weight: 600;
   color: var(--accent);
   margin-bottom: 8px;
   font-size: 14px;
 }

 .settings-select {
   width: 100%;
   padding: 10px 12px;
   border: 1px solid rgba(255,255,255,.14);
   border-radius: 8px;
   background: rgba(255,255,255,.06);
   color: var(--ink);
   font-size: 14px;
   font-weight: 500;
   transition: all 0.2s ease;
 }

 .settings-select:focus {
   outline: none;
   border-color: var(--accent);
   background: rgba(255,255,255,.1);
   box-shadow: 0 0 0 2px rgba(99,102,241,.2);
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
      position: relative;
      height: 80px;
      margin-bottom: 20px;
      text-align: center;
      overflow: visible;
    }

   .player-display {
     display: flex;
     justify-content: center;
     gap: 20px;
     align-items: center;
     height: 100%;
   }

               .player-tile {
       background: linear-gradient(180deg, rgba(255,255,255,.02), rgba(255,255,255,.01));
       border: 1px solid rgba(255,255,255,.08);
       border-radius: 12px;
       padding: 12px 18px;
       min-width: 100px;
       transition: all 0.3s ease;
       opacity: 0.4;
       filter: grayscale(1);
       transform: scale(0.9);
     }

         .player-tile.winner {
       background: linear-gradient(135deg, rgba(99,102,241,.25), rgba(139,92,246,.25));
       border: 1px solid rgba(99,102,241,.35);
       opacity: 1;
       filter: grayscale(0);
       transform: scale(1.05) !important;
       box-shadow: 0 8px 25px rgba(99,102,241,.3);
     }

               .player-name {
       font-weight: 600;
       font-size: 16px;
       color: var(--ink);
       text-align: center;
       cursor: pointer;
       user-select: none;
       transition: color 0.2s ease;
     }

     .player-name:hover {
       /* No hover effects */
     }

     .player-tile:not(.scorable) .player-name:hover {
       /* No hover effects */
     }

     .player-name.editing {
       background: rgba(255,255,255,.1);
       border-radius: 4px;
       padding: 2px 6px;
       outline: 2px solid var(--accent);
     }

     .player-score {
       font-size: 12px;
       color: var(--muted);
       margin-top: 4px;
       font-weight: 500;
     }

           .player-tile.scorable {
        cursor: pointer;
        /* No size changes - only winner class should change size */
      }

             .player-tile.scorable:hover {
         /* No hover effects - tiles stay at their current size */
       }

       .player-tile.scorable:active {
         /* No active effects */
       }

      .player-tile:not(.scorable) {
        cursor: default;
        transform: scale(0.9) !important;
      }

             .player-tile:not(.scorable):hover {
         transform: scale(0.9) !important;
         box-shadow: none !important;
       }

       .player-tile:not(.scorable):active {
         transform: scale(0.9) !important;
       }

       .player-tile:not(.scorable) .player-name:hover {
         color: var(--ink) !important;
       }

           /* Exit Button Styles */
      .exit-btn {
        background: rgba(255,255,255,.04) !important;
        border: 1px solid rgba(255,255,255,.08) !important;
        color: var(--muted) !important;
        font-size: 16px !important;
        padding: 6px 10px !important;
        border-radius: 8px !important;
        opacity: 0.7;
        transition: opacity 0.2s ease;
        min-width: auto !important;
        width: auto !important;
      }

      .exit-btn:hover {
        opacity: 1;
        background: rgba(255,255,255,.08) !important;
      }

                                 @media (max-width: 520px) {
        .controls { grid-template-columns: 1fr 1fr; }
        .category-grid { grid-template-columns: 1fr; }
        #connDot { display: none; }
        .progress { display: none; }
        #toggle { display: none; }
                 .exit-btn { font-size: 14px !important; padding: 4px 8px !important; }
       }

       /* Modal Dialog Styles */
       .modal-overlay {
         position: fixed;
         top: 0;
         left: 0;
         right: 0;
         bottom: 0;
         background: rgba(0, 0, 0, 0.7);
         display: flex;
         align-items: center;
         justify-content: center;
         z-index: 1000;
         backdrop-filter: blur(4px);
         transition: opacity 0.3s ease;
       }

       .modal-overlay.hidden {
         opacity: 0;
         pointer-events: none;
       }

       .modal-dialog {
         background: linear-gradient(180deg, rgba(255,255,255,.08), rgba(255,255,255,.04));
         border: 1px solid rgba(255,255,255,.12);
         border-radius: 16px;
         padding: 0;
         max-width: 400px;
         width: 90vw;
         box-shadow: 0 20px 60px rgba(0,0,0,.5), 0 8px 25px rgba(0,0,0,.3);
         transform: scale(0.9);
         transition: transform 0.3s ease;
       }

       .modal-overlay:not(.hidden) .modal-dialog {
         transform: scale(1);
       }

       .modal-header {
         padding: 20px 24px 16px;
         border-bottom: 1px solid rgba(255,255,255,.08);
       }

       .modal-header h3 {
         margin: 0;
         font-size: 18px;
         font-weight: 600;
         color: var(--accent);
       }

       .modal-content {
         padding: 20px 24px;
       }

       .modal-content p {
         margin: 0;
         font-size: 15px;
         line-height: 1.4;
         color: var(--text);
       }

       .modal-actions {
         padding: 16px 24px 20px;
         display: flex;
         gap: 12px;
         justify-content: flex-end;
       }

       .modal-btn {
         padding: 10px 20px;
         border-radius: 10px;
         font-size: 14px;
         font-weight: 600;
         border: 1px solid rgba(255,255,255,.14);
         cursor: pointer;
         transition: all 0.2s ease;
         min-width: 80px;
       }

       .modal-btn.cancel {
         background: rgba(255,255,255,.06);
         color: var(--muted);
       }

       .modal-btn.cancel:hover {
         background: rgba(255,255,255,.1);
         color: var(--text);
       }

       .modal-btn.confirm {
         background: linear-gradient(180deg, rgba(239,68,68,.25), rgba(239,68,68,.15));
         color: #fca5a5;
         border-color: rgba(239,68,68,.3);
       }

       .modal-btn.confirm:hover {
         background: linear-gradient(180deg, rgba(239,68,68,.35), rgba(239,68,68,.25));
         transform: translateY(-1px);
       }

                @media (max-width: 520px) {
           .modal-dialog {
             width: 95vw;
             margin: 20px;
           }
           
           .modal-actions {
             flex-direction: column;
           }
           
           .modal-btn {
             width: 100%;
           }
           
                        .file-input .hint {
               display: none;
             }
             
             .reset-hint {
               display: none;
             }
         }
 </style>
</head><body>
  <div id=connDot class="bad"></div>
  
     <div class="app app-container">
     <!-- Quiz Interface -->
     <div id="quizInterface">
             <!-- Reset Button -->
       <div class="reset-section">
         <button id="resetAllData" class="reset-btn">🗑️ Reset All Data</button>
         <div class="reset-hint">This will clear all loaded categories, scores, and player names</div>
       </div>

       <!-- Lightboard Settings Button -->
       <div class="file-input" id="lightboardModeSection">
         <button id="lightboardSettingsBtn" class="lightboard-settings-btn">
           💡 Lightboard
         </button>
         <div class="hint">Configure game mode and player colors</div>
       </div>

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
            <button id="exitBtn" class="exit-btn" title="Exit to Categories">↩</button>
          </div>

                                       <!-- Game Status in Quiz Mode -->
                       <div class="game-status" id="gameStatus">
              <div class="player-display">
                <div class="player-tile" id="player1Tile">
                  <div class="player-name">Player 1</div>
                  <div class="player-score" id="player1Score">0</div>
                </div>
                <div class="player-tile" id="player2Tile">
                  <div class="player-name">Player 2</div>
                  <div class="player-score" id="player2Score">0</div>
                </div>
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

           <!-- Exit Confirmation Dialog -->
      <div class="modal-overlay hidden" id="confirmModal">
        <div class="modal-dialog">
          <div class="modal-header">
            <h3>Exit Quiz?</h3>
          </div>
          <div class="modal-content">
            <p>Are you sure you want to exit this quiz and return to categories?</p>
          </div>
          <div class="modal-actions">
            <button class="modal-btn cancel" id="cancelExit">Cancel</button>
            <button class="modal-btn confirm" id="confirmExit">Exit</button>
          </div>
        </div>
      </div>

      <!-- Reset Data Confirmation Dialog -->
      <div class="modal-overlay hidden" id="resetModal">
        <div class="modal-dialog">
          <div class="modal-header">
            <h3>Reset All Data?</h3>
          </div>
          <div class="modal-content">
            <p>Are you sure you want to reset all data? This will clear:</p>
            <ul style="margin: 12px 0; padding-left: 20px; color: var(--muted);">
              <li>All loaded categories</li>
              <li>Player scores</li>
              <li>Player names</li>
              <li>Current quiz progress</li>
            </ul>
            <p style="color: #fca5a5; font-weight: 600;">This action cannot be undone.</p>
          </div>
          <div class="modal-actions">
            <button class="modal-btn cancel" id="cancelReset">Cancel</button>
            <button class="modal-btn confirm" id="confirmReset">Reset All Data</button>
          </div>
        </div>
      </div>

      <!-- Lightboard Settings Dialog -->
      <div class="modal-overlay hidden" id="lightboardModal">
        <div class="modal-dialog">
          <div class="modal-header">
            <h3>🎮 Lightboard Settings</h3>
          </div>
          <div class="modal-content">
            <div class="settings-section">
              <label for="lightboardMode">Game Mode:</label>
              <select id="lightboardMode" class="settings-select">
                <option value="1">Territory</option>
                <option value="2">Swap Sides</option>
                <option value="3">Split Scoring</option>
                <option value="4">Score Order</option>
                <option value="5">Race</option>
                <option value="6">Tug O War</option>
              </select>
            </div>
            
            <div class="settings-section">
              <label for="lightboardP1Color">Player 1 Color:</label>
              <select id="lightboardP1Color" class="settings-select">
                <option value="0">Red</option>
                <option value="1">Blue</option>
                <option value="2">Green</option>
                <option value="3">Magenta</option>
                <option value="4">Orange</option>
              </select>
            </div>
            
            <div class="settings-section">
              <label for="lightboardP2Color">Player 2 Color:</label>
              <select id="lightboardP2Color" class="settings-select">
                <option value="0">Red</option>
                <option value="1">Blue</option>
                <option value="2">Green</option>
                <option value="3">Magenta</option>
                <option value="4">Orange</option>
              </select>
            </div>
            
            <div class="settings-section">
              <label for="damageMultiplier">Damage Multiplier:</label>
              <select id="damageMultiplier" class="settings-select">
                <option value="1">Single (1x)</option>
                <option value="2">Double (2x)</option>
                <option value="3">Triple (3x)</option>
                <option value="4">Quadruple (4x)</option>
                <option value="5">Quintuple (5x)</option>
              </select>
            </div>
          </div>
          <div class="modal-actions">
            <button class="modal-btn cancel" id="cancelLightboard">Cancel</button>
            <button class="modal-btn confirm" id="confirmLightboard">Apply Settings</button>
          </div>
        </div>
      </div>
     
   </div>

<script>
// WebSocket connection
const ws=new WebSocket('ws://'+location.hostname+':81');
const connDot=document.getElementById('connDot');

// WebSocket connection handlers
ws.onopen = function() {
  console.log('WebSocket connected');
  // Send current lightboard settings to ESP32 when connection is established
  ws.send(JSON.stringify({
    action: 'lightboardSettings',
    mode: lightboardGameMode,
    p1Color: lightboardP1ColorIndex,
    p2Color: lightboardP2ColorIndex
  }));
};

 // Quiz elements
 const quizInterface = document.getElementById('quizInterface');
 const resetAllData = document.getElementById('resetAllData');
 const fileInputSection = document.getElementById('fileInputSection');
 const lightboardModeSection = document.getElementById('lightboardModeSection');
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
const btnExit = document.getElementById('exitBtn');
const card = document.getElementById('card');
const csvFileInput = document.getElementById('csvFile');

 // Modal elements
 const confirmModal = document.getElementById('confirmModal');
 const cancelExit = document.getElementById('cancelExit');
 const confirmExit = document.getElementById('confirmExit');
 const resetModal = document.getElementById('resetModal');
 const cancelReset = document.getElementById('cancelReset');
 const confirmReset = document.getElementById('confirmReset');
 const lightboardModal = document.getElementById('lightboardModal');
 const lightboardSettingsBtn = document.getElementById('lightboardSettingsBtn');
 const cancelLightboard = document.getElementById('cancelLightboard');
 const confirmLightboard = document.getElementById('confirmLightboard');

// Game status elements in quiz mode
const gameStatus = document.getElementById('gameStatus');
const player1Tile = document.getElementById('player1Tile');
const player2Tile = document.getElementById('player2Tile');
const player1Name = document.querySelector('#player1Tile .player-name');
const player2Name = document.querySelector('#player2Tile .player-name');
const player1ScoreEl = document.getElementById('player1Score');
const player2ScoreEl = document.getElementById('player2Score');

// Lightboard elements (moved to modal)
let lightboardGameMode = 1; // Default to Territory mode
let lightboardP1ColorIndex = 0; // Red
let lightboardP2ColorIndex = 1; // Blue
let damageMultiplier = 3; // Default to triple damage

// Quiz state
let QA = [];
let order = [];
let idx = 0;
let availableCategories = [];

// Player names with persistence
let player1NameText = 'Player 1';
let player2NameText = 'Player 2';

// Quiz state persistence
let currentCategory = null;
let currentQuestionIndex = 0;
let savedOrder = null; // Store the shuffled order to restore exactly

// Scoring system
let player1Score = 0;
let player2Score = 0;
let roundComplete = false;

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

// Load data from localStorage
function loadPersistedData() {
  try {
    // Load player names
    const savedPlayer1Name = localStorage.getItem('player1Name');
    const savedPlayer2Name = localStorage.getItem('player2Name');
    if (savedPlayer1Name) player1NameText = savedPlayer1Name;
    if (savedPlayer2Name) player2NameText = savedPlayer2Name;
    
    // Load scores
    const savedPlayer1Score = localStorage.getItem('player1Score');
    const savedPlayer2Score = localStorage.getItem('player2Score');
    if (savedPlayer1Score) player1Score = parseInt(savedPlayer1Score);
    if (savedPlayer2Score) player2Score = parseInt(savedPlayer2Score);
    
    // Update player name displays
    player1Name.textContent = player1NameText;
    player2Name.textContent = player2NameText;
    updateScoreDisplay();
    
    // Load categories from localStorage
    const savedCategories = localStorage.getItem('quizCategories');
    if (savedCategories) {
      availableCategories = JSON.parse(savedCategories);
      if (availableCategories.length > 0) {
        showCategorySelector();
        createCategoryButtons(availableCategories);
        
        // Load current quiz state
        const savedCurrentCategory = localStorage.getItem('currentCategory');
        const savedQuestionIndex = localStorage.getItem('currentQuestionIndex');
        const savedOrderData = localStorage.getItem('savedOrder');
        
        if (savedCurrentCategory && savedQuestionIndex !== null) {
          currentCategory = savedCurrentCategory;
          currentQuestionIndex = parseInt(savedQuestionIndex);
          
          // Restore the saved order if available
          if (savedOrderData) {
            try {
              savedOrder = JSON.parse(savedOrderData);
            } catch (error) {
              console.error('Error parsing saved order:', error);
              savedOrder = null;
            }
          }
          
          // Restore the quiz to where you left off
          restoreQuizState();
        }
        
        return; // Don't show sample questions if we have saved categories
      }
    }
  } catch (error) {
    console.error('Error loading persisted data:', error);
  }
  
  // Fallback to sample questions if no saved data
  availableCategories = [{
    filename: 'sample.csv',
    name: 'Sample Questions',
    questions: sampleQuestions
  }];
  showCategorySelector();
  createCategoryButtons(availableCategories);
}

// Save data to localStorage with deferred execution for better performance
function savePersistedData() {
  const saveData = () => {
    try {
      // Save player names
      localStorage.setItem('player1Name', player1NameText);
      localStorage.setItem('player2Name', player2NameText);
      
      // Save scores
      localStorage.setItem('player1Score', player1Score.toString());
      localStorage.setItem('player2Score', player2Score.toString());
      
      // Save categories
      localStorage.setItem('quizCategories', JSON.stringify(availableCategories));
      
      // Save current quiz state
      if (currentCategory) {
        localStorage.setItem('currentCategory', currentCategory);
        localStorage.setItem('currentQuestionIndex', currentQuestionIndex.toString());
        // Save the current question order to restore exactly
        if (order.length > 0) {
          localStorage.setItem('savedOrder', JSON.stringify(order));
        }
      }
    } catch (error) {
      console.error('Error saving persisted data:', error);
    }
  };

  // Use requestIdleCallback for better performance if available
  if (window.requestIdleCallback) {
    requestIdleCallback(saveData);
  } else {
    // Fallback for browsers that don't support requestIdleCallback
    setTimeout(saveData, 0);
  }
}

// Initialize quiz
function initQuiz() {
  loadPersistedData();
  loadLightboardSettings();
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
  
  // Batch DOM updates for better performance
  const updates = [
    { element: qEl, property: 'textContent', value: qa.q },
    { element: answerText, property: 'textContent', value: qa.a },
    { element: counterEl, property: 'textContent', value: `${idx+1} / ${QA.length}` }
  ];
  
  // Apply all updates at once
  updates.forEach(update => {
    if (update.element && update.property && update.value !== undefined) {
      update.element[update.property] = update.value;
    }
  });
  
  // Handle answer visibility efficiently
  if (hideAnswer) {
    aEl.classList.remove('show');
    btnToggle.textContent = 'Show Answer';
  }
  
  // Handle category badge efficiently
  if (qa.category) {
    categoryBadge.textContent = qa.category;
    categoryBadge.classList.remove('hidden');
  } else {
    categoryBadge.classList.add('hidden');
  }
}

// Consolidated navigation function
function navigate(direction) {
  if (direction === 'next') {
    idx = idx < order.length - 1 ? idx + 1 : 0;
  } else {
    idx = idx > 0 ? idx - 1 : order.length - 1;
  }
  
  render();
  currentQuestionIndex = idx;
  savePersistedData();
  
  // Reset game state for quiz navigation (doesn't reset lightboard)
  ws.send(JSON.stringify({action: 'reset', quizNav: true}));
  hideWinner();
  aEl.classList.remove('show');
  btnToggle.textContent = 'Show Answer';
}

function next() {
  navigate('next');
}

function prev() {
  navigate('prev');
}

function toggleAnswer() {
  aEl.classList.toggle('show');
  btnToggle.textContent = aEl.classList.contains('show') ? 'Hide Answer' : 'Show Answer';
}

// Initialize quiz mode status
function initQuizMode() {
  // Reset player tiles to default state
  player1Tile.classList.remove('winner');
  player2Tile.classList.remove('winner');
  updateScoreDisplay();
}

// Update score display
function updateScoreDisplay() {
  player1ScoreEl.textContent = player1Score;
  player2ScoreEl.textContent = player2Score;
}

// Add scorable state to player tiles
function addScorableState() {
  // Only add scorable class if the tile is not already a winner
  if (!player1Tile.classList.contains('winner')) {
    player1Tile.classList.add('scorable');
  }
  if (!player2Tile.classList.contains('winner')) {
    player2Tile.classList.add('scorable');
  }
  roundComplete = true;
}

// Remove scorable state from player tiles
function removeScorableState() {
  player1Tile.classList.remove('scorable');
  player2Tile.classList.remove('scorable');
  roundComplete = false;
}

// Award point to player
function awardPoint(player) {
  if (!roundComplete) return;
  
  // Update score based on damage multiplier
  if (player === 'Player 1') {
    player1Score += damageMultiplier;
    // Send message to ESP32 to award points to Player 1
    ws.send(JSON.stringify({action: 'awardPoint', player: 1, multiplier: damageMultiplier}));
  } else if (player === 'Player 2') {
    player2Score += damageMultiplier;
    // Send message to ESP32 to award points to Player 2
    ws.send(JSON.stringify({action: 'awardPoint', player: 2, multiplier: damageMultiplier}));
  }
  
  updateScoreDisplay();
  savePersistedData();
  
  // Reset game and advance to next question
  removeScorableState();
  hideWinner();
  aEl.classList.remove('show');
  btnToggle.textContent = 'Show Answer';
  next();
}

// Quiz event listeners
btnNext.addEventListener('click', next);
btnPrev.addEventListener('click', prev);
btnToggle.addEventListener('click', toggleAnswer);
btnExit.addEventListener('click', showExitConfirmation);

// Consolidated scoring event listener
function handlePlayerClick(player) {
  return function(e) {
    if (roundComplete && !isEditingName) {
      e.preventDefault();
      e.stopPropagation();
      awardPoint(player);
    }
  };
}

player1Tile.addEventListener('click', handlePlayerClick('Player 1'));
player2Tile.addEventListener('click', handlePlayerClick('Player 2'));

// Secret long-press functionality for mobile
let pressTimer;
let isLongPress = false;

card.addEventListener('touchstart', function(e) {
  isLongPress = false;
  pressTimer = setTimeout(() => {
    isLongPress = true;
    toggleAnswer();
  }, 800); // 800ms long press
});

card.addEventListener('touchend', function(e) {
  clearTimeout(pressTimer);
});

card.addEventListener('touchmove', function(e) {
  clearTimeout(pressTimer);
});

// Regular click for desktop
card.addEventListener('click', function(e) {
  if (!isLongPress) {
    toggleAnswer();
  }
});

// Keyboard shortcuts
window.addEventListener('keydown', (e) => {
  if (e.key === 'ArrowRight') { e.preventDefault(); next(); }
  else if (e.key === 'ArrowLeft') { e.preventDefault(); prev(); }
  else if (e.key === ' ' || e.code === 'Space') { e.preventDefault(); toggleAnswer(); }
});

// Game functions
function showWinner(player) {
  // Remove winner class from both tiles
  player1Tile.classList.remove('winner');
  player2Tile.classList.remove('winner');
  
  // Add winner class to the winning player's tile
  if (player === 'Player 1') {
    player1Tile.classList.add('winner');
  } else if (player === 'Player 2') {
    player2Tile.classList.add('winner');
  }
  
  // Enable scoring immediately
  addScorableState();
}

function hideWinner() {
  player1Tile.classList.remove('winner');
  player2Tile.classList.remove('winner');
  removeScorableState();
}

function resetGame() {
  ws.send(JSON.stringify({action:'reset'}));
  hideWinner();
}

function showExitConfirmation() {
  confirmModal.classList.remove('hidden');
}

function hideExitConfirmation() {
  confirmModal.classList.add('hidden');
}

function exitToCategories() {
  // Reset game state
  hideWinner();
  aEl.classList.remove('show');
  btnToggle.textContent = 'Show Answer';
  
  // Clear current quiz state
  currentCategory = null;
  currentQuestionIndex = 0;
  savedOrder = null;
  localStorage.removeItem('currentCategory');
  localStorage.removeItem('currentQuestionIndex');
  localStorage.removeItem('savedOrder');
  
  // Reset scores
  player1Score = 0;
  player2Score = 0;
  roundComplete = false;
  localStorage.removeItem('player1Score');
  localStorage.removeItem('player2Score');
  updateScoreDisplay();
  removeScorableState();
  
  // Hide modal
  hideExitConfirmation();
  
  // Reset lightboard when exiting quiz
  ws.send(JSON.stringify({action: 'reset'}));
  
  // Return to category selector
  showCategorySelector();
}

// WebSocket event handling
ws.onmessage=e=>{
  const d=JSON.parse(e.data);
  if(d.connected!==undefined){
    if(d.connected){
      connDot.className='ok';
    }else{
      connDot.className='bad';
      hideWinner();
    }
  }
  if(d.lightboardConnected!==undefined){
    // Update lightboard connection status in UI if needed
    console.log('Lightboard connected:', d.lightboardConnected);
  }
     if(d.winner!==undefined){
     if(d.winner&&d.winner!=='none'){
       showWinner(d.winner);
       // Automatically reveal answer when someone wins
       aEl.classList.add('show');
       btnToggle.textContent = 'Hide Answer';
     }else{
       hideWinner();
       // Hide answer when game resets
       aEl.classList.remove('show');
       btnToggle.textContent = 'Show Answer';
     }
   }
  // Handle toolboard controls for quiz
  if(d.quizAction){
    switch(d.quizAction) {
      case 'next': next(); break;
      case 'prev': prev(); break;
      case 'toggle': toggleAnswer(); break;
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

   // === RESET FUNCTIONALITY ===
  resetAllData.addEventListener('click', showResetConfirmation);
  
  function showResetConfirmation() {
    resetModal.classList.remove('hidden');
  }
  
  function hideResetConfirmation() {
    resetModal.classList.add('hidden');
  }

  // Lightboard modal functions
  function showLightboardSettings() {
    // Load current settings from localStorage first
    loadLightboardSettings();
    
    // Set current values in modal
    document.getElementById('lightboardMode').value = lightboardGameMode;
    document.getElementById('lightboardP1Color').value = lightboardP1ColorIndex;
    document.getElementById('lightboardP2Color').value = lightboardP2ColorIndex;
    document.getElementById('damageMultiplier').value = damageMultiplier;
    lightboardModal.classList.remove('hidden');
  }

  function hideLightboardSettings() {
    lightboardModal.classList.add('hidden');
  }

  function applyLightboardSettings() {
    const newMode = parseInt(document.getElementById('lightboardMode').value);
    const newP1Color = parseInt(document.getElementById('lightboardP1Color').value);
    const newP2Color = parseInt(document.getElementById('lightboardP2Color').value);
    const newMultiplier = parseInt(document.getElementById('damageMultiplier').value);
    
    // Update local variables
    lightboardGameMode = newMode;
    lightboardP1ColorIndex = newP1Color;
    lightboardP2ColorIndex = newP2Color;
    damageMultiplier = newMultiplier;
    
    // Save settings to localStorage
    saveLightboardSettings();
    
    // Send settings to server
    ws.send(JSON.stringify({
      action: 'lightboardSettings',
      mode: newMode,
      p1Color: newP1Color,
      p2Color: newP2Color
    }));
    
    hideLightboardSettings();
  }

  function saveLightboardSettings() {
    localStorage.setItem('lightboardGameMode', lightboardGameMode.toString());
    localStorage.setItem('lightboardP1ColorIndex', lightboardP1ColorIndex.toString());
    localStorage.setItem('lightboardP2ColorIndex', lightboardP2ColorIndex.toString());
    localStorage.setItem('damageMultiplier', damageMultiplier.toString());
  }

  function loadLightboardSettings() {
    const savedMode = localStorage.getItem('lightboardGameMode');
    const savedP1Color = localStorage.getItem('lightboardP1ColorIndex');
    const savedP2Color = localStorage.getItem('lightboardP2ColorIndex');
    const savedMultiplier = localStorage.getItem('damageMultiplier');
    
    if (savedMode !== null) {
      lightboardGameMode = parseInt(savedMode);
    }
    if (savedP1Color !== null) {
      lightboardP1ColorIndex = parseInt(savedP1Color);
    }
    if (savedP2Color !== null) {
      lightboardP2ColorIndex = parseInt(savedP2Color);
    }
    if (savedMultiplier !== null) {
      damageMultiplier = parseInt(savedMultiplier);
    }
  }
  
  function resetAllDataFunction() {
  // Clear all localStorage data
  localStorage.clear();
  
  // Reset all variables to default state
  availableCategories = [];
  player1Score = 0;
  player2Score = 0;
  player1NameText = 'Player 1';
  player2NameText = 'Player 2';
  currentCategory = null;
  currentQuestionIndex = 0;
  savedOrder = null;
  QA = [];
  order = [];
  idx = 0;
  roundComplete = false;
  
  // Reset lightboard settings to defaults
  lightboardGameMode = 1;
  lightboardP1ColorIndex = 0;
  lightboardP2ColorIndex = 1;
  damageMultiplier = 3;
  
  // Update UI
  player1Name.textContent = player1NameText;
  player2Name.textContent = player2NameText;
  updateScoreDisplay();
  removeScorableState();
  
  // Clear file list
  fileList.innerHTML = '';
  loadedFiles.classList.add('hidden');
  
  // Reset lightboard when resetting all data
  ws.send(JSON.stringify({action: 'reset'}));
  
  // Show sample questions
  availableCategories = [{
    filename: 'sample.csv',
    name: 'Sample Questions',
    questions: sampleQuestions
  }];
  showCategorySelector();
  createCategoryButtons(availableCategories);
  
  // Hide modal
  hideResetConfirmation();
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

  // Helper function to finish loading
  const finishLoading = () => {
    if (availableCategories.length > 0) {
      showCategorySelector();
      createCategoryButtons(availableCategories);
      savePersistedData();
    }
  };

  // Helper function to process file
  const processFile = (file) => {
    if (file.type === 'text/csv' || file.name.endsWith('.csv')) {
      const reader = new FileReader();
      
      reader.onload = function(e) {
        try {
          const csvText = e.target.result;
          const csvData = parseCSV(csvText);
          
          // Convert CSV data to the expected format
          const categoryName = file.name.replace('.csv', '').replace(/_/g, ' ').replace(/\b\w/g, l => l.toUpperCase());
          const questions = csvData.map(row => {
            const question = row.Question || row.question || row.Q || row.q || Object.values(row)[0];
            const answer = row.Answer || row.answer || row.A || row.a || Object.values(row)[1];
            // Use the filename as the category name for all questions in this file
            const category = row.Category || row.category || row.Cat || row.cat || categoryName;
            return { q: question, a: answer, category: category };
          }).filter(qa => qa.q && qa.a);
          
          if (questions.length > 0) {
            const categoryName = file.name.replace('.csv', '').replace(/_/g, ' ').replace(/\b\w/g, l => l.toUpperCase());
            availableCategories.push({
              filename: file.name,
              name: categoryName,
              questions: questions
            });
            
            addFileToList(file.name, `${questions.length} questions`, 'success');
          } else {
            addFileToList(file.name, 'No questions', 'error');
          }
        } catch (error) {
          console.error('Error parsing CSV:', error);
          addFileToList(file.name, 'Error', 'error');
        }
        
        loadedCount++;
        if (loadedCount === totalFiles) finishLoading();
      };
      
      reader.onerror = function() {
        addFileToList(file.name, 'Read failed', 'error');
        loadedCount++;
        if (loadedCount === totalFiles) finishLoading();
      };
      
      reader.readAsText(file);
    } else {
      addFileToList(file.name, 'Not CSV', 'error');
      loadedCount++;
      if (loadedCount === totalFiles) finishLoading();
    }
  };
  
  Array.from(files).forEach(processFile);
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
  resetAllData.parentElement.classList.remove('hidden');
  lightboardModeSection.classList.remove('hidden');
}

 function showQuizDisplay() {
   categorySelector.classList.add('hidden');
   quizDisplay.classList.remove('hidden');
   fileInputSection.classList.add('hidden');
   resetAllData.parentElement.classList.add('hidden');
   lightboardModeSection.classList.add('hidden');
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

// Consolidated category loading function
function loadCategoryData(categoryData, isCombined = false) {
  if (isCombined) {
    // Combine all questions from all categories
    QA = [];
    const categoryNames = [];
    
    categoryData.forEach(category => {
      const questionsWithCategory = category.questions.map(qa => ({
        ...qa,
        category: category.name
      }));
      QA = QA.concat(questionsWithCategory);
      categoryNames.push(category.name);
    });
    
    quizTitle.textContent = `Mixed: ${categoryNames.join(', ')}`;
    currentCategory = 'combined';
  } else {
    QA = categoryData.questions;
    quizTitle.textContent = categoryData.name;
    currentCategory = categoryData.filename;
  }
  
  currentQuestionIndex = 0;
  showQuizDisplay();
  setOrder(true);
  render(true);
  savePersistedData();
}

function loadCategory(filename) {
  const category = availableCategories.find(cat => cat.filename === filename);
  if (!category) {
    alert('Category not found');
    return;
  }
  loadCategoryData(category);
}

function loadCombinedCategories(categories) {
  loadCategoryData(categories, true);
}

// Player name editing functionality
let nameEditTimer;
let isEditingName = false;

function setupPlayerNameEditing() {
  // Use double-tap for mobile editing (more reliable than long press)
  let lastTap = 0;
  let tapTimer;
  
  // Player 1 name editing
  player1Name.addEventListener('touchend', function(e) {
    if (roundComplete) return; // Don't allow editing during scoring phase
    
    const currentTime = new Date().getTime();
    const tapLength = currentTime - lastTap;
    
    if (tapLength < 500 && tapLength > 0) {
      // Double tap detected
      e.preventDefault();
      startEditingName(player1Name, 'Player 1');
    } else {
      // Single tap - wait for potential double tap
      tapTimer = setTimeout(() => {
        // Single tap confirmed
      }, 500);
    }
    lastTap = currentTime;
  });

  // Player 2 name editing
  let lastTap2 = 0;
  let tapTimer2;
  
  player2Name.addEventListener('touchend', function(e) {
    if (roundComplete) return; // Don't allow editing during scoring phase
    
    const currentTime = new Date().getTime();
    const tapLength = currentTime - lastTap2;
    
    if (tapLength < 500 && tapLength > 0) {
      // Double tap detected
      e.preventDefault();
      startEditingName(player2Name, 'Player 2');
    } else {
      // Single tap - wait for potential double tap
      tapTimer2 = setTimeout(() => {
        // Single tap confirmed
      }, 500);
    }
    lastTap2 = currentTime;
  });

  // Desktop double-click for editing
  player1Name.addEventListener('dblclick', function(e) {
    if (roundComplete) return; // Don't allow editing during scoring phase
    e.preventDefault();
    startEditingName(player1Name, 'Player 1');
  });

  player2Name.addEventListener('dblclick', function(e) {
    if (roundComplete) return; // Don't allow editing during scoring phase
    e.preventDefault();
    startEditingName(player2Name, 'Player 2');
  });
}

function startEditingName(nameElement, defaultName) {
  if (isEditingName) return;
  
  isEditingName = true;
  nameElement.classList.add('editing');
  
  const currentName = nameElement.textContent;
  const input = document.createElement('input');
  input.type = 'text';
  input.value = currentName;
     input.style.cssText = `
     background: transparent;
     border: none;
     color: var(--ink);
     font-weight: 600;
     font-size: 16px;
     text-align: center;
     width: 100%;
     outline: none;
     font-family: inherit;
     -webkit-user-select: text;
     user-select: text;
     margin: 0;
     padding: 0;
     box-sizing: border-box;
   `;
  
  // Clear any existing content and add the input
  nameElement.innerHTML = '';
  nameElement.appendChild(input);
  
  // Force focus and selection on mobile
  setTimeout(() => {
    input.focus();
    input.select();
    // Force keyboard to appear on mobile
    input.click();
  }, 100);
  
     function finishEditing() {
     const newName = input.value.trim() || defaultName;
     nameElement.textContent = newName;
     nameElement.classList.remove('editing');
     isEditingName = false;
     
     // Update stored names and save
     if (nameElement === player1Name) {
       player1NameText = newName;
     } else if (nameElement === player2Name) {
       player2NameText = newName;
     }
     savePersistedData();
   }
  
  input.addEventListener('blur', finishEditing);
     input.addEventListener('keydown', function(e) {
     if (e.key === 'Enter') {
       finishEditing();
     } else if (e.key === 'Escape') {
       // Restore original name without saving
       nameElement.textContent = currentName;
       nameElement.classList.remove('editing');
       isEditingName = false;
     }
   });
  
  // Handle mobile keyboard "Done" button
  input.addEventListener('input', function(e) {
    // This ensures the input is properly handled on mobile
  });
}

 // Modal event listeners
 cancelExit.addEventListener('click', hideExitConfirmation);
 confirmExit.addEventListener('click', exitToCategories);
 cancelReset.addEventListener('click', hideResetConfirmation);
 confirmReset.addEventListener('click', resetAllDataFunction);
 lightboardSettingsBtn.addEventListener('click', showLightboardSettings);
 cancelLightboard.addEventListener('click', hideLightboardSettings);
 confirmLightboard.addEventListener('click', applyLightboardSettings);
 
 // Close modals when clicking overlay
 confirmModal.addEventListener('click', function(e) {
   if (e.target === confirmModal) {
     hideExitConfirmation();
   }
 });
 
 resetModal.addEventListener('click', function(e) {
   if (e.target === resetModal) {
     hideResetConfirmation();
   }
 });

 lightboardModal.addEventListener('click', function(e) {
   if (e.target === lightboardModal) {
     hideLightboardSettings();
   }
 });
 
 // Close modals with Escape key
 document.addEventListener('keydown', function(e) {
   if (e.key === 'Escape') {
     if (!confirmModal.classList.contains('hidden')) {
       hideExitConfirmation();
     } else if (!resetModal.classList.contains('hidden')) {
       hideResetConfirmation();
     } else if (!lightboardModal.classList.contains('hidden')) {
       hideLightboardSettings();
     }
   }
 });

// Restore quiz state to where you left off
function restoreQuizState() {
  const restoreOrder = () => {
    if (savedOrder && savedOrder.length === QA.length) {
      order = [...savedOrder];
      idx = currentQuestionIndex;
    } else {
      setOrder(true);
      idx = currentQuestionIndex;
    }
    render(true);
  };

  if (currentCategory === 'combined') {
    // Restore combined categories
    QA = [];
    const categoryNames = [];
    
    availableCategories.forEach(category => {
      const questionsWithCategory = category.questions.map(qa => ({
        ...qa,
        category: category.name
      }));
      QA = QA.concat(questionsWithCategory);
      categoryNames.push(category.name);
    });

    quizTitle.textContent = `Mixed: ${categoryNames.join(', ')}`;
    showQuizDisplay();
    restoreOrder();
  } else {
    // Restore specific category
    const category = availableCategories.find(cat => cat.filename === currentCategory);
    if (category) {
      QA = category.questions;
      quizTitle.textContent = category.name;
      showQuizDisplay();
      restoreOrder();
    }
  }
}

// Lightboard mode change handler (removed - now handled by modal)

// Initialize quiz on load
document.addEventListener('DOMContentLoaded', function() {
  initQuiz();
  initQuizMode();
  setupPlayerNameEditing();
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
void OnDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  Serial.print("Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  // Handle different message types based on length
  if (len == sizeof(struct_message)) {
    // Player 2 message
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
      Serial.println("Lightboard message received - connection confirmed");

      if (lightboardData.action == 1) {
        // Heartbeat - just update connection status
        Serial.println("Lightboard heartbeat received");
        
        // If this is a reconnection or first connection, resync the lightboard settings
        if (wasDisconnected || !lightboardWasConnected) {
          Serial.println("Lightboard connected - resyncing settings");
          // Send current game mode and player colors to resync
          sendLightboardUpdate(4); // Send settings update
        }
      }
    }
  }
}

// ===================== Lightboard Communication =====================
void sendLightboardUpdate(uint8_t action) {
  // Always send heartbeats (action 1) regardless of connection status
  // Other actions require connection
  if (!lightboardConnected && action != 1) return;
  
  lightboardData.deviceId = 1; // Player 1
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
  Serial.printf("Sent lightboard update: action=%d, mode=%d\n", action, lightboardGameMode);
}

void updateLightboardGameState() {
  // Send game state update to lightboard (mode, colors only)
  // Individual points are handled by sendLightboardPointUpdate()
  sendLightboardUpdate(2); // game-state action
}

void sendLightboardPointUpdate(uint8_t scoringPlayer) {
  // Send point update to lightboard with which player scored
  if (!lightboardConnected) return;
  
  lightboardData.deviceId = 1; // Player 1
  lightboardData.action = 3; // point update
  lightboardData.winner = scoringPlayer; // Which player scored the point
  
  esp_now_send(lightboardAddress, (uint8_t*)&lightboardData, sizeof(lightboardData));
  Serial.printf("Sent lightboard point update: Player %d scored\n", scoringPlayer);
}

void awardPointToPlayer(uint8_t playerId) {
  // Award a point to the specified player on the lightboard
  // playerId: 1 = Player 1, 2 = Player 2
  
  if (playerId != 1 && playerId != 2) {
    Serial.printf("Invalid player ID: %d. Must be 1 or 2.\n", playerId);
    return;
  }
  
  if (!lightboardConnected) {
    Serial.println("Lightboard not connected - cannot award point");
    return;
  }
  
  // Send point update to lightboard
  sendLightboardPointUpdate(playerId);
  
  // Update local game state
  if (playerId == 1) {
    winner = "Player 1";
    player1HitTime = micros(); // Use current time as hit time
  } else {
    winner = "Player 2";
    player2HitTime = micros(); // Use current time as hit time
  }
  
  // Broadcast winner to web interface
  String j = "{\"winner\":\"" + winner + "\"}";
  ws.broadcastTXT(j);
  
  // Update lightboard game state
  updateLightboardGameState();
  
  Serial.printf("Awarded point to Player %d\n", playerId);
}

void awardMultiplePointsToPlayer(uint8_t playerId, int multiplier) {
  // Award multiple points to the specified player on the lightboard
  // playerId: 1 = Player 1, 2 = Player 2
  // multiplier: number of points to award
  
  if (playerId != 1 && playerId != 2) {
    Serial.printf("Invalid player ID: %d. Must be 1 or 2.\n", playerId);
    return;
  }
  
  if (multiplier < 1 || multiplier > 5) {
    Serial.printf("Invalid multiplier: %d. Must be 1-5.\n", multiplier);
    return;
  }
  
  if (!lightboardConnected) {
    Serial.println("Lightboard not connected - cannot award points");
    return;
  }
  
  // Send multiple point updates to lightboard
  for (int i = 0; i < multiplier; i++) {
    sendLightboardPointUpdate(playerId);
    delay(100); // Small delay between points for visual effect
  }
  
  // Update local game state (only once at the end)
  if (playerId == 1) {
    winner = "Player 1";
    player1HitTime = micros(); // Use current time as hit time
  } else {
    winner = "Player 2";
    player2HitTime = micros(); // Use current time as hit time
  }
  
  // Broadcast winner to web interface
  String j = "{\"winner\":\"" + winner + "\"}";
  ws.broadcastTXT(j);
  
  // Update lightboard game state
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
    
    // Broadcast winner to web interface
    String j = "{\"winner\":\"" + winner + "\"}";
    ws.broadcastTXT(j);
    
    // Update lightboard
    updateLightboardGameState();
  }
}

void resetGame() {
  winner = "none";
  player1HitTime = 0;
  player2HitTime = 0;
  gameActive = true;
  
  // Reset lightboard state
  lightboardWinner = 0;
  lightboardCelebrating = false;
  lightboardP1Pos = -1;
  lightboardP2Pos = 38;
  lightboardNextLedPos = 0;
  lightboardTugBoundary = 18;
  lightboardP1RacePos = -1;
  lightboardP2RacePos = -1;
  
  // Send reset to Player 2
  myData.action = 3; // reset request
  esp_now_send(player2Address, (uint8_t*)&myData, sizeof(myData));
  
  // Send reset to lightboard
  sendLightboardUpdate(5); // reset action
  
  // Broadcast reset to web interface
  String j = "{\"winner\":\"none\"}";
  ws.broadcastTXT(j);
  
  Serial.println("Game reset");
}

void resetGameForQuiz() {
  // Light version of reset for quiz navigation - doesn't reset lightboard state
  winner = "none";
  player1HitTime = 0;
  player2HitTime = 0;
  gameActive = true;
  
  // Broadcast reset to web interface
  String j = "{\"winner\":\"none\"}";
  ws.broadcastTXT(j);
  
  Serial.println("Game reset for quiz navigation");
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
      j += "\"lightboardConnected\":" + String(lightboardConnected ? "true" : "false") + ",";
      j += "\"winner\":\"" + winner + "\"";
      j += "}";
      ws.sendTXT(num, j);
      break;
    case WStype_TEXT:
      if (length > 0) {
        String message = String((char*)payload);
        if (message.indexOf("reset") != -1) {
          // Check if this is a quiz navigation reset or a full game reset
          if (message.indexOf("quizNav") != -1) {
            resetGameForQuiz();
          } else {
            resetGame();
          }
        } else if (message.indexOf("awardPoint") != -1) {
          // Handle point award from web interface
          DynamicJsonDocument doc(1024);
          deserializeJson(doc, message);
          if (doc.containsKey("player")) {
            int player = doc["player"];
            int multiplier = 1;
            if (doc.containsKey("multiplier")) {
              multiplier = doc["multiplier"];
            }
            
            if (player == 1 || player == 2) {
              // Award multiple points based on multiplier
              awardMultiplePointsToPlayer(player, multiplier);
            }
          }
        } else if (message.indexOf("lightboardMode") != -1) {
          // Handle lightboard mode change from web interface
          DynamicJsonDocument doc(1024);
          deserializeJson(doc, message);
          if (doc.containsKey("mode")) {
            int newMode = doc["mode"];
            if (newMode >= 1 && newMode <= 6) {
              lightboardGameMode = newMode;
              Serial.printf("Lightboard mode changed to: %d\n", newMode);
              sendLightboardUpdate(4); // mode-change action
            }
          }
        } else if (message.indexOf("lightboardSettings") != -1) {
          // Handle lightboard settings change from web interface
          DynamicJsonDocument doc(1024);
          deserializeJson(doc, message);
          if (doc.containsKey("mode") && doc.containsKey("p1Color") && doc.containsKey("p2Color")) {
            int newMode = doc["mode"];
            int newP1Color = doc["p1Color"];
            int newP2Color = doc["p2Color"];
            
            if (newMode >= 1 && newMode <= 6 && newP1Color >= 0 && newP1Color <= 4 && newP2Color >= 0 && newP2Color <= 4) {
              lightboardGameMode = newMode;
              lightboardP1ColorIndex = newP1Color;
              lightboardP2ColorIndex = newP2Color;
              Serial.printf("Lightboard settings updated: mode=%d, p1Color=%d, p2Color=%d\n", newMode, newP1Color, newP2Color);
              sendLightboardUpdate(4); // mode-change action
            }
          }
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
    // Handle game mode FIRST if active (priority over quiz controls)
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
      
    } else {
      // Do not map toolboard hits to quiz actions.
      // Questions should advance only via UI button or when a point is awarded.
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
               ",\"lightboardConnected\":" + String(lightboardConnected ? "true" : "false") + 
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
  
  // Check for lightboard connection timeout
  if (lightboardConnected && (millis() - lastLightboardHeartbeat > heartbeatTimeout)) {
    lightboardWasConnected = lightboardConnected; // Store previous state
    lightboardConnected = false;
    lightboardMacLearned = false; // Reset MAC learning to force rediscovery
    Serial.println("Lightboard connection lost - resetting discovery");
  }

  // Send heartbeat to Player 2
  static unsigned long lastHeartbeatSend = 0;
  if (millis() - lastHeartbeatSend >= 1000) {
    myData.action = 1; // heartbeat
    esp_now_send(player2Address, (uint8_t*)&myData, sizeof(myData));
    Serial.println("Sent heartbeat to Player 2");
    lastHeartbeatSend = millis();
  }
  
  // Send heartbeat to lightboard (always try, even if MAC not learned yet)
  static unsigned long lastLightboardHeartbeatSend = 0;
  if (millis() - lastLightboardHeartbeatSend >= 1000) {
    sendLightboardUpdate(1); // heartbeat action
    if (lightboardMacLearned) {
      Serial.println("Sent heartbeat to lightboard (MAC learned)");
    } else {
      Serial.println("Sent heartbeat to lightboard (using hardcoded MAC)");
    }
    lastLightboardHeartbeatSend = millis();
  }
}
