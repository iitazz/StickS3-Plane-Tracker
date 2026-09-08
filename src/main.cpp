#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>

#include "config.h"
#include "hal.h"

struct Plane {
  String icao24;
  String callsign;
  String route;
  float latitude;
  float longitude;
  float altitudeMeters;
  float speedKmh;
  float heading;
  float distanceKm;
};

struct TrackerSettings {
  String airport;
  float latitude;
  float longitude;
  float rangeKm;
  unsigned long refreshIntervalMs;
  bool autoRotate;
  String openSkyClientId;
  String openSkyClientSecret;
};

String cachedOAuthToken = "";
unsigned long oauthTokenExpiresAt = 0;

enum AppMode { MODE_MENU, MODE_TRACKER, MODE_FIRMWARE_UPDATE };

Plane planes[MAX_PLANES];
size_t planeCount = 0;
size_t selectedPlane = 0;
String selectedPlaneIcao24 = "";
unsigned long lastRefresh = 0;
String statusText = "STARTING";
String lastContentType = "";
String lastJsonError = "";
String lastResponsePreview = "";
int lastHttpCode = 0;
float sweepAngle = 0.0f;
unsigned long ipOverlayUntil = 0;
bool ipOverlayDrawn = false;
WebServer webServer(80);
TrackerSettings settings;
unsigned long lastDraw = 0;
unsigned long lastImuCheck = 0;
uint8_t displayRotation = 1;
AppMode appMode = MODE_MENU;
uint8_t menuSelection = 0;
bool exitComboHandled = false;
bool debugServerStarted = false;
bool debugRoutesConfigured = false;
bool provisioningMode = false;
bool updateFailed = false;
size_t updateBytesWritten = 0;
String updateFailure = "";
String lastUpdateResult = "idle";
bool updateInProgress = false;
bool updateReady = false;
bool updateScreenError = false;
bool updateScreenNeedsRedraw = true;
size_t lastUpdateScreenBytes = 0;
unsigned long updateButtonIgnoreUntil = 0;
bool menuNeedsRedraw = true;
bool webUiNeedsRedraw = true;
bool trackerPaused = false;
uint8_t latestUpdateBuffer[4096];

// Off-screen framebuffer used to draw the radar without visible flicker.
// All primitives are rendered into the sprite, then pushed in one transfer.
halCanvas radarCanvas(&halDisplay);
bool radarCanvasReady = false;
int radarCanvasWidth = 0;
int radarCanvasHeight = 0;
int radarCanvasRotation = -1;

void fetchPlanes();
void connectWifi();
void exitToMenu();
void drawFirmwareUpdate();

constexpr char ROUTE_URL_PREFIX[] = "https://api.adsbdb.com/v0/callsign/";
constexpr char GITHUB_LATEST_RELEASE_URL[] = "https://api.github.com/repos/iitazz/StickS3-Plane-Tracker/releases/latest";
constexpr char SETUP_AP_NAME[] = "PlaneTracker-Setup";
constexpr char SETUP_AP_PASSWORD[] = "planeconfig";
constexpr char FIRMWARE_VERSION[] = "1.2.0";
constexpr uint8_t LATEST_UPDATE_MAX_ATTEMPTS = 4;
constexpr int LATEST_UPDATE_TIMEOUT_MS = 30000;
constexpr unsigned long LATEST_UPDATE_RETRY_DELAY_MS = 1500;

constexpr char DEBUG_PAGE[] = R"rawliteral(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Plane Tracker</title><style>
body{font:16px monospace;background:#101512;color:#d7ffe0;max-width:900px;margin:24px auto;padding:0 16px}
h1{color:#50e0ff;font-size:22px}section{border:1px solid #276b3c;padding:14px;margin:12px 0;white-space:pre-wrap;overflow-wrap:anywhere}
dt{color:#8cff9b;margin-top:10px}dd{margin:3px 0 0 0;color:#fff}button{background:#194d2a;color:#fff;border:1px solid #56d878;padding:9px 14px;margin-top:12px;cursor:pointer}input{background:#17221a;color:#fff;border:1px solid #4c9d61;padding:8px;width:130px}label{display:inline-block;margin:7px 12px 7px 0}
.info-box{font-size:12px;color:#8cff9b;background:#14291c;border-left:3px solid #50e0ff;padding:10px 14px;margin:12px 0;line-height:1.5}
.btn-danger{background:#5a1e1e!important;border:1px solid #e04444!important;color:#ffcaca!important}
</style></head><body><h1>Plane Tracker <small id="firmwareVersion"></small></h1>
<section><form action="/api/config" method="post">
<label>Airport preset <select id="airportPreset"><option value="">Manual coordinates</option><option value="FCO" data-lat="41.8003" data-lon="12.2389">FCO - Rome</option><option value="LHR" data-lat="51.4700" data-lon="-0.4543">LHR - London</option><option value="CDG" data-lat="49.0097" data-lon="2.5479">CDG - Paris</option><option value="AMS" data-lat="52.3086" data-lon="4.7639">AMS - Amsterdam</option><option value="FRA" data-lat="50.0379" data-lon="8.5622">FRA - Frankfurt</option><option value="MAD" data-lat="40.4983" data-lon="-3.5676">MAD - Madrid</option><option value="JFK" data-lat="40.6413" data-lon="-73.7781">JFK - New York</option><option value="LAX" data-lat="33.9416" data-lon="-118.4085">LAX - Los Angeles</option><option value="ORD" data-lat="41.9742" data-lon="-87.9073">ORD - Chicago</option><option value="DXB" data-lat="25.2532" data-lon="55.3657">DXB - Dubai</option><option value="HND" data-lat="35.5494" data-lon="139.7798">HND - Tokyo</option><option value="SIN" data-lat="1.3644" data-lon="103.9915">SIN - Singapore</option></select></label><label>Airport <input name="airport" maxlength="12"></label><label>Latitude <input name="latitude" type="number" step="0.0001"></label><label>Longitude <input name="longitude" type="number" step="0.0001"></label><label>Radar range km <input name="range" type="number" min="5" max="500" step="1"></label><label>Refresh sec <input name="refresh" type="number" min="10" max="3600" step="1"></label><label>Auto rotate <input name="autorotate" type="checkbox"></label><br><label>OpenSky Client ID <input name="opensky_id" maxlength="64" placeholder="Optional"></label><label>OpenSky Client Secret <input name="opensky_sec" type="password" maxlength="64" placeholder="Optional"></label> <button type="button" id="clearOpenSkyBtn" class="btn-danger" onclick="clearOpenSky()" style="display:none">Remove Saved Credentials</button><div class="info-box"><b>OpenSky API Credentials (Optional):</b><br>Providing credentials is <b>completely optional</b>. By default, the device uses the public OpenSky API in anonymous mode (400 requests/day limit).<br><br>If you want higher rate limits (up to <b>4,000 requests/day</b> for more frequent updates), create a free account and generate an API Client (<b>Client ID</b> and <b>Client Secret</b>) under your settings as specified in the <a href="https://openskynetwork.github.io/opensky-api/rest.html#authentication" target="_blank" style="color:#50e0ff">OpenSky API Documentation</a>.<br><br><b>On-Device Encryption:</b> Stored credentials are <b>hardware-encrypted</b> on device NVS storage using unique ESP32 silicon eFuse MAC keys. They are <b>never pre-filled</b> in the Web UI, never exposed in status API responses, and cannot be extracted from flash memory.</div><br><button type="submit">Save and refresh</button></form></section>
<section><button onclick="load()">Refresh diagnostics</button><dl id="data">Loading...</dl></section>
<section><h2>Wi-Fi setup</h2><form action="/api/wifi" method="post"><label>Found networks <select id="wifiNetworks"><option value="">Scan for networks</option></select></label><button id="wifiScanButton" type="button" onclick="scanWifi()">Scan networks</button> <span id="wifiScanStatus"></span><br><label>Network <input id="wifiSsid" name="ssid" maxlength="32" required></label><label>Password <input name="password" type="password" maxlength="64"></label><br><button type="submit">Save Wi-Fi and reboot</button></form></section>
<section><h2>Firmware update</h2><form id="firmwareForm" action="/api/update" method="post" enctype="multipart/form-data"><input name="firmware" type="file" accept=".bin,application/octet-stream" required><br><button id="firmwareButton" type="submit">Upload firmware and install</button> <span id="firmwareStatus"></span></form><button id="latestFirmwareButton" type="button">Install latest GitHub release</button> <span id="latestFirmwareStatus"></span></section>
<script>
const firmwareForm=document.querySelector("#firmwareForm");
firmwareForm.addEventListener("submit",async event=>{
  event.preventDefault();
  const button=document.querySelector("#firmwareButton");
  const status=document.querySelector("#firmwareStatus");
  button.disabled=true;
  button.textContent="Uploading...";
  status.textContent="Uploading firmware; do not disconnect";
  try{
    const response=await fetch(firmwareForm.action,{method:"POST",body:new FormData(firmwareForm)});
    const message=await response.text();
    if(!response.ok)throw new Error(message);
    status.textContent="Update ready. Press BLUE on the device to reboot."
  }catch(error){
    status.textContent=error.message.startsWith("Firmware update failed")?error.message:"Upload connection lost; check the device screen"
  }finally{
    button.disabled=false;
    button.textContent="Upload firmware and install"
  }
});
document.querySelector("#latestFirmwareButton").addEventListener("click",async()=>{
  const button=document.querySelector("#latestFirmwareButton");
  const status=document.querySelector("#latestFirmwareStatus");
  button.disabled=true;
  status.textContent="Downloading latest GitHub release (retries automatically; can take a few minutes); do not disconnect";
  try{
    const response=await fetch("/api/update-latest",{method:"POST"});
    const message=await response.text();
    if(!response.ok)throw new Error(message);
    status.textContent=message
  }catch(error){
    status.textContent=error.message
  }finally{
    button.disabled=false
  }
});
async function scanWifi(){
  const select=document.querySelector("#wifiNetworks");
  const button=document.querySelector("#wifiScanButton");
  const status=document.querySelector("#wifiScanStatus");
  button.disabled=true;
  button.textContent="Scanning...";
  status.textContent="Scanning nearby networks";
  select.innerHTML='<option value="">Scanning...</option>';
  try{
    const r=await fetch("/api/wifi/scan");
    if(!r.ok)throw new Error(await r.text());
    const networks=await r.json();
    select.innerHTML='<option value="">Select a network</option>';
    for(const network of networks){
      const option=document.createElement("option");
      option.value=network.ssid;
      option.textContent=network.ssid+" ("+network.rssi+" dBm)";
      select.appendChild(option)
    }
    if(!networks.length){
      select.innerHTML='<option value="">No networks found</option>';
      status.textContent="Scan finished: no networks found"
    }else status.textContent="Scan finished: "+networks.length+" network"+(networks.length===1?"":"s")
  }catch(error){
    select.innerHTML='<option value="">Scan failed</option>';
    status.textContent="Scan failed";
    alert(error.message)
  }finally{
    button.disabled=false;
    button.textContent="Scan networks"
  }
}
document.querySelector("#wifiNetworks").addEventListener("change",event=>{
  if(event.target.value)document.querySelector("#wifiSsid").value=event.target.value
});
let configDirty=false;
const configForm=document.querySelector('form[action="/api/config"]');
const airportPreset=document.querySelector("#airportPreset");
configForm.addEventListener("input",()=>configDirty=true);
airportPreset.addEventListener("change",()=>{
  const option=airportPreset.selectedOptions[0];
  if(option.value){
    document.querySelector("[name=airport]").value=option.value;
    document.querySelector("[name=latitude]").value=option.dataset.lat;
    document.querySelector("[name=longitude]").value=option.dataset.lon
  }
  configDirty=true
});
async function clearOpenSky(){
  if(!confirm("Remove saved OpenSky credentials and revert to the Public API?")) return;
  const body=new URLSearchParams();
  body.append("clear_opensky","1");
  body.append("airport",document.querySelector('[name=airport]').value||"FCO");
  body.append("latitude",document.querySelector('[name=latitude]').value||"41.9028");
  body.append("longitude",document.querySelector('[name=longitude]').value||"12.4964");
  body.append("range",document.querySelector('[name=range]').value||"65");
  body.append("refresh",document.querySelector('[name=refresh]').value||"30");
  if(document.querySelector('[name=autorotate]').checked) body.append("autorotate","on");
  const r=await fetch('/api/config',{method:'POST',body:body});
  if(r.ok){
    alert("OpenSky credentials removed! Reverted to Public API.");
    configDirty=false;
    load();
  }else{
    alert("Failed to remove credentials.");
  }
}
async function load(){
  const r=await fetch("/api/status");
  const d=await r.json();
  document.querySelector("#firmwareVersion").textContent="FW v"+d.firmwareVersion;
  if(!configDirty){
    for(const k of ["airport","latitude","longitude","rangeKm","refreshSeconds"]){
      const e=document.querySelector("[name="+({rangeKm:"range",refreshSeconds:"refresh"}[k]||k)+"]");
      if(e)e.value=d[k]||""
    }
    const userEl=document.querySelector("[name=opensky_id]");
    const secEl=document.querySelector("[name=opensky_sec]");
    const clearBtn=document.querySelector("#clearOpenSkyBtn");
    if(userEl&&secEl){
      userEl.value="";
      secEl.value="";
      if(d.hasOpenSkyAuth){
        userEl.placeholder="Saved & Encrypted on device";
        secEl.placeholder="Saved & Encrypted on device";
        if(clearBtn) clearBtn.style.display="inline-block";
      }else{
        userEl.placeholder="Optional";
        secEl.placeholder="Optional";
        if(clearBtn) clearBtn.style.display="none";
      }
    }
    const matchingPreset=[...airportPreset.options].find(option=>option.value===d.airport&&Math.abs(Number(option.dataset.lat)-Number(d.latitude))<0.0001&&Math.abs(Number(option.dataset.lon)-Number(d.longitude))<0.0001);
    airportPreset.value=matchingPreset?d.airport:"";
    document.querySelector("[name=autorotate]").checked=!!d.autoRotate
  }
  let out="";
  for(const [k,v] of Object.entries(d)){
    out+="<dt>"+k+"</dt><dd>"+String(v).replace(/&/g,"&amp;").replace(/</g,"&lt;")+"</dd>"
  }
  document.querySelector("#data").innerHTML=out;
}
load();
</script></body></html>
)rawliteral";

float toRadians(float degrees) { return degrees * PI / 180.0f; }

float distanceKm(float lat1, float lon1, float lat2, float lon2) {
  const float dLat = toRadians(lat2 - lat1);
  const float dLon = toRadians(lon2 - lon1);
  const float a = sinf(dLat / 2) * sinf(dLat / 2) +
                  cosf(toRadians(lat1)) * cosf(toRadians(lat2)) *
                      sinf(dLon / 2) * sinf(dLon / 2);
  return 6371.0088f * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
}

String encryptString(const String &input) {
  if (input.isEmpty()) return "";
  uint64_t chipId = ESP.getEfuseMac();
  uint8_t key[8];
  memcpy(key, &chipId, 8);
  String out = "";
  for (size_t i = 0; i < input.length(); i++) {
    uint8_t c = (uint8_t)input[i] ^ key[i % 8] ^ (uint8_t)(i * 31 + 0x5A);
    char buf[3];
    snprintf(buf, sizeof(buf), "%02X", c);
    out += buf;
  }
  return out;
}

String decryptString(const String &hexInput) {
  if (hexInput.isEmpty() || hexInput.length() % 2 != 0) return "";
  uint64_t chipId = ESP.getEfuseMac();
  uint8_t key[8];
  memcpy(key, &chipId, 8);
  String out = "";
  for (size_t i = 0; i < hexInput.length(); i += 2) {
    char hex[3] = { hexInput[i], hexInput[i+1], 0 };
    uint8_t c = (uint8_t)strtol(hex, NULL, 16);
    size_t pos = i / 2;
    char original = (char)(c ^ key[pos % 8] ^ (uint8_t)(pos * 31 + 0x5A));
    out += original;
  }
  return out;
}

String getOpenSkyOAuthToken() {
  if (settings.openSkyClientId.isEmpty() || settings.openSkyClientSecret.isEmpty()) {
    return "";
  }
  if (cachedOAuthToken.length() > 0 && millis() < oauthTokenExpiresAt) {
    return cachedOAuthToken;
  }

  Serial.println("[OpenSky] Requesting OAuth2 client credentials token...");
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, "https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token")) {
    Serial.println("[OpenSky] OAuth HTTP begin failed");
    return "";
  }

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  const String payload = "grant_type=client_credentials&client_id=" + settings.openSkyClientId +
                         "&client_secret=" + settings.openSkyClientSecret;
  const int code = http.POST(payload);

  if (code != HTTP_CODE_OK) {
    Serial.printf("[OpenSky] OAuth HTTP %d: %s\n", code, http.errorToString(code).c_str());
    http.end();
    return "";
  }

  const String response = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);
  if (error) {
    Serial.printf("[OpenSky] OAuth JSON parse error: %s\n", error.c_str());
    return "";
  }

  const char *token = doc["access_token"];
  int expiresIn = doc["expires_in"] | 1800;
  if (!token || strlen(token) == 0) {
    Serial.println("[OpenSky] OAuth response missing access_token");
    return "";
  }

  cachedOAuthToken = String(token);
  const unsigned long marginMs = (expiresIn > 60 ? (expiresIn - 30) : 30) * 1000UL;
  oauthTokenExpiresAt = millis() + marginMs;
  Serial.printf("[OpenSky] OAuth token acquired (expires in %d sec)\n", expiresIn);
  return cachedOAuthToken;
}

void loadSettings() {
  Preferences preferences;
  preferences.begin("plane-tracker", true);
  settings.airport = preferences.getString("airport", "FCO");
  if (settings.airport == "ROME") settings.airport = "FCO";
  settings.latitude = preferences.getFloat("latitude", RADAR_LAT);
  settings.longitude = preferences.getFloat("longitude", RADAR_LON);
  settings.rangeKm = preferences.getFloat("range", RADAR_RANGE_KM);
  settings.refreshIntervalMs = preferences.getULong("refresh", REFRESH_INTERVAL_MS);
  settings.autoRotate = preferences.getBool("autorotate", AUTO_ROTATE_DEFAULT);
  settings.openSkyClientId = preferences.getString("opensky_id", "");
  if (settings.openSkyClientId.isEmpty()) {
    settings.openSkyClientId = preferences.getString("opensky_user", "");
  }
  String encSec = preferences.getString("opensky_encsec", "");
  if (encSec.length() > 0) {
    settings.openSkyClientSecret = decryptString(encSec);
  } else {
    String oldEncPass = preferences.getString("opensky_enc", "");
    if (oldEncPass.length() > 0) {
      settings.openSkyClientSecret = decryptString(oldEncPass);
    } else {
      settings.openSkyClientSecret = preferences.getString("opensky_pass", "");
    }
  }
  lastUpdateResult = preferences.getString("lastUpdate", "idle");
  preferences.end();
}

void saveSettings() {
  Preferences preferences;
  preferences.begin("plane-tracker", false);
  preferences.putString("airport", settings.airport);
  preferences.putFloat("latitude", settings.latitude);
  preferences.putFloat("longitude", settings.longitude);
  preferences.putFloat("range", settings.rangeKm);
  preferences.putULong("refresh", settings.refreshIntervalMs);
  preferences.putBool("autorotate", settings.autoRotate);
  if (settings.openSkyClientId.length() > 0) {
    preferences.putString("opensky_id", settings.openSkyClientId);
  } else {
    preferences.remove("opensky_id");
  }
  if (settings.openSkyClientSecret.length() > 0) {
    preferences.putString("opensky_encsec", encryptString(settings.openSkyClientSecret));
  } else {
    preferences.remove("opensky_encsec");
  }
  preferences.remove("opensky_user");
  preferences.remove("opensky_pass");
  preferences.remove("opensky_enc");
  preferences.end();
}

bool usableWifiSsid(const String &ssid) {
  return ssid.length() > 0 && ssid != "your-wifi-name";
}

void loadWifiCredentials(String &ssid, String &password) {
  Preferences preferences;
  preferences.begin("wifi", true);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");
  preferences.end();
}

void saveWifiCredentials(const String &ssid, const String &password) {
  Preferences preferences;
  preferences.begin("wifi", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.end();
}

String openSkyUrl() {
  const float latitudeDelta = settings.rangeKm / 111.0f;
  const float longitudeDelta = settings.rangeKm / (111.0f * max(0.2f, cosf(toRadians(settings.latitude))));
  return "https://opensky-network.org/api/states/all?lamin=" + String(settings.latitude - latitudeDelta, 4) +
         "&lomin=" + String(settings.longitude - longitudeDelta, 4) +
         "&lamax=" + String(settings.latitude + latitudeDelta, 4) +
         "&lomax=" + String(settings.longitude + longitudeDelta, 4);
}

String scrollingText(const String &value, size_t width) {
  if (value.length() <= width) return value;
  const String padded = value + "   ";
  const size_t offset = (millis() / 250UL) % padded.length();
  const String cycle = padded + padded;
  return cycle.substring(offset, offset + width);
}

String planeDetailsTicker(const Plane &plane) {
  const String callsign = plane.callsign.length() ? plane.callsign : "UNKNOWN";
  const String altitude = plane.altitudeMeters < 0 ? "--" : String(plane.altitudeMeters, 0) + "m";
  return callsign + " | RTE " + plane.route + " | ALT " + altitude +
         " | DST " + String(plane.distanceKm, 1) + "km   ";
}

void drawBatteryIndicator(int right, int top) {
  const int battery = halGetBatteryLevel();
  const uint16_t color = battery <= 20 ? TFT_RED : battery <= 50 ? TFT_YELLOW : TFT_GREEN;
  const bool charging = halIsCharging();
  const int bodyX = right - 20;
  halDisplay.setTextFont(1);
  halDisplay.setTextSize(1);
  halDisplay.setTextColor(color, TFT_BLACK);
  halDisplay.drawString(String(battery) + "%", bodyX - 24, top);
  halDisplay.drawRect(bodyX, top, 16, 8, color);
  halDisplay.fillRect(bodyX + 16, top + 2, 2, 4, color);
  const int fillWidth = battery * 12 / 100;
  if (fillWidth > 0) halDisplay.fillRect(bodyX + 2, top + 2, fillWidth, 4, color);
  if (charging) {
    halDisplay.drawLine(bodyX + 7, top + 1, bodyX + 5, top + 4, TFT_WHITE);
    halDisplay.drawLine(bodyX + 5, top + 4, bodyX + 8, top + 4, TFT_WHITE);
    halDisplay.drawLine(bodyX + 8, top + 4, bodyX + 6, top + 7, TFT_WHITE);
  }
}

void updateAutoRotation() {
  if (!settings.autoRotate || millis() - lastImuCheck < 250) return;
  lastImuCheck = millis();

  float ax, ay, az, gx, gy, gz;
  if (!halReadImu(&ax, &ay, &az, &gx, &gy, &gz)) return;

  const float motion = fabsf(gx) + fabsf(gy) + fabsf(gz);
  if (motion < 15.0f) return;

  uint8_t nextRotation = displayRotation;
  const float landscapeAxis = fabsf(ax) > fabsf(ay) ? ax : ay;
  nextRotation = landscapeAxis > 0.0f ? 1 : 3;
  if (nextRotation != displayRotation) {
    displayRotation = nextRotation;
    halDisplay.setRotation(displayRotation);
    lastDraw = 0;
  }
}

String jsonEscape(const String &value) {
  String escaped = value;
  escaped.replace("\\", "\\\\");
  escaped.replace("\"", "\\\"");
  escaped.replace("\r", "\\r");
  escaped.replace("\n", "\\n");
  return escaped;
}

void handleDebugPage() {
  webServer.send(200, "text/html", DEBUG_PAGE);
}

void handleDebugStatus() {
  const esp_partition_t *runningPartition = esp_ota_get_running_partition();
  const esp_partition_t *bootPartition = esp_ota_get_boot_partition();
  String body = "{";
  body += "\"firmwareVersion\":\"" + String(FIRMWARE_VERSION) + "\",";
  body += "\"runningPartition\":\"" + String(runningPartition ? runningPartition->label : "unknown") + "\",";
  body += "\"bootPartition\":\"" + String(bootPartition ? bootPartition->label : "unknown") + "\",";
  body += "\"updateBytes\":" + String(updateBytesWritten) + ",";
  body += "\"updateResult\":\"" + jsonEscape(lastUpdateResult) + "\",";
  body += "\"airport\":\"" + jsonEscape(settings.airport) + "\",";
  body += "\"latitude\":" + String(settings.latitude, 4) + ",";
  body += "\"longitude\":" + String(settings.longitude, 4) + ",";
  body += "\"rangeKm\":" + String(settings.rangeKm, 1) + ",";
  body += "\"refreshSeconds\":" + String(settings.refreshIntervalMs / 1000UL) + ",";
  body += "\"autoRotate\":" + String(settings.autoRotate ? "true" : "false") + ",";
  body += "\"hasOpenSkyAuth\":" + String((settings.openSkyClientId.length() > 0 && settings.openSkyClientSecret.length() > 0) ? "true" : "false") + ",";
  body += "\"wifi\":\"" + jsonEscape(provisioningMode ? "setup-ap" : WiFi.status() == WL_CONNECTED ? "connected" : "disconnected") + "\",";
  body += "\"setupSsid\":\"" + jsonEscape(SETUP_AP_NAME) + "\",";
  body += "\"ip\":\"" + jsonEscape(provisioningMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\",";
  body += "\"status\":\"" + jsonEscape(statusText) + "\",";
  body += "\"httpCode\":" + String(lastHttpCode) + ",";
  body += "\"contentType\":\"" + jsonEscape(lastContentType) + "\",";
  body += "\"jsonError\":\"" + jsonEscape(lastJsonError) + "\",";
  body += "\"responsePreview\":\"" + jsonEscape(lastResponsePreview) + "\",";
  body += "\"planes\":" + String(planeCount) + ",";
  body += "\"selectedPlane\":" + String(planeCount ? selectedPlane + 1 : 0) + ",";
  body += "\"uptimeMs\":" + String(millis());
  body += "}";
  webServer.send(200, "application/json", body);
}

void handleConfigSave() {
  const float latitude = webServer.arg("latitude").toFloat();
  const float longitude = webServer.arg("longitude").toFloat();
  const float rangeKm = webServer.arg("range").toFloat();
  const unsigned long refreshSeconds = webServer.arg("refresh").toInt();
  if (webServer.arg("airport").isEmpty() || latitude < -90.0f || latitude > 90.0f ||
      longitude < -180.0f || longitude > 180.0f || rangeKm < 5.0f || rangeKm > 500.0f ||
      refreshSeconds < 10 || refreshSeconds > 3600) {
    webServer.send(400, "text/plain", "Invalid tracker settings");
    return;
  }

  settings.airport = webServer.arg("airport").substring(0, 12);
  settings.latitude = latitude;
  settings.longitude = longitude;
  settings.rangeKm = rangeKm;
  settings.refreshIntervalMs = refreshSeconds * 1000UL;
  settings.autoRotate = webServer.hasArg("autorotate");

  if (webServer.hasArg("clear_opensky")) {
    settings.openSkyClientId = "";
    settings.openSkyClientSecret = "";
    cachedOAuthToken = "";
    oauthTokenExpiresAt = 0;
  } else {
    String newId = webServer.arg("opensky_id");
    newId.trim();
    if (newId.length() > 0) {
      settings.openSkyClientId = newId;
    }

    String newSec = webServer.arg("opensky_sec");
    newSec.trim();
    if (newSec.length() > 0) {
      settings.openSkyClientSecret = newSec;
      cachedOAuthToken = "";
      oauthTokenExpiresAt = 0;
    }
  }

  saveSettings();
  lastRefresh = 0;
  statusText = "CONFIGURED";
  webServer.sendHeader("Location", "/");
  webServer.send(303, "text/plain", "Settings saved");
  fetchPlanes();
}

void handleWifiSave() {
  const String ssid = webServer.arg("ssid");
  const String password = webServer.arg("password");
  if (!usableWifiSsid(ssid) || password.length() < 8) {
    webServer.send(400, "text/plain", "Enter a network name and a password with at least 8 characters");
    return;
  }

  saveWifiCredentials(ssid, password);
  webServer.send(200, "text/plain", "Wi-Fi saved. Rebooting and attempting to connect.");
  delay(250);
  ESP.restart();
}

void handleWifiScan() {
  const int networkCount = WiFi.scanNetworks(false, true);
  String body = "[";
  bool first = true;
  for (int index = 0; index < networkCount; ++index) {
    const String ssid = WiFi.SSID(index);
    if (ssid.isEmpty()) continue;
    if (!first) body += ",";
    body += "{\"ssid\":\"" + jsonEscape(ssid) + "\",\"rssi\":" + String(WiFi.RSSI(index)) + "}";
    first = false;
  }
  body += "]";
  WiFi.scanDelete();
  webServer.send(200, "application/json", body);
}

void handleUpdateUpload() {
  HTTPUpload &upload = webServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    updateBytesWritten = 0;
    updateFailure = "";
    updateFailed = false;
    updateInProgress = true;
    updateReady = false;
    updateScreenError = false;
    updateScreenNeedsRedraw = true;
    lastUpdateScreenBytes = 0;
    lastDraw = 0;
    drawFirmwareUpdate();
    lastUpdateResult = "receiving " + upload.filename;
    Serial.println("OTA upload started: " + upload.filename);
    if (!upload.filename.endsWith(".bin")) {
      updateFailed = true;
      updateFailure = "Select a .bin firmware file";
      lastUpdateResult = updateFailure;
      updateInProgress = false;
      updateScreenError = true;
      updateScreenNeedsRedraw = true;
      appMode = MODE_FIRMWARE_UPDATE;
      updateButtonIgnoreUntil = millis() + 1500;
      drawFirmwareUpdate();
      return;
    }
    updateFailed = !Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
    if (updateFailed) {
      updateFailure = Update.errorString();
      lastUpdateResult = updateFailure;
      updateInProgress = false;
      updateScreenError = true;
      updateScreenNeedsRedraw = true;
      appMode = MODE_FIRMWARE_UPDATE;
      updateButtonIgnoreUntil = millis() + 1500;
      drawFirmwareUpdate();
    }
  } else if (upload.status == UPLOAD_FILE_WRITE && !updateFailed) {
    if (updateBytesWritten == 0 && upload.currentSize > 0 && upload.buf[0] != 0xE9) {
      updateFailed = true;
      updateFailure = "Not an ESP32 firmware image";
      lastUpdateResult = updateFailure;
      updateInProgress = false;
      updateScreenError = true;
      updateScreenNeedsRedraw = true;
      drawFirmwareUpdate();
      Update.abort();
      return;
    }
    const size_t written = Update.write(upload.buf, upload.currentSize);
    updateBytesWritten += written;
    if (updateBytesWritten - lastUpdateScreenBytes >= 65536) {
      drawFirmwareUpdate();
      lastUpdateScreenBytes = updateBytesWritten;
    }
    delay(1);
    if (written != upload.currentSize) {
      updateFailed = true;
      updateFailure = Update.errorString();
      lastUpdateResult = updateFailure;
      updateInProgress = false;
      updateScreenError = true;
      updateScreenNeedsRedraw = true;
      drawFirmwareUpdate();
    }
  } else if (upload.status == UPLOAD_FILE_END && !updateFailed) {
    if (updateBytesWritten == 0) {
      updateFailed = true;
      updateFailure = "Upload has no data";
      updateInProgress = false;
      updateScreenError = true;
      updateScreenNeedsRedraw = true;
      appMode = MODE_FIRMWARE_UPDATE;
      updateButtonIgnoreUntil = millis() + 1500;
      Update.abort();
      drawFirmwareUpdate();
    } else if (!Update.end(true)) {
      updateFailed = true;
      updateFailure = Update.errorString();
      lastUpdateResult = updateFailure;
      updateInProgress = false;
      updateScreenError = true;
      updateScreenNeedsRedraw = true;
      appMode = MODE_FIRMWARE_UPDATE;
      updateButtonIgnoreUntil = millis() + 1500;
      drawFirmwareUpdate();
    } else {
      updateInProgress = false;
      updateReady = true;
      updateScreenNeedsRedraw = true;
      appMode = MODE_FIRMWARE_UPDATE;
      updateButtonIgnoreUntil = millis() + 1500;
      drawFirmwareUpdate();
      lastUpdateResult = "committed " + String(updateBytesWritten) + " bytes";
      Serial.println("OTA upload committed: " + String(updateBytesWritten) + " bytes");
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    updateFailed = true;
    updateFailure = "Upload aborted";
    lastUpdateResult = updateFailure;
    updateInProgress = false;
    updateScreenError = true;
    updateScreenNeedsRedraw = true;
    appMode = MODE_FIRMWARE_UPDATE;
    updateButtonIgnoreUntil = millis() + 1500;
    drawFirmwareUpdate();
    Update.abort();
  }
}

void handleUpdateSave() {
  if (updateFailed || Update.hasError()) {
    const String error = updateFailure.length() ? updateFailure : Update.errorString();
    updateFailed = false;
    updateFailure = "";
    lastUpdateResult = "failed: " + error;
    updateInProgress = false;
    updateReady = false;
    updateScreenError = true;
    updateScreenNeedsRedraw = true;
    Preferences preferences;
    preferences.begin("plane-tracker", false);
    preferences.putString("lastUpdate", lastUpdateResult);
    preferences.end();
    webServer.send(500, "text/plain", "Firmware update failed: " + error);
    return;
  }

  webServer.send(200, "text/plain", "Firmware updated. Press the device button to reboot.");
  lastUpdateResult = "update ready; press device button";
}

// Downloads a release asset with retries, Wi-Fi recovery and HTTP Range resume
// so a dropped connection continues from the last written byte instead of failing.
void downloadLatestFirmware(const String &assetUrl) {
  long expectedTotal = -1;
  bool updateBegun = false;
  unsigned long retryDelayMs = LATEST_UPDATE_RETRY_DELAY_MS;

  for (uint8_t attempt = 1; attempt <= LATEST_UPDATE_MAX_ATTEMPTS && !updateFailed; attempt++) {
    if (WiFi.status() != WL_CONNECTED) {
      lastUpdateResult = "Wi-Fi lost; reconnecting";
      connectWifi();
      if (WiFi.status() != WL_CONNECTED) {
        updateFailure = "Wi-Fi lost during download";
        updateFailed = true;
        break;
      }
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient firmware;
    firmware.setTimeout(LATEST_UPDATE_TIMEOUT_MS);
    firmware.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    firmware.setReuse(false);
    if (!firmware.begin(client, assetUrl)) {
      updateFailure = "Could not start GitHub firmware download";
      updateFailed = true;
      break;
    }
    firmware.addHeader("User-Agent", "StickS3-Plane-Tracker");
    firmware.addHeader("Accept", "application/octet-stream");
    if (updateBytesWritten > 0) {
      firmware.addHeader("Range", "bytes=" + String(updateBytesWritten) + "-");
    }

    const int firmwareResult = firmware.GET();
    if (firmwareResult != HTTP_CODE_OK && firmwareResult != HTTP_CODE_PARTIAL_CONTENT) {
      updateFailure = "GitHub firmware download failed: " + firmware.errorToString(firmwareResult);
      firmware.end();
      if (firmwareResult > 0 && firmwareResult < 500 && firmwareResult != 429) break;  // permanent error; retrying is pointless
      delay(retryDelayMs);
      retryDelayMs *= 2;
      continue;
    }

    // A 200 reply to a resumed request means the stream restarted from byte 0.
    if (firmwareResult == HTTP_CODE_OK && updateBytesWritten > 0) {
      Update.abort();
      updateBegun = false;
      updateBytesWritten = 0;
    }

    if (!updateBegun) {
      const int remaining = firmware.getSize();
      expectedTotal = remaining > 0 ? (long)(updateBytesWritten + remaining) : -1;
      updateBegun = Update.begin(expectedTotal > 0 ? (size_t)expectedTotal : UPDATE_SIZE_UNKNOWN, U_FLASH);
      if (!updateBegun) {
        updateFailure = Update.errorString();
        updateFailed = true;
        firmware.end();
        break;
      }
    }

    Stream &stream = firmware.getStream();
    while (firmware.connected() && (expectedTotal < 0 || updateBytesWritten < (size_t)expectedTotal)) {
      const size_t requested = expectedTotal < 0 ? sizeof(latestUpdateBuffer) : min(sizeof(latestUpdateBuffer), (size_t)expectedTotal - updateBytesWritten);
      const size_t read = stream.readBytes(latestUpdateBuffer, requested);
      if (read == 0) break;  // stalled; retry on a fresh connection with Range resume
      if (updateBytesWritten == 0 && latestUpdateBuffer[0] != 0xE9) {
        updateFailure = "Downloaded file is not an ESP32 firmware image";
        updateFailed = true;
        break;
      }
      const size_t written = Update.write(latestUpdateBuffer, read);
      updateBytesWritten += written;
      if (written != read) {
        updateFailure = Update.errorString();
        updateFailed = true;
        break;
      }
      if (updateBytesWritten - lastUpdateScreenBytes >= 65536) {
        drawFirmwareUpdate();
        lastUpdateScreenBytes = updateBytesWritten;
      }
    }
    firmware.end();

    if (updateFailed) break;
    if (expectedTotal > 0 && updateBytesWritten < (size_t)expectedTotal) {
      lastUpdateResult = "connection dropped at " + String(updateBytesWritten) + " bytes; resuming";
      delay(retryDelayMs);
      retryDelayMs *= 2;
      continue;
    }
    if (expectedTotal < 0 && updateBytesWritten == 0) {
      updateFailure = "GitHub firmware download was empty";
      updateFailed = true;
      break;
    }
    if (!Update.end(true)) {
      updateFailure = Update.errorString();
      updateFailed = true;
    }
    updateBegun = false;
    break;
  }

  if (updateBegun && updateFailed) Update.abort();
}

void handleLatestUpdate() {
  connectWifi();
  if (provisioningMode || WiFi.status() != WL_CONNECTED) {
    webServer.send(503, "text/plain", "Wi-Fi not connected");
    return;
  }

  JsonDocument release;
  String apiError = "no response";
  bool releaseOk = false;
  for (uint8_t attempt = 1; attempt <= 2 && !releaseOk; attempt++) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient api;
    api.setTimeout(15000);
    api.begin(client, GITHUB_LATEST_RELEASE_URL);
    api.addHeader("Accept", "application/vnd.github+json");
    api.addHeader("User-Agent", "StickS3-Plane-Tracker");
    const int apiResult = api.GET();
    if (apiResult == HTTP_CODE_OK) {
      const DeserializationError parseError = deserializeJson(release, api.getStream());
      releaseOk = !parseError;
      if (parseError) apiError = "invalid release response";
    } else {
      apiError = api.errorToString(apiResult);
    }
    api.end();
    if (!releaseOk && attempt < 2) delay(1000);
  }
  if (!releaseOk) {
    webServer.send(502, "text/plain", "GitHub release lookup failed: " + apiError);
    return;
  }

  String assetUrl;
  String assetName;
  for (JsonObject asset : release["assets"].as<JsonArray>()) {
    const String name = asset["name"] | "";
    if (name.startsWith("plane-tracking") && name.endsWith(".bin")) {
      assetName = name;
      assetUrl = asset["browser_download_url"] | "";
      break;
    }
  }
  if (assetUrl.isEmpty()) {
    webServer.send(404, "text/plain", "Latest GitHub release has no plane-tracking .bin asset");
    return;
  }

  updateBytesWritten = 0;
  updateFailure = "";
  updateFailed = false;
  updateInProgress = true;
  updateReady = false;
  updateScreenError = false;
  updateScreenNeedsRedraw = true;
  lastUpdateScreenBytes = 0;
  lastDraw = 0;
  lastUpdateResult = "downloading " + assetName;
  drawFirmwareUpdate();

  downloadLatestFirmware(assetUrl);

  updateInProgress = false;
  if (updateFailed) {
    updateReady = false;
    updateScreenError = true;
    updateScreenNeedsRedraw = true;
    appMode = MODE_FIRMWARE_UPDATE;
    updateButtonIgnoreUntil = millis() + 1500;
    lastUpdateResult = "failed: " + updateFailure;
    drawFirmwareUpdate();
    webServer.send(500, "text/plain", updateFailure);
    return;
  }

  updateReady = true;
  updateScreenNeedsRedraw = true;
  appMode = MODE_FIRMWARE_UPDATE;
  updateButtonIgnoreUntil = millis() + 1500;
  lastUpdateResult = "committed " + String(updateBytesWritten) + " bytes from " + assetName;
  drawFirmwareUpdate();
  webServer.send(200, "text/plain", "Latest release downloaded. Press BLUE to reboot.");
}

void startDebugServer() {
  if (debugServerStarted || (!provisioningMode && WiFi.status() != WL_CONNECTED)) return;
  if (!debugRoutesConfigured) {
    webServer.on("/", HTTP_GET, handleDebugPage);
    webServer.on("/api/status", HTTP_GET, handleDebugStatus);
    webServer.on("/api/config", HTTP_POST, handleConfigSave);
    webServer.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
    webServer.on("/api/wifi", HTTP_POST, handleWifiSave);
    webServer.on("/api/update", HTTP_POST, handleUpdateSave, handleUpdateUpload);
    webServer.on("/api/update-latest", HTTP_POST, handleLatestUpdate);
    debugRoutesConfigured = true;
  }
  webServer.begin();
  debugServerStarted = true;
  const IPAddress address = provisioningMode ? WiFi.softAPIP() : WiFi.localIP();
  Serial.print("Debug UI: http://");
  Serial.println(address);
  if (provisioningMode) {
    Serial.print("Setup network: ");
    Serial.println(SETUP_AP_NAME);
    Serial.println("Setup password: planeconfig");
  }
}

void fetchSelectedRoute() {
  if (planeCount == 0 || planes[selectedPlane].callsign.isEmpty()) return;

  Plane &plane = planes[selectedPlane];
  plane.route = "LOOKUP";
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  http.begin(client, String(ROUTE_URL_PREFIX) + plane.callsign);
  const int result = http.GET();
  if (result != HTTP_CODE_OK) {
    plane.route = "NO ROUTE";
    http.end();
    return;
  }

  JsonDocument document;
  const DeserializationError error = deserializeJson(document, http.getStream());
  http.end();
  if (error) {
    plane.route = "NO ROUTE";
    return;
  }

  const char *origin = document["response"]["flightroute"]["origin"]["iata_code"] | nullptr;
  const char *destination = document["response"]["flightroute"]["destination"]["iata_code"] | nullptr;
  if (origin && destination) {
    plane.route = String(origin) + " > " + destination;
  } else {
    plane.route = "NO ROUTE";
  }
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;

  String ssid;
  String password;
  loadWifiCredentials(ssid, password);
  if (!usableWifiSsid(ssid)) {
    WiFi.mode(WIFI_AP);
    provisioningMode = WiFi.softAP(SETUP_AP_NAME, SETUP_AP_PASSWORD);
    statusText = provisioningMode ? "SETUP AP" : "NO WIFI";
    Serial.print("Wi-Fi setup AP: ");
    Serial.println(WiFi.softAPIP());
    return;
  }

  statusText = "WIFI...";
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  const unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 12000) {
    delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    provisioningMode = false;
    statusText = "ONLINE";
    return;
  }

  WiFi.mode(WIFI_AP);
  provisioningMode = WiFi.softAP(SETUP_AP_NAME, SETUP_AP_PASSWORD);
  statusText = provisioningMode ? "SETUP AP" : "NO WIFI";
}

void fetchPlanes() {
  const String previousSelectedIcao24 = selectedPlane < planeCount ? planes[selectedPlane].icao24 : selectedPlaneIcao24;
  connectWifi();
  if (provisioningMode || WiFi.status() != WL_CONNECTED) {
    lastHttpCode = 0;
    lastJsonError = "Wi-Fi not connected";
    lastRefresh = millis();
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  http.begin(client, openSkyUrl());
  
  String bearerToken = getOpenSkyOAuthToken();
  if (bearerToken.length() > 0) {
    http.addHeader("Authorization", "Bearer " + bearerToken);
  }
  const char *responseHeaders[] = {"Content-Type"};
  http.collectHeaders(responseHeaders, 1);
  http.addHeader("Accept", "application/json");
  const int result = http.GET();
  lastHttpCode = result;
  lastContentType = http.header("Content-Type");
  if (result != HTTP_CODE_OK) {
    statusText = "HTTP " + String(result);
    lastJsonError = http.errorToString(result);
    Serial.printf("OpenSky HTTP %d: %s\n", result, lastJsonError.c_str());
    http.end();
    lastRefresh = millis();
    return;
  }

  const String responseBody = http.getString();
  lastResponsePreview = responseBody.substring(0, 180);
  JsonDocument document;
  const DeserializationError error = deserializeJson(document, responseBody);
  http.end();
  if (error) {
    statusText = "JSON ERROR";
    lastJsonError = error.c_str();
    Serial.printf("OpenSky JSON error: %s\nResponse: %s\n", lastJsonError.c_str(), lastResponsePreview.c_str());
    lastRefresh = millis();
    return;
  }

  lastJsonError = "";
  Serial.printf("OpenSky OK: %d bytes, %u planes in range\n", responseBody.length(), planeCount);

  planeCount = 0;
  for (JsonVariant state : document["states"].as<JsonArray>()) {
    if (planeCount >= MAX_PLANES) break;
    if (state[5].isNull() || state[6].isNull()) continue;

    Plane &plane = planes[planeCount];
    plane.icao24 = state[0].as<const char *>();
    plane.callsign = state[1].as<const char *>();
    plane.callsign.trim();
    plane.longitude = state[5].as<float>();
    plane.latitude = state[6].as<float>();
    plane.altitudeMeters = state[7].is<float>() ? state[7].as<float>() : -1.0f;
    plane.speedKmh = state[9].is<float>() ? state[9].as<float>() * 3.6f : -1.0f;
    plane.heading = state[10].is<float>() ? state[10].as<float>() : -1.0f;
    plane.distanceKm = distanceKm(settings.latitude, settings.longitude, plane.latitude, plane.longitude);
    if (plane.distanceKm <= settings.rangeKm) planeCount++;
  }

  for (size_t i = 0; i < planeCount; ++i) {
    for (size_t j = i + 1; j < planeCount; ++j) {
      if (planes[j].distanceKm < planes[i].distanceKm) {
        Plane temporary = planes[i];
        planes[i] = planes[j];
        planes[j] = temporary;
      }
    }
  }
  selectedPlane = planeCount == 0 ? 0 : planeCount - 1;
  if (previousSelectedIcao24.length()) {
    for (size_t i = 0; i < planeCount; ++i) {
      if (planes[i].icao24 == previousSelectedIcao24) {
        selectedPlane = i;
        break;
      }
    }
  }
  selectedPlaneIcao24 = planeCount > 0 ? planes[selectedPlane].icao24 : previousSelectedIcao24;
  statusText = planeCount == 0 ? "NO TRAFFIC" : "OPEN SKY";
  lastRefresh = millis();
  fetchSelectedRoute();
}

// Ensures the sprite exists and matches the current display size/rotation.
// The canvas is recreated when the auto-rotate feature changes orientation.
bool ensureRadarCanvas() {
  const int width = halDisplay.width();
  const int height = halDisplay.height();
  const int rotation = halDisplay.getRotation();
  if (radarCanvasReady && width == radarCanvasWidth && height == radarCanvasHeight &&
      rotation == radarCanvasRotation) {
    return true;
  }
  radarCanvas.deleteSprite();
  
  bool psram = psramFound();
  radarCanvas.setPsram(psram);
  radarCanvas.setColorDepth(16);
  if (radarCanvas.createSprite(width, height) == nullptr) {
    if (psram) {
      radarCanvas.setPsram(false);
      if (radarCanvas.createSprite(width, height) != nullptr) {
        goto sprite_success;
      }
    }
    // Fallback to 8-bit color depth (requires 50% less RAM)
    radarCanvas.setColorDepth(8);
    if (radarCanvas.createSprite(width, height) == nullptr) {
      radarCanvasReady = false;
      Serial.println("[ERROR] Failed to allocate radarCanvas sprite!");
      return false;
    }
  }

sprite_success:
  radarCanvas.setTextFont(2);
  radarCanvas.setTextSize(1);
  radarCanvas.setTextWrap(false);
  radarCanvasWidth = width;
  radarCanvasHeight = height;
  radarCanvasRotation = rotation;
  radarCanvasReady = true;
  return true;
}

void drawRadar() {
  Serial.println("[UI] drawRadar() called...");
  bool useSprite = ensureRadarCanvas();
  if (!useSprite) {
    Serial.println("[UI] Using direct display rendering (RAM fallback)");
  }
  lgfx::LovyanGFX &gfx = useSprite ? static_cast<lgfx::LovyanGFX&>(radarCanvas) : static_cast<lgfx::LovyanGFX&>(halDisplay);
  const int width = halDisplay.width();
  const int height = halDisplay.height();
  const int infoWidth = min(104, width / 2);
  const int radarWidth = width - infoWidth;
  const int radius = min(height / 2 - 12, radarWidth / 2 - 6);
  const int centerX = radarWidth / 2;
  const int centerY = height / 2;
  const int infoX = radarWidth + 4;

  if (!useSprite) halDisplay.startWrite();

  gfx.fillScreen(TFT_BLACK);
  gfx.drawRect(1, 1, width - 2, height - 2, TFT_DARKGREEN);
  gfx.drawLine(radarWidth, 0, radarWidth, height, TFT_DARKGREEN);

  gfx.setTextSize(1);
  gfx.setTextColor(TFT_CYAN, TFT_BLACK);
  gfx.drawString(scrollingText(settings.airport + " " + (trackerPaused ? "PAUSED" : statusText), 15), infoX, 3);

  if (planeCount > 0) {
    const Plane &plane = planes[selectedPlane];
    const String callsign = plane.callsign.length() ? plane.callsign : "UNKNOWN";

    gfx.setTextColor(TFT_YELLOW, TFT_BLACK);
    gfx.drawString(scrollingText(callsign, 12), infoX, 20);
    gfx.setTextColor(TFT_WHITE, TFT_BLACK);
    gfx.drawString("#" + String(selectedPlane + 1) + "/" + String(planeCount), infoX, 36);
    gfx.drawString("DST " + String(plane.distanceKm, 1) + " km", infoX, 52);
    gfx.drawString("ALT " + String(plane.altitudeMeters < 0 ? 0 : plane.altitudeMeters, 0) + " m", infoX, 68);
    gfx.drawString("SPD " + String(plane.speedKmh < 0 ? 0 : plane.speedKmh, 0) + " km/h", infoX, 84);
    gfx.drawString("HDG " + String(plane.heading < 0 ? 0 : plane.heading, 0) + " deg", infoX, 100);
    gfx.drawString(scrollingText(planeDetailsTicker(plane), 15), infoX, 116);
  } else {
    gfx.setTextColor(TFT_WHITE, TFT_BLACK);
    gfx.drawString("No aircraft", infoX, 28);
    gfx.drawString(hasTouchscreen ? "TAP REFRESH" : "BLUE SELECT", infoX, 48);
  }

  gfx.drawCircle(centerX, centerY, radius, TFT_DARKGREEN);
  gfx.drawCircle(centerX, centerY, radius * 2 / 3, TFT_DARKGREEN);
  gfx.drawCircle(centerX, centerY, radius / 3, TFT_DARKGREEN);
  gfx.drawLine(centerX - radius, centerY, centerX + radius, centerY, TFT_DARKGREEN);
  gfx.drawLine(centerX, centerY - radius, centerX, centerY + radius, TFT_DARKGREEN);
  gfx.setTextColor(TFT_CYAN, TFT_BLACK);
  gfx.drawString(String(settings.rangeKm, 0) + " km", 4, 2);

  for (size_t i = 0; i < planeCount; ++i) {
    const float bearing = atan2f(toRadians(planes[i].longitude - settings.longitude) *
                    cosf(toRadians(planes[i].latitude)),
                  toRadians(planes[i].latitude - settings.latitude));
    const float radial = min(planes[i].distanceKm / settings.rangeKm, 1.0f) * radius;
    const int x = centerX + sinf(bearing) * radial;
    const int y = centerY - cosf(bearing) * radial;
    gfx.fillCircle(x, y, i == selectedPlane ? 4 : 2,
                i == selectedPlane ? TFT_YELLOW : TFT_RED);
    if (i == selectedPlane && planes[i].heading >= 0.0f && planes[i].heading < 360.0f) {
      const float headingRadians = toRadians(planes[i].heading - 90.0f);
      const int lineStartX = x + cosf(headingRadians) * 5;
      const int lineStartY = y + sinf(headingRadians) * 5;
      const int lineEndX = x + cosf(headingRadians) * 11;
      const int lineEndY = y + sinf(headingRadians) * 11;
      gfx.drawLine(lineStartX, lineStartY, lineEndX, lineEndY, TFT_YELLOW);
    }
  }

  const float sweepRadians = toRadians(sweepAngle);
  gfx.drawLine(centerX, centerY, centerX + cosf(sweepRadians) * radius,
                      centerY + sinf(sweepRadians) * radius, TFT_GREEN);
  gfx.setTextColor(TFT_CYAN, TFT_BLACK);
  gfx.setTextSize(1);

  // On touchscreen devices, render touch hint buttons at bottom of info panel
  if (hasTouchscreen) {
    gfx.drawRect(infoX, height - 18, infoWidth - 6, 16, TFT_DARKGREEN);
    gfx.setTextColor(TFT_GREEN, TFT_BLACK);
    gfx.drawString("< PREV|NEXT >", infoX + 2, height - 16);
  }

  if (useSprite) {
    halDisplay.startWrite();
    radarCanvas.pushSprite(&halDisplay, 0, 0);
    halDisplay.endWrite();
  } else {
    halDisplay.endWrite();
  }
  drawBatteryIndicator(radarWidth - 4, 5);
}

void drawIpOverlay() {
  if (ipOverlayUntil == 0 || (long)(ipOverlayUntil - millis()) <= 0) return;

  const int width = halDisplay.width();
  const int height = halDisplay.height();
  const String address = provisioningMode      ? WiFi.softAPIP().toString()
                          : WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString()
                                                           : "NO WIFI";
  halDisplay.fillRect(8, 30, width - 16, height - 60, TFT_BLACK);
  halDisplay.drawRect(8, 30, width - 16, height - 60, TFT_CYAN);
  halDisplay.setTextColor(TFT_CYAN, TFT_BLACK);
  halDisplay.setTextSize(1);
  halDisplay.drawString("DEVICE WEB UI", 18, 42);
  halDisplay.setTextColor(TFT_WHITE, TFT_BLACK);
  halDisplay.drawString(address, 18, 62);
  halDisplay.setTextColor(TFT_DARKGREEN, TFT_BLACK);
  halDisplay.drawString("http://" + address, 18, 76);
}

bool ipOverlayActive() {
  return ipOverlayUntil != 0 && (long)(ipOverlayUntil - millis()) > 0;
}

void drawMenu() {
  const int width = halDisplay.width();
  const int height = halDisplay.height();
  halDisplay.fillScreen(TFT_BLACK);
  halDisplay.drawRect(1, 1, width - 2, height - 2, TFT_DARKGREEN);
  halDisplay.setTextSize(1);
  halDisplay.setTextColor(TFT_CYAN, TFT_BLACK);
  halDisplay.drawString("PLANE TRACKER", 10, 5);
  halDisplay.setTextColor(TFT_DARKGREEN, TFT_BLACK);
  halDisplay.drawString(provisioningMode ? "SETUP AP" : "WEB UI ON", width - 70, 5);
  halDisplay.drawLine(8, 19, width - 8, 19, TFT_DARKGREEN);
  const int rowX = 8;
  const int rowWidth = width - 16;
  const int rowHeight = 27;
  const int rowY = 27;
  halDisplay.fillRect(rowX, rowY, rowWidth, rowHeight, TFT_GREEN);
  halDisplay.fillRect(rowX, rowY, 4, rowHeight, TFT_YELLOW);
  const int iconX = 27;
  const int iconY = rowY + rowHeight / 2 - 1;
  halDisplay.setTextColor(TFT_BLACK, TFT_BLACK);
  halDisplay.drawCircle(iconX, iconY, 8, TFT_BLACK);
  halDisplay.drawCircle(iconX, iconY, 3, TFT_BLACK);
  halDisplay.drawLine(iconX, iconY, iconX + 8, iconY - 6, TFT_BLACK);
  halDisplay.drawString("PLANE RADAR", 48, rowY + 9);
  halDisplay.setTextColor(TFT_DARKGREEN, TFT_BLACK);
  halDisplay.drawString(hasTouchscreen ? "TAP SELECT" : "SELECT", width - (hasTouchscreen ? 70 : 48), height - 13);
  menuNeedsRedraw = false;
}

void drawFirmwareUpdate() {
  const int width = halDisplay.width();
  const int height = halDisplay.height();
  halDisplay.fillScreen(TFT_BLACK);
  halDisplay.drawRect(1, 1, width - 2, height - 2, TFT_DARKGREEN);
  halDisplay.setTextSize(1);
  halDisplay.setTextColor(TFT_CYAN, TFT_BLACK);
  halDisplay.drawString("FIRMWARE UPDATE", 10, 8);
  halDisplay.setTextColor(updateScreenError ? TFT_RED : updateReady ? TFT_GREEN : TFT_WHITE, TFT_BLACK);
  halDisplay.drawString(updateScreenError ? "UPDATE FAILED" : updateReady ? "UPDATE READY" : "UPLOADING", 10, 32);
  halDisplay.setTextColor(TFT_WHITE, TFT_BLACK);
  halDisplay.drawString(String(updateBytesWritten) + " bytes", 10, 52);
  if (!updateScreenError && !updateReady) {
    halDisplay.drawRect(10, 72, width - 20, 12, TFT_DARKGREEN);
    halDisplay.fillRect(12, 74, min((int)(updateBytesWritten / 8192), width - 24), 8, TFT_GREEN);
  } else if (updateReady) {
    halDisplay.setTextColor(TFT_YELLOW, TFT_BLACK);
    halDisplay.drawString(hasTouchscreen ? "TAP SCREEN TO REBOOT" : "PRESS BLUE TO REBOOT", 10, 76);
  } else {
    halDisplay.setTextColor(TFT_RED, TFT_BLACK);
    halDisplay.drawString(scrollingText(updateFailure, 20), 10, 76);
  }
  halDisplay.setTextColor(TFT_DARKGREEN, TFT_BLACK);
  halDisplay.drawString(updateReady ? (hasTouchscreen ? "TAP REBOOT" : "BLUE REBOOT") : (hasTouchscreen ? "TAP BACK" : "BLUE BACK"), 10, height - 14);
  drawBatteryIndicator(width - 4, 5);
  updateScreenNeedsRedraw = false;
}

void enterTracker() {
  appMode = MODE_TRACKER;
  trackerPaused = false;
  lastDraw = 0;
  webUiNeedsRedraw = true;
  fetchPlanes();
}

void exitToMenu() {
  appMode = MODE_MENU;
  ipOverlayUntil = 0;
  ipOverlayDrawn = false;
  exitComboHandled = false;
  lastDraw = 0;
  menuNeedsRedraw = true;
}

void handleMenuButtons() {
  if (controlNext() || controlPrevious()) menuSelection = 0;
  if (controlSelect()) {
    enterTracker();
  }
  if (controlShowIp()) {
    ipOverlayUntil = millis() + 5000;
    ipOverlayDrawn = false;
  }
}

void handleTrackerButtons() {
  if (controlExit()) {
    trackerPaused = !trackerPaused;
    lastDraw = 0;
    return;
  }
  if (controlShowIp()) {
    ipOverlayUntil = millis() + 5000;
    ipOverlayDrawn = false;
    return;
  }
  if (controlNext() && planeCount > 0) {
    selectedPlane = (selectedPlane + 1) % planeCount;
    selectedPlaneIcao24 = planes[selectedPlane].icao24;
    fetchSelectedRoute();
  }
  if (controlPrevious() && planeCount > 0) {
    selectedPlane = selectedPlane == 0 ? planeCount - 1 : selectedPlane - 1;
    selectedPlaneIcao24 = planes[selectedPlane].icao24;
    fetchSelectedRoute();
  }
  if (controlSelect()) fetchPlanes();
}

void handleFirmwareUpdateButtons() {
  if ((long)(updateButtonIgnoreUntil - millis()) > 0) return;
  if (!controlSelect()) return;
  if (updateReady && !updateInProgress && !updateFailed) {
    Preferences preferences;
    preferences.begin("plane-tracker", false);
    preferences.putString("lastUpdate", "rebooted after OTA " + String(FIRMWARE_VERSION));
    preferences.end();
    delay(100);
    ESP.restart();
  }
  updateReady = false;
  updateScreenError = false;
  updateScreenNeedsRedraw = true;
  menuNeedsRedraw = true;
  appMode = MODE_MENU;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("==========================================");
  Serial.println("[MAIN] ESP32-S3 Plane Tracker starting...");
  Serial.printf("[MAIN] Reset reason: %d\n", (int)esp_reset_reason());
  Serial.printf("[MAIN] PSRAM found: %s (Size: %d bytes)\n", psramFound() ? "YES" : "NO", psramFound() ? ESP.getPsramSize() : 0);
  Serial.printf("[MAIN] Free Heap: %d bytes\n", ESP.getFreeHeap());

  Serial.println("[MAIN] Initializing Hardware Abstraction Layer...");
  halInit();

  displayRotation = 1;
  halDisplay.setTextFont(2);
  Serial.printf("[MAIN] halDisplay width=%d, height=%d\n", halDisplay.width(), halDisplay.height());

  Serial.println("[MAIN] Loading NVS settings...");
  loadSettings();

  Serial.println("[MAIN] Connecting Wi-Fi...");
  connectWifi();

  Serial.println("[MAIN] Starting Debug Web Server...");
  startDebugServer();

  Serial.println("[MAIN] Entering Tracker mode...");
  enterTracker();
  Serial.println("[MAIN] Setup completed successfully!");
  Serial.println("==========================================");
}

void loop() {
  halUpdate();
  if (debugServerStarted) webServer.handleClient();
  if (appMode == MODE_MENU) {
    handleMenuButtons();
    if (menuNeedsRedraw) drawMenu();
  } else if (appMode == MODE_TRACKER) {
    handleTrackerButtons();
    if (!trackerPaused) updateAutoRotation();
    if (ipOverlayActive()) {
      if (!ipOverlayDrawn) {
        drawRadar();
        drawIpOverlay();
        ipOverlayDrawn = true;
      }
    } else {
      if (ipOverlayDrawn) {
        ipOverlayDrawn = false;
        lastDraw = 0;
      }
      if (!trackerPaused && millis() - lastRefresh >= settings.refreshIntervalMs) fetchPlanes();
      if ((!trackerPaused && millis() - lastDraw >= DISPLAY_FRAME_MS) ||
          (trackerPaused && lastDraw == 0)) {
        lastDraw = millis();
        drawRadar();
        if (!trackerPaused) sweepAngle = fmodf(sweepAngle + 4.0f, 360.0f);
      }
    }
  } else {
    handleFirmwareUpdateButtons();
    if ((updateInProgress && millis() - lastDraw >= DISPLAY_FRAME_MS) ||
        (!updateInProgress && updateScreenNeedsRedraw)) {
      lastDraw = millis();
      drawFirmwareUpdate();
    }
  }
  delay(20);
}
