// Captive portal HTTP routes, JSON API responses, and status serialization.

static String gpsDopString() {
  TinyGPSCustom *fields[] = {&pdopGn, &pdopGp, &hdopGn, &hdopGp, &vdopGn, &vdopGp};
  for (TinyGPSCustom *field : fields) {
    if (field->isValid()) {
      return String(field->value());
    }
  }
  if (gps.hdop.isValid()) {
    return String(gps.hdop.hdop(), 1);
  }
  return "";
}

static void closeHttpConnection() {
  WiFiClient client = server.client();
  client.flush();
  client.stop();
}

static void sendStaticResponse(int code, const char *contentType, const char *body) {
  server.send(code, contentType, body);
  closeHttpConnection();
}

static void sendJson(JsonDocument &doc) {
  String output;
  output.reserve(measureJson(doc));
  serializeJson(doc, output);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", output);
  closeHttpConnection();
}

static bool streamLittleFsFile(File &file, const char *contentType) {
  if (!file) {
    return false;
  }

  uint8_t *buffer = static_cast<uint8_t *>(
      heap_caps_malloc_prefer(FILE_STREAM_BUFFER_SIZE, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT));
  if (!buffer) {
    buffer = static_cast<uint8_t *>(heap_caps_malloc(FILE_STREAM_BUFFER_SIZE, MALLOC_CAP_8BIT));
  }
  if (!buffer) {
    sendStaticResponse(500, "text/plain", "stream buffer allocation failed");
    return false;
  }

  server.setContentLength(file.size());
  server.send(200, contentType, "");

  bool ok = true;
  while (file.available()) {
    int bytesRead = file.read(buffer, FILE_STREAM_BUFFER_SIZE);
    if (bytesRead <= 0) {
      ok = false;
      break;
    }
    server.sendContent(reinterpret_cast<const char *>(buffer), static_cast<size_t>(bytesRead));
    yield();
  }

  heap_caps_free(buffer);
  closeHttpConnection();
  return ok;
}

static bool clientAcceptsGzip() {
  String acceptEncoding = server.header("Accept-Encoding");
  acceptEncoding.toLowerCase();
  return acceptEncoding.indexOf("gzip") >= 0;
}

static void handleRoot() {
  bool useGzip = clientAcceptsGzip() && portalGzipPresent;
  File file = LittleFS.open(useGzip ? PORTAL_GZIP_PATH : PORTAL_HTML_PATH, "r");
  if (!file) {
    sendStaticResponse(500, "text/html",
                       "<!doctype html><html><body><h1>T-Beam NTP Server</h1>"
                       "<p>Portal file missing from LittleFS. Upload the filesystem image.</p></body></html>");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Vary", "Accept-Encoding");
  if (useGzip) {
    server.sendHeader("Content-Encoding", "gzip");
  }
  streamLittleFsFile(file, "text/html");
  file.close();
}

static void handleSettingsGet() {
  PsramJsonDocument doc(12288);
  settingsToJson(doc);
  sendJson(doc);
}

static void handleTimezones() {
  File file = LittleFS.open(TIMEZONE_DB_PATH, "r");
  if (!file) {
    sendStaticResponse(404, "text/plain", "timezone preset file not found");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  streamLittleFsFile(file, "text/tab-separated-values");
  file.close();
}

static void handleSettingsSave() {
  if (!server.hasArg("plain")) {
    sendStaticResponse(400, "application/json", "{\"ok\":false,\"error\":\"missing body\"}");
    return;
  }
  PsramJsonDocument doc(12288);
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  if (error) {
    sendStaticResponse(400, "application/json", "{\"ok\":false,\"error\":\"invalid json\"}");
    return;
  }
  applySettingsJson(doc);
  applyPowerSettings();
  if (!saveSettings()) {
    sendStaticResponse(500, "application/json", "{\"ok\":false,\"error\":\"save failed\"}");
    return;
  }
  sendStaticResponse(200, "application/json", "{\"ok\":true,\"rebootRecommended\":true}");
}

static void handleScan() {
  PsramJsonDocument doc(12288);
  JsonArray networks = doc.createNestedArray("networks");
  int count = WiFi.scanNetworks(false, true, false, 180);
  if (count < 0) {
    doc["error"] = "scan failed";
    sendJson(doc);
    return;
  }
  for (int i = 0; i < count && i < 40; ++i) {
    wifi_auth_mode_t authMode = WiFi.encryptionType(i);
    JsonObject n = networks.createNestedObject();
    n["ssid"] = WiFi.SSID(i);
    n["rssi"] = WiFi.RSSI(i);
    n["channel"] = WiFi.channel(i);
    n["secure"] = authMode != WIFI_AUTH_OPEN;
    n["auth"] = wifiAuthModeLabel(authMode);
  }
  doc["count"] = count;
  WiFi.scanDelete();
  sendJson(doc);
}

static void handleReboot() {
  sendStaticResponse(200, "application/json", "{\"ok\":true}");
  delay(250);
  ESP.restart();
}

struct StatusSnapshot {
  uint64_t nowUsec;
  bool synced;
  bool servingTime;
  int16_t localOffsetMinutes;
  bool gpsFix;
};

static StatusSnapshot captureStatusSnapshot() {
  uint64_t nowUsec = 0;
  bool synced = getClockUnixUsec(nowUsec);
  bool servingTime = synced && hasFreshGpsTime();
  return {
      nowUsec,
      synced,
      servingTime,
      servingTime ? currentLocalOffsetMinutes(nowUsec) : currentLocalOffsetMinutes(),
      hasFreshGpsFix()};
}

static void writeStatusRootJson(JsonDocument &doc, const StatusSnapshot &snapshot) {
  doc["mode"] = networkMode == NetworkMode::Sta ? "STA" : "AP";
  doc["networkMode"] = networkStartModeName(settings.networkStartMode);
  doc["version"] = FIRMWARE_VERSION;
  doc["hardwareRevision"] = hardwareRevisionLabel();
  doc["pmic"] = pmicModelLabel();
  doc["pmicRegister03Valid"] = pmicRegister03Valid;
  doc["pmicRegister03"] = pmicRegister03Valid ? pmicRegister03 : -1;
  doc["ip"] = networkMode == NetworkMode::Sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  doc["hostname"] = sanitizedHostname();
  doc["clients"] = networkMode == NetworkMode::ApFallback ? WiFi.softAPgetStationNum() : 0;
  doc["apSecurity"] = apSecurityModeName(settings.apSecurityMode);
  doc["utc"] = snapshot.servingTime ? formatUtc(snapshot.nowUsec) : "";
  doc["local"] = snapshot.servingTime ? formatLocal(snapshot.nowUsec) : "";
  doc["localOffsetMinutes"] = snapshot.localOffsetMinutes;
  doc["timeZone"] = snapshot.servingTime ? localTimezoneName(snapshot.nowUsec) : "";
  doc["localTimeMode"] = localTimeModeName(settings.localTimeMode);
  doc["ianaTimeZone"] = settings.ianaTimeZone;
  doc["timeZoneDatabasePresent"] = timezoneDbPresent;
  doc["dstActive"] = snapshot.servingTime && isLocalDstActive(snapshot.nowUsec);
  doc["ntpRequests"] = ntpRequestCount;
  doc["ntpResponses"] = ntpResponseCount;
  doc["ntpSuppressed"] = ntpSuppressedCount;
  doc["dnsQueries"] = dnsQueryCount;
  doc["dnsAliasHits"] = dnsAliasHitCount;
}

static void writeClockStatusJson(JsonDocument &doc, const StatusSnapshot &snapshot) {
  JsonObject clock = doc.createNestedObject("clock");
  clock["synced"] = snapshot.servingTime;
  clock["hasClock"] = snapshot.synced;
  clock["pps"] = hasRecentPps();
  clock["lastSyncMs"] = lastClockSyncMs;
  clock["lastGpsTimeAgeMs"] = lastNmeaUpdateMs ? static_cast<int32_t>(millis() - lastNmeaUpdateMs) : -1;
  clock["lastPpsAgeMs"] = lastPpsMs ? static_cast<int32_t>(millis() - lastPpsMs) : -1;
}

static void writeGpsStatusJson(JsonDocument &doc, const StatusSnapshot &snapshot) {
  JsonObject gpsJson = doc.createNestedObject("gps");
  gpsJson["seen"] = gps.charsProcessed() > 0;
  gpsJson["fix"] = snapshot.gpsFix;
  gpsJson["sats"] = gps.satellites.isValid() ? gps.satellites.value() : 0;
  gpsJson["lat"] = snapshot.gpsFix ? gps.location.lat() : 0.0;
  gpsJson["lon"] = snapshot.gpsFix ? gps.location.lng() : 0.0;
  gpsJson["alt"] = gps.altitude.isValid() ? gps.altitude.meters() : 0.0;
  gpsJson["dop"] = gpsDopString();
  gpsJson["grid"] = snapshot.gpsFix ? maidenhead(gps.location.lat(), gps.location.lng()) : "";
  gpsJson["chars"] = gps.charsProcessed();
  gpsJson["sentences"] = gps.sentencesWithFix();
}

static void writePowerStatusJson(JsonDocument &doc) {
  JsonObject power = doc.createNestedObject("power");
  power["online"] = powerState.online;
  power["pmic"] = pmicModelLabel();
  power["pmicRegister03Valid"] = pmicRegister03Valid;
  power["pmicRegister03"] = pmicRegister03Valid ? pmicRegister03 : -1;
  power["batteryPresent"] = powerState.batteryPresent;
  power["charging"] = powerState.charging;
  power["discharging"] = powerState.discharging;
  power["vbusPresent"] = powerState.vbusPresent;
  power["batteryMv"] = powerState.batteryMv;
  power["batteryCurrentMa"] = powerState.batteryCurrentMa;
  power["batteryPercent"] = powerState.batteryPercent;
  power["vbusMv"] = powerState.vbusMv;
  power["vbusCurrentMa"] = powerState.vbusCurrentMa;
  power["systemMv"] = powerState.systemMv;
  power["temperatureC"] = powerState.temperatureC;
  power["warning"] = powerState.warning;
  power["cutoffPending"] = powerState.cutoffPending;
}

static void writeHeapStatusJson(JsonDocument &doc) {
  JsonObject heap = doc.createNestedObject("heap");
  heap["free"] = ESP.getFreeHeap();
  heap["minFree"] = ESP.getMinFreeHeap();
  heap["psramSize"] = ESP.getPsramSize();
  heap["psramFree"] = ESP.getFreePsram();
}

static void writeDisplayStatusJson(JsonDocument &doc) {
  JsonObject displaySettings = doc.createNestedObject("display");
  displaySettings["page"] = oledPage;
  displaySettings["sleeping"] = oledSleeping;
}

static void handleStatus() {
  StatusSnapshot snapshot = captureStatusSnapshot();
  PsramJsonDocument doc(12288);
  writeStatusRootJson(doc, snapshot);
  writeClockStatusJson(doc, snapshot);
  writeGpsStatusJson(doc, snapshot);
  writePowerStatusJson(doc);
  writeHeapStatusJson(doc);
  doc["i2c"] = i2cDevices;
  writeDisplayStatusJson(doc);
  sendJson(doc);
}

static String portalBaseUrl() {
  IPAddress address = networkMode == NetworkMode::Sta ? WiFi.localIP() : WiFi.softAPIP();
  return String("http://") + address.toString() + "/";
}

static void handleNotFound() {
  if (networkMode == NetworkMode::ApFallback) {
    server.sendHeader("Location", portalBaseUrl(), true);
    sendStaticResponse(302, "text/plain", "Redirecting");
  } else {
    sendStaticResponse(404, "text/plain", "Not found");
  }
}

static void handleCaptiveProbe() {
  server.sendHeader("Location", portalBaseUrl(), true);
  sendStaticResponse(302, "text/plain", "Redirecting");
}

static void beginWeb() {
  static const char *headerKeys[] = {"Accept-Encoding"};
  server.collectHeaders(headerKeys, 1);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/settings", HTTP_GET, handleSettingsGet);
  server.on("/api/timezones", HTTP_GET, handleTimezones);
  server.on("/api/save", HTTP_POST, handleSettingsSave);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/generate_204", HTTP_GET, handleCaptiveProbe);
  server.on("/hotspot-detect.html", HTTP_GET, handleCaptiveProbe);
  server.on("/connecttest.txt", HTTP_GET, handleCaptiveProbe);
  server.on("/ncsi.txt", HTTP_GET, handleCaptiveProbe);
  server.onNotFound(handleNotFound);
  server.begin();
}

