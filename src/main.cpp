#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>
#include <HTTPClient.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>

#include "config.h"

struct Plane {
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
};

enum AppMode { MODE_MENU, MODE_TRACKER, MODE_WEB_UI, MODE_FIRMWARE_UPDATE };

Plane planes[MAX_PLANES];
size_t planeCount = 0;
size_t selectedPlane = 0;
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
bool menuNeedsRedraw = true;
bool webUiNeedsRedraw = true;

void fetchPlanes();
void connectWifi();
void exitToMenu();
void stopDebugServer();
void drawFirmwareUpdate();

// Centralized physical control mapping for every app mode.
// M5.BtnB is the top button; M5.BtnA is the blue button beside the display.
bool controlNext() { return M5.BtnB.wasSingleClicked(); }
bool controlSelect() { return M5.BtnA.wasSingleClicked(); }
bool controlPrevious() { return M5.BtnB.wasDoubleClicked(); }
bool controlExit() { return M5.BtnB.wasHold(); }
bool controlShowIp() { return M5.BtnA.wasHold(); }

constexpr char ROUTE_URL_PREFIX[] = "https://api.adsbdb.com/v0/callsign/";
constexpr char SETUP_AP_NAME[] = "PlaneTracker-Setup";
constexpr char SETUP_AP_PASSWORD[] = "planeconfig";
constexpr char FIRMWARE_VERSION[] = "1.0.23";

constexpr char DEBUG_PAGE[] = R"rawliteral(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Plane Tracker</title><style>
body{font:16px monospace;background:#101512;color:#d7ffe0;max-width:900px;margin:24px auto;padding:0 16px}
h1{color:#50e0ff;font-size:22px}section{border:1px solid #276b3c;padding:14px;margin:12px 0;white-space:pre-wrap;overflow-wrap:anywhere}
dt{color:#8cff9b;margin-top:10px}dd{margin:3px 0 0 0;color:#fff}button{background:#194d2a;color:#fff;border:1px solid #56d878;padding:9px 14px;margin-top:12px}input{background:#17221a;color:#fff;border:1px solid #4c9d61;padding:8px;width:130px}label{display:inline-block;margin:7px 12px 7px 0}
</style></head><body><h1>Plane Tracker <small id="firmwareVersion"></small></h1><section><form action="/api/config" method="post">
<label>Airport preset <select id="airportPreset"><option value="">Manual coordinates</option><option value="FCO" data-lat="41.8003" data-lon="12.2389">FCO - Rome</option><option value="LHR" data-lat="51.4700" data-lon="-0.4543">LHR - London</option><option value="CDG" data-lat="49.0097" data-lon="2.5479">CDG - Paris</option><option value="AMS" data-lat="52.3086" data-lon="4.7639">AMS - Amsterdam</option><option value="FRA" data-lat="50.0379" data-lon="8.5622">FRA - Frankfurt</option><option value="MAD" data-lat="40.4983" data-lon="-3.5676">MAD - Madrid</option><option value="JFK" data-lat="40.6413" data-lon="-73.7781">JFK - New York</option><option value="LAX" data-lat="33.9416" data-lon="-118.4085">LAX - Los Angeles</option><option value="ORD" data-lat="41.9742" data-lon="-87.9073">ORD - Chicago</option><option value="DXB" data-lat="25.2532" data-lon="55.3657">DXB - Dubai</option><option value="HND" data-lat="35.5494" data-lon="139.7798">HND - Tokyo</option><option value="SIN" data-lat="1.3644" data-lon="103.9915">SIN - Singapore</option></select></label><label>Airport <input name="airport" maxlength="12"></label><label>Latitude <input name="latitude" type="number" step="0.0001"></label><label>Longitude <input name="longitude" type="number" step="0.0001"></label><label>Radar range km <input name="range" type="number" min="5" max="500" step="1"></label><label>Refresh sec <input name="refresh" type="number" min="10" max="3600" step="1"></label><label>Auto rotate <input name="autorotate" type="checkbox"></label><br><button type="submit">Save and refresh</button></form></section><section><button onclick="load()">Refresh diagnostics</button><dl id="data">Loading...</dl></section>
<section><h2>Wi-Fi setup</h2><form action="/api/wifi" method="post"><label>Found networks <select id="wifiNetworks"><option value="">Scan for networks</option></select></label><button id="wifiScanButton" type="button" onclick="scanWifi()">Scan networks</button> <span id="wifiScanStatus"></span><br><label>Network <input id="wifiSsid" name="ssid" maxlength="32" required></label><label>Password <input name="password" type="password" maxlength="64"></label><br><button type="submit">Save Wi-Fi and reboot</button></form></section>
<section><h2>Firmware update</h2><form id="firmwareForm" action="/api/update" method="post" enctype="multipart/form-data"><input name="firmware" type="file" accept=".bin,application/octet-stream" required><br><button id="firmwareButton" type="submit">Upload firmware and reboot</button> <span id="firmwareStatus"></span></form></section>
<section><form action="/api/webui" method="post"><button name="action" value="disable" type="submit">Disable Web UI</button></form></section>
<script>const firmwareForm=document.querySelector('#firmwareForm');firmwareForm.addEventListener('submit',async event=>{event.preventDefault();const button=document.querySelector('#firmwareButton');const status=document.querySelector('#firmwareStatus');button.disabled=true;button.textContent='Uploading...';status.textContent='Uploading firmware; do not disconnect';try{const response=await fetch(firmwareForm.action,{method:'POST',body:new FormData(firmwareForm)});const message=await response.text();if(!response.ok)throw new Error(message);status.textContent='Update ready. Press BLUE on the device to reboot.'}catch(error){status.textContent=error.message.startsWith('Firmware update failed')?error.message:'Upload connection lost; check the device screen'}finally{button.disabled=false;button.textContent='Upload firmware and reboot'}});</script>
<script>async function scanWifi(){const select=document.querySelector('#wifiNetworks');const button=document.querySelector('#wifiScanButton');const status=document.querySelector('#wifiScanStatus');button.disabled=true;button.textContent='Scanning...';status.textContent='Scanning nearby networks';select.innerHTML='<option value="">Scanning...</option>';try{const r=await fetch('/api/wifi/scan');if(!r.ok)throw new Error(await r.text());const networks=await r.json();select.innerHTML='<option value="">Select a network</option>';for(const network of networks){const option=document.createElement('option');option.value=network.ssid;option.textContent=network.ssid+' ('+network.rssi+' dBm)';select.appendChild(option)}if(!networks.length){select.innerHTML='<option value="">No networks found</option>';status.textContent='Scan finished: no networks found'}else status.textContent='Scan finished: '+networks.length+' network'+(networks.length===1?'':'s')}catch(error){select.innerHTML='<option value="">Scan failed</option>';status.textContent='Scan failed';alert(error.message)}finally{button.disabled=false;button.textContent='Scan networks'}}document.querySelector('#wifiNetworks').addEventListener('change',event=>{if(event.target.value)document.querySelector('#wifiSsid').value=event.target.value});let configDirty=false;const configForm=document.querySelector('form[action="/api/config"]');const airportPreset=document.querySelector('#airportPreset');configForm.addEventListener('input',()=>configDirty=true);airportPreset.addEventListener('change',()=>{const option=airportPreset.selectedOptions[0];if(option.value){document.querySelector('[name=airport]').value=option.value;document.querySelector('[name=latitude]').value=option.dataset.lat;document.querySelector('[name=longitude]').value=option.dataset.lon}configDirty=true});async function load(){const r=await fetch('/api/status');const d=await r.json();document.querySelector('#firmwareVersion').textContent='FW v'+d.firmwareVersion;if(!configDirty){for(const k of ['airport','latitude','longitude','rangeKm','refreshSeconds']){const e=document.querySelector('[name='+({rangeKm:'range',refreshSeconds:'refresh'}[k]||k)+']');if(e)e.value=d[k]}const matchingPreset=[...airportPreset.options].find(option=>option.value===d.airport&&Math.abs(Number(option.dataset.lat)-Number(d.latitude))<0.0001&&Math.abs(Number(option.dataset.lon)-Number(d.longitude))<0.0001);airportPreset.value=matchingPreset?d.airport:'';document.querySelector('[name=autorotate]').checked=!!d.autoRotate}let out='';for(const [k,v] of Object.entries(d)){out+='<dt>'+k+'</dt><dd>'+String(v).replace(/&/g,'&amp;').replace(/</g,'&lt;')+'</dd>'}document.querySelector('#data').innerHTML=out}load();setInterval(load,5000)</script>
</body></html>
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
  const int battery = constrain((int)M5.Power.getBatteryLevel(), 0, 100);
  const uint16_t color = battery <= 20 ? TFT_RED : battery <= 50 ? TFT_YELLOW : TFT_GREEN;
  const int bodyX = right - 20;
  M5.Display.drawRect(bodyX, top, 16, 8, color);
  M5.Display.fillRect(bodyX + 16, top + 2, 2, 4, color);
  const int fillWidth = battery * 12 / 100;
  if (fillWidth > 0) M5.Display.fillRect(bodyX + 2, top + 2, fillWidth, 4, color);
}

void updateAutoRotation() {
  if (!settings.autoRotate || millis() - lastImuCheck < 250) return;
  lastImuCheck = millis();

  float ax;
  float ay;
  float az;
  float gx;
  float gy;
  float gz;
  if (!M5.Imu.getAccelData(&ax, &ay, &az) ||
      !M5.Imu.getGyroData(&gx, &gy, &gz)) return;

  const float motion = fabsf(gx) + fabsf(gy) + fabsf(gz);
  if (motion < 15.0f) return;

  uint8_t nextRotation = displayRotation;
  const float landscapeAxis = fabsf(ax) > fabsf(ay) ? ax : ay;
  nextRotation = landscapeAxis > 0.0f ? 1 : 3;
  if (nextRotation != displayRotation) {
    displayRotation = nextRotation;
    M5.Display.setRotation(displayRotation);
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
    updateInProgress = true;
    updateReady = false;
    updateScreenError = false;
    updateScreenNeedsRedraw = true;
    lastUpdateScreenBytes = 0;
    lastDraw = 0;
    appMode = MODE_FIRMWARE_UPDATE;
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
    if (!Update.end(true)) {
      updateFailed = true;
      updateFailure = Update.errorString();
      lastUpdateResult = updateFailure;
      updateInProgress = false;
      updateScreenError = true;
      updateScreenNeedsRedraw = true;
      drawFirmwareUpdate();
    } else {
      updateInProgress = false;
      updateReady = true;
      updateScreenNeedsRedraw = true;
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

void handleWebUiControl() {
  if (webServer.arg("action") != "disable") {
    webServer.send(400, "text/plain", "Unknown Web UI action");
    return;
  }

  webServer.send(200, "text/plain", "Web UI disabled. The device is returning to the menu.");
  delay(100);
  stopDebugServer();
  exitToMenu();
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
    webServer.on("/api/webui", HTTP_POST, handleWebUiControl);
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

void stopDebugServer() {
  if (!debugServerStarted) return;
  webServer.stop();
  debugServerStarted = false;
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
  if (selectedPlane >= planeCount) selectedPlane = planeCount == 0 ? 0 : planeCount - 1;
  statusText = planeCount == 0 ? "NO TRAFFIC" : "OPEN SKY";
  lastRefresh = millis();
  fetchSelectedRoute();
}

void drawRadar() {
  const int width = M5.Display.width();
  const int height = M5.Display.height();
  const int infoWidth = min(104, width / 2);
  const int radarWidth = width - infoWidth;
  const int radius = min(height / 2 - 12, radarWidth / 2 - 6);
  const int centerX = radarWidth / 2;
  const int centerY = height / 2;
  const int infoX = radarWidth + 4;

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.drawRect(1, 1, width - 2, height - 2, TFT_DARKGREEN);
  M5.Display.drawLine(radarWidth, 0, radarWidth, height, TFT_DARKGREEN);
  drawBatteryIndicator(radarWidth - 4, 5);

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Display.drawString(scrollingText(settings.airport + " " + statusText, 15), infoX, 3);

  if (planeCount > 0) {
    const Plane &plane = planes[selectedPlane];
    const String callsign = plane.callsign.length() ? plane.callsign : "UNKNOWN";

    M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5.Display.drawString(scrollingText(callsign, 12), infoX, 20);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawString("#" + String(selectedPlane + 1) + "/" + String(planeCount), infoX, 36);
    M5.Display.drawString("DST " + String(plane.distanceKm, 1) + " km", infoX, 52);
    M5.Display.drawString("ALT " + String(plane.altitudeMeters < 0 ? 0 : plane.altitudeMeters, 0) + " m", infoX, 68);
    M5.Display.drawString("SPD " + String(plane.speedKmh < 0 ? 0 : plane.speedKmh, 0) + " km/h", infoX, 84);
    M5.Display.drawString("HDG " + String(plane.heading < 0 ? 0 : plane.heading, 0) + " deg", infoX, 100);
    M5.Display.drawString(scrollingText(planeDetailsTicker(plane), 15), infoX, 116);
  } else {
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawString("No aircraft", infoX, 28);
    M5.Display.drawString("BLUE SELECT", infoX, 48);
  }

  M5.Display.drawCircle(centerX, centerY, radius, TFT_DARKGREEN);
  M5.Display.drawCircle(centerX, centerY, radius * 2 / 3, TFT_DARKGREEN);
  M5.Display.drawCircle(centerX, centerY, radius / 3, TFT_DARKGREEN);
  M5.Display.drawLine(centerX - radius, centerY, centerX + radius, centerY, TFT_DARKGREEN);
  M5.Display.drawLine(centerX, centerY - radius, centerX, centerY + radius, TFT_DARKGREEN);
  M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Display.drawString(String(settings.rangeKm, 0) + " km", 4, 2);

  for (size_t i = 0; i < planeCount; ++i) {
    const float bearing = atan2f(toRadians(planes[i].longitude - settings.longitude) *
                    cosf(toRadians(planes[i].latitude)),
                  toRadians(planes[i].latitude - settings.latitude));
    const float radial = min(planes[i].distanceKm / settings.rangeKm, 1.0f) * radius;
    const int x = centerX + cosf(bearing) * radial;
    const int y = centerY + sinf(bearing) * radial;
    M5.Display.fillCircle(x, y, i == selectedPlane ? 4 : 2,
                i == selectedPlane ? TFT_YELLOW : TFT_RED);
  }

  const float sweepRadians = toRadians(sweepAngle);
  M5.Display.drawLine(centerX, centerY, centerX + cosf(sweepRadians) * radius,
                      centerY + sinf(sweepRadians) * radius, TFT_GREEN);
  M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Display.setTextSize(1);
}

void drawIpOverlay() {
  if (ipOverlayUntil == 0 || (long)(ipOverlayUntil - millis()) <= 0) return;

  const int width = M5.Display.width();
  const int height = M5.Display.height();
  const String address = WiFi.status() == WL_CONNECTED
                             ? WiFi.localIP().toString()
                             : "NO WIFI";
  M5.Display.fillRect(8, 30, width - 16, height - 60, TFT_BLACK);
  M5.Display.drawRect(8, 30, width - 16, height - 60, TFT_CYAN);
  M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Display.setTextSize(1);
  M5.Display.drawString("DEVICE WEB UI", 18, 42);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.drawString(address, 18, 62);
  M5.Display.setTextColor(TFT_DARKGREEN, TFT_BLACK);
  M5.Display.drawString("http://" + address, 18, 76);
}

bool ipOverlayActive() {
  return ipOverlayUntil != 0 && (long)(ipOverlayUntil - millis()) > 0;
}

void drawMenu() {
  const int width = M5.Display.width();
  const int height = M5.Display.height();
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.drawRect(1, 1, width - 2, height - 2, TFT_DARKGREEN);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Display.drawString("LAUNCHER", 10, 5);
  M5.Display.setTextColor(TFT_DARKGREEN, TFT_BLACK);
  M5.Display.drawString(provisioningMode ? "SETUP AP" : "PLANE TRACKER", width - 92, 5);
  M5.Display.drawLine(8, 19, width - 8, 19, TFT_DARKGREEN);

  const int rowX = 8;
  const int rowWidth = width - 16;
  const int rowHeight = 27;
  const int rowY[2] = {27, 57};
  for (int index = 0; index < 2; ++index) {
    const bool selected = menuSelection == index;
    if (selected) {
      M5.Display.fillRect(rowX, rowY[index], rowWidth, rowHeight, TFT_GREEN);
      M5.Display.fillRect(rowX, rowY[index], 4, rowHeight, TFT_YELLOW);
    }
  }

  const int radarIconX = 27;
  const int radarIconY = rowY[0] + rowHeight / 2 - 1;
  const uint16_t radarColor = menuSelection == 0 ? TFT_BLACK : TFT_GREEN;
  M5.Display.setTextColor(radarColor, TFT_BLACK);
  M5.Display.drawCircle(radarIconX, radarIconY, 8, radarColor);
  M5.Display.drawCircle(radarIconX, radarIconY, 3, radarColor);
  M5.Display.drawLine(radarIconX, radarIconY, radarIconX + 8, radarIconY - 6,
                      radarColor);
  M5.Display.setTextColor(menuSelection == 0 ? TFT_BLACK : TFT_WHITE, TFT_BLACK);
  M5.Display.drawString("PLANE RADAR", 48, rowY[0] + 9);

  const int webIconX = 27;
  const int webIconY = rowY[1] + rowHeight / 2 - 1;
  const uint16_t webColor = menuSelection == 1 ? TFT_BLACK : TFT_GREEN;
  M5.Display.drawRect(webIconX - 9, webIconY - 6, 18, 12, webColor);
  M5.Display.drawLine(webIconX - 7, webIconY - 2, webIconX + 7, webIconY - 2, webColor);
  M5.Display.fillCircle(webIconX - 5, webIconY - 4, 1, webColor);
  M5.Display.fillCircle(webIconX - 1, webIconY - 4, 1, webColor);
  M5.Display.setTextColor(menuSelection == 1 ? TFT_BLACK : TFT_WHITE, TFT_BLACK);
  M5.Display.drawString("WEB UI", 48, rowY[1] + 9);

  M5.Display.setTextColor(TFT_DARKGREEN, TFT_BLACK);
  M5.Display.drawString("NEXT", 10, height - 13);
  M5.Display.drawString("SELECT", width - 48, height - 13);
  menuNeedsRedraw = false;
}

void drawWebUi() {
  const int width = M5.Display.width();
  const int height = M5.Display.height();
  const bool hasAddress = provisioningMode || WiFi.status() == WL_CONNECTED;
  const String address = provisioningMode
                             ? WiFi.softAPIP().toString()
                             : WiFi.localIP().toString();
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.drawRect(1, 1, width - 2, height - 2, TFT_DARKGREEN);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Display.drawString(provisioningMode ? "WIFI SETUP ACTIVE" : "WEB UI ACTIVE", 12, 16);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.drawString(hasAddress ? "http://" + address : "NO WIFI", 12, 48);
  M5.Display.drawString(provisioningMode ? "AP: PlaneTracker-Setup" : debugServerStarted ? "SERVER ON" : "NO WIFI", 12, 70);
  drawBatteryIndicator(width - 4, 5);
  M5.Display.setTextColor(TFT_DARKGREEN, TFT_BLACK);
  M5.Display.drawString("BLUE DISABLE", 12, height - 22);
  M5.Display.drawString("BLUE HOLD IP", width - 76, height - 22);
  webUiNeedsRedraw = false;
}

void drawFirmwareUpdate() {
  const int width = M5.Display.width();
  const int height = M5.Display.height();
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.drawRect(1, 1, width - 2, height - 2, TFT_DARKGREEN);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Display.drawString("FIRMWARE UPDATE", 10, 8);
  M5.Display.setTextColor(updateScreenError ? TFT_RED : updateReady ? TFT_GREEN : TFT_WHITE, TFT_BLACK);
  M5.Display.drawString(updateScreenError ? "UPDATE FAILED" : updateReady ? "UPDATE READY" : "UPLOADING", 10, 32);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.drawString(String(updateBytesWritten) + " bytes", 10, 52);
  if (!updateScreenError && !updateReady) {
    M5.Display.drawRect(10, 72, width - 20, 12, TFT_DARKGREEN);
    M5.Display.fillRect(12, 74, min((int)(updateBytesWritten / 8192), width - 24), 8, TFT_GREEN);
  } else if (updateReady) {
    M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5.Display.drawString("PRESS BLUE TO REBOOT", 10, 76);
  } else {
    M5.Display.setTextColor(TFT_RED, TFT_BLACK);
    M5.Display.drawString(scrollingText(updateFailure, 20), 10, 76);
  }
  M5.Display.setTextColor(TFT_DARKGREEN, TFT_BLACK);
  M5.Display.drawString(updateReady ? "BLUE REBOOT" : "BLUE BACK", 10, height - 14);
  drawBatteryIndicator(width - 4, 5);
  updateScreenNeedsRedraw = false;
}

void enterTracker() {
  appMode = MODE_TRACKER;
  lastDraw = 0;
  webUiNeedsRedraw = true;
  fetchPlanes();
}

void enterWebUi() {
  connectWifi();
  startDebugServer();
  appMode = MODE_WEB_UI;
  lastDraw = 0;
  webUiNeedsRedraw = true;
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
  if (controlNext()) {
    menuSelection = (menuSelection + 1) % 2;
    menuNeedsRedraw = true;
  }
  if (controlPrevious()) {
    menuSelection = menuSelection == 0 ? 1 : menuSelection - 1;
    menuNeedsRedraw = true;
  }
  if (controlSelect()) {
    if (menuSelection == 0) {
      enterTracker();
    } else if (menuSelection == 1) {
      enterWebUi();
    }
  }
  if (controlShowIp()) {
    ipOverlayUntil = millis() + 5000;
    ipOverlayDrawn = false;
  }
}

void handleTrackerButtons() {
  if (controlExit()) {
    exitToMenu();
    return;
  }
  if (controlShowIp()) {
    ipOverlayUntil = millis() + 5000;
    ipOverlayDrawn = false;
    return;
  }
  if (controlNext() && planeCount > 0) {
    selectedPlane = (selectedPlane + 1) % planeCount;
    fetchSelectedRoute();
  }
  if (controlPrevious() && planeCount > 0) {
    selectedPlane = selectedPlane == 0 ? planeCount - 1 : selectedPlane - 1;
    fetchSelectedRoute();
  }
  if (controlSelect()) fetchPlanes();
}

void handleWebUiButtons() {
  if (controlExit()) exitToMenu();
  if (controlSelect()) {
    stopDebugServer();
    exitToMenu();
    return;
  }
  if (controlShowIp()) {
    ipOverlayUntil = millis() + 5000;
    ipOverlayDrawn = false;
  }
}

void handleFirmwareUpdateButtons() {
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
  Serial.print("Reset reason: ");
  Serial.println((int)esp_reset_reason());
  auto configuration = M5.config();
  M5.begin(configuration);
  M5.Display.setRotation(1);
  displayRotation = 1;
  M5.Display.setTextFont(2);
  loadSettings();
  connectWifi();
  startDebugServer();
}

void loop() {
  M5.update();
  if (debugServerStarted) webServer.handleClient();
  if (appMode == MODE_MENU) {
    handleMenuButtons();
    if (menuNeedsRedraw) drawMenu();
  } else if (appMode == MODE_TRACKER) {
    handleTrackerButtons();
    updateAutoRotation();
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
      if (millis() - lastRefresh >= settings.refreshIntervalMs) fetchPlanes();
      if (millis() - lastDraw >= DISPLAY_FRAME_MS) {
        lastDraw = millis();
        drawRadar();
        sweepAngle = fmodf(sweepAngle + 4.0f, 360.0f);
      }
    }
  } else if (appMode == MODE_WEB_UI) {
    handleWebUiButtons();
    if (!ipOverlayActive() && ipOverlayDrawn) {
      ipOverlayDrawn = false;
      webUiNeedsRedraw = true;
    }
    if (webUiNeedsRedraw) drawWebUi();
    if (ipOverlayActive() && !ipOverlayDrawn) {
      drawIpOverlay();
      ipOverlayDrawn = true;
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
