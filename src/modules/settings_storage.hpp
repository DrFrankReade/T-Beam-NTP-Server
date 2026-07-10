// Settings defaults, validation, JSON serialization, LittleFS persistence, and identity helpers.

static void copyText(char *dst, size_t len, const char *src) {
  if (len == 0) {
    return;
  }
  if (!src) {
    src = "";
  }
  snprintf(dst, len, "%s", src);
}

static const char *networkStartModeName(NetworkStartMode mode) {
  switch (mode) {
    case NetworkStartMode::StandaloneAp:
      return "standaloneAp";
    case NetworkStartMode::ClientWithApFallback:
      return "clientWithApFallback";
  }
  return "standaloneAp";
}

static NetworkStartMode parseNetworkStartMode(const char *text, NetworkStartMode fallback) {
  if (!text) {
    return fallback;
  }
  if (strcmp(text, "standaloneAp") == 0 || strcmp(text, "standalone") == 0) {
    return NetworkStartMode::StandaloneAp;
  }
  if (strcmp(text, "clientWithApFallback") == 0 || strcmp(text, "client") == 0) {
    return NetworkStartMode::ClientWithApFallback;
  }
  return fallback;
}

static const char *apSecurityModeName(ApSecurityMode mode) {
  switch (mode) {
    case ApSecurityMode::Open:
      return "open";
    case ApSecurityMode::Wpa2:
      return "wpa2";
    case ApSecurityMode::WpaWpa2:
      return "wpa-wpa2";
    case ApSecurityMode::Wpa2Wpa3:
      return "wpa2-wpa3";
    case ApSecurityMode::Wpa3:
      return "wpa3";
  }
  return "wpa2";
}

static ApSecurityMode parseApSecurityMode(const char *text, ApSecurityMode fallback) {
  if (!text) {
    return fallback;
  }
  if (strcmp(text, "open") == 0) {
    return ApSecurityMode::Open;
  }
  if (strcmp(text, "wpa") == 0 || strcmp(text, "wpa-wpa2") == 0) {
    return ApSecurityMode::WpaWpa2;
  }
  if (strcmp(text, "wpa2-wpa3") == 0) {
    return ApSecurityMode::Wpa2Wpa3;
  }
  if (strcmp(text, "wpa3") == 0) {
    return ApSecurityMode::Wpa3;
  }
  if (strcmp(text, "wpa2") == 0) {
    return ApSecurityMode::Wpa2;
  }
  return fallback;
}

static wifi_auth_mode_t apWifiAuthMode(ApSecurityMode mode) {
  switch (mode) {
    case ApSecurityMode::Open:
      return WIFI_AUTH_OPEN;
    case ApSecurityMode::WpaWpa2:
      return WIFI_AUTH_WPA_WPA2_PSK;
    case ApSecurityMode::Wpa2Wpa3:
      return WIFI_AUTH_WPA2_WPA3_PSK;
    case ApSecurityMode::Wpa3:
      return WIFI_AUTH_WPA3_PSK;
    case ApSecurityMode::Wpa2:
      return WIFI_AUTH_WPA2_PSK;
  }
  return WIFI_AUTH_WPA2_PSK;
}

static const char *wifiAuthModeLabel(wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN:
      return "Open";
    case WIFI_AUTH_WEP:
      return "WEP";
    case WIFI_AUTH_WPA_PSK:
      return "WPA";
    case WIFI_AUTH_WPA2_PSK:
      return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
      return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:
      return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
      return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK:
      return "WAPI";
    default:
      return "Unknown";
  }
}

static bool isAsciiAlpha(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static bool isAsciiDigit(char c) {
  return c >= '0' && c <= '9';
}

static bool isAsciiAlnum(char c) {
  return isAsciiAlpha(c) || isAsciiDigit(c);
}

static bool isPosixTimezoneChar(char c) {
  return c >= 33 && c <= 126;
}

static bool isIanaTimezoneChar(char c) {
  return isAsciiAlnum(c) || c == '/' || c == '_' || c == '-' || c == '+';
}

static void copyPosixTimezone(char *dst, size_t len, const char *src) {
  if (len == 0) {
    return;
  }
  if (!src) {
    src = "";
  }

  size_t out = 0;
  for (size_t i = 0; src[i] != '\0' && out + 1 < len; ++i) {
    char c = src[i];
    if (isPosixTimezoneChar(c)) {
      dst[out++] = c;
    }
  }
  dst[out] = '\0';
}

static void copyIanaTimeZone(char *dst, size_t len, const char *src) {
  if (len == 0) {
    return;
  }
  if (!src) {
    src = "";
  }

  size_t out = 0;
  for (size_t i = 0; src[i] != '\0' && out + 1 < len; ++i) {
    char c = src[i];
    if (isIanaTimezoneChar(c)) {
      dst[out++] = c;
    }
  }
  dst[out] = '\0';
}

static const char *localTimeModeName(LocalTimeMode mode) {
  switch (mode) {
    case LocalTimeMode::Offset:
      return "offset";
    case LocalTimeMode::Iana:
      return "iana";
    case LocalTimeMode::Posix:
      return "posix";
  }
  return "iana";
}

static LocalTimeMode parseLocalTimeMode(const char *text, LocalTimeMode fallback) {
  if (!text) {
    return fallback;
  }
  if (strcmp(text, "offset") == 0) {
    return LocalTimeMode::Offset;
  }
  if (strcmp(text, "iana") == 0) {
    return LocalTimeMode::Iana;
  }
  if (strcmp(text, "posix") == 0) {
    return LocalTimeMode::Posix;
  }
  return fallback;
}

static void invalidateTimeZoneCache() {
  cachedIanaZone = "";
  cachedIanaPosixTimezone = "";
  cachedIanaLookupDone = false;
  cachedIanaFound = false;
  activePosixTimezone = "";
  posixTimezoneApplied = false;
}

static uint16_t clampU16(uint16_t value, uint16_t low, uint16_t high) {
  if (value < low) {
    return low;
  }
  if (value > high) {
    return high;
  }
  return value;
}

static int16_t clampI16(int16_t value, int16_t low, int16_t high) {
  if (value < low) {
    return low;
  }
  if (value > high) {
    return high;
  }
  return value;
}

static IPAddress parseIp(const char *text, IPAddress fallback) {
  IPAddress ip;
  if (text && ip.fromString(text)) {
    return ip;
  }
  return fallback;
}

static String ipToString(const IPAddress &ip) {
  return ip.toString();
}

static String normalizeHost(String host) {
  host.trim();
  host.toLowerCase();
  while (host.endsWith(".")) {
    host.remove(host.length() - 1);
  }
  return host;
}

static void addNtpHost(const String &rawHost) {
  String host = normalizeHost(rawHost);
  if (host.length() == 0 || settings.ntpHostCount >= MAX_NTP_HOSTS) {
    return;
  }
  for (size_t i = 0; i < settings.ntpHostCount; ++i) {
    if (settings.ntpHosts[i] == host) {
      return;
    }
  }
  settings.ntpHosts[settings.ntpHostCount++] = host;
}

static void addDefaultNtpHosts() {
  const char *defaults[] = {
      "time.cloudflare.com", "time.google.com", "time1.google.com",
      "time2.google.com", "time3.google.com", "time4.google.com",
      "time.android.com", "time.apple.com", "time-ios.apple.com",
      "time.windows.com", "pool.ntp.org", "0.pool.ntp.org",
      "1.pool.ntp.org", "2.pool.ntp.org", "3.pool.ntp.org",
      "us.pool.ntp.org", "0.us.pool.ntp.org", "1.us.pool.ntp.org",
      "2.us.pool.ntp.org", "3.us.pool.ntp.org",
      "north-america.pool.ntp.org", "time.nist.gov",
      "time-a-g.nist.gov", "time-b-g.nist.gov", "time-c-g.nist.gov",
      "time-d-g.nist.gov", "time-a-wwv.nist.gov", "time-b-wwv.nist.gov",
      "time-a-b.nist.gov", "time-b-b.nist.gov", "ntp.ubuntu.com",
      "time.aws.com", "time.facebook.com", "time1.facebook.com",
      "time2.facebook.com", "time3.facebook.com", "time4.facebook.com",
      "clock.isc.org", "ntp.org", "nist.time.gov"};
  settings.ntpHostCount = 0;
  for (const char *host : defaults) {
    addNtpHost(host);
  }
}

static void setWifiDefaults() {
  copyText(settings.hostname, sizeof(settings.hostname), "tbeam-ntp");
  settings.networkStartMode = NetworkStartMode::StandaloneAp;
  copyText(settings.staSsid, sizeof(settings.staSsid), "");
  copyText(settings.staPassword, sizeof(settings.staPassword), "");
  settings.staDhcp = true;
  settings.staIp = IPAddress(192, 168, 1, 50);
  settings.staGateway = IPAddress(192, 168, 1, 1);
  settings.staSubnet = IPAddress(255, 255, 255, 0);
  settings.staDns1 = IPAddress(192, 168, 1, 1);
  settings.staDns2 = IPAddress(1, 1, 1, 1);
  settings.staConnectTimeoutSec = 25;
}

static void setApDefaults() {
  copyText(settings.apSsid, sizeof(settings.apSsid), "TBeam-NTP-{mac}");
  copyText(settings.apPassword, sizeof(settings.apPassword), DEFAULT_AP_PASSWORD);
  settings.apSecurityMode = ApSecurityMode::Wpa2;
  settings.apIp = IPAddress(192, 168, 4, 1);
  settings.apSubnet = IPAddress(255, 255, 255, 0);
  settings.apChannel = 6;
  settings.apMaxClients = 8;
}

static void setStandaloneDefaults() {
  settings.dhcpOption42 = true;
  settings.dnsNtpAliases = true;
  settings.dnsWildcardCaptive = true;
  addDefaultNtpHosts();
}

static void setTimeDefaults() {
  settings.autoLocalOffset = false;
  settings.manualOffsetMinutes = 0;
  settings.observeDst = true;
  settings.localTimeMode = LocalTimeMode::Iana;
  copyIanaTimeZone(settings.ianaTimeZone, sizeof(settings.ianaTimeZone), DEFAULT_IANA_TIME_ZONE);
  copyPosixTimezone(settings.posixTimezone, sizeof(settings.posixTimezone), DEFAULT_POSIX_TZ);
}

static void setDisplayDefaults() {
  settings.oledAutoCycle = true;
  settings.oledCycleSeconds = 5;
  settings.oledScreensaverTimeoutSec = 300;
  settings.portalTimeoutSec = 300;
}

static void setPowerDefaults() {
  settings.chargeEnabled = true;
  settings.chargeCurrentMa = 280;
  settings.chargeTargetMv = 4100;
  settings.vbusCurrentLimitMa = 500;
  settings.warningVoltageMv = 3500;
  settings.cutoffVoltageMv = 3200;
  settings.sysPowerDownMv = 3000;
  settings.powerKeyOffSeconds = 6;
}

static void setHardDefaults() {
  setWifiDefaults();
  setApDefaults();
  setStandaloneDefaults();
  setTimeDefaults();
  setDisplayDefaults();
  setPowerDefaults();
}

static bool applyNetworkStartModeJson(JsonDocument &doc) {
  if (!doc["networkMode"].is<const char *>()) {
    return false;
  }
  settings.networkStartMode = parseNetworkStartMode(doc["networkMode"], settings.networkStartMode);
  return true;
}

static void applyHostnameJson(JsonDocument &doc) {
  if (doc["hostname"].is<const char *>()) {
    copyText(settings.hostname, sizeof(settings.hostname), doc["hostname"]);
  }
}

static void applyWifiSettingsJson(JsonObject wifi) {
  if (wifi.isNull()) {
    return;
  }

  if (wifi["ssid"].is<const char *>()) {
    copyText(settings.staSsid, sizeof(settings.staSsid), wifi["ssid"]);
  }
  if (wifi["password"].is<const char *>()) {
    copyText(settings.staPassword, sizeof(settings.staPassword), wifi["password"]);
  }

  settings.staDhcp = wifi["dhcp"] | settings.staDhcp;
  settings.staIp = parseIp(wifi["staticIp"] | nullptr, settings.staIp);
  settings.staGateway = parseIp(wifi["gateway"] | nullptr, settings.staGateway);
  settings.staSubnet = parseIp(wifi["subnet"] | nullptr, settings.staSubnet);
  settings.staDns1 = parseIp(wifi["dns1"] | nullptr, settings.staDns1);
  settings.staDns2 = parseIp(wifi["dns2"] | nullptr, settings.staDns2);
  settings.staConnectTimeoutSec = clampU16(wifi["connectTimeoutSec"] | settings.staConnectTimeoutSec, 5, 90);
}

static void applyApSettingsJson(JsonObject ap) {
  if (ap.isNull()) {
    return;
  }

  if (ap["ssid"].is<const char *>()) {
    copyText(settings.apSsid, sizeof(settings.apSsid), ap["ssid"]);
  }
  if (ap["password"].is<const char *>()) {
    copyText(settings.apPassword, sizeof(settings.apPassword), ap["password"]);
  }
  if (ap["security"].is<const char *>()) {
    settings.apSecurityMode = parseApSecurityMode(ap["security"], settings.apSecurityMode);
  }

  settings.apIp = parseIp(ap["ip"] | nullptr, settings.apIp);
  settings.apSubnet = parseIp(ap["subnet"] | nullptr, settings.apSubnet);
  settings.apChannel = clampU16(ap["channel"] | settings.apChannel, 1, 13);
  settings.apMaxClients = clampU16(ap["maxClients"] | settings.apMaxClients, 1, 10);
}

static void normalizeNetworkSettings(bool explicitNetworkStartMode) {
  if (!explicitNetworkStartMode && strlen(settings.staSsid) > 0) {
    settings.networkStartMode = NetworkStartMode::ClientWithApFallback;
  }
  if (settings.apSecurityMode != ApSecurityMode::Open && strlen(settings.apPassword) < 8) {
    copyText(settings.apPassword, sizeof(settings.apPassword), DEFAULT_AP_PASSWORD);
  }
}

static void applyNtpHostsJson(JsonArray hosts) {
  settings.ntpHostCount = 0;
  for (JsonVariant value : hosts) {
    if (value.is<const char *>()) {
      addNtpHost(value.as<const char *>());
    }
  }
}

static void applyStandaloneSettingsJson(JsonObject standalone) {
  if (standalone.isNull()) {
    return;
  }

  settings.dhcpOption42 = standalone["dhcpOption42"] | settings.dhcpOption42;
  settings.dnsNtpAliases = standalone["dnsNtpAliases"] | settings.dnsNtpAliases;
  settings.dnsWildcardCaptive = standalone["dnsWildcardCaptive"] | settings.dnsWildcardCaptive;
  if (standalone["ntpHosts"].is<JsonArray>()) {
    applyNtpHostsJson(standalone["ntpHosts"].as<JsonArray>());
  }
}

static void applyTimeSettingsJson(JsonObject time) {
  if (time.isNull()) {
    return;
  }

  settings.autoLocalOffset = time["autoLocalOffset"] | settings.autoLocalOffset;
  settings.manualOffsetMinutes = clampI16(time["manualOffsetMinutes"] | settings.manualOffsetMinutes, -14 * 60, 14 * 60);
  settings.observeDst = time["observeDst"] | settings.observeDst;

  if (time["mode"].is<const char *>()) {
    settings.localTimeMode = parseLocalTimeMode(time["mode"], settings.localTimeMode);
  } else if (time["observeDst"].is<bool>()) {
    settings.localTimeMode = settings.observeDst ? LocalTimeMode::Posix : LocalTimeMode::Offset;
  }
  if (time["ianaTimeZone"].is<const char *>()) {
    copyIanaTimeZone(settings.ianaTimeZone, sizeof(settings.ianaTimeZone), time["ianaTimeZone"]);
  }
  if (time["posixTimezone"].is<const char *>()) {
    copyPosixTimezone(settings.posixTimezone, sizeof(settings.posixTimezone), time["posixTimezone"]);
  }

  invalidateTimeZoneCache();
}

static void applyDisplaySettingsJson(JsonObject displaySettings) {
  if (displaySettings.isNull()) {
    return;
  }

  settings.oledAutoCycle = displaySettings["autoCycle"] | settings.oledAutoCycle;
  settings.oledCycleSeconds = clampU16(displaySettings["cycleSeconds"] | settings.oledCycleSeconds, 2, 60);
  settings.oledScreensaverTimeoutSec = clampU16(displaySettings["screensaverTimeoutSec"] | settings.oledScreensaverTimeoutSec, 0, 3600);
  settings.portalTimeoutSec = clampU16(displaySettings["portalTimeoutSec"] | settings.portalTimeoutSec, 0, 7200);
}

static uint16_t normalizedVbusLimit(uint16_t limitMa) {
  if (limitMa == 100 || limitMa == 500 || limitMa == 0) {
    return limitMa;
  }
  return 500;
}

static uint8_t normalizedPowerKeySeconds(uint8_t seconds) {
  if (seconds == 4 || seconds == 6 || seconds == 8 || seconds == 10) {
    return seconds;
  }
  return 6;
}

static void applyPowerSettingsJson(JsonObject power) {
  if (power.isNull()) {
    return;
  }

  settings.chargeEnabled = power["chargeEnabled"] | settings.chargeEnabled;
  settings.chargeCurrentMa = clampU16(power["chargeCurrentMa"] | settings.chargeCurrentMa, 100, 700);
  settings.chargeTargetMv = clampU16(power["chargeTargetMv"] | settings.chargeTargetMv, 4100, 4200);
  settings.vbusCurrentLimitMa = normalizedVbusLimit(power["vbusCurrentLimitMa"] | settings.vbusCurrentLimitMa);
  settings.warningVoltageMv = clampU16(power["warningVoltageMv"] | settings.warningVoltageMv, 3200, 3900);
  settings.cutoffVoltageMv = clampU16(power["cutoffVoltageMv"] | settings.cutoffVoltageMv, 3000, 3600);
  settings.sysPowerDownMv = clampU16(power["sysPowerDownMv"] | settings.sysPowerDownMv, 2600, 3300);
  settings.powerKeyOffSeconds = normalizedPowerKeySeconds(power["powerKeyOffSeconds"] | settings.powerKeyOffSeconds);
}

static bool applySettingsJson(JsonDocument &doc) {
  bool explicitNetworkStartMode = applyNetworkStartModeJson(doc);
  applyHostnameJson(doc);
  applyWifiSettingsJson(doc["wifi"].as<JsonObject>());
  applyApSettingsJson(doc["ap"].as<JsonObject>());
  normalizeNetworkSettings(explicitNetworkStartMode);
  applyStandaloneSettingsJson(doc["standalone"].as<JsonObject>());
  applyTimeSettingsJson(doc["time"].as<JsonObject>());
  applyDisplaySettingsJson(doc["display"].as<JsonObject>());
  applyPowerSettingsJson(doc["power"].as<JsonObject>());
  return true;
}

static String expandedApSsid();

static void writeWifiSettingsJson(JsonDocument &doc) {
  JsonObject wifi = doc.createNestedObject("wifi");
  wifi["ssid"] = settings.staSsid;
  wifi["password"] = settings.staPassword;
  wifi["dhcp"] = settings.staDhcp;
  wifi["staticIp"] = ipToString(settings.staIp);
  wifi["gateway"] = ipToString(settings.staGateway);
  wifi["subnet"] = ipToString(settings.staSubnet);
  wifi["dns1"] = ipToString(settings.staDns1);
  wifi["dns2"] = ipToString(settings.staDns2);
  wifi["connectTimeoutSec"] = settings.staConnectTimeoutSec;
}

static void writeApSettingsJson(JsonDocument &doc) {
  JsonObject ap = doc.createNestedObject("ap");
  ap["ssid"] = settings.apSsid;
  ap["password"] = settings.apPassword;
  ap["security"] = apSecurityModeName(settings.apSecurityMode);
  ap["ip"] = ipToString(settings.apIp);
  ap["subnet"] = ipToString(settings.apSubnet);
  ap["channel"] = settings.apChannel;
  ap["maxClients"] = settings.apMaxClients;
  ap["expandedSsid"] = expandedApSsid();
}

static void writeStandaloneSettingsJson(JsonDocument &doc) {
  JsonObject standalone = doc.createNestedObject("standalone");
  standalone["dhcpOption42"] = settings.dhcpOption42;
  standalone["dnsNtpAliases"] = settings.dnsNtpAliases;
  standalone["dnsWildcardCaptive"] = settings.dnsWildcardCaptive;
  JsonArray hosts = standalone.createNestedArray("ntpHosts");
  for (size_t i = 0; i < settings.ntpHostCount; ++i) {
    hosts.add(settings.ntpHosts[i]);
  }
}

static void writeTimeSettingsJson(JsonDocument &doc) {
  JsonObject time = doc.createNestedObject("time");
  time["mode"] = localTimeModeName(settings.localTimeMode);
  time["autoLocalOffset"] = settings.autoLocalOffset;
  time["manualOffsetMinutes"] = settings.manualOffsetMinutes;
  time["observeDst"] = settings.observeDst;
  time["ianaTimeZone"] = settings.ianaTimeZone;
  time["posixTimezone"] = settings.posixTimezone;
  time["databasePath"] = TIMEZONE_DB_PATH;
}

static void writeDisplaySettingsJson(JsonDocument &doc) {
  JsonObject displaySettings = doc.createNestedObject("display");
  displaySettings["autoCycle"] = settings.oledAutoCycle;
  displaySettings["cycleSeconds"] = settings.oledCycleSeconds;
  displaySettings["screensaverTimeoutSec"] = settings.oledScreensaverTimeoutSec;
  displaySettings["portalTimeoutSec"] = settings.portalTimeoutSec;
}

static void writePowerSettingsJson(JsonDocument &doc) {
  JsonObject power = doc.createNestedObject("power");
  power["chargeEnabled"] = settings.chargeEnabled;
  power["chargeCurrentMa"] = settings.chargeCurrentMa;
  power["chargeTargetMv"] = settings.chargeTargetMv;
  power["vbusCurrentLimitMa"] = settings.vbusCurrentLimitMa;
  power["warningVoltageMv"] = settings.warningVoltageMv;
  power["cutoffVoltageMv"] = settings.cutoffVoltageMv;
  power["sysPowerDownMv"] = settings.sysPowerDownMv;
  power["powerKeyOffSeconds"] = settings.powerKeyOffSeconds;
}

static void settingsToJson(JsonDocument &doc) {
  doc["networkMode"] = networkStartModeName(settings.networkStartMode);
  doc["hostname"] = settings.hostname;

  writeWifiSettingsJson(doc);
  writeApSettingsJson(doc);
  writeStandaloneSettingsJson(doc);
  writeTimeSettingsJson(doc);
  writeDisplaySettingsJson(doc);
  writePowerSettingsJson(doc);
}

static bool littleFsFileExists(const char *path);

static bool loadSettingsFile(const char *path) {
  if (!littleFsFileExists(path)) {
    return false;
  }
  File file = LittleFS.open(path, "r");
  if (!file) {
    return false;
  }

  PsramJsonDocument doc(12288);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) {
    Serial.printf("Settings parse failed for %s: %s\n", path, error.c_str());
    return false;
  }
  return applySettingsJson(doc);
}

static bool loadSettings() {
  setHardDefaults();
  if (loadSettingsFile("/settings.json")) {
    Serial.println("Loaded /settings.json");
    return true;
  }
  if (loadSettingsFile("/settings.default.json")) {
    Serial.println("Loaded /settings.default.json");
    return true;
  }
  Serial.println("Using compiled defaults");
  return false;
}

static bool saveSettings() {
  PsramJsonDocument doc(12288);
  settingsToJson(doc);
  File file = LittleFS.open("/settings.json", "w");
  if (!file) {
    return false;
  }
  serializeJsonPretty(doc, file);
  file.flush();
  file.close();
  return true;
}

static bool littleFsFileExists(const char *path) {
  String target = path ? path : "";
  if (target.startsWith("/")) {
    target.remove(0, 1);
  }
  File root = LittleFS.open("/");
  if (!root) {
    return false;
  }
  File file = root.openNextFile();
  while (file) {
    String name = file.name();
    if (name.startsWith("/")) {
      name.remove(0, 1);
    }
    bool match = name == target;
    file.close();
    if (match) {
      root.close();
      return true;
    }
    file = root.openNextFile();
  }
  root.close();
  return false;
}

static void refreshLittleFsPresenceFlags() {
  timezoneDbPresent = littleFsFileExists(TIMEZONE_DB_PATH);
  portalGzipPresent = littleFsFileExists(PORTAL_GZIP_PATH);
}

static String macSuffix() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[7];
  snprintf(buf, sizeof(buf), "%02X%02X%02X",
           static_cast<uint8_t>(mac >> 16),
           static_cast<uint8_t>(mac >> 8),
           static_cast<uint8_t>(mac));
  return String(buf);
}

static String expandedApSsid() {
  String ssid = settings.apSsid;
  ssid.replace("{mac}", macSuffix());
  if (ssid.length() == 0) {
    ssid = "TBeam-NTP-" + macSuffix();
  }
  return ssid.substring(0, 32);
}

static bool isHostnameChar(char c) {
  return (c >= 'a' && c <= 'z') || isAsciiDigit(c) || c == '-';
}

static bool isHostnameSeparator(char c) {
  return c == '_' || c == ' ' || c == '.';
}

static char normalizedHostnameChar(char c) {
  if (isHostnameChar(c)) {
    return c;
  }
  if (isHostnameSeparator(c)) {
    return '-';
  }
  return '\0';
}

static void trimHostnameDashes(String &host) {
  while (host.startsWith("-")) {
    host.remove(0, 1);
  }
  while (host.endsWith("-")) {
    host.remove(host.length() - 1);
  }
}

static String sanitizedHostname() {
  String host = settings.hostname;
  host.trim();
  host.toLowerCase();
  String clean;
  for (size_t i = 0; i < host.length() && clean.length() < 31; ++i) {
    char c = normalizedHostnameChar(host.charAt(i));
    if (c != '\0') {
      clean += c;
    }
  }
  clean.trim();
  trimHostnameDashes(clean);
  if (clean.length() == 0) {
    clean = "tbeam-ntp";
  }
  return clean;
}

