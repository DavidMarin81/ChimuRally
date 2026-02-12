// Y espabila
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WebSocketsServer.h>

// ---------------- WIFI ----------------
const char* ssid = "RALLY_PRO_V26";
const char* password = "password123";

ESP8266WebServer server(80);
WebSocketsServer webSocket(81);

// ---------------- SENSOR ----------------
const int sensorPin = 4;  // D2 (GPIO4)
volatile int pulsesQueued = 0;
volatile unsigned long lastDebounceTime = 0;
const int debounceDelay = 15;

// ---------------- BACKEND RALLY ----------------
long distReal_mm = 0;
long partialOffset_mm = 0;  // offset para parcial
float distIdeal = 0.0f;
float factorW = 1.0000f;        // calibración
float basePulseKm = 0.001050f;  // tu factor base por pulso (km)

float wheelPerimeter = 2.0f;  // metros
int magnetCount = 1;

float lastOfficialMeters = 0.0f;
float lastMeasuredMeters = 0.0f;
float lastCorrectionPercent = 0.0f;

bool raceRunning = false;
unsigned long lastLoopMs = 0;
unsigned long totalRaceMs = 0;

// ---------------- RUTAS/TRAMOS EN BACKEND (Opción B) ----------------
static const int MAX_STAGES = 20;
static const int MAX_SEGS = 200;

struct Segment {
  float km;     // km objetivo (sobre ideal)
  float speed;  // km/h
};

struct Stage {
  uint32_t id;
  String name;
  Segment segs[MAX_SEGS];
  int segCount;
};

Stage stages[MAX_STAGES];
int stageCount = 0;
uint32_t currentStageId = 0;

// Marca para reenviar estructura completa a todos cuando cambie algo
volatile bool stagesDirty = true;

// ---------------- Utils ----------------
static String jsonEscape(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\\') out += "\\\\";
    else if (c == '"') out += "\\\"";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out;
}

static Stage* findStageById(uint32_t id) {
  for (int i = 0; i < stageCount; i++) {
    if (stages[i].id == id) return &stages[i];
  }
  return nullptr;
}

static void ensureDefaultStage() {
  if (stageCount > 0) return;
  stages[0].id = (uint32_t)millis() + 1000;
  stages[0].name = "TRAMO 1";
  stages[0].segCount = 0;
  stageCount = 1;
  currentStageId = stages[0].id;
  stagesDirty = true;
}

static void sortSegments(Stage* st) {
  // Inserción simple (MAX_SEGS pequeño)
  for (int i = 1; i < st->segCount; i++) {
    Segment key = st->segs[i];
    int j = i - 1;
    while (j >= 0 && st->segs[j].km > key.km) {
      st->segs[j + 1] = st->segs[j];
      j--;
    }
    st->segs[j + 1] = key;
  }
}

static float backendCurrentSpeedKmh() {
  Stage* st = findStageById(currentStageId);
  if (!st || st->segCount == 0) return 0.0f;

  // Igual que tu JS: usa distIdeal para decidir el segmento vigente
  float spd = st->segs[0].speed;
  for (int i = 0; i < st->segCount; i++) {
    if (distIdeal >= st->segs[i].km) spd = st->segs[i].speed;
    else break;
  }
  return spd;
}

double distPartial() {
  return (distReal_mm - partialOffset_mm) / 1000000.0;
}


// ---------------- INTERRUPCIÓN SENSOR ----------------
void IRAM_ATTR sensorISR() {
  unsigned long currentTime = millis();
  // debounce muy simple
  if ((currentTime - lastDebounceTime) > (unsigned long)debounceDelay) {
    pulsesQueued++;
    lastDebounceTime = currentTime;
  }
}

// ---------------- JSON builders ----------------
static String buildStagesJson() {
  // {"t":"stages","current":123,"stages":[{"id":...,"name":"...","segments":[{"km":...,"speed":...},...]},...]}
  String j;
  j.reserve(2048);
  j += "{\"t\":\"stages\",\"current\":";
  j += String(currentStageId);
  j += ",\"stages\":[";
  for (int i = 0; i < stageCount; i++) {
    if (i) j += ",";
    j += "{\"id\":";
    j += String(stages[i].id);
    j += ",\"name\":\"";
    j += jsonEscape(stages[i].name);
    j += "\",\"segments\":[";
    for (int k = 0; k < stages[i].segCount; k++) {
      if (k) j += ",";
      j += "{\"km\":";
      j += String(stages[i].segs[k].km, 3);
      j += ",\"speed\":";
      j += String(stages[i].segs[k].speed, 1);
      j += "}";
    }
    j += "]}";
  }
  j += "]}";
  return j;
}

static String buildTeleJson() {
  float spd = backendCurrentSpeedKmh();
  double distReal_km = distReal_mm / 1000000.0;
  double distPartial_km = (distReal_mm - partialOffset_mm) / 1000000.0;
  double errM = (distReal_km - distIdeal) * 1000.0;

  String j;
  j.reserve(256);
  j += "{\"t\":\"tele\",";
  j += "\"real\":";
  j += String(distReal_km, 6);
  j += ",\"ideal\":";
  j += String(distIdeal, 3);
  j += ",\"partial\":";
  j += String(distPartial_km, 6);
  j += ",\"error\":";
  j += String((int)errM);
  j += ",\"time\":";
  j += String(totalRaceMs / 1000);
  j += ",\"speed\":";
  j += String(spd, 1);
  j += ",\"running\":";
  j += (raceRunning ? "true" : "false");
  j += ",\"stage\":";
  j += String(currentStageId);
  j += ",\"wheelPerimeter\":";
  j += String(wheelPerimeter, 3);

  j += ",\"magnetCount\":";
  j += String(magnetCount);

  j += ",\"basePulseKm\":";
  j += String(basePulseKm, 8);

  j += ",\"factorW\":";
  j += String(factorW, 6);

  j += ",\"finalPulseKm\":";
  j += String(basePulseKm * factorW, 8);

  j += ",\"lastOfficial\":";
  j += String(lastOfficialMeters, 2);

  j += ",\"lastMeasured\":";
  j += String(lastMeasuredMeters, 2);

  j += ",\"lastCorrection\":";
  j += String(lastCorrectionPercent, 2);

  j += "}";
  return j;
}

void broadcastTele() {
  String payload = buildTeleJson();
  webSocket.broadcastTXT(payload);
}

void broadcastStages() {
  String payload = buildStagesJson();
  webSocket.broadcastTXT(payload);
  stagesDirty = false;
}

// ---------------- FRONTEND COMPLETO (tu UI) ----------------
const char PAGE_MAIN[] PROGMEM = R"=====(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, viewport-fit=cover">
    <meta name="apple-mobile-web-app-capable" content="yes">
    <meta name="theme-color" content="#000000">

    <title>Rally Pro Sim - V26 Backend Total</title>
    <style>
        html, body {
            height: 100%;
        }

        body { font-family: 'Courier New', monospace; background: #000; color: #fff; text-align: center; margin: 0; touch-action: manipulation; overflow-x: hidden; padding-bottom: 80px; }
        .header-clock { background: #1a1a1a; padding: 5px; border-bottom: 2px solid #ff9900; display: flex; justify-content: space-around; align-items: center; }
        .clock-val { font-size: 1.5rem; color: #ff9900; font-weight: bold; }
        .clock-adj { background: #333; color: #fff; padding: 5px 10px; font-size: 0.8rem; border-radius: 4px; border: 1px solid #444; }
        .tabs { display: flex; width: 100%; border-bottom: 2px solid #333; background: #111; position: sticky; top: 0; z-index: 100; }
        .tab-btn { flex: 1; padding: 12px 2px; background: #111; color: #888; border: none; font-size: 0.7rem; font-weight: bold; cursor: pointer; }
        .tab-btn.active { background: #003300; color: #0f0; border-bottom: 4px solid #0f0; }
        .view { display: none; padding: 10px; }
        .view.active { display: block; }
        .error-bar-system { height: 60px; background: #000; margin: 5px; position: relative; border: 3px solid #555; overflow: hidden; border-radius: 8px; }
        .cursor-line { width: 20px; height: 100%; background: #fff; position: absolute; left: 50%; transform: translateX(-50%); transition: left 0.1s linear; box-shadow: 0 0 15px white; z-index: 2; }
        .center-line { position: absolute; left: 50%; width: 2px; height: 100%; background: #0f0; z-index: 1; }
        .bg-ok { background-color: #002200 !important; border-color: #0f0 !important; }
        .bg-early { background-color: #000044 !important; border-color: #3388ff !important; }
        .bg-late { background-color: #440000 !important; border-color: #f00 !important; }
        .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 6px; padding: 5px; }
        .panel { border: 1px solid #444; background: #111; border-radius: 4px; display: flex; flex-direction: column; justify-content: center; min-height: 90px; }
        .panel-split { padding: 0 !important; overflow: hidden; }
        .split-top { border-bottom: 1px solid #333; padding: 5px 0; flex: 1; }
        .split-bottom { background: #080808; padding: 8px 0; flex: 1.5; }
        .val { font-size: 10vw; font-weight: bold; font-variant-numeric: tabular-nums; line-height: 1; }
        .val-crono { font-size: 13vw; color: #fff; letter-spacing: -1px; }
        .label { color: #ff9900; font-size: 0.6rem; text-transform: uppercase; letter-spacing: 1px; display: block; margin-bottom: 2px;}
        input[type="number"], input[type="text"], input[type="time"], select { background: #000; color: #0f0; border: 1px solid #555; padding: 8px; font-size: 1rem; text-align: center; }
        button { cursor: pointer; border: none; font-weight: bold; border-radius: 5px; text-transform: uppercase; }
        .btn-main { padding: 12px; width: 30%; font-size: 0.9rem; }
        .btn-start { background: #2e7d32; color: #fff; }
        .btn-stop { background: #c62828; color: #fff; display: none; }
        .control-panel { background: #111; border: 1px solid #444; margin: 5px; padding: 10px; border-radius: 8px; }
        .control-row { display: flex; align-items: center; justify-content: space-between; margin-bottom: 8px; gap: 10px; }
        .control-item { flex: 1; display: flex; flex-direction: column; align-items: center; }
        .btn-wakelock { background: #222; color: #666; font-size: 0.6rem; padding: 6px; margin-top: 5px; width: 100%; border: 1px solid #444; border-radius: 4px; transition: 0.3s; }
        .btn-wakelock.active { background: #004400; color: #0f0; border-color: #0f0; font-weight: bold; box-shadow: 0 0 5px #00ff00; }
        #pilot-ui {
            display: none;
            position: fixed;
            inset: 0;
            background: #000;
            z-index: 2000;
            flex-direction: column;
        }

        #pilot-dist {
            flex: 3;
            display: flex;
            align-items: center;
            justify-content: center;
            font-size: clamp(6rem, 20vw, 12rem);
        }

        #pilot-error-container {
            display: flex;
            flex: 2;
            height:80px;

        }

        #pilot-err {
            flex: 3;
            display: flex;
            align-items: center;
            justify-content: center;
            font-size: clamp(2rem, 15vw, 10rem);
        }

        #pilot-ui button {
            flex: 1;
        }


        #throttle-container { display: none; }
        table td { border-bottom: 1px solid #222; padding: 6px; }

        @media screen and (orientation: landscape) {
            #pilot-ui {
                width: 100%;
            }

            #pilot-dist {
                font-size: 8vh;
            }

            #pilot-err {
                font-size: 6vh;
            }

            #pilot-error-container {
                display: flex;
                height: 12vh;
                width: 98%;
            }

        }

    </style>
</head>
<body>
    <div class="header-clock">
        <button class="clock-adj" onclick="adjustClock(-1)">-1s</button>
        <div>
            <span class="label" style="margin:0">RELOJ CARRERA</span>
            <div id="official-clock" class="clock-val">00:00:00</div>
        </div>
        <button class="clock-adj" onclick="adjustClock(1)">+1s</button>
    </div>

    <div class="tabs">
        <button class="tab-btn active" onclick="switchTab('race')">🏁 CARRERA</button>
        <button class="tab-btn" onclick="switchTab('setup')">📝 RUTAS</button>
        <button class="tab-btn" onclick="switchTab('calib')">⚙️ CALIB</button>
        <button class="tab-btn" onclick="switchTab('pilot')">📡 PILOTO</button>
    </div>

    <div id="view-race" class="view active">
        <div id="race-error-container" class="error-bar-system">
            <div class="center-line"></div>
            <div id="race-cursor" class="cursor-line"></div>
        </div>

        <div class="grid">
            <div class="panel">
                <span class="label">TRIP TOTAL</span>
                <div id="real" class="val" style="color:#00ffff">0.000</div>
                <div style="display:flex; justify-content:center; gap:5px; margin-top:5px;">
                    <button onclick="adjustDist(-0.010)" style="background:#333; padding:8px;">-10m</button>
                    <button onclick="adjustDist(0.010)" style="background:#333; padding:8px;">+10m</button>
                </div>
            </div>
            <div class="panel">
                <span class="label">IDEAL</span>
                <div id="ideal" class="val" style="color:#ffff00">0.000</div>
                <div id="current-media-display" style="color:#ff9900; font-size:0.8rem; font-weight:bold;">0.0 km/h</div>
            </div>
            <div class="panel">
                <span class="label">PARCIAL</span>
                <div id="partial" class="val" style="color:#fff">0.000</div>
                <button onclick="resetPartial()" style="background:#444; color:#aaa; font-size:0.6rem; padding:4px; margin-top:4px;">RESET</button>
            </div>
            <div class="panel panel-split">
                <div class="split-top">
                    <span class="label">ERROR (m)</span>
                    <div id="error-m" class="val" style="font-size: 8vw;">0</div>
                </div>
                <div class="split-bottom">
                    <span class="label">CRONÓMETRO</span>
                    <div id="timer" class="val val-crono">00:00</div>
                </div>
            </div>
        </div>

        <div class="control-panel">
            <div class="control-row">
                <div class="control-item">
                    <span class="label">SALIDA AUTO</span>
                    <input type="time" id="start-time-input" step="1" style="width:100%; font-size:0.8rem;">
                    <button id="btn-wakelock" class="btn-wakelock" onclick="toggleWakeLock()">PANTALLA: NORMAL</button>
                </div>
                <div class="control-item">
                    <span class="label">TRAMO ACTIVO</span>
                    <select id="race-stage-select" onchange="syncStageChoice(this)" style="width:100%; color:#fff; border-color:#ff9900; font-weight:bold;"></select>
                </div>
                <div class="control-item">
                    <span class="label">SINCRO TRIP</span>
                    <div style="display:flex; width:100%;">
                        <input type="number" id="sync-km-input" placeholder="0.0" style="width:60%; font-size:0.8rem;">
                        <button onclick="syncDistance()" style="background:#ff9900; color:#000; width:40%;">OK</button>
                    </div>
                </div>
            </div>
        </div>

        <div style="padding: 10px; background: #1a1a1a; border-top: 2px solid #333;">
            <div style="margin-bottom: 10px;">
                <button id="btnStart" class="btn-main btn-start" onclick="startRace()">START</button>
                <button id="btnStop" class="btn-main btn-stop" onclick="pauseRace()">PAUSE</button>
                <button class="btn-main" onclick="resetAll()" style="background:#444;">RST</button>
            </div>
            <div id="sensor-status" style="color:#0f0; font-size:0.7rem;">SISTEMA WIRELESS ACTIVO</div>
        </div>
    </div>

    <div id="view-setup" class="view">
        <select id="stage-select" onchange="syncStageChoice(this)" style="width:70%;"></select>
        <button onclick="createNewStage()" style="background:#2e7d32; padding:10px;">+</button>
        <input type="text" id="stage-name-input" onchange="updateStageName()" style="width:90%; margin-top:10px;">
        <div style="background:#111; padding:10px; margin-top:10px; text-align:left;">
            <div style="margin-bottom:10px;">
                <label><input type="radio" name="inputMode" value="speed" checked onchange="toggleInputMode()"> MEDIA</label>
                <label><input type="radio" name="inputMode" value="time" onchange="toggleInputMode()"> TABLA</label>
            </div>
            KM: 
            <input type="number" id="input-km" step="0.001" style="width:80px;">
            <span id="mode-speed-container">SPD: <input type="number" id="input-spd" step="0.1" style="width:70px;"></span>
            <span id="mode-time-container" style="display:none;">M: <input type="number" id="table-m" style="width:50px;"> S: <input type="number" id="table-s" style="width:50px;"></span>
            <button class="btn-add" onclick="addSegment()" style="background:#ff9900; color:#000; padding:10px; width:100%; margin-top:10px;">AÑADIR HITO</button>
        </div>
        <table style="width:100%; margin-top:10px; border-collapse: collapse;"><tbody id="segments-body"></tbody></table>
    </div>

    <div id="view-calib" class="view">
        <div style="background:#111; padding:15px; border:1px solid #333; margin-bottom:10px;">
            <div class="label">TRAMO DE CALIBRACIÓN SENSOR</div>
            <div id="calib-trip" style="font-size:3rem; color:#00ffff;">0.0</div>
            <button id="btnCalibStart" class="btn-start" onclick="toggleCalibRun()" style="width:100%; padding:15px;">INICIAR / PARAR</button>
            <div id="calib-input-group" style="display:none; margin-top:10px;">
                REAL(m): <input type="number" id="official-dist-input" value="1000" style="width:100px;">
                <button onclick="calculateFactor()" style="background:#ff9900; color:#000; padding:10px;">APLICAR</button>
            </div>
        </div>

        <hr style="margin:20px 0; border-color:#333;">

        <div style="background:#111; padding:15px; border:1px solid #333;">
            <div class="label">CALIBRACIÓN POR RUEDA</div>

            <div style="margin-top:10px;">
                PERÍMETRO (m):
                <input type="number" id="wheel-perimeter-input" step="0.001" value="2.000" style="width:100px;">
            </div>

            <div style="margin-top:10px;">
                IMANES:
                <input type="number" id="wheel-magnets-input" value="1" style="width:80px;">
            </div>

            <button onclick="applyWheelCalibration()" 
                    style="background:#2e7d32; color:#fff; padding:10px; margin-top:15px; width:100%;">
                APLICAR POR RUEDA
            </button>
        </div>

        <hr style="margin:25px 0; border-color:#444;">

        <div style="background:#080808; padding:15px; border:1px solid #333;">
            <div class="label">DATOS ACTUALES DE CALIBRACIÓN</div>

            <div style="margin-top:10px;">
                Perímetro rueda: 
                <span id="calib-perimeter" style="color:#00ffff;">0.000</span> m
            </div>

            <div style="margin-top:5px;">
                Imanes: 
                <span id="calib-magnets" style="color:#00ffff;">0</span>
            </div>

            <div style="margin-top:5px;">
                Km por pulso (base): 
                <span id="calib-base" style="color:#ffff00;">0.00000000</span>
            </div>

            <div style="margin-top:5px;">
                Factor ajuste: 
                <span id="calib-factor" style="color:#ff9900;">0.000000</span>
            </div>

            <div style="margin-top:5px; font-weight:bold;">
                Km finales por pulso: 
                <span id="calib-final" style="color:#00ff00;">0.00000000</span>
            </div>
        </div>

        <hr style="margin:25px 0; border-color:#444;">

        <div style="background:#111; padding:10px; margin-top:10px; text-align:left; border:1px solid #333;">
          <div class="label">HITOS (CALIB)</div>

          <div style="margin-bottom:10px;">
            <label><input type="radio" name="calibInputMode" value="speed" checked onchange="toggleCalibInputMode()"> MEDIA</label>
            <label style="margin-left:10px;"><input type="radio" name="calibInputMode" value="time" onchange="toggleCalibInputMode()"> TABLA</label>
          </div>

          KM:
          <input type="number" id="calib-input-km" step="0.001" style="width:90px;">

          <span id="calib-mode-speed-container">
            SPD:
            <input type="number" id="calib-input-spd" step="0.1" style="width:80px;">
          </span>

          <span id="calib-mode-time-container" style="display:none;">
            M: <input type="number" id="calib-table-m" style="width:60px;">
            S: <input type="number" id="calib-table-s" style="width:60px;">
          </span>

          <button onclick="addSegmentFromCalib()"
                  style="background:#ff9900; color:#000; padding:10px; width:100%; margin-top:10px;">
            AÑADIR HITO
          </button>
        </div>

        <table style="width:100%; margin-top:10px; border-collapse: collapse;">
          <tbody id="calib-segments-body"></tbody>
        </table>

    </div>

    <div id="view-pilot" class="view">
        <button onclick="enterPilotMode()" style="background:#2e7d32; padding:40px; font-size:1.5rem; color:#fff; width:100%;">MODO PILOTO</button>
    </div>

    <div id="pilot-ui">
        <div id="pilot-dist" style="font-weight:bold; color:#00ffff;">0.000</div>
        <div id="pilot-error-container" class="error-bar-system">
            <div class="center-line"></div>
            <div id="pilot-cursor" class="cursor-line"></div>
        </div>
        <div id="pilot-err" style="font-weight:bold;">0</div>
        <button onclick="exitPilotMode()" style="color:#444; margin-top:20px;">CERRAR</button>
    </div>

<script>
/*
  FRONTEND "Tonto":
  - NO calcula ideal/error/crono/medias
  - Solo pinta datos que llegan del ESP
  - La lista de tramos/segmentos viene del ESP
*/

let rallyData = [];         // espejo de backend (para pintar selects/tabla)
let currentStageId = null;  // espejo de backend
let isRunning = false;
let isCalibrating = false;

let clockOffset = 0;
let wakeLock = null;

// --- WebSocket ---
let socket = new WebSocket('ws://' + window.location.hostname + ':81');

socket.onopen = () => {
  socket.send("GET_ALL"); // pide rutas + estado
};

socket.onmessage = (event) => {
  let msg = {};
  try { msg = JSON.parse(event.data); } catch(e){ return; }

  if(msg.t === "tele") {
    // pintar telemetría (SIN cálculos)
    document.getElementById('real').innerText   = Number(msg.real).toFixed(3);
    document.getElementById('ideal').innerText  = Number(msg.ideal).toFixed(3);
    document.getElementById('partial').innerText= Number(msg.partial).toFixed(3);
    document.getElementById('error-m').innerText= msg.error;
    document.getElementById('current-media-display').innerText = Number(msg.speed).toFixed(1) + " km/h";

    // crono
    let m = Math.floor(msg.time / 60);
    let s = msg.time % 60;
    document.getElementById('timer').innerText =
      m.toString().padStart(2,'0') + ":" + s.toString().padStart(2,'0');

    // barra error carrera
    updateErrorBar("race", msg.error);

    // modo piloto
    updatePilotUI({ d: Number(msg.real).toFixed(3), e: msg.error });

    // estado botones
    isRunning = !!msg.running;
    document.getElementById('btnStart').style.display = isRunning ? "none" : "inline-block";
    document.getElementById('btnStop').style.display  = isRunning ? "inline-block" : "none";

    // calib
    if(isCalibrating) {
      document.getElementById('calib-trip').innerText = Number(msg.real).toFixed(3);
    }

    // --- datos calibración ---
    if(msg.wheelPerimeter !== undefined){
        document.getElementById('calib-perimeter').innerText =
            Number(msg.wheelPerimeter).toFixed(3);

        document.getElementById('calib-magnets').innerText =
            msg.magnetCount;

        document.getElementById('calib-base').innerText =
            Number(msg.basePulseKm).toFixed(8);

        document.getElementById('calib-factor').innerText =
            Number(msg.factorW).toFixed(6);

        document.getElementById('calib-final').innerText =
            Number(msg.finalPulseKm).toFixed(8);
    }

    } else if(msg.t === "stages") {

    console.log("STAGES RECIBIDO: ", msg);
    console.log("STAGE ACTUAL:", currentStageId);
    console.log("RALLY DATA:", rallyData);


    rallyData = msg.stages || [];
    currentStageId = msg.current || null;

    renderStageSelect();
    loadStageData();  // ← esto ya pinta RUTAS

    let st = rallyData.find(x=> Number(x.id) === Number(currentStageId));
    renderCalibSegmentsTable(st ? (st.segments || []) : []);
  }

};

// --- Reloj visual (solo UI) ---
function updateClock() {
  const now = new Date(Date.now() + clockOffset);
  const hh = now.getHours().toString().padStart(2,'0');
  const mm = now.getMinutes().toString().padStart(2,'0');
  const ss = now.getSeconds().toString().padStart(2,'0');
  const timeStr = `${hh}:${mm}:${ss}`;
  document.getElementById('official-clock').innerText = timeStr;

  // start automático por hora (solo dispara comando, no calcula)
  const targetTime = document.getElementById('start-time-input').value;
  if (targetTime && !isRunning) {
    if (timeStr === targetTime || (targetTime.length === 5 && timeStr.startsWith(targetTime + ":00"))) {
      startRace();
      document.getElementById('start-time-input').value = "";
    }
  }
}
setInterval(updateClock, 1000);
function adjustClock(s) { clockOffset += (s * 1000); }

function applyWheelCalibration(){
    let perim = parseFloat(document.getElementById('wheel-perimeter-input').value);
    let magnets = parseInt(document.getElementById('wheel-magnets-input').value);

    if(isNaN(perim) || isNaN(magnets) || perim <= 0 || magnets <= 0){
        alert("Valores inválidos");
        return;
    }

    socket.send("CALC_WHEEL:" + perim + ":" + magnets);
}

// --- WakeLock (solo UI) ---
async function toggleWakeLock() {
  if ('wakeLock' in navigator) {
    if (wakeLock === null) {
      try {
        wakeLock = await navigator.wakeLock.request('screen');
        document.getElementById('btn-wakelock').innerText = "PANTALLA: ENCENDIDA";
        document.getElementById('btn-wakelock').classList.add('active');
      } catch (err) {}
    } else {
      wakeLock.release(); wakeLock = null;
      document.getElementById('btn-wakelock').innerText = "PANTALLA: NORMAL";
      document.getElementById('btn-wakelock').classList.remove('active');
    }
  }
}

// --- UI helpers ---
function updateErrorBar(prefix, errorMeters) {
  // tu UI usa % 0..100. Mantenemos tu lógica simple:
  let e = Number(errorMeters);
  let pos = 50 + e;
  if(pos < 0) pos = 0;
  if(pos > 100) pos = 100;

  document.getElementById(prefix + '-cursor').style.left = pos + "%";
  const cont = document.getElementById(prefix + '-error-container');
  cont.className = "error-bar-system";

  if(Math.abs(e) < 5) cont.classList.add('bg-ok');
  else if(e > 0) cont.classList.add('bg-early');
  else cont.classList.add('bg-late');
}

// --- Tabs ---
function switchTab(t){
  document.querySelectorAll('.view').forEach(e=>e.classList.remove('active'));
  document.querySelectorAll('.tab-btn').forEach(e=>e.classList.remove('active'));
  document.getElementById('view-'+t).classList.add('active');
  document.querySelectorAll('.tab-btn').forEach(b=>{
    if(b.getAttribute('onclick') && b.getAttribute('onclick').includes(`'${t}'`)) b.classList.add('active');
  });

  if(t==='setup'){
    renderStageSelect();
    loadStageData();
  }
}

// --- RUTAS / TRAMOS: ahora backend ---
function syncStageChoice(el) {
  currentStageId = parseInt(el.value, 10);
  socket.send("STAGE_SEL:" + currentStageId);
}
function createNewStage(){
  socket.send("STAGE_NEW");
}
function updateStageName(){
  let name = document.getElementById('stage-name-input').value || "";
  if(currentStageId) socket.send("STAGE_NAME:" + currentStageId + ":" + encodeURIComponent(name));
}
function renderStageSelect(){
  const s1=document.getElementById('stage-select');
  const s2=document.getElementById('race-stage-select');
  [s1,s2].forEach(sel=>{
    sel.innerHTML="";
    rallyData.forEach(st=>{
      const opt=document.createElement("option");
      opt.value = st.id;
      opt.textContent = st.name;
      if(st.id == currentStageId) opt.selected = true;
      sel.appendChild(opt);
    });
  });
}
function loadStageData(){
  let st = rallyData.find(x=> Number(x.id) === Number(currentStageId));
  if(!st) return;
  document.getElementById('stage-name-input').value = st.name;
  renderSegmentsTable(st.segments || []);
}
function toggleInputMode(){
  const m=document.querySelector('input[name="inputMode"]:checked').value;
  document.getElementById('mode-speed-container').style.display = (m==='speed')?'inline':'none';
  document.getElementById('mode-time-container').style.display  = (m==='time')?'inline':'none';
}

function addSegment(){

  if(!currentStageId) return;

  const mode = document.querySelector('input[name="inputMode"]:checked').value;

  let st = rallyData.find(x => Number(x.id) === Number(currentStageId));
  if(!st) return;

  let lastKm = 0;
  if(st.segments && st.segments.length > 0){
      lastKm = Number(st.segments[st.segments.length - 1].km);
  }

  let km = 0;
  let spd = 0;

  // =========================
  // MODO MEDIA (acumulado)
  // =========================
  if(mode === "speed"){

    const metersInc = parseFloat(document.getElementById('input-km').value);
    spd = parseFloat(document.getElementById('input-spd').value);

    if(isNaN(metersInc) || isNaN(spd)) return;

    km = lastKm + (metersInc / 1000.0);

    document.getElementById('input-km').value = "";
    document.getElementById('input-spd').value = "";
  }

  // =========================
  // MODO TABLA (incremental)
  // =========================
  else {

      const metersInc = parseFloat(document.getElementById('table-m').value);
      const seconds   = parseFloat(document.getElementById('table-s').value);

      if(isNaN(metersInc) || isNaN(seconds) || metersInc <= 0 || seconds <= 0) return;

      // nueva distancia acumulada
      km = lastKm + (metersInc / 1000.0);

      // calcular velocidad
      spd = (metersInc / seconds) * 3.6;

      document.getElementById('table-m').value = "";
      document.getElementById('table-s').value = "";
  }

  socket.send("SEG_ADD:" + currentStageId + ":" + km.toFixed(3) + ":" + spd.toFixed(1));
}


function addSegmentFromCalib(){
  if(!currentStageId) return;

  const mode = document.querySelector('input[name="calibInputMode"]:checked').value;
  const km = parseFloat(document.getElementById('calib-input-km').value);
  if(isNaN(km)) return;

  let spd = 0;
  if(mode === "speed"){
    spd = parseFloat(document.getElementById('calib-input-spd').value);
    if(isNaN(spd)) return;
  } else {
    const mm = parseFloat(document.getElementById('calib-table-m').value);
    const ss = parseFloat(document.getElementById('calib-table-s').value);
    if(isNaN(mm) || isNaN(ss)) return;
    const totalSec = (mm*60) + ss;
    if(totalSec <= 0) return;
    spd = (km * 3600) / totalSec;
  }

  socket.send("SEG_ADD:" + currentStageId + ":" + km.toFixed(3) + ":" + spd.toFixed(1));

  // limpiar
  document.getElementById('calib-input-km').value = "";
  document.getElementById('calib-input-spd').value = "";
  document.getElementById('calib-table-m').value = "";
  document.getElementById('calib-table-s').value = "";
}

function renderCalibSegmentsTable(seg){
  const b = document.getElementById('calib-segments-body');
  b.innerHTML = "";

  if(!seg || seg.length === 0){
    b.innerHTML = `
      <tr><td colspan="3" style="color:#666; padding:10px;">SIN HITOS</td></tr>
    `;
    return;
  }

  seg.forEach((g,i)=>{
    const tr = document.createElement("tr");
    tr.innerHTML = `
      <td>${Number(g.km).toFixed(3)} km</td>
      <td>${Number(g.speed).toFixed(1)} km/h</td>
      <td>
        <button onclick="delSeg(${i})"
                style="background:#220000;color:#ff4444;border:1px solid #aa0000;padding:6px 8px;border-radius:4px;">
          🗑
        </button>
      </td>
    `;
    b.appendChild(tr);
  });
}

function renderSegmentsTable(seg){

  const b = document.getElementById('segments-body');
  b.innerHTML = "";

  if(!seg || seg.length === 0){
    b.innerHTML = `
      <tr>
        <td colspan="3" style="color:#666; padding:10px;">
          SIN HITOS
        </td>
      </tr>
    `;
    return;
  }

  seg.forEach((g,i)=>{

    const tr = document.createElement("tr");

    tr.innerHTML = `
      <td>${Number(g.km).toFixed(3)} km</td>
      <td>${Number(g.speed).toFixed(1)} km/h</td>
      <td>
        <button onclick="delSeg(${i})"
                style="background:#220000;
                       color:#ff4444;
                       border:1px solid #aa0000;
                       padding:6px 8px;
                       border-radius:4px;">
          🗑
        </button>
      </td>
    `;

    b.appendChild(tr);
  });
}

function delSeg(i){
  if(!currentStageId) return;
  socket.send("SEG_DEL:" + currentStageId + ":" + i);
}

// --- Calibración (backend) ---
function toggleCalibRun(){
  isCalibrating = !isCalibrating;
  if(isCalibrating){
    socket.send("C_START");
    document.getElementById('btnCalibStart').innerText="STOP";
    document.getElementById('calib-input-group').style.display="none";
  } else {
    document.getElementById('btnCalibStart').innerText="INICIAR";
    document.getElementById('calib-input-group').style.display="block";
  }
}
function calculateFactor(){
  const om = parseFloat(document.getElementById('official-dist-input').value);
  if(isNaN(om)) return;
  socket.send("F" + om);
  switchTab('race');
}

// --- Sincronía/ajustes distancia (backend) ---
function syncDistance(){
  let val=document.getElementById('sync-km-input').value;
  if(val!==""){
    socket.send("S" + val);
    document.getElementById('sync-km-input').value="";
  }
}
function adjustDist(val){ socket.send("A" + val); }

// --- Control carrera (backend) ---
function startRace(){ socket.send("START"); }
function pauseRace(){ socket.send("STOP"); }
function resetAll(){ socket.send("RESET"); }
function resetPartial(){ socket.send("P_RESET"); }

// --- Modo Piloto (solo UI) ---
function enterPilotMode(){ document.getElementById('pilot-ui').style.display='flex'; toggleWakeLock(); }
function exitPilotMode(){ document.getElementById('pilot-ui').style.display='none'; }
function updatePilotUI(data){
  document.getElementById('pilot-dist').innerText = data.d;
  document.getElementById('pilot-err').innerText  = data.e;
  updateErrorBar("pilot", data.e);

  const e = Number(data.e);
  if(Math.abs(e)<5) document.getElementById('pilot-err').style.color="#0f0";
  else if(e>0) document.getElementById('pilot-err').style.color="#3388ff";
  else document.getElementById('pilot-err').style.color="#f00";
}

async function activateWakeLock() {
    if ('wakeLock' in navigator) {
        try {
            wakeLock = await navigator.wakeLock.request('screen');
            console.log("WakeLock activo");
        } catch (err) {
            console.log("No se pudo activar WakeLock");
        }
    }
}

// Activar WakeLock automáticamente al cargar
window.addEventListener('load', () => {
    activateWakeLock();
});
</script>
</body>
</html>)=====";

// ---------------- WebSocket parsing ----------------
static void handleCommand(String msg) {

  msg.trim();
  ensureDefaultStage();

  // =========================
  // GET ALL
  // =========================
  if (msg == "GET_ALL") {
    String s1 = buildStagesJson();
    webSocket.broadcastTXT(s1);

    String s2 = buildTeleJson();
    webSocket.broadcastTXT(s2);

    stagesDirty = false;
    return;
  }

  // =========================
  // SEG_ADD  (ANTES que "S")
  // =========================
  if (msg.startsWith("SEG_ADD:")) {

    int p1 = msg.indexOf(':');
    int p2 = msg.indexOf(':', p1 + 1);
    int p3 = msg.indexOf(':', p2 + 1);

    float km = msg.substring(p2 + 1, p3).toFloat();
    float spd = msg.substring(p3 + 1).toFloat();

    Stage* st = findStageById(currentStageId);
    if (!st) return;
    if (st->segCount >= MAX_SEGS) return;

    st->segs[st->segCount].km = km;
    st->segs[st->segCount].speed = spd;
    st->segCount++;

    stagesDirty = true;

    Serial.print("Segmento añadido. Total: ");
    Serial.println(st->segCount);

    return;
  }

  // =========================
  // SEG_DEL
  // =========================
  if (msg.startsWith("SEG_DEL:")) {

    int p1 = msg.indexOf(':');
    int p2 = msg.indexOf(':', p1 + 1);

    int idx = msg.substring(p2 + 1).toInt();

    Stage* st = findStageById(currentStageId);
    if (!st) return;
    if (idx < 0 || idx >= st->segCount) return;

    for (int i = idx; i < st->segCount - 1; i++)
      st->segs[i] = st->segs[i + 1];

    st->segCount--;

    stagesDirty = true;
    return;
  }

  // =========================
  // STAGE_NEW
  // =========================
  if (msg == "STAGE_NEW") {

    if (stageCount >= MAX_STAGES) return;

    uint32_t id = (uint32_t)millis() + random(1000, 9999);

    stages[stageCount].id = id;
    stages[stageCount].name = "TRAMO " + String(stageCount + 1);
    stages[stageCount].segCount = 0;

    stageCount++;
    currentStageId = id;
    stagesDirty = true;

    return;
  }

  // =========================
  // STAGE_SEL
  // =========================
  if (msg.startsWith("STAGE_SEL:")) {

    uint32_t id = msg.substring(strlen("STAGE_SEL:")).toInt();

    if (findStageById(id)) {
      currentStageId = id;
      stagesDirty = true;
    }

    return;
  }

  // =========================
  // STAGE_NAME
  // =========================
  if (msg.startsWith("STAGE_NAME:")) {

    int p1 = msg.indexOf(':');
    int p2 = msg.indexOf(':', p1 + 1);

    uint32_t id = msg.substring(p1 + 1, p2).toInt();
    String name = msg.substring(p2 + 1);

    Stage* st = findStageById(id);
    if (st) {
      st->name = name;
      stagesDirty = true;
    }

    return;
  }

  // =========================
  // START / STOP / RESET
  // =========================
  if (msg == "START") {
    raceRunning = true;
    lastLoopMs = millis();
    return;
  }

  if (msg == "STOP") {
    raceRunning = false;
    return;
  }

  if (msg == "RESET") {
    raceRunning = false;
    distReal_mm = 0;
    distIdeal = 0;
    totalRaceMs = 0;
    partialOffset_mm = 0;
    return;
  }

  if (msg == "P_RESET") {
    partialOffset_mm = distReal_mm;
    return;
  }

  // =========================
  // CALIB START
  // =========================
  if (msg == "C_START") {
    distReal_mm = 0;
    partialOffset_mm = 0;
    return;
  }

  // =========================
  // FACTOR CALIB
  // =========================
  if (msg.startsWith("F")) {

    float officialMeters = msg.substring(1).toFloat();

    if (distReal_mm > 0 && officialMeters > 0) {

      float measuredMeters = distReal_mm / 1000.0f;
      float correction = officialMeters / measuredMeters;

      factorW = factorW * correction;
    }

    return;
  }

  // =========================
  // AJUSTE DISTANCIA  (ANTES que "S")
  // =========================
  if (msg.startsWith("A")) {

    float km = msg.substring(1).toFloat();
    distReal_mm += (long)(km * 1000000.0);
    return;
  }

  // =========================
  // SYNC DISTANCIA (AL FINAL)
  // =========================
  if (msg.startsWith("S")) {

    float km = msg.substring(1).toFloat();
    distReal_mm = (long)(km * 1000000.0);
    partialOffset_mm = distReal_mm;
    return;
  }
}


// ---------------- WebSocket events ----------------
void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    ensureDefaultStage();
    // Enviar stages al conectar
    String s1 = buildStagesJson();
    webSocket.sendTXT(num, s1);

    String s2 = buildTeleJson();
    webSocket.sendTXT(num, s2);
  } else if (type == WStype_TEXT) {

    String msg = String((char*)payload);

    Serial.print("Mensaje WebSocket recibido: ");
    Serial.println(msg);

    handleCommand(msg);

    // Forzar envío inmediato de stages tras SEG_ADD
    if (msg.startsWith("SEG_ADD:") || 
        msg.startsWith("SEG_DEL:") || 
        msg.startsWith("STAGE_NEW") ||
        msg.startsWith("STAGE_NAME:") ||
        msg.startsWith("STAGE_SEL:")) {

        String s = buildStagesJson();
        webSocket.sendTXT(num, s);
    }

    // Siempre enviar tele
    String t = buildTeleJson();
    webSocket.sendTXT(num, t);
}

}

// ---------------- Setup/Loop ----------------
void setup() {

  Serial.begin(115200);
  delay(500);
  Serial.println("Sistema iniciado");

  pinMode(sensorPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(sensorPin), sensorISR, RISING);

  WiFi.softAP(ssid, password);

  server.on("/", []() {
    server.send_P(200, "text/html", PAGE_MAIN);
  });
  server.begin();

  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);

  ensureDefaultStage();
  lastLoopMs = millis();
}

void loop() {
  server.handleClient();
  webSocket.loop();

  unsigned long now = millis();

  // --- integrar pulsos ---
  int p = 0;
  noInterrupts();
  if (pulsesQueued > 0) {
    p = pulsesQueued;
    pulsesQueued = 0;
  }
  interrupts();

  if (p > 0) {
    // km += factorW * base * pulsos
    // milímetros por pulso
    long mm_per_pulse = (long)((wheelPerimeter * 1000.0) / magnetCount);

    // aplicar factorW
    mm_per_pulse = (long)(mm_per_pulse * factorW);

    distReal_mm += mm_per_pulse * p;
  }

  // --- motor de carrera ---
  unsigned long dtMs = now - lastLoopMs;
  if (dtMs > 500) dtMs = 500;  // clamp
  float dt = dtMs / 1000.0f;

  if (raceRunning) {
    totalRaceMs += dtMs;
    float spd = backendCurrentSpeedKmh();  // km/h desde la tabla del tramo
    distIdeal += (spd / 3600.0f) * dt;
  }

  lastLoopMs = now;

  // --- broadcast tele periódica ---
  static unsigned long lastTele = 0;
  if (now - lastTele >= 100) {
    broadcastTele();
    lastTele = now;
  }

  // --- broadcast stages si cambiaron ---
  static unsigned long lastStages = 0;
  if (stagesDirty) {
    broadcastStages();
    lastStages = now;
  }
}
