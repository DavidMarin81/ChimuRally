#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WebSocketsServer.h>

// --- CONFIGURACIÓN WIFI ---
const char* ssid = "RALLY_PRO_V26";
const char* password = "password123";

ESP8266WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);

// --- HARDWARE SENSOR ---
const int sensorPin = 4; // Pin D2 (GPIO4)
volatile int pulsesQueued = 0;
unsigned long lastDebounceTime = 0;
const int debounceDelay = 15; 

// --- VARIABLES DE ESTADO ---
float distReal = 0.0;
float factorW = 1.0000; // Se sincroniza con el Calib del HTML

void IRAM_ATTR sensorISR() {
  unsigned long currentTime = millis();
  if ((currentTime - lastDebounceTime) > debounceDelay) {
    pulsesQueued++;
    lastDebounceTime = currentTime;
  }
}

// --- TU HTML CALCADO Y CONECTADO AL SENSOR ---
const char PAGE_MAIN[] PROGMEM = R"=====(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <title>Rally Pro Sim - V25 Master (No Sleep)</title>
    <style>
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
        #pilot-ui { display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: #000; z-index: 2000; flex-direction: column; justify-content: center; align-items: center; }
        #throttle-container { display: none; } /* Ocultamos el acelerador virtual */
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
                <button onclick="distPartial=0" style="background:#444; color:#aaa; font-size:0.6rem; padding:4px; margin-top:4px;">RESET</button>
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
            KM: <input type="number" id="input-km" step="0.001" style="width:80px;">
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
    </div>

    <div id="view-pilot" class="view">
        <button onclick="enterPilotMode()" style="background:#2e7d32; padding:40px; font-size:1.5rem; color:#fff; width:100%;">MODO PILOTO</button>
    </div>

    <div id="pilot-ui">
        <div id="pilot-dist" style="font-size:28vw; font-weight:bold; color:#00ffff;">0.000</div>
        <div id="pilot-error-container" class="error-bar-system" style="width:90%; height:80px; margin:20px 0;">
            <div class="center-line"></div>
            <div id="pilot-cursor" class="cursor-line"></div>
        </div>
        <div id="pilot-err" style="font-size:18vw; font-weight:bold;">0</div>
        <button onclick="exitPilotMode()" style="color:#444; margin-top:20px;">CERRAR</button>
    </div>

    <script>
        let rallyData=[]; let currentStageId=null;
        let isRunning=false, lastFrameTime=0, distReal=0, distIdeal=0, distPartial=0, totalTime=0;
        let clockOffset = 0;
        let isCalibrating=false;
        let wakeLock = null;

        // --- CONEXIÓN SENSOR ---
        let socket = new WebSocket('ws://' + window.location.hostname + ':81');
        socket.onmessage = (event) => {
            let data = parseFloat(event.data);
            distReal = data; // La distancia viene directa del sensor ESP8266
            if(isCalibrating) document.getElementById('calib-trip').innerText = distReal.toFixed(3);
            updatePilotUI({d: distReal.toFixed(3), e: Math.round((distReal-distIdeal)*1000)});
        };

        function updateClock() {
            const now = new Date(Date.now() + clockOffset);
            const hh = now.getHours().toString().padStart(2,'0');
            const mm = now.getMinutes().toString().padStart(2,'0');
            const ss = now.getSeconds().toString().padStart(2,'0');
            const timeStr = ${hh}:${mm}:${ss};
            document.getElementById('official-clock').innerText = timeStr;
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
                }
            }
        }

        // --- MOTOR RECALIBRADO PARA SENSOR ---
        function gameLoop(ct){
            let dt=(ct-lastFrameTime)/1000; lastFrameTime=ct; if(dt>0.5)dt=0.5;
            if(isRunning){
                totalTime+=dt; 
                let tspd=0; 
                let s=rallyData.find(x=>x.id===currentStageId);
                if(s) {
                    for(let g of s.segments){ 
                        if(distIdeal>=g.km) tspd=g.speed; else break; 
                    }
                }
                distIdeal+=(tspd/3600)*dt;
                updateUI(tspd);
            }
            requestAnimationFrame(gameLoop);
        }

        function updateUI(tspd){
            document.getElementById('real').innerText=distReal.toFixed(3);
            document.getElementById('ideal').innerText=distIdeal.toFixed(3);
            document.getElementById('partial').innerText=distPartial.toFixed(3);
            document.getElementById('current-media-display').innerText=tspd.toFixed(1) + " km/h";
            let e = (distReal-distIdeal)*1000;
            document.getElementById('error-m').innerText=Math.round(e);
            let m=Math.floor(totalTime/60), s=Math.floor(totalTime%60);
            document.getElementById('timer').innerText=${m.toString().padStart(2,'0')}:${s.toString().padStart(2,'0')};
            let pos=50+e; if(pos<0)pos=0; if(pos>100)pos=100;
            document.getElementById('race-cursor').style.left=pos+"%";
            const cont=document.getElementById('race-error-container'); cont.className="error-bar-system";
            if(Math.abs(e)<5) cont.classList.add('bg-ok'); else if(e>0) cont.classList.add('bg-early'); else cont.classList.add('bg-late');
        }

        function syncStageChoice(el) { currentStageId = parseInt(el.value); renderStageSelect(); loadStageData(); }
        function createNewStage(){let id=Date.now(); rallyData.push({id:id,name:"TRAMO "+(rallyData.length+1),segments:[]}); currentStageId=id; renderStageSelect(); loadStageData();}
        function renderStageSelect(){
            const s1=document.getElementById('stage-select'), s2=document.getElementById('race-stage-select');
            [s1, s2].forEach(s => { s.innerHTML=""; rallyData.forEach(st=>{ s.innerHTML+=<option value="${st.id}" ${st.id==currentStageId?'selected':''}>${st.name}</option>; }); });
        }
        function loadStageData(){let s=rallyData.find(x=>x.id===currentStageId); if(s){document.getElementById('stage-name-input').value=s.name; renderSegmentsTable(s.segments);}}
        function updateStageName(){let s=rallyData.find(x=>x.id===currentStageId); if(s)s.name=document.getElementById('stage-name-input').value; saveToStorage(); renderStageSelect();}
        function addSegment(){
            let s=rallyData.find(x=>x.id===currentStageId);
            if(!s)return;
            let km=parseFloat(document.getElementById('input-km').value);
            let spd=parseFloat(document.getElementById('input-spd').value);
            s.segments.push({km:km, speed:spd});
            s.segments.sort((a,b)=>a.km-b.km);
            saveToStorage(); renderSegmentsTable(s.segments);
        }
        function toggleCalibRun(){ isCalibrating=!isCalibrating; if(isCalibrating){ socket.send("C_START"); document.getElementById('btnCalibStart').innerText="STOP"; } else { document.getElementById('btnCalibStart').innerText="INICIAR"; document.getElementById('calib-input-group').style.display="block"; } }
        function calculateFactor(){ let om=parseFloat(document.getElementById('official-dist-input').value); socket.send("F" + om); switchTab('race'); }
        function syncDistance(){ let val=document.getElementById('sync-km-input').value; if(val!==""){socket.send("S" + val); document.getElementById('sync-km-input').value="";} }
        function adjustDist(val){ socket.send("A" + val); }
        function startRace(){isRunning=true; document.getElementById('btnStart').style.display="none"; document.getElementById('btnStop').style.display="inline-block";}
        function pauseRace(){isRunning=false; document.getElementById('btnStart').style.display="inline-block"; document.getElementById('btnStop').style.display="none";}
        function resetAll(){ pauseRace(); socket.send("R"); distIdeal=0; totalTime=0; updateUI(0); }
        function switchTab(t){ document.querySelectorAll('.view').forEach(e=>e.classList.remove('active')); document.querySelectorAll('.tab-btn').forEach(e=>e.classList.remove('active')); document.getElementById('view-'+t).classList.add('active'); if(t==='setup'){ renderStageSelect(); loadStageData(); } }
        function toggleInputMode(){ const m=document.querySelector('input[name="inputMode"]:checked').value; document.getElementById('mode-speed-container').style.display=m==='speed'?'inline':'none'; document.getElementById('mode-time-container').style.display=m==='time'?'inline':'none'; }
        function renderSegmentsTable(seg){ const b=document.getElementById('segments-body'); b.innerHTML=""; seg.forEach((g,i)=>{ b.innerHTML+=<tr><td>KM ${g.km.toFixed(3)}</td><td>${g.speed.toFixed(1)} km/h</td><td><button onclick="delSeg(${i})" style="color:red;">X</button></td></tr>; }); }
        function delSeg(i){ let s=rallyData.find(x=>x.id===currentStageId); s.segments.splice(i,1); saveToStorage(); renderSegmentsTable(s.segments); }
        function loadFromStorage(){ const d=localStorage.getItem('rallyProData_v9'); if(d)rallyData=JSON.parse(d); }
        function saveToStorage(){localStorage.setItem('rallyProData_v9',JSON.stringify(rallyData));}

        function enterPilotMode(){ document.getElementById('pilot-ui').style.display='flex'; toggleWakeLock(); }
        function exitPilotMode(){ document.getElementById('pilot-ui').style.display='none'; }
        function updatePilotUI(data){
            document.getElementById('pilot-dist').innerText=data.d;
            document.getElementById('pilot-err').innerText=data.e;
            let pos=50+(data.e/2); if(pos<0)pos=0; if(pos>100)pos=100;
            document.getElementById('pilot-cursor').style.left=pos+"%";
            const cont=document.getElementById('pilot-error-container'); cont.className="error-bar-system";
            if(Math.abs(data.e)<5) { cont.classList.add('bg-ok'); document.getElementById('pilot-err').style.color="#0f0"; }
            else if(data.e>0) { cont.classList.add('bg-early'); document.getElementById('pilot-err').style.color="#3388ff"; }
            else { cont.classList.add('bg-late'); document.getElementById('pilot-err').style.color="#f00"; }
        }

        loadFromStorage(); if(rallyData.length===0) createNewStage();
        renderStageSelect(); lastFrameTime=performance.now(); requestAnimationFrame(gameLoop);
    </script>
</body>
</html>
)=====";

// --- GESTIÓN WEBSOCKETS (Sincronización Corregida) ---
void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  if (type == WStype_TEXT) {
    String msg = String((char*)payload);
    
    if (msg == "R") {
      distReal = 0;
    } 
    else if (msg.startsWith("S")) {
      distReal = msg.substring(1).toFloat();
    }
    else if (msg.startsWith("A")) {
      distReal += msg.substring(1).toFloat();
    }
    else if (msg.startsWith("F")) {
      // Calibración: Calcula factor basado en metros reales recorridos
      float officialMeters = msg.substring(1).toFloat();
      if(distReal > 0) factorW = officialMeters / (distReal / factorW); 
    }
    else if (msg == "C_START") {
      distReal = 0;
    }

    String response = String(distReal, 3);
    webSocket.broadcastTXT(response);
  }
}

void setup() {
  pinMode(sensorPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(sensorPin), sensorISR, RISING);

  WiFi.softAP(ssid, password);
  server.on("/", []() { server.send_P(200, "text/html", PAGE_MAIN); });
  server.begin();
  
  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);
}

void loop() {
  server.handleClient();
  webSocket.loop();

  if (pulsesQueued > 0) {
    distReal += (factorW * 0.001050 * pulsesQueued); // 0.001050 es un factor base ajustable
    pulsesQueued = 0;
    
    String response = String(distReal, 3);
    webSocket.broadcastTXT(response);
  }
}