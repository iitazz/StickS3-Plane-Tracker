/*
 * Rome ADS-B Radar for Bruce JS
 * Device: M5StickC Plus2
 *
 * Area: Fiumicino Airport (FCO/LIRF), 65 km display radius
 *
 * Controls:
 *   Previous / Up   -> previous aircraft
 *   Next / Down     -> next aircraft
 *   Select          -> refresh aircraft and selected route
 *   Escape / Back   -> exit
 */
 
var display = require("display");
var keyboard = require("keyboard");
var wifi = require("wifi");
 
var ROME_LAT = 41.9028;
var ROME_LON = 12.4964;
var RANGE_KM = 65;
var REFRESH_MS = 15000;
var FRAME_MS = 300;
var MAX_PLANES = 40;
 
var API_URL = "https://opensky-network.org/api/states/all?lamin=41.3&lomin=11.8&lamax=42.5&lomax=13.2";
var ROUTE_URL = "http://api.adsb.lol/api/0/routeset";
var ADSBDB_ROUTE_URL = "http://api.adsbdb.com/v0/callsign/";
 
var BLACK = display.color(0, 0, 0);
var DARK_GREEN = display.color(0, 55, 12);
var GRID_GREEN = display.color(0, 105, 28);
var GREEN = display.color(0, 255, 70);
var DIM_GREEN = display.color(0, 150, 45);
var WHITE = display.color(240, 255, 240);
var YELLOW = display.color(255, 220, 40);
var RED = display.color(255, 55, 55);
var CYAN = display.color(50, 220, 255);
 
var screenW = display.width();
var screenH = display.height();
 
var radarDiameter = screenH - 8;
if (radarDiameter > screenW - 98) {
  radarDiameter = screenW - 98;
}
if (radarDiameter < 70) {
  radarDiameter = screenH - 8;
}
 
var radarRadius = Math.floor(radarDiameter / 2);
var radarX = radarRadius + 4;
var radarY = Math.floor(screenH / 2);
var panelX = radarX + radarRadius + 5;
var panelW = screenW - panelX;
 
var planes = [];
var selectedIndex = 0;
var lastRefresh = 0;
var lastStatus = "START";
var sweepAngle = 0;
var running = true;
var panelDirty = true;
var routeCache = {};
 
function trimText(value) {
  if (value === undefined || value === null) {
    return "";
  }
 
  return String(value).replace(/^\s+|\s+$/g, "");
}
 
function valueOr(value, fallback) {
  if (value === undefined || value === null || value === "") {
    return fallback;
  }
 
  return value;
}
 
function clamp(value, minimum, maximum) {
  if (value < minimum) {
    return minimum;
  }
  if (value > maximum) {
    return maximum;
  }
  return value;
}
 
function formatNumber(value, decimals, fallback) {
  if (typeof value !== "number" || isNaN(value)) {
    return fallback;
  }
 
  return value.toFixed(decimals);
}
 
function formatAltitude(value) {
  if (value === "ground") {
    return "GROUND";
  }
 
  if (typeof value === "number" && !isNaN(value)) {
    return Math.round(value * 0.3048) + " m";
  }
 
  return "-- m";
}
 
function distanceKm(lat1, lon1, lat2, lon2) {
  var toRad = Math.PI / 180;
  var dLat = (lat2 - lat1) * toRad;
  var dLon = (lon2 - lon1) * toRad;
  var a =
    Math.sin(dLat / 2) * Math.sin(dLat / 2) +
    Math.cos(lat1 * toRad) *
      Math.cos(lat2 * toRad) *
      Math.sin(dLon / 2) *
      Math.sin(dLon / 2);
 
  var c = 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
  return 6371.0088 * c;
}
 
function bearingDegrees(lat1, lon1, lat2, lon2) {
  var toRad = Math.PI / 180;
  var y = Math.sin((lon2 - lon1) * toRad) * Math.cos(lat2 * toRad);
  var x =
    Math.cos(lat1 * toRad) * Math.sin(lat2 * toRad) -
    Math.sin(lat1 * toRad) *
      Math.cos(lat2 * toRad) *
      Math.cos((lon2 - lon1) * toRad);
 
  var bearing = Math.atan2(y, x) / toRad;
  if (bearing < 0) {
    bearing += 360;
  }
 
  return bearing;
}
 
function radarPoint(plane) {
  var distance = distanceKm(ROME_LAT, ROME_LON, plane.lat, plane.lon);
  var bearing = bearingDegrees(ROME_LAT, ROME_LON, plane.lat, plane.lon);
  var angle = (bearing - 90) * Math.PI / 180;
  var radial = clamp(distance / RANGE_KM, 0, 1) * radarRadius;
 
  return {
    x: Math.round(radarX + Math.cos(angle) * radial),
    y: Math.round(radarY + Math.sin(angle) * radial),
    distance: distance,
    bearing: bearing
  };
}
 
 
function routeKey(plane) {
  if (!plane) {
    return "";
  }
 
  return trimText(plane.flight)
    .replace(/\s+/g, "")
    .toUpperCase();
}
 
function formatRoute(route) {
  if (!route) {
    return "";
  }
 
  return String(route)
    .replace(/-/g, " > ")
    .replace(/~/g, " > ")
    .replace(/,/g, " > ")
    .replace(/\s+/g, " ");
}
 
function parseAdsbLolRoute(responseBody) {
  var payload = JSON.parse(responseBody);
 
  if (typeof payload === "string") {
    payload = JSON.parse(payload);
  }
 
  var result = null;
  var route = "";
 
  if (payload && payload.length > 0) {
    result = payload[0];
  } else if (payload && payload.planes && payload.planes.length > 0) {
    result = payload.planes[0];
  }
 
  if (!result) {
    return "";
  }
 
  if (
    result._airport_codes_iata &&
    result._airport_codes_iata !== "unknown"
  ) {
    route = trimText(result._airport_codes_iata);
  } else if (
    result.airport_codes &&
    result.airport_codes !== "unknown"
  ) {
    route = trimText(result.airport_codes);
  } else if (
    result.airport_codes_iata &&
    result.airport_codes_iata !== "unknown"
  ) {
    route = trimText(result.airport_codes_iata);
  }
 
  if (!route) {
    return "";
  }
 
  route = formatRoute(route);
 
  if (result.plausible === false) {
    route += "?";
  }
 
  return route;
}
 
function fetchAdsbLolRoute(plane, callsign) {
  try {
    var requestBody = JSON.stringify({
      planes: [
        {
          callsign: callsign,
          lat: plane.lat,
          lng: plane.lon
        }
      ]
    });
 
    var response = wifi.httpFetch(ROUTE_URL, {
      method: "POST",
      body: requestBody,
      responseType: "string",
      headers: [
        ["Content-Type", "application/json"],
        ["Accept", "application/json"]
      ]
    });
 
    if (!response || !response.ok || !response.body) {
      return "";
    }
 
    return parseAdsbLolRoute(response.body);
  } catch (error) {
    return "";
  }
}
 
function fetchAdsbDbRoute(callsign) {
  try {
    var response = wifi.httpFetch(
      ADSBDB_ROUTE_URL + encodeURIComponent(callsign)
    );
 
    if (!response || !response.ok || !response.body) {
      return "";
    }
 
    var payload = JSON.parse(response.body);
    var flightRoute = null;
 
    if (
      payload &&
      payload.response &&
      payload.response.flightroute
    ) {
      flightRoute = payload.response.flightroute;
    }
 
    if (
      !flightRoute ||
      !flightRoute.origin ||
      !flightRoute.destination
    ) {
      return "";
    }
 
    var origin =
      trimText(flightRoute.origin.iata_code) ||
      trimText(flightRoute.origin.icao_code);
 
    var destination =
      trimText(flightRoute.destination.iata_code) ||
      trimText(flightRoute.destination.icao_code);
 
    if (!origin || !destination) {
      return "";
    }
 
    return origin + " > " + destination;
  } catch (error) {
    return "";
  }
}
 
function fetchRouteForSelected() {
  if (planes.length === 0 || selectedIndex >= planes.length) {
    return;
  }
 
  var plane = planes[selectedIndex];
  var callsign = routeKey(plane);
 
  if (!callsign) {
    plane.route = "NO CALLSIGN";
    panelDirty = true;
    return;
  }
 
  if (routeCache[callsign] !== undefined) {
    plane.route = routeCache[callsign];
    panelDirty = true;
    return;
  }
 
  plane.route = "ROUTE...";
  panelDirty = true;
  drawPanel();
 
  var route = fetchAdsbLolRoute(plane, callsign);
 
  if (!route) {
    plane.route = "TRY ADSBDB";
    panelDirty = true;
    drawPanel();
 
    route = fetchAdsbDbRoute(callsign);
  }
 
  if (route) {
    plane.route = route;
  } else {
    plane.route = "NO ROUTE";
  }
 
  routeCache[callsign] = plane.route;
  panelDirty = true;
}
 
function fetchPlanes() {
  lastStatus = "SYNC";
  panelDirty = true;
 
  if (!wifi.connected()) {
    wifi.connectDialog();
  }
 
  if (!wifi.connected()) {
    lastStatus = "NO WIFI";
    lastRefresh = now();
    panelDirty = true;
    return;
  }
 
  try {
    var response = wifi.httpFetch(API_URL, {
      responseType: "string",
      headers: [
        ["Accept", "application/json"],
        ["User-Agent", "Mozilla/5.0"]
      ]
    });
 
    if (!response) {
      lastStatus = "NO RESPONSE";
      lastRefresh = now();
      panelDirty = true;
      return;
    }
 
    if (!response.ok) {
      lastStatus = "HTTP " + response.status;
      lastRefresh = now();
      panelDirty = true;
      return;
    }
 
    if (!response.body || response.body.length === 0) {
      lastStatus = "EMPTY DATA";
      lastRefresh = now();
      panelDirty = true;
      return;
    }
 
    var payload = JSON.parse(response.body);
 
    var source = payload.ac || payload.aircraft || [];
    var normalizedSource = [];
    var stateIndex;

    /* Convert OpenSky state vectors and SI units to the radar's aircraft shape. */
    if (payload.states && payload.states.length) {
      for (stateIndex = 0; stateIndex < payload.states.length; stateIndex++) {
        var state = payload.states[stateIndex];
        normalizedSource.push({
          hex: state[0],
          flight: state[1],
          lat: state[6],
          lon: state[5],
          alt_baro:
            typeof state[7] === "number" ? state[7] / 0.3048 : state[7],
          gs: typeof state[9] === "number" ? state[9] / 0.514444 : state[9],
          track: state[10],
          baro_rate:
            typeof state[11] === "number" ? state[11] / 0.00508 : state[11],
          squawk: state[14]
        });
      }
      source = normalizedSource;
    }
    var nextPlanes = [];
    var i;
 
    for (i = 0; i < source.length && nextPlanes.length < MAX_PLANES; i++) {
      var aircraft = source[i];
 
      if (
        typeof aircraft.lat !== "number" ||
        typeof aircraft.lon !== "number" ||
        isNaN(aircraft.lat) ||
        isNaN(aircraft.lon)
      ) {
        continue;
      }
 
      var distance = distanceKm(
        ROME_LAT,
        ROME_LON,
        aircraft.lat,
        aircraft.lon
      );
 
      if (distance > RANGE_KM) {
        continue;
      }
 
      nextPlanes.push({
        hex: trimText(aircraft.hex),
        flight: trimText(aircraft.flight),
        registration: trimText(aircraft.r),
        aircraftType: trimText(aircraft.t),
        lat: aircraft.lat,
        lon: aircraft.lon,
        altitude: aircraft.alt_baro,
        speed: aircraft.gs,
        track: aircraft.track,
        verticalRate: aircraft.baro_rate,
        squawk: trimText(aircraft.squawk),
        seen: aircraft.seen,
        distance: distance,
        route: routeCache[trimText(aircraft.flight)] || ""
      });
    }
 
    /* Sort nearest aircraft first. */
    nextPlanes.sort(function (a, b) {
      return a.distance - b.distance;
    });
 
    planes = nextPlanes;
 
    if (planes.length === 0) {
      selectedIndex = 0;
      lastStatus = "NO TRAFFIC";
    } else {
      if (selectedIndex >= planes.length) {
        selectedIndex = planes.length - 1;
      }
      if (selectedIndex < 0) {
        selectedIndex = 0;
      }
      lastStatus = "ADSB.LOL";
    }
  } catch (error) {
    /*
     * Some Bruce builds do not provide the global println() function.
     * Do not log here, because logging must never crash the app.
     */
    var message = String(error);
    if (message.length > 12) {
      message = message.substring(0, 12);
    }
    lastStatus = "ERR " + message;
    panelDirty = true;
  }
 
  lastRefresh = now();
  panelDirty = true;
 
  if (planes.length > 0) {
    fetchRouteForSelected();
  }
}
 
function drawRadarGrid() {
  display.drawCircle(radarX, radarY, radarRadius, GRID_GREEN);
  display.drawCircle(
    radarX,
    radarY,
    Math.floor(radarRadius * 0.66),
    DARK_GREEN
  );
  display.drawCircle(
    radarX,
    radarY,
    Math.floor(radarRadius * 0.33),
    DARK_GREEN
  );
 
  display.drawLine(
    radarX - radarRadius,
    radarY,
    radarX + radarRadius,
    radarY,
    DARK_GREEN
  );
  display.drawLine(
    radarX,
    radarY - radarRadius,
    radarX,
    radarY + radarRadius,
    DARK_GREEN
  );
 
  display.setTextSize(1);
  display.setTextColor(DIM_GREEN);
  display.drawText("N", radarX - 3, radarY - radarRadius + 2);
  display.drawText(
    String(RANGE_KM) + "km",
    radarX - 8,
    radarY + radarRadius - 10
  );
 
  display.drawFillCircle(radarX, radarY, 2, GREEN);
}
 
function drawSweep() {
  var angle = (sweepAngle - 90) * Math.PI / 180;
  var endX = Math.round(radarX + Math.cos(angle) * radarRadius);
  var endY = Math.round(radarY + Math.sin(angle) * radarRadius);
 
  var trailAngle = (sweepAngle - 97) * Math.PI / 180;
  var trailX = Math.round(radarX + Math.cos(trailAngle) * radarRadius);
  var trailY = Math.round(radarY + Math.sin(trailAngle) * radarRadius);
 
  display.drawLine(radarX, radarY, trailX, trailY, DARK_GREEN);
  display.drawLine(radarX, radarY, endX, endY, GREEN);
}
 
function drawPlanes() {
  var i;
 
  for (i = 0; i < planes.length; i++) {
    var point = radarPoint(planes[i]);
 
    if (i === selectedIndex) {
      display.drawCircle(point.x, point.y, 4, WHITE);
      display.drawFillCircle(point.x, point.y, 2, YELLOW);
    } else {
      display.drawFillCircle(point.x, point.y, 2, GREEN);
    }
  }
}
 
function drawPanelLine(text, y, color) {
  display.setTextSize(1);
  display.setTextColor(color || GREEN);
  display.drawText(text, panelX, y);
}
 
function drawInfoPanel() {
  display.drawLine(panelX - 3, 0, panelX - 3, screenH, GRID_GREEN);
 
  drawPanelLine("ROME ADS-B", 3, CYAN);
  drawPanelLine(lastStatus + " " + planes.length, 14, lastStatus === "ADSB.LOL" ? GREEN : YELLOW);
 
  if (planes.length === 0) {
    drawPanelLine("No aircraft", 34, WHITE);
    drawPanelLine("SEL refresh", 48, DIM_GREEN);
    drawPanelLine("ESC exit", 59, DIM_GREEN);
    return;
  }
 
  var plane = planes[selectedIndex];
  var point = radarPoint(plane);
  var callsign = valueOr(plane.flight, valueOr(plane.registration, plane.hex));
 
  if (callsign.length > 14) {
    callsign = callsign.substring(0, 14);
  }
 
  drawPanelLine(
    (selectedIndex + 1) + "/" + planes.length + " " + callsign,
    27,
    YELLOW
  );
  var routeText = plane.route || "ROUTE...";
  if (routeText.length > 15) {
    routeText = routeText.substring(0, 15);
  }
 
  drawPanelLine("RTE " + routeText, 39, WHITE);
  drawPanelLine("ALT " + formatAltitude(plane.altitude), 51, GREEN);
  drawPanelLine(
    "SPD " +
      formatNumber(
        typeof plane.speed === "number" ? plane.speed * 1.852 : plane.speed,
        0,
        "--"
      ) +
      " km/h",
    63,
    GREEN
  );
  drawPanelLine(
    "HDG " + formatNumber(plane.track, 0, "--") + " deg",
    75,
    GREEN
  );
  drawPanelLine(
    "DST " + formatNumber(point.distance, 1, "--") + " km",
    87,
    GREEN
  );
 
  var verticalText = "--";
  if (typeof plane.verticalRate === "number" && !isNaN(plane.verticalRate)) {
    var verticalMetric = plane.verticalRate * 0.00508;
 
    if (verticalMetric > 0) {
      verticalText = "+" + verticalMetric.toFixed(1);
    } else {
      verticalText = verticalMetric.toFixed(1);
    }
  }
 
  drawPanelLine("V/S " + verticalText + " m/s", 99, GREEN);
  drawPanelLine("PREV/NEXT select", 115, DIM_GREEN);
  drawPanelLine("SEL refresh", 126, DIM_GREEN);
}
 
function drawRadarFrame() {
  /*
   * Clear only the radar region. The information panel remains untouched,
   * avoiding full-screen flashing on every animation frame.
   */
  display.drawFillRect(
    0,
    0,
    panelX - 4,
    screenH,
    BLACK
  );
 
  drawRadarGrid();
  drawPlanes();
  drawSweep();
}
 
function drawPanel() {
  display.drawFillRect(
    panelX - 3,
    0,
    screenW - panelX + 3,
    screenH,
    BLACK
  );
  drawInfoPanel();
  panelDirty = false;
}
 
function selectPrevious() {
  if (planes.length === 0) {
    return;
  }
 
  selectedIndex--;
  if (selectedIndex < 0) {
    selectedIndex = planes.length - 1;
  }
  panelDirty = true;
  fetchRouteForSelected();
}
 
function selectNext() {
  if (planes.length === 0) {
    return;
  }
 
  selectedIndex++;
  if (selectedIndex >= planes.length) {
    selectedIndex = 0;
  }
  panelDirty = true;
  fetchRouteForSelected();
}
 
function handleOptionalKeyboardKeys() {
  /*
   * getKeysPressed() is useful on devices/keyboards that Bruce maps as keys.
   * Stock Bruce may not expose the original I2C CardKB here, but these
   * checks are harmless and provide compatibility if support is added.
   */
  var keys;
 
  try {
    keys = keyboard.getKeysPressed();
  } catch (error) {
    return;
  }
 
  if (!keys || keys.length === 0) {
    return;
  }
 
  var i;
  for (i = 0; i < keys.length; i++) {
    var key = String(keys[i]);
 
    if (key === "ArrowUp" || key === "Up" || key === "UP") {
      selectPrevious();
    } else if (
      key === "ArrowDown" ||
      key === "Down" ||
      key === "DOWN"
    ) {
      selectNext();
    } else if (key === "Enter") {
      fetchPlanes();
    } else if (key === "Escape" || key === "Esc") {
      running = false;
    }
  }
}
 
display.fill(BLACK);
display.setTextColor(GREEN);
display.setTextSize(2);
display.drawText("ROME RADAR", 12, 35);
display.setTextSize(1);
display.drawText("Connecting to ADS-B...", 12, 62);
 
display.fill(BLACK);
fetchPlanes();
drawPanel();
 
while (running) {
  if (keyboard.getPrevPress()) {
    selectPrevious();
  }
 
  if (keyboard.getNextPress()) {
    selectNext();
  }
 
  if (keyboard.getSelPress()) {
    fetchPlanes();
  }
 
  if (keyboard.getEscPress()) {
    running = false;
  }
 
  handleOptionalKeyboardKeys();
 
  if (now() - lastRefresh >= REFRESH_MS) {
    fetchPlanes();
  }
 
  drawRadarFrame();
 
  if (panelDirty) {
    drawPanel();
  }

  sweepAngle += 12;
  if (sweepAngle >= 360) {
    sweepAngle -= 360;
  }
 
  delay(FRAME_MS);
}
 
display.fill(BLACK);
display.setTextColor(GREEN);
display.setTextSize(1);
display.drawText("Radar closed", 10, 10);
delay(500);