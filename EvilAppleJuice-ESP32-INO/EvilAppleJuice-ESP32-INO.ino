// EvilAppleJuice-ESP32 with WiFi Control (Hidden AP + Web UI)
// Based on the previous work of chipik / _hexway / ECTO-1A & SAY-10 / ronaldstoner
#include <Arduino.h>

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>

#include <esp_arduino_version.h>

#include "devices.hpp"
#include "led.hpp"

// ============================================================
// Bluetooth maximum transmit power
// ============================================================
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C2) || defined(CONFIG_IDF_TARGET_ESP32S3)
#define MAX_TX_POWER ESP_PWR_LVL_P21  // ESP32C3 ESP32C2 ESP32S3
#elif defined(CONFIG_IDF_TARGET_ESP32H2) || defined(CONFIG_IDF_TARGET_ESP32C6)
#define MAX_TX_POWER ESP_PWR_LVL_P20  // ESP32H2 ESP32C6
#else
#define MAX_TX_POWER ESP_PWR_LVL_P9   // Default
#endif

// ============================================================
// WiFi AP Configuration (visible + optional STA)
// ============================================================
const char* WIFI_AP_SSID     = "EAJ-Control";   // AP SSID，手机可直接扫描连接
const char* WIFI_AP_PASSWORD = "";               // 留空 = 开放式；若设置密码需 >=8 字符
const int   WIFI_AP_CHANNEL  = 6;                // 建议 1/6/11
const bool  WIFI_AP_HIDDEN   = false;            // false = 公开可见，方便手机扫描连接
const int   WIFI_MAX_CONN    = 4;

// STA mode (optional): connect to home WiFi so the device is reachable from LAN
String  staSSID     = "";
String  staPassword = "";
bool    staEnabled  = false;   // true = try STA after AP is up

WebServer server(80);

// ============================================================
// Global State
// ============================================================
BLEAdvertising *pAdvertising;  // global variable

// Persistent runtime controls
int       currentMode       = 0;        // 0..8  对应 9 种 LED/设备模式
bool      useManualDevice   = false;    // true 时忽略 currentMode 对应设备，改用 manualDeviceIndex
int       manualDeviceIndex = 0;        // 0..NUM_DEVICES-1
bool      broadcastEnabled  = true;     // 总开关
uint32_t  delayMilliseconds = 100;      // 单轮广播持续时间 (ms)
// Power mode: 0 = dynamic random (original), 1..5 = fixed (MAX..MAX-4)
int       powerMode         = 0;

// Runtime stats (for the Web UI)
uint32_t  totalBroadcastCount = 0;
String    lastDeviceName      = "(idle)";
uint32_t  lastBroadcastMs     = 0;

Preferences preferences;

#define RIGHT_LED 12
#define LEFT_LED 13
const int BOOT_BUTTON_PIN = 9;
const unsigned long LONG_PRESS_TIME = 1000; // 1 seconds

// LED flash timer (independent of broadcast loop so FLASH is clearly visible)
unsigned long lastFlashToggle = 0;
bool          flashState      = false;
const unsigned long FLASH_PERIOD_MS = 500; // 500ms on / 500ms off = 1Hz visible blink

// ============================================================
// Helper: Resolve which device to broadcast right now
// ============================================================
AppleDevice getCurrentDevice() {
  if (useManualDevice) {
    if (manualDeviceIndex >= 0 && manualDeviceIndex < NUM_DEVICES) {
      return ALL_DEVICES[manualDeviceIndex];
    }
  }
  // Fallback to mode-based selection (mirrors original switch logic)
  switch (currentMode) {
    case LEFT_OFF_RIGHT_OFF:    return ALL_DEVICES[AIRPODS];
    case LEFT_OFF_RIGHT_FLASH: { // random
      int idx = random(0, (int)(sizeof(ALL_DEVICES) / sizeof(ALL_DEVICES[0])));
      return ALL_DEVICES[idx];
    }
    case LEFT_OFF_RIGHT_ON:     return ALL_DEVICES[SOFTWARE_UPDATE];
    case LEFT_FLASH_RIGHT_OFF:  return ALL_DEVICES[AIRPODS_GEN_2];
    case LEFT_FLASH_RIGHT_FLASH:return ALL_DEVICES[VISION_PRO];
    case LEFT_FLASH_RIGHT_ON:   return ALL_DEVICES[AIRPODS_MAX];
    case LEFT_ON_RIGHT_OFF:     return ALL_DEVICES[APPLETV_SETUP];
    case LEFT_ON_RIGHT_FLASH:   return ALL_DEVICES[TRANSFER_NUMBER];
    case LEFT_ON_RIGHT_ON:      return ALL_DEVICES[APPLETV_PAIR];
    default:                    return ALL_DEVICES[HOMEPOD_SETUP];
  }
}

// ============================================================
// Persistence helpers
// ============================================================
void saveAllPreferences() {
  preferences.begin("my-app", false);
  preferences.putInt("mode",      currentMode);
  preferences.putBool("manual",   useManualDevice);
  preferences.putInt("devIdx",    manualDeviceIndex);
  preferences.putBool("brdOn",    broadcastEnabled);
  preferences.putUInt("delay",    delayMilliseconds);
  preferences.putInt("pwr",       powerMode);
  preferences.putString("staSSID", staSSID);
  preferences.putString("staPass", staPassword);
  preferences.putBool("staEn",    staEnabled);
  preferences.end();
}

void loadAllPreferences() {
  preferences.begin("my-app", false);
  currentMode       = preferences.getInt("mode",    0);
  useManualDevice   = preferences.getBool("manual", false);
  manualDeviceIndex = preferences.getInt("devIdx",  0);
  broadcastEnabled  = preferences.getBool("brdOn",  true);
  delayMilliseconds = preferences.getUInt("delay",  100);
  powerMode         = preferences.getInt("pwr",     0);
  staSSID           = preferences.getString("staSSID", "");
  staPassword       = preferences.getString("staPass", "");
  staEnabled        = preferences.getBool("staEn",   false);
  preferences.end();
}

void resetMode(){
  currentMode = 0;
  useManualDevice = false;
  Serial.printf("Resetting mode to %d (manual disabled)\n", currentMode);
  saveAllPreferences();
}

void nextMode(){
  useManualDevice = false; // button press returns to mode-driven control
  currentMode = (currentMode + 1) % (sizeof(stateTable) / sizeof(stateTable[0]));
  Serial.printf("Updating mode to %d\n", currentMode);
  saveAllPreferences();
}

// ============================================================
// Advertisement packet building
// ============================================================
void setAdvertisementData(BLEAdvertisementData &oAdvertisementData, const AppleDevice& dev) {
  uint8_t packet[31];
  size_t packetLen;
  generatePacket(dev, packet, packetLen);
  lastDeviceName  = dev.name;
  lastBroadcastMs = millis();
  totalBroadcastCount++;
  Serial.printf("Broadcasting %s (Length: %d)...\n", dev.name, (int)packetLen);

  #ifdef ESP_ARDUINO_VERSION_MAJOR
    #if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
        oAdvertisementData.addData(String((char*)packet, packetLen));
    #else
        oAdvertisementData.addData(std::string((char*)packet, packetLen));
    #endif
  #endif
}

// ============================================================
// LED helpers
// ============================================================
bool shouldBeLitOn(LEDMode mode) {
  switch (mode) {
    case ON:    return true;
    case OFF:   return false;
    case FLASH: return true;
    default:    return false;
  }
}

bool shouldBeLitOff(LEDMode mode) {
  switch (mode) {
    case ON:    return false;
    case OFF:   return true;
    case FLASH: return true;
    default:    return false;
  }
}

// Unified LED update: each LED mode maps to a distinct, non-repeating visual state.
//   ON    -> steady HIGH
//   OFF   -> steady LOW
//   FLASH -> blink at FLASH_PERIOD_MS using the independent timer (clearly visible)
void updateLEDs() {
  // Toggle flash state on a fixed period regardless of broadcast timing.
  unsigned long now = millis();
  if (now - lastFlashToggle >= FLASH_PERIOD_MS) {
    lastFlashToggle = now;
    flashState = !flashState;
  }
  LEDMode l = stateTable[currentMode][0];
  LEDMode r = stateTable[currentMode][1];
  digitalWrite(LEFT_LED,  (l == ON) ? HIGH : (l == OFF ? LOW : (flashState ? HIGH : LOW)));
  digitalWrite(RIGHT_LED, (r == ON) ? HIGH : (r == OFF ? LOW : (flashState ? HIGH : LOW)));
}

void applyPower() {
  if (powerMode == 0) {
    // Original dynamic random strategy
    int rand_val = random(100);
    if (rand_val < 70) {
      esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, MAX_TX_POWER);
    } else if (rand_val < 85) {
      esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, (esp_power_level_t)(MAX_TX_POWER - 1));
    } else if (rand_val < 95) {
      esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, (esp_power_level_t)(MAX_TX_POWER - 2));
    } else if (rand_val < 99) {
      esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, (esp_power_level_t)(MAX_TX_POWER - 3));
    } else {
      esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, (esp_power_level_t)(MAX_TX_POWER - 4));
    }
  } else {
    // Fixed power level.  powerMode 1..5 -> offset 0..4
    int offset = powerMode - 1;
    if (offset < 0) offset = 0;
    if (offset > 4) offset = 4;
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, (esp_power_level_t)(MAX_TX_POWER - offset));
  }
}

// ============================================================
// ==========   WIFI + WEB UI   ================================
// ============================================================

// --------  Control Page HTML (inline raw string)  ------------
static const char CONTROL_HTML[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>EvilAppleJuice 控制中心</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}
  body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
       background:#0f1220;color:#e6e8f0;padding:14px;max-width:560px;margin:0 auto;}
  h1{font-size:18px;margin-bottom:12px;color:#fff;display:flex;align-items:center;gap:8px}
  .dot{width:10px;height:10px;border-radius:50%;background:#4ade80;display:inline-block;box-shadow:0 0 10px #4ade80}
  .card{background:#1a1e33;border-radius:14px;padding:14px;margin-bottom:14px;border:1px solid #2a2f4a}
  .card h2{font-size:14px;margin-bottom:10px;color:#9ca3ff;letter-spacing:.5px}
  .row{display:flex;align-items:center;justify-content:space-between;margin:8px 0}
  .label{font-size:13px;color:#b8bcd0}
  .value{font-size:13px;color:#fff;font-weight:600}
  .grid9{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
  .mode-btn{border:1px solid #32375a;background:#222742;color:#d8dbef;border-radius:10px;
            padding:10px 4px;font-size:12px;cursor:pointer;transition:.15s}
  .mode-btn.active{background:linear-gradient(135deg,#6366f1,#8b5cf6);color:#fff;border-color:transparent;
                   box-shadow:0 3px 10px rgba(139,92,246,.4)}
  .big{display:flex;gap:8px;margin-top:8px}
  .big-btn{flex:1;padding:14px;border-radius:12px;border:0;font-size:15px;font-weight:700;cursor:pointer}
  .big-btn.on{background:linear-gradient(135deg,#22c55e,#16a34a);color:#fff}
  .big-btn.off{background:linear-gradient(135deg,#ef4444,#dc2626);color:#fff}
  select,input[type=range]{width:100%;background:#222742;border:1px solid #32375a;color:#fff;
                           border-radius:10px;padding:10px;font-size:14px}
  .sl-row{display:flex;gap:10px;align-items:center;margin-top:6px}
  .sl-row span{min-width:46px;text-align:right;color:#9ca3ff;font-size:13px;font-variant-numeric:tabular-nums}
  .footer{text-align:center;color:#565b7a;font-size:11px;margin-top:10px}
  .toast{position:fixed;top:14px;left:50%;transform:translateX(-50%);background:rgba(99,102,241,.95);
         color:#fff;padding:8px 18px;border-radius:20px;font-size:13px;opacity:0;transition:.3s;pointer-events:none;z-index:99}
  .toast.show{opacity:1}
</style>
</head>
<body>
<h1><span class="dot" id="st-dot"></span> EvilAppleJuice 控制中心</h1>

<!-- Status Card -->
<div class="card">
  <h2>◆ 运行状态</h2>
  <div class="row"><span class="label">AP IP</span><span class="value" id="s-ip">-</span></div>
  <div class="row"><span class="label">广播总开关</span><span class="value" id="s-brd">-</span></div>
  <div class="row"><span class="label">控制模式</span><span class="value" id="s-mode">-</span></div>
  <div class="row"><span class="label">当前设备</span><span class="value" id="s-dev">-</span></div>
  <div class="row"><span class="label">广播间隔</span><span class="value" id="s-delay">-</span></div>
  <div class="row"><span class="label">功率模式</span><span class="value" id="s-pwr">-</span></div>
  <div class="row"><span class="label">累计广播</span><span class="value" id="s-cnt">-</span></div>
</div>

<!-- Power Switch -->
<div class="card">
  <h2>◆ 广播开关</h2>
  <div class="big">
    <button class="big-btn on"  id="btn-on"  onclick="setBroadcast(1)">▶ 开启广播</button>
    <button class="big-btn off" id="btn-off" onclick="setBroadcast(0)">■ 停止广播</button>
  </div>
</div>

<!-- Mode Switch -->
<div class="card">
  <h2>◆ 快速模式（9 种预设，点此会关闭"指定设备"）</h2>
  <div class="grid9" id="mode-grid"></div>
</div>

<!-- Device Selection -->
<div class="card">
  <h2>◆ 直接选择设备（覆盖模式）</h2>
  <select id="dev-select" onchange="applyDevice()"></select>
  <div style="margin-top:10px;display:flex;gap:8px">
    <button class="big-btn" style="flex:1;background:#3b82f6;color:#fff;font-size:13px;padding:10px" onclick="applyDevice()">✓ 应用指定设备</button>
    <button class="big-btn" style="flex:1;background:#64748b;color:#fff;font-size:13px;padding:10px" onclick="revertMode()">↩ 恢复模式控制</button>
  </div>
</div>

<!-- Settings -->
<div class="card">
  <h2>◆ 参数设置</h2>
  <div class="row"><span class="label">广播间隔</span></div>
  <div class="sl-row">
    <input type="range" id="r-delay" min="20" max="2000" step="10" value="100" oninput="document.getElementById('v-delay').textContent=this.value+' ms'">
    <span id="v-delay">100 ms</span>
  </div>
  <div class="row" style="margin-top:14px"><span class="label">发射功率</span></div>
  <div class="sl-row">
    <input type="range" id="r-pwr" min="0" max="5" step="1" value="0" oninput="document.getElementById('v-pwr').textContent=pwrLabels[+this.value]">
    <span id="v-pwr">动态随机</span>
  </div>
  <div style="margin-top:14px">
    <button class="big-btn" style="width:100%;background:linear-gradient(135deg,#6366f1,#8b5cf6);color:#fff" onclick="applySettings()">✓ 保存设置</button>
  </div>
</div>

<!-- WiFi STA Configuration (software scan, not phone system scan) -->
<div class="card">
  <h2>◆ WiFi 网络（软件扫描，连接到家庭 WiFi）</h2>
  <div class="row">
    <span class="label">STA 状态</span>
    <span class="value" id="s-sta">-</span>
  </div>
  <div style="margin-top:10px;display:flex;gap:8px">
    <button class="big-btn" style="flex:1;background:#0ea5e9;color:#fff;font-size:13px;padding:10px" onclick="scanWifi()">🔍 扫描附近网络</button>
  </div>
  <div id="wifi-list" style="margin-top:10px;max-height:200px;overflow-y:auto"></div>
  <div style="margin-top:10px">
    <input type="text" id="wifi-ssid" placeholder="WiFi 名称 (SSID)" style="width:100%;background:#222742;border:1px solid #32375a;color:#fff;border-radius:10px;padding:10px;font-size:14px;margin-bottom:8px">
    <input type="password" id="wifi-pass" placeholder="密码（无密码留空）" style="width:100%;background:#222742;border:1px solid #32375a;color:#fff;border-radius:10px;padding:10px;font-size:14px;margin-bottom:8px">
    <button class="big-btn" style="width:100%;background:linear-gradient(135deg,#0ea5e9,#06b6d4);color:#fff" onclick="connectWifi()">✓ 连接此网络</button>
  </div>
</div>

<div class="footer">EvilAppleJuice-ESP32 · WiFi 控制版<br>AP SSID: <b>EAJ-Control</b>（公开可见，可直接扫描连接）</div>
<div class="toast" id="toast"></div>

<script>
const MODE_LABELS = [
  "0 双灯灭 [Audio] AirPods",
  "1 右闪 [随机] 任意设备",
  "2 右亮 [Audio] 软件更新",
  "3 左闪 [Audio] AirPods Gen2",
  "4 双闪 [Setup] Vision Pro",
  "5 左闪右亮 [Audio] AirPods Max",
  "6 左亮 [Setup] AppleTV Setup",
  "7 左亮右闪 [Setup] 号码转移",
  "8 双灯亮 [Setup] AppleTV Pair"
];
const PWR_LABELS  = ["动态随机","最大 MAX","MAX-1","MAX-2","MAX-3","MAX-4"];
const pwrLabels = PWR_LABELS;
let devList = [];

function toast(msg){
  const t=document.getElementById('toast');t.textContent=msg;t.classList.add('show');
  setTimeout(()=>t.classList.remove('show'),1500);
}
function qs(id){return document.getElementById(id)}

function buildUI(){
  const grid=qs('mode-grid');
  MODE_LABELS.forEach((lbl,i)=>{
    const b=document.createElement('button');
    b.className='mode-btn';b.textContent=lbl;b.onclick=()=>setMode(i);b.id='m'+i;
    grid.appendChild(b);
  });
  const sel=qs('dev-select');
  devList.forEach((d,i)=>{
    const o=document.createElement('option');o.value=i;o.text=`${i}. ${d.name}  [${d.type==0?'Audio':'Setup'} ID=0x${d.modelId.toString(16)}]`;
    sel.appendChild(o);
  });
}

async function fetchJSON(url,opt){
  const r=await fetch(url,opt);return r.json();
}
async function refresh(){
  try{
    const s=await fetchJSON('/api/status');
    qs('s-ip').textContent=s.ip;
    qs('s-brd').textContent=s.broadcast?'✅ 广播中':'⏸ 已停止';
    qs('s-brd').style.color=s.broadcast?'#4ade80':'#fbbf24';
    qs('st-dot').style.background=s.broadcast?'#4ade80':'#6b7280';
    qs('st-dot').style.boxShadow=s.broadcast?'0 0 10px #4ade80':'none';
    if(s.useManual){
      qs('s-mode').textContent='指定设备模式';
      qs('s-mode').style.color='#facc15';
    }else{
      qs('s-mode').textContent='预设模式 '+s.mode+'  ('+MODE_LABELS[s.mode].slice(2)+')';
      qs('s-mode').style.color='#93c5fd';
    }
    qs('s-dev').textContent=s.lastDev;
    qs('s-delay').textContent=s.delay+' ms';
    qs('s-pwr').textContent=PWR_LABELS[s.pwr] || s.pwr;
    qs('s-cnt').textContent=s.count;
    // highlight
    document.querySelectorAll('.mode-btn').forEach(b=>b.classList.remove('active'));
    if(!s.useManual){ const b=qs('m'+s.mode); if(b) b.classList.add('active'); }
    // sync controls
    qs('r-delay').value=s.delay; qs('v-delay').textContent=s.delay+' ms';
    qs('r-pwr').value=s.pwr;   qs('v-pwr').textContent=PWR_LABELS[s.pwr];
    qs('dev-select').value=s.useManual?s.manualIdx:0;
    // STA status
    const staEl = qs('s-sta');
    if(s.staConnected){
      staEl.textContent='✅ 已连接 '+s.staSSID+' ('+s.staIp+')';
      staEl.style.color='#4ade80';
    }else if(s.staSSID){
      staEl.textContent='⏳ 连接中: '+s.staSSID;
      staEl.style.color='#fbbf24';
    }else{
      staEl.textContent='未连接家庭 WiFi';
      staEl.style.color='#94a3b8';
    }
  }catch(e){}
}
async function setMode(m){
  await fetch('/api/mode?m='+m);toast('已切到模式 '+m);refresh();
}
async function applyDevice(){
  const i=+qs('dev-select').value;
  await fetch('/api/device?d='+i);toast('已指定设备：'+devList[i].name);refresh();
}
async function revertMode(){
  await fetch('/api/device?d=-1');toast('已恢复模式控制');refresh();
}
async function setBroadcast(on){
  await fetch('/api/broadcast?on='+on);toast(on?'广播已开启':'广播已停止');refresh();
}
async function applySettings(){
  const d=+qs('r-delay').value, p=+qs('r-pwr').value;
  await fetch('/api/settings?delay='+d+'&pwr='+p);toast('设置已保存');refresh();
}
async function scanWifi(){
  toast('正在扫描...');
  const list=await fetchJSON('/api/wifi/scan');
  const box=qs('wifi-list');
  if(!list||!list.length){ box.innerHTML='<div style="color:#94a3b8;font-size:12px;padding:8px">未发现网络</div>'; return; }
  box.innerHTML=list.map(n=>{
    const pct=Math.max(0,Math.min(100,2*(n.rssi+100)));
    const enc=n.enc===0?'🔓':'🔒';
    return `<div style="display:flex;align-items:center;gap:8px;padding:6px 8px;background:#161a2e;border-radius:8px;margin-bottom:4px;cursor:pointer" onclick="pickWifi('${n.ssid.replace(/'/g,"\\'")}')">
      <span style="flex:1;font-size:13px">${enc} ${n.ssid||'(隐藏)'}</span>
      <span style="font-size:11px;color:#94a3b8">${n.rssi}dBm · CH${n.ch}</span>
    </div>`;
  }).join('');
  toast('发现 '+list.length+' 个网络');
}
function pickWifi(ssid){ qs('wifi-ssid').value=ssid; toast('已选择: '+ssid); }
async function connectWifi(){
  const ssid=qs('wifi-ssid').value.trim();
  const pass=qs('wifi-pass').value;
  if(!ssid){ toast('请输入 WiFi 名称'); return; }
  toast('连接中...');
  const r=await fetchJSON('/api/wifi/connect?ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass));
  if(r.ok){ toast('✅ 已连接: '+r.ip); }else{ toast('❌ 连接失败 (status:'+r.status+')'); }
  refresh();
}
async function init(){
  devList=await fetchJSON('/api/devices');
  buildUI();
  refresh();
  setInterval(refresh, 1500);
}
init();
</script>
</body>
</html>
)=====";

// ---  JSON escape helper for device list  ---
String jsonEscape(const char* s){
  String out="\"";
  for(;*s;s++){
    if(*s=='"'||*s=='\\') out+='\\';
    out+=*s;
  }
  out+="\"";
  return out;
}

// --------  Route Handlers  --------
void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", CONTROL_HTML);
}

void handleApiStatus() {
  IPAddress ip = WiFi.softAPIP();
  bool staConnected = (WiFi.status() == WL_CONNECTED);
  String json = "{";
  json += "\"ip\":\""              + ip.toString()                                   + "\",";
  json += "\"staIp\":\""           + (staConnected ? WiFi.localIP().toString() : "") + "\",";
  json += "\"staConnected\":"      + String(staConnected ? "true":"false")           + ",";
  json += "\"staSSID\":"           + jsonEscape(staSSID.c_str())                     + ",";
  json += "\"broadcast\":"         + String(broadcastEnabled ? "true":"false")       + ",";
  json += "\"mode\":"              + String(currentMode)                              + ",";
  json += "\"useManual\":"         + String(useManualDevice ? "true":"false")         + ",";
  json += "\"manualIdx\":"         + String(manualDeviceIndex)                        + ",";
  json += "\"lastDev\":"           + jsonEscape(lastDeviceName.c_str())               + ",";
  json += "\"delay\":"             + String((unsigned long)delayMilliseconds)         + ",";
  json += "\"pwr\":"               + String(powerMode)                                + ",";
  json += "\"count\":"             + String(totalBroadcastCount)                      + "";
  json += "}";
  server.send(200, "application/json", json);
}

void handleApiDevices() {
  String json = "[";
  const int N = NUM_DEVICES;
  for (int i=0;i<N;i++){
    const AppleDevice& d = ALL_DEVICES[i];
    json += "{";
    json += "\"name\":"  + jsonEscape(d.name)     + ",";
    json += "\"modelId\":"+ String(d.modelId)     + ",";
    json += "\"type\":"   + String((int)d.type);
    json += "}";
    if(i<N-1) json += ",";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleApiMode() {
  if (!server.hasArg("m")) { server.send(400,"application/json","{\"err\":\"missing m\"}"); return; }
  int m = server.arg("m").toInt();
  int totalModes = sizeof(stateTable)/sizeof(stateTable[0]);
  if (m<0 || m>=totalModes) { server.send(400,"application/json","{\"err\":\"invalid m\"}"); return; }
  currentMode = m;
  useManualDevice = false;
  saveAllPreferences();
  server.send(200,"application/json","{\"ok\":true}");
}

void handleApiDevice() {
  if (!server.hasArg("d")) { server.send(400,"application/json","{\"err\":\"missing d\"}"); return; }
  long d = server.arg("d").toInt();
  if (d == -1) {
    // revert to mode-driven
    useManualDevice = false;
  } else if (d>=0 && d<NUM_DEVICES) {
    useManualDevice   = true;
    manualDeviceIndex = (int)d;
  } else {
    server.send(400,"application/json","{\"err\":\"invalid d\"}");
    return;
  }
  saveAllPreferences();
  server.send(200,"application/json","{\"ok\":true}");
}

void handleApiBroadcast() {
  if (!server.hasArg("on")) { server.send(400,"application/json","{\"err\":\"missing on\"}"); return; }
  int on = server.arg("on").toInt();
  broadcastEnabled = (on != 0);
  saveAllPreferences();
  server.send(200,"application/json","{\"ok\":true}");
}

void handleApiSettings() {
  bool changed = false;
  if (server.hasArg("delay")) {
    uint32_t d = server.arg("delay").toInt();
    if (d>=20 && d<=5000) { delayMilliseconds = d; changed=true; }
  }
  if (server.hasArg("pwr")) {
    int p = server.arg("pwr").toInt();
    if (p>=0 && p<=5) { powerMode = p; changed=true; }
  }
  if (changed) saveAllPreferences();
  server.send(200,"application/json",changed?"{\"ok\":true}":"{\"ok\":true,\"note\":\"no change\"}");
}

// --------  WiFi Scan + STA connect  --------
void handleApiWifiScan() {
  // Software scan (not the phone's system WiFi scan): ESP32 scans nearby APs.
  int n = WiFi.scanNetworks(false, true, 200, WIFI_AP_CHANNEL);
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    json += "{";
    json += "\"ssid\":"    + jsonEscape(WiFi.SSID(i).c_str()) + ",";
    json += "\"rssi\":"    + String(WiFi.RSSI(i))             + ",";
    json += "\"enc\":"     + String((int)WiFi.encryptionType(i)) + ",";
    json += "\"ch\":"      + String(WiFi.channel(i));
    json += "}";
  }
  json += "]";
  WiFi.scanDelete();
  server.send(200, "application/json", json);
}

void handleApiWifiConnect() {
  if (!server.hasArg("ssid")) { server.send(400,"application/json","{\"err\":\"missing ssid\"}"); return; }
  String ssid = server.arg("ssid");
  String pass = server.hasArg("pass") ? server.arg("pass") : "";
  staSSID     = ssid;
  staPassword = pass;
  staEnabled  = true;
  saveAllPreferences();

  // Switch to AP+STA mode and connect to the chosen network
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(staSSID.c_str(), staPassword.c_str());
  // Give it a short while; don't block the HTTP response too long
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
    delay(200);
  }
  bool ok = (WiFi.status() == WL_CONNECTED);
  String json = "{\"ok\":";
  json += ok ? "true" : "false";
  json += ",\"ip\":\"" + (ok ? WiFi.localIP().toString() : String("")) + "\"";
  json += ",\"status\":" + String((int)WiFi.status()) + "}";
  server.send(200, "application/json", json);
}

void handleApiWifiStatus() {
  bool connected = (WiFi.status() == WL_CONNECTED);
  String json = "{";
  json += "\"staEnabled\":" + String(staEnabled ? "true":"false") + ",";
  json += "\"connected\":"  + String(connected ? "true":"false")  + ",";
  json += "\"ssid\":"       + jsonEscape(staSSID.c_str())          + ",";
  json += "\"ip\":\""       + (connected ? WiFi.localIP().toString() : "") + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

// ============================================================
// setup()
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("Starting ESP32 BLE (WiFi Control Edition)");

  // ----- Load prefs -----
  loadAllPreferences();
  Serial.printf("Loaded -> mode:%d manual:%d/%d brd:%d delay:%lu pwr:%d\n",
                currentMode, useManualDevice?1:0, manualDeviceIndex,
                broadcastEnabled?1:0, (unsigned long)delayMilliseconds, powerMode);

  // ----- Pins -----
  pinMode(RIGHT_LED, OUTPUT);
  pinMode(LEFT_LED, OUTPUT);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

  // ----- WiFi Hidden AP -----
  Serial.printf("Starting WiFi AP hidden SSID: %s\n", WIFI_AP_SSID);
  WiFi.mode(WIFI_AP);
  // 5th param ssid_hidden = true -> 隐藏 SSID
  bool apOK = WiFi.softAP(WIFI_AP_SSID,
                          (strlen(WIFI_AP_PASSWORD)>0)?WIFI_AP_PASSWORD:NULL,
                          WIFI_AP_CHANNEL,
                          WIFI_AP_HIDDEN,
                          WIFI_MAX_CONN);
  Serial.printf("AP status: %s, IP=%s\n", apOK?"OK":"FAILED", WiFi.softAPIP().toString().c_str());

  // ----- STA auto-connect (if configured) -----
  if (staEnabled && staSSID.length() > 0) {
    Serial.printf("STA auto-connect to: %s\n", staSSID.c_str());
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(staSSID.c_str(), staPassword.c_str());
  }

  // ----- BLE -----
  BLEDevice::init("AirPods 69");
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, MAX_TX_POWER);
  BLEServer *pServer = BLEDevice::createServer();
  pAdvertising = pServer->getAdvertising();

  // Note: MAC is randomized per burst in loop() via setDeviceAddress (Bluedroid)
  // or BLEDevice::setOwnAddr + setOwnAddrType(RANDOM) (NimBLE).

  // ----- Web Routes -----
  // 注意：使用双参数形式 (不含 HTTP_GET)，兼容带/不带 query string 的各种 WebServer 版本
  server.on("/",                  handleRoot);
  server.on("/api/status",        handleApiStatus);
  server.on("/api/devices",       handleApiDevices);
  server.on("/api/mode",          handleApiMode);
  server.on("/api/device",        handleApiDevice);
  server.on("/api/broadcast",     handleApiBroadcast);
  server.on("/api/settings",      handleApiSettings);
  server.on("/api/wifi/scan",     handleApiWifiScan);
  server.on("/api/wifi/connect",  handleApiWifiConnect);
  server.on("/api/wifi/status",   handleApiWifiStatus);

  // onNotFound：先打印日志（方便排查404），再对去掉 query 的纯路径做一次兜底重匹配
  server.onNotFound([](){
    String uri = server.uri();
    Serial.printf("HTTP 404 -> uri=%s method=%d\n", uri.c_str(), (int)server.method());
    // 去掉 query string (取 ? 之前部分)
    int q = uri.indexOf('?');
    String purePath = (q<0) ? uri : uri.substring(0, q);
    if      (purePath == "/")                  { handleRoot();          return; }
    else if (purePath == "/api/status")        { handleApiStatus();     return; }
    else if (purePath == "/api/devices")       { handleApiDevices();    return; }
    else if (purePath == "/api/mode")          { handleApiMode();       return; }
    else if (purePath == "/api/device")        { handleApiDevice();     return; }
    else if (purePath == "/api/broadcast")     { handleApiBroadcast();  return; }
    else if (purePath == "/api/settings")      { handleApiSettings();   return; }
    else if (purePath == "/api/wifi/scan")     { handleApiWifiScan();    return; }
    else if (purePath == "/api/wifi/connect")  { handleApiWifiConnect(); return; }
    else if (purePath == "/api/wifi/status")   { handleApiWifiStatus();  return; }
    server.send(404, "text/plain", "404 Not Found: " + uri);
  });
  server.begin();
  Serial.println("HTTP server started on :80 (routes use loose matching + onNotFound fallback)");
}

// ============================================================
// loop()
// ============================================================
void loop() {
  // ---- Handle HTTP as often as possible ----
  server.handleClient();

  // ---- LED indicator (unified: ON / OFF / FLASH are all distinct) ----
  updateLEDs();

  // ---- BOOT button (still works!) ----
  if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
    unsigned long startTime = millis();
    while(digitalRead(BOOT_BUTTON_PIN) == LOW) { server.handleClient(); delay(5); }

    unsigned long pressDuration = millis() - startTime;
    if (pressDuration > LONG_PRESS_TIME) {
      Serial.println("BOOT button long pressed -> resetMode()");
      resetMode();
    } else {
      Serial.println("BOOT button short pressed -> nextMode()");
      nextMode();
    }
  }

  // ---- Master switch ----
  if (!broadcastEnabled) {
    // Idle: short delay then re-loop so HTTP stays responsive + LEDs keep flashing
    updateLEDs();
    delay(20);
    return;
  }

  // ---- Resolve device to broadcast ----
  AppleDevice dev = getCurrentDevice();

  // ---- Randomize MAC (apply via setDeviceAddress so Apple devices see a new addr each burst) ----
  esp_bd_addr_t dummy_addr = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  for (int i = 0; i < 6; i++){
    dummy_addr[i] = random(256);
    if (i == 0){ dummy_addr[i] |= 0xF0; }  // random non-resolvable / random static address range
  }
  // Bluedroid path: setDeviceAddress() 同时写入随机地址并把广播 own_addr_type 置为 RANDOM。
  // 参数避免引用易变的 BLE_ADDR_TYPE_RANDOM 宏，直接使用其数值 0x01。
  // NimBLE 路径: 只改 controller 随机地址（NimBLE 会在广播时使用它）。
#if defined(CONFIG_BLUEDROID_ENABLED)
  pAdvertising->setDeviceAddress(dummy_addr, (esp_ble_addr_type_t)0x01);
#else
  // NimBLE 路径：广播前先确保 own addr type 为 RANDOM(1)（setOwnAddrType 需 core >= 3.3.0），
  // 再写入本轮随机地址；低版本核心跳过 type 设置，仅改地址。
  #if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 0)
    BLEDevice::setOwnAddrType(1); // 1 = BLE_OWN_ADDR_RANDOM
  #endif
  BLEDevice::setOwnAddr(dummy_addr);
#endif

  // ---- Build advertisement ----
  BLEAdvertisementData oAdvertisementData = BLEAdvertisementData();
  setAdvertisementData(oAdvertisementData, dev);

  // ---- PDU type: ADV_IND only (connectable undirected) ----
  // Apple pairing popups only respond to connectable undirected advertisements.
  // IMPORTANT: the value used below is the esp_ble_adv_type_t enum, NOT the raw
  // "Bluetooth spec PDU type" bitfield. Correct mapping:
  //   0x00 = ADV_IND            (connectable undirected)  <-- the one Apple answers
  //   0x01 = ADV_DIRECT_IND_HIGH (directed to ONE target peer) <-- almost never triggers
  //   0x02 = ADV_SCAN_IND / 0x03 = ADV_NONCONN_IND        (unconnectable -> no popup)
  // A previous edit mistakenly set 0x01 ("ADV_IND"), which is actually DIRECT_IND,
  // so the phone treated it as directed advertising and never showed any popup.
  pAdvertising->setAdvertisementType(0x00); // 0x00 = ADV_IND

  pAdvertising->setAdvertisementData(oAdvertisementData);

  // 20ms 固定间隔选项 (默认禁用，按需启用)
  //pAdvertising->setMinInterval(0x20);
  //pAdvertising->setMaxInterval(0x20);
  //pAdvertising->setMinPreferred(0x20);
  //pAdvertising->setMaxPreferred(0x20);

  // ---- Advertise burst ----
  pAdvertising->start();

  // Non-blocking-ish delay (keep polling HTTP + keep LEDs flashing)
  uint32_t t0 = millis();
  while (millis() - t0 < delayMilliseconds) {
    server.handleClient();
    updateLEDs();
    delay(4);
  }
  pAdvertising->stop();

  // ---- Apply tx power (dynamic or fixed, per powerMode) ----
  applyPower();
}
