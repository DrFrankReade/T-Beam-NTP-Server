#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <TinyGPS++.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <ESPmDNS.h>
#include <SSD1306Wire.h>
#include <XPowersLib.h>

#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

extern "C" {
#include "dhcpserver/dhcpserver.h"
#include "dhcpserver/dhcpserver_options.h"
#include "lwip/ip_addr.h"
}

#if CONFIG_BT_ENABLED
#include <esp_bt.h>
#endif

static constexpr uint8_t PIN_I2C_SDA = 21;
static constexpr uint8_t PIN_I2C_SCL = 22;
static constexpr uint8_t PIN_PMU_IRQ = 35;
static constexpr uint8_t PIN_GPS_RX = 34;
static constexpr uint8_t PIN_GPS_TX = 12;
static constexpr uint8_t PIN_GPS_PPS = 37;
static constexpr uint8_t PIN_USER_BUTTON = 38;

static constexpr uint32_t SERIAL_BAUD = 115200;
static constexpr uint32_t GPS_BAUD = 9600;
static constexpr uint16_t NTP_PORT = 123;
static constexpr uint16_t DNS_PORT = 53;
static constexpr size_t NTP_PACKET_SIZE = 48;
static constexpr size_t DNS_PACKET_MAX_SIZE = 512;
static constexpr size_t DNS_RESPONSE_MAX_SIZE = 576;
static constexpr size_t DNS_HEADER_SIZE = 12;
static constexpr size_t MAX_NTP_HOSTS = 64;
static constexpr size_t POSIX_TZ_MAX_LEN = 96;
static constexpr uint64_t UNIX_TO_NTP_SECONDS = 2208988800ULL;
static constexpr uint8_t OLED_PAGE_COUNT = 7;
static constexpr uint32_t POWER_POLL_INTERVAL_MS = 1000;
static constexpr uint32_t BATTERY_CUTOFF_DEBOUNCE_MS = 30000;
static constexpr uint32_t USER_BUTTON_FACTORY_RESET_MS = 10000;
static constexpr uint32_t FACTORY_RESET_CONFIRM_WINDOW_MS = 30000;
static constexpr uint32_t OLED_MANUAL_CYCLE_PAUSE_MS = 30000;
static constexpr const char *FIRMWARE_VERSION = "v0.1.9";
static constexpr const char *DEFAULT_POSIX_TZ = "PST8PDT,M3.2.0/2,M11.1.0/2";
static constexpr const char *DEFAULT_IANA_TIME_ZONE = "America/Los_Angeles";
static constexpr const char *DEFAULT_AP_PASSWORD = "tbeam-ntp";
static constexpr const char *TIMEZONE_DB_PATH = "/timezones.current.tsv";
static constexpr const char *PORTAL_HTML_PATH = "/index.html";
static constexpr const char *PORTAL_GZIP_PATH = "/index.html.gz";
static constexpr size_t FILE_STREAM_BUFFER_SIZE = 1024;

enum class NetworkMode {
  ApFallback,
  Sta
};

enum class NetworkStartMode : uint8_t {
  StandaloneAp,
  ClientWithApFallback
};

enum class ApSecurityMode : uint8_t {
  Open,
  Wpa2,
  WpaWpa2,
  Wpa2Wpa3,
  Wpa3
};

enum class HardwareRevision : uint8_t {
  Unknown,
  TBeamV11,
  TBeamV12
};

enum class PmicModel : uint8_t {
  None,
  Axp192,
  Axp2101
};

enum class LocalTimeMode : uint8_t {
  Offset,
  Iana,
  Posix
};

struct DeviceSettings {
  char hostname[32];
  NetworkStartMode networkStartMode;

  char staSsid[33];
  char staPassword[65];
  bool staDhcp;
  IPAddress staIp;
  IPAddress staGateway;
  IPAddress staSubnet;
  IPAddress staDns1;
  IPAddress staDns2;
  uint8_t staConnectTimeoutSec;

  char apSsid[33];
  char apPassword[65];
  ApSecurityMode apSecurityMode;
  IPAddress apIp;
  IPAddress apSubnet;
  uint8_t apChannel;
  uint8_t apMaxClients;

  bool dhcpOption42;
  bool dnsNtpAliases;
  bool dnsWildcardCaptive;
  String ntpHosts[MAX_NTP_HOSTS];
  size_t ntpHostCount;

  bool autoLocalOffset;
  int16_t manualOffsetMinutes;
  bool observeDst;
  LocalTimeMode localTimeMode;
  char ianaTimeZone[64];
  char posixTimezone[POSIX_TZ_MAX_LEN];

  bool oledAutoCycle;
  uint8_t oledCycleSeconds;
  uint16_t oledScreensaverTimeoutSec;
  uint16_t portalTimeoutSec;

  bool chargeEnabled;
  uint16_t chargeCurrentMa;
  uint16_t chargeTargetMv;
  uint16_t vbusCurrentLimitMa;
  uint16_t warningVoltageMv;
  uint16_t cutoffVoltageMv;
  uint16_t sysPowerDownMv;
  uint8_t powerKeyOffSeconds;
};

struct PowerState {
  bool online = false;
  bool batteryPresent = false;
  bool charging = false;
  bool discharging = false;
  bool vbusPresent = false;
  uint16_t batteryMv = 0;
  float batteryCurrentMa = 0.0f;
  uint16_t vbusMv = 0;
  float vbusCurrentMa = 0.0f;
  uint16_t systemMv = 0;
  float temperatureC = 0.0f;
  int batteryPercent = -1;
  bool warning = false;
  bool cutoffPending = false;
};

class CaptivePortalWebServer : public WebServer {
public:
  using WebServer::WebServer;

  void handleClient() override {
    static constexpr uint32_t CLIENT_DATA_WAIT_MS = 250;

    if (_currentStatus == HC_NONE) {
      _currentClient = _server.available();
      if (!_currentClient) {
        if (_nullDelay) {
          delay(1);
        }
        return;
      }

      _currentStatus = HC_WAIT_READ;
      _statusChange = millis();
    }

    bool keepCurrentClient = false;
    bool callYield = false;

    if (_currentClient.connected()) {
      switch (_currentStatus) {
        case HC_NONE:
          break;
        case HC_WAIT_READ:
          if (_currentClient.available()) {
            if (_parseRequest(_currentClient)) {
              _currentClient.setTimeout(1);
              _contentLength = CONTENT_LENGTH_NOT_SET;
              _handleRequest();
            }
          } else {
            keepCurrentClient = millis() - _statusChange <= CLIENT_DATA_WAIT_MS;
            callYield = true;
          }
          break;
        case HC_WAIT_CLOSE:
          callYield = true;
          break;
      }
    }

    if (!keepCurrentClient) {
      _currentClient = WiFiClient();
      _currentStatus = HC_NONE;
      _currentUpload.reset();
      _currentRaw.reset();
    }

    if (callYield) {
      yield();
    }
  }
};

static DeviceSettings settings;
static PowerState powerState;
static CaptivePortalWebServer server(80);
static WiFiUDP ntpUdp;
static WiFiUDP dnsUdp;
static HardwareSerial GPSSerial(1);
static TinyGPSPlus gps;
static TinyGPSCustom pdopGp(gps, "GPGSA", 15);
static TinyGPSCustom hdopGp(gps, "GPGSA", 16);
static TinyGPSCustom vdopGp(gps, "GPGSA", 17);
static TinyGPSCustom pdopGn(gps, "GNGSA", 15);
static TinyGPSCustom hdopGn(gps, "GNGSA", 16);
static TinyGPSCustom vdopGn(gps, "GNGSA", 17);
static SSD1306Wire display(0x3C, PIN_I2C_SDA, PIN_I2C_SCL);
static XPowersAXP192 pmu192(Wire);
static XPowersAXP2101 pmu2101(Wire);
static XPowersLibInterface *pmu = nullptr;

static NetworkMode networkMode = NetworkMode::ApFallback;
static HardwareRevision hardwareRevision = HardwareRevision::Unknown;
static PmicModel pmicModel = PmicModel::None;
static uint8_t pmicRegister03 = 0;
static bool pmicRegister03Valid = false;
static bool oledOnline = false;
static bool ntpOnline = false;
static bool dnsOnline = false;
static bool mdnsOnline = false;
static bool timezoneDbPresent = false;
static bool portalGzipPresent = false;
static uint32_t ntpRequestCount = 0;
static uint32_t ntpResponseCount = 0;
static uint32_t ntpSuppressedCount = 0;
static uint32_t dnsQueryCount = 0;
static uint32_t dnsAliasHitCount = 0;
static uint32_t staLostSinceMs = 0;
static uint32_t lowBatterySinceMs = 0;
static uint32_t lastPowerPollMs = 0;
static uint32_t lastOledUpdateMs = 0;
static uint32_t lastOledCycleMs = 0;
static uint32_t lastOledActivityMs = 0;
static uint32_t oledAutoCyclePausedUntilMs = 0;
static uint32_t lastI2cScanMs = 0;
static String activePosixTimezone;
static String cachedIanaZone;
static String cachedIanaPosixTimezone;
static bool cachedIanaLookupDone = false;
static bool cachedIanaFound = false;
static bool posixTimezoneApplied = false;
static String i2cDevices = "";
static uint8_t oledPage = 0;
static bool oledSleeping = false;
static bool lastDisplayGpsFix = false;
static uint32_t splashUntilMs = 0;
static bool bootForceApMode = false;

static portMUX_TYPE ppsMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool ppsPending = false;
static volatile uint64_t isrPpsUsec = 0;
static volatile uint32_t isrPpsCount = 0;
static volatile bool pmuIrqPending = false;

static bool clockValid = false;
static bool clockPpsDisciplined = false;
static uint64_t clockAnchorUsec = 0;
static time_t clockAnchorEpoch = 0;
static uint64_t lastPpsUsec = 0;
static uint32_t lastPpsMs = 0;
static uint32_t lastClockSyncMs = 0;
static time_t lastNmeaEpoch = 0;
static uint32_t lastNmeaUpdateMs = 0;

static void wakeOled();
static void wakeOledForPowerWarning();

struct PsramJsonAllocator {
  void *allocate(size_t size) {
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
  }

  void deallocate(void *pointer) {
    heap_caps_free(pointer);
  }

  void *reallocate(void *pointer, size_t newSize) {
    return heap_caps_realloc_prefer(pointer, newSize, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
  }
};

using PsramJsonDocument = BasicJsonDocument<PsramJsonAllocator>;

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

static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097LL + static_cast<int>(doe) - 719468LL;
}

static time_t gpsToUnix(uint16_t year, uint8_t month, uint8_t day,
                        uint8_t hour, uint8_t minute, uint8_t second) {
  int64_t days = daysFromCivil(year, month, day);
  return static_cast<time_t>(days * 86400LL + hour * 3600LL + minute * 60LL + second);
}

static void setSystemTime(uint64_t unixUsec) {
  timeval tv;
  tv.tv_sec = unixUsec / 1000000ULL;
  tv.tv_usec = unixUsec % 1000000ULL;
  settimeofday(&tv, nullptr);
}

static void setClockAnchor(time_t epoch, uint64_t anchorUsec, bool ppsDisciplined) {
  if (epoch < 1700000000) {
    return;
  }
  clockAnchorEpoch = epoch;
  clockAnchorUsec = anchorUsec;
  clockValid = true;
  clockPpsDisciplined = ppsDisciplined;
  lastClockSyncMs = millis();
  setSystemTime(static_cast<uint64_t>(epoch) * 1000000ULL);
}

static bool getClockUnixUsec(uint64_t &unixUsec) {
  if (!clockValid) {
    return false;
  }
  uint64_t now = esp_timer_get_time();
  uint64_t elapsed = now >= clockAnchorUsec ? now - clockAnchorUsec : 0;
  unixUsec = static_cast<uint64_t>(clockAnchorEpoch) * 1000000ULL + elapsed;
  return true;
}

static bool hasFreshGpsFix(uint32_t maxAgeMs = 10000) {
  return gps.location.isValid() && gps.location.age() <= maxAgeMs &&
         gps.satellites.isValid() && gps.satellites.value() > 0;
}

static bool hasFreshGpsTime(uint32_t maxAgeMs = 10000) {
  return clockValid && lastNmeaUpdateMs != 0 &&
         (millis() - lastNmeaUpdateMs) <= maxAgeMs &&
         hasFreshGpsFix(maxAgeMs);
}

static bool hasRecentPps(uint32_t maxAgeMs = 2500) {
  return clockPpsDisciplined && lastPpsMs != 0 && (millis() - lastPpsMs) <= maxAgeMs;
}

static String formatTm(const tm &out) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           out.tm_year + 1900, out.tm_mon + 1, out.tm_mday,
           out.tm_hour, out.tm_min, out.tm_sec);
  return String(buf);
}

static String formatUtc(uint64_t unixUsec, int16_t offsetMinutes = 0) {
  time_t seconds = static_cast<time_t>(unixUsec / 1000000ULL) + offsetMinutes * 60;
  tm out;
  gmtime_r(&seconds, &out);
  return formatTm(out);
}

static bool lookupTimeZonePreset(const char *zoneName, String &posixRule) {
  if (!zoneName || zoneName[0] == '\0') {
    return false;
  }

  File file = LittleFS.open(TIMEZONE_DB_PATH, "r");
  if (!file) {
    return false;
  }

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line[0] == '#') {
      continue;
    }
    int tab = line.indexOf('\t');
    if (tab <= 0) {
      continue;
    }
    String name = line.substring(0, tab);
    if (name == zoneName) {
      posixRule = line.substring(tab + 1);
      posixRule.trim();
      return posixRule.length() > 0;
    }
  }

  return false;
}

static bool selectedIanaTimeZoneRule(String &posixRule) {
  String selected(settings.ianaTimeZone);
  if (!cachedIanaLookupDone || cachedIanaZone != selected) {
    cachedIanaZone = selected;
    cachedIanaPosixTimezone = "";
    cachedIanaFound = lookupTimeZonePreset(settings.ianaTimeZone, cachedIanaPosixTimezone);
    cachedIanaLookupDone = true;
  }
  if (!cachedIanaFound) {
    return false;
  }
  posixRule = cachedIanaPosixTimezone;
  return true;
}

static bool configuredPosixRule(String &rule) {
  if (settings.localTimeMode == LocalTimeMode::Offset) {
    return false;
  }

  if (settings.localTimeMode == LocalTimeMode::Iana && selectedIanaTimeZoneRule(rule)) {
    return true;
  }

  if (settings.posixTimezone[0] != '\0') {
    rule = settings.posixTimezone;
    return true;
  }

  return false;
}

static bool applyPosixTimezoneRule(const String &rule) {
  if (rule.length() == 0) {
    return false;
  }

  if (!posixTimezoneApplied || activePosixTimezone != rule) {
    setenv("TZ", rule.c_str(), 1);
    tzset();
    activePosixTimezone = rule;
    posixTimezoneApplied = true;
  }
  return true;
}

static bool applyConfiguredLocalTimezone() {
  String rule;
  return configuredPosixRule(rule) && applyPosixTimezoneRule(rule);
}

static int16_t offsetFromLocalTm(time_t utcSeconds, const tm &localTm) {
  time_t localAsUtc = gpsToUnix(localTm.tm_year + 1900, localTm.tm_mon + 1, localTm.tm_mday,
                                localTm.tm_hour, localTm.tm_min, localTm.tm_sec);
  long diffMinutes = static_cast<long>((localAsUtc - utcSeconds) / 60);
  return clampI16(diffMinutes, -14 * 60, 14 * 60);
}

static int16_t currentLocalOffsetMinutes(uint64_t unixUsec = 0) {
  if (unixUsec != 0 && applyConfiguredLocalTimezone()) {
    time_t seconds = static_cast<time_t>(unixUsec / 1000000ULL);
    tm localTm;
    localtime_r(&seconds, &localTm);
    return offsetFromLocalTm(seconds, localTm);
  }

  return settings.manualOffsetMinutes;
}

static bool isLocalDstActive(uint64_t unixUsec) {
  if (!applyConfiguredLocalTimezone()) {
    return false;
  }
  time_t seconds = static_cast<time_t>(unixUsec / 1000000ULL);
  tm localTm;
  localtime_r(&seconds, &localTm);
  return localTm.tm_isdst > 0;
}

static String localTimezoneName(uint64_t unixUsec) {
  if (!applyConfiguredLocalTimezone()) {
    return "";
  }
  time_t seconds = static_cast<time_t>(unixUsec / 1000000ULL);
  tm localTm;
  localtime_r(&seconds, &localTm);
  char buf[16];
  strftime(buf, sizeof(buf), "%Z", &localTm);
  return String(buf);
}

static String formatLocal(uint64_t unixUsec) {
  if (applyConfiguredLocalTimezone()) {
    time_t seconds = static_cast<time_t>(unixUsec / 1000000ULL);
    tm localTm;
    localtime_r(&seconds, &localTm);
    return formatTm(localTm);
  }
  return formatUtc(unixUsec, currentLocalOffsetMinutes());
}

static String maidenhead(double lat, double lon) {
  if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
    return "";
  }
  lon += 180.0;
  lat += 90.0;

  int fieldLon = static_cast<int>(lon / 20.0);
  int fieldLat = static_cast<int>(lat / 10.0);
  lon -= fieldLon * 20.0;
  lat -= fieldLat * 10.0;

  int squareLon = static_cast<int>(lon / 2.0);
  int squareLat = static_cast<int>(lat / 1.0);
  lon -= squareLon * 2.0;
  lat -= squareLat * 1.0;

  int subsquareLon = static_cast<int>(lon / (2.0 / 24.0));
  int subsquareLat = static_cast<int>(lat / (1.0 / 24.0));

  char buf[7];
  snprintf(buf, sizeof(buf), "%c%c%d%d%c%c",
           'A' + fieldLon, 'A' + fieldLat, squareLon, squareLat,
           'a' + subsquareLon, 'a' + subsquareLat);
  return String(buf);
}

static void IRAM_ATTR onPps() {
  uint64_t now = esp_timer_get_time();
  portENTER_CRITICAL_ISR(&ppsMux);
  isrPpsUsec = now;
  isrPpsCount++;
  ppsPending = true;
  portEXIT_CRITICAL_ISR(&ppsMux);
}

static void IRAM_ATTR onPmuIrq() {
  pmuIrqPending = true;
}

static void handlePps() {
  bool pending = false;
  uint64_t ppsUsec = 0;
  portENTER_CRITICAL(&ppsMux);
  if (ppsPending) {
    pending = true;
    ppsUsec = isrPpsUsec;
    ppsPending = false;
  }
  portEXIT_CRITICAL(&ppsMux);

  if (!pending) {
    return;
  }

  lastPpsUsec = ppsUsec;
  lastPpsMs = millis();

  if (clockValid) {
    uint64_t elapsed = ppsUsec >= clockAnchorUsec ? ppsUsec - clockAnchorUsec : 1000000ULL;
    uint32_t wholeSeconds = max<uint32_t>(1, (elapsed + 500000ULL) / 1000000ULL);
    setClockAnchor(clockAnchorEpoch + wholeSeconds, ppsUsec, true);
  } else if (lastNmeaEpoch > 0 && millis() - lastNmeaUpdateMs < 2000) {
    setClockAnchor(lastNmeaEpoch + 1, ppsUsec, true);
  }
}

static void consumeGps() {
  while (GPSSerial.available()) {
    gps.encode(static_cast<char>(GPSSerial.read()));
  }

  if (gps.date.isValid() && gps.time.isValid() &&
      (gps.time.isUpdated() || gps.date.isUpdated())) {
    time_t epoch = gpsToUnix(gps.date.year(), gps.date.month(), gps.date.day(),
                             gps.time.hour(), gps.time.minute(), gps.time.second());
    lastNmeaEpoch = epoch;
    lastNmeaUpdateMs = millis();

    uint64_t nowUsec = esp_timer_get_time();
    uint64_t timeUsec = nowUsec;
    uint32_t centiseconds = gps.time.centisecond();
    if (centiseconds <= 99) {
      timeUsec -= static_cast<uint64_t>(centiseconds) * 10000ULL;
    }

    // NMEA gives the absolute UTC second; PPS gives the precise second edge.
    // Pair a recent PPS edge with the NMEA epoch whenever the receiver provides both.
    if (lastPpsUsec != 0 && millis() - lastPpsMs < 1200) {
      setClockAnchor(epoch, lastPpsUsec, true);
    } else {
      setClockAnchor(epoch, timeUsec, false);
    }
  }
}

static const char *hardwareRevisionLabel() {
  switch (hardwareRevision) {
    case HardwareRevision::TBeamV11:
      return "T-Beam v1.1";
    case HardwareRevision::TBeamV12:
      return "T-Beam v1.2";
    default:
      return "T-Beam unknown";
  }
}

static const char *pmicModelLabel() {
  switch (pmicModel) {
    case PmicModel::Axp192:
      return "AXP192";
    case PmicModel::Axp2101:
      return "AXP2101";
    default:
      return "none";
  }
}

static bool readPmicRegister(uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(AXP192_SLAVE_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(static_cast<uint8_t>(AXP192_SLAVE_ADDRESS), static_cast<uint8_t>(1)) != 1) {
    return false;
  }
  value = Wire.read();
  return true;
}

static void rememberDetectedPmic(PmicModel model, uint8_t chipId) {
  pmicModel = model;
  pmicRegister03 = chipId;
  pmicRegister03Valid = true;
  if (model == PmicModel::Axp192) {
    hardwareRevision = HardwareRevision::TBeamV11;
    pmu = &pmu192;
  } else if (model == PmicModel::Axp2101) {
    hardwareRevision = HardwareRevision::TBeamV12;
    pmu = &pmu2101;
  }
}

static uint8_t chargeCurrentOptionAxp192(uint16_t ma) {
  struct CurrentMap {
    uint16_t ma;
    uint8_t opt;
  };
  static const CurrentMap map[] = {
      {100, XPOWERS_AXP192_CHG_CUR_100MA},
      {190, XPOWERS_AXP192_CHG_CUR_190MA},
      {280, XPOWERS_AXP192_CHG_CUR_280MA},
      {360, XPOWERS_AXP192_CHG_CUR_360MA},
      {450, XPOWERS_AXP192_CHG_CUR_450MA},
      {550, XPOWERS_AXP192_CHG_CUR_550MA},
      {630, XPOWERS_AXP192_CHG_CUR_630MA},
      {700, XPOWERS_AXP192_CHG_CUR_700MA},
  };
  uint8_t selected = map[0].opt;
  for (const CurrentMap &entry : map) {
    if (ma >= entry.ma) {
      selected = entry.opt;
    }
  }
  return selected;
}

static uint8_t chargeCurrentOptionAxp2101(uint16_t ma) {
  struct CurrentMap {
    uint16_t ma;
    uint8_t opt;
  };
  static const CurrentMap map[] = {
      {100, XPOWERS_AXP2101_CHG_CUR_100MA},
      {125, XPOWERS_AXP2101_CHG_CUR_125MA},
      {150, XPOWERS_AXP2101_CHG_CUR_150MA},
      {175, XPOWERS_AXP2101_CHG_CUR_175MA},
      {200, XPOWERS_AXP2101_CHG_CUR_200MA},
      {300, XPOWERS_AXP2101_CHG_CUR_300MA},
      {400, XPOWERS_AXP2101_CHG_CUR_400MA},
      {500, XPOWERS_AXP2101_CHG_CUR_500MA},
      {600, XPOWERS_AXP2101_CHG_CUR_600MA},
      {700, XPOWERS_AXP2101_CHG_CUR_700MA},
  };
  uint8_t selected = map[0].opt;
  for (const CurrentMap &entry : map) {
    if (ma >= entry.ma) {
      selected = entry.opt;
    }
  }
  return selected;
}

static uint8_t chargeVoltageOptionAxp192(uint16_t mv) {
  if (mv <= 4100) {
    return XPOWERS_AXP192_CHG_VOL_4V1;
  }
  if (mv <= 4150) {
    return XPOWERS_AXP192_CHG_VOL_4V15;
  }
  return XPOWERS_AXP192_CHG_VOL_4V2;
}

static uint8_t chargeVoltageOptionAxp2101(uint16_t mv) {
  if (mv <= 4000) {
    return XPOWERS_AXP2101_CHG_VOL_4V;
  }
  if (mv <= 4100) {
    return XPOWERS_AXP2101_CHG_VOL_4V1;
  }
  return XPOWERS_AXP2101_CHG_VOL_4V2;
}

static uint8_t powerOffOption(uint8_t seconds) {
  if (seconds <= 4) {
    return XPOWERS_POWEROFF_4S;
  }
  if (seconds <= 6) {
    return XPOWERS_POWEROFF_6S;
  }
  if (seconds <= 8) {
    return XPOWERS_POWEROFF_8S;
  }
  return XPOWERS_POWEROFF_10S;
}

static uint16_t normalizedSysPowerDownMv() {
  uint16_t sysDown = settings.sysPowerDownMv;
  sysDown = (sysDown / 100) * 100;
  return clampU16(sysDown, 2600, 3300);
}

static void applyCommonPowerSettings() {
  if (!pmu) {
    return;
  }
  pmu->setSysPowerDownVoltage(normalizedSysPowerDownMv());
  pmu->setPowerKeyPressOffTime(powerOffOption(settings.powerKeyOffSeconds));
  pmu->setPowerKeyPressOnTime(XPOWERS_POWERON_128MS);
}

static void applyPowerSettingsAxp192() {
  applyCommonPowerSettings();
  pmu192.setVbusVoltageLimit(XPOWERS_AXP192_VBUS_VOL_LIM_4V5);
  if (settings.vbusCurrentLimitMa == 100) {
    pmu192.setVbusCurrentLimit(XPOWERS_AXP192_VBUS_CUR_LIM_100MA);
  } else if (settings.vbusCurrentLimitMa == 0) {
    pmu192.setVbusCurrentLimit(XPOWERS_AXP192_VBUS_CUR_LIM_OFF);
  } else {
    pmu192.setVbusCurrentLimit(XPOWERS_AXP192_VBUS_CUR_LIM_500MA);
  }

  pmu192.setChargerConstantCurr(chargeCurrentOptionAxp192(settings.chargeCurrentMa));
  pmu192.setChargeTargetVoltage(chargeVoltageOptionAxp192(settings.chargeTargetMv));
  pmu192.setChargerTerminationCurr(XPOWERS_AXP192_CHG_ITERM_LESS_10_PERCENT);

  if (settings.chargeEnabled) {
    pmu192.enableCharge();
  } else {
    pmu192.disableCharge();
  }
}

static void applyPowerSettingsAxp2101() {
  applyCommonPowerSettings();
  pmu2101.setVbusVoltageLimit(XPOWERS_AXP2101_VBUS_VOL_LIM_4V52);
  if (settings.vbusCurrentLimitMa == 100) {
    pmu2101.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_100MA);
  } else {
    pmu2101.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_500MA);
  }

  pmu2101.setChargerConstantCurr(chargeCurrentOptionAxp2101(settings.chargeCurrentMa));
  pmu2101.setChargeTargetVoltage(chargeVoltageOptionAxp2101(settings.chargeTargetMv));
  pmu2101.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);
  pmu2101.enableChargerTerminationLimit();

  if (settings.chargeEnabled) {
    pmu2101.enableCellbatteryCharge();
  } else {
    pmu2101.disableCellbatteryCharge();
  }
}

static void applyPowerSettings() {
  if (!powerState.online) {
    return;
  }

  if (pmicModel == PmicModel::Axp2101) {
    applyPowerSettingsAxp2101();
  } else if (pmicModel == PmicModel::Axp192) {
    applyPowerSettingsAxp192();
  }
}

static void configureAxp192Rails() {
  pmu192.setChargingLedMode(XPOWERS_CHG_LED_CTRL_CHG);
  pmu192.disableTSPinMeasure();
  pmu192.setProtectedChannel(XPOWERS_DCDC3);
  pmu192.setProtectedChannel(XPOWERS_DCDC1);
  pmu192.setDC1Voltage(3300);
  pmu192.enableDC1();
  pmu192.setLDO3Voltage(3300);
  pmu192.enableLDO3();
  pmu192.setLDO2Voltage(3300);
  pmu192.disableLDO2();
  pmu192.disableDC2();
}

static void configureAxp2101Rails() {
  pmu2101.setChargingLedMode(XPOWERS_CHG_LED_CTRL_CHG);
  pmu2101.disableTSPinMeasure();
  pmu2101.setProtectedChannel(XPOWERS_DCDC1);

  pmu->disablePowerOutput(XPOWERS_DCDC2);
  pmu->disablePowerOutput(XPOWERS_DCDC3);
  pmu->disablePowerOutput(XPOWERS_DCDC4);
  pmu->disablePowerOutput(XPOWERS_DCDC5);
  pmu->disablePowerOutput(XPOWERS_ALDO1);
  pmu->setPowerChannelVoltage(XPOWERS_ALDO2, 3300);
  pmu->disablePowerOutput(XPOWERS_ALDO2);
  pmu->setPowerChannelVoltage(XPOWERS_ALDO3, 3300);
  pmu->enablePowerOutput(XPOWERS_ALDO3);
  pmu->disablePowerOutput(XPOWERS_ALDO4);
  pmu->disablePowerOutput(XPOWERS_BLDO1);
  pmu->disablePowerOutput(XPOWERS_BLDO2);
  pmu->disablePowerOutput(XPOWERS_DLDO1);
  pmu->disablePowerOutput(XPOWERS_DLDO2);
  pmu->disablePowerOutput(XPOWERS_CPULDO);
  pmu->setPowerChannelVoltage(XPOWERS_VBACKUP, 3300);
  pmu->enablePowerOutput(XPOWERS_VBACKUP);
}

static void enablePowerTelemetry() {
  if (!pmu) {
    return;
  }
  pmu->enableBattDetection();
  pmu->enableVbusVoltageMeasure();
  pmu->enableBattVoltageMeasure();
  pmu->enableSystemVoltageMeasure();
  pmu->enableTemperatureMeasure();
}

static void configurePmuInterrupts() {
  if (!pmu) {
    return;
  }
  if (pmicModel == PmicModel::Axp2101) {
    pmu2101.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu2101.clearIrqStatus();
    pmu2101.enableIRQ(XPOWERS_AXP2101_VBUS_REMOVE_IRQ |
                      XPOWERS_AXP2101_VBUS_INSERT_IRQ |
                      XPOWERS_AXP2101_BAT_CHG_DONE_IRQ |
                      XPOWERS_AXP2101_BAT_CHG_START_IRQ |
                      XPOWERS_AXP2101_BAT_REMOVE_IRQ |
                      XPOWERS_AXP2101_BAT_INSERT_IRQ |
                      XPOWERS_AXP2101_PKEY_SHORT_IRQ |
                      XPOWERS_AXP2101_PKEY_LONG_IRQ);
  } else {
    pmu192.disableIRQ(XPOWERS_AXP192_ALL_IRQ);
    pmu192.clearIrqStatus();
    pmu192.enableIRQ(XPOWERS_AXP192_VBUS_REMOVE_IRQ |
                     XPOWERS_AXP192_VBUS_INSERT_IRQ |
                     XPOWERS_AXP192_BAT_CHG_DONE_IRQ |
                     XPOWERS_AXP192_BAT_CHG_START_IRQ |
                     XPOWERS_AXP192_BAT_REMOVE_IRQ |
                     XPOWERS_AXP192_BAT_INSERT_IRQ |
                     XPOWERS_AXP192_PKEY_SHORT_IRQ |
                     XPOWERS_AXP192_PKEY_LONG_IRQ);
  }
}

static bool beginDetectedPmic(PmicModel model) {
  if (model == PmicModel::Axp2101 &&
      pmu2101.begin(Wire, AXP2101_SLAVE_ADDRESS, PIN_I2C_SDA, PIN_I2C_SCL)) {
    rememberDetectedPmic(PmicModel::Axp2101, pmu2101.getChipID());
    configureAxp2101Rails();
    return true;
  }
  if (model == PmicModel::Axp192 &&
      pmu192.begin(Wire, AXP192_SLAVE_ADDRESS, PIN_I2C_SDA, PIN_I2C_SCL)) {
    rememberDetectedPmic(PmicModel::Axp192, pmu192.getChipID());
    configureAxp192Rails();
    return true;
  }
  return false;
}

static void beginPower() {
  uint8_t reg03 = 0;
  if (readPmicRegister(XPOWERS_AXP192_IC_TYPE, reg03)) {
    pmicRegister03 = reg03;
    pmicRegister03Valid = true;
    if (reg03 == XPOWERS_AXP2101_CHIP_ID) {
      powerState.online = beginDetectedPmic(PmicModel::Axp2101);
    } else if (reg03 == XPOWERS_AXP192_CHIP_ID) {
      powerState.online = beginDetectedPmic(PmicModel::Axp192);
    }
  }

  if (!powerState.online) {
    powerState.online = beginDetectedPmic(PmicModel::Axp2101) ||
                        beginDetectedPmic(PmicModel::Axp192);
  }

  if (!powerState.online) {
    pmu = nullptr;
    pmicModel = PmicModel::None;
    hardwareRevision = HardwareRevision::Unknown;
    Serial.println("PMU not detected; continuing without battery telemetry");
    return;
  }

  Serial.printf("%s %s PMU online, register 0x03 = 0x%02X\n",
                hardwareRevisionLabel(), pmicModelLabel(), pmicRegister03);
  enablePowerTelemetry();
  configurePmuInterrupts();
  pinMode(PIN_PMU_IRQ, INPUT);
  attachInterrupt(PIN_PMU_IRQ, onPmuIrq, FALLING);
  applyPowerSettings();
}

static bool powerPollDue() {
  return powerState.online && millis() - lastPowerPollMs >= POWER_POLL_INTERVAL_MS;
}

static float measuredBatteryCurrentMa() {
  if (pmicModel != PmicModel::Axp192) {
    return 0.0f;
  }
  if (powerState.charging) {
    return pmu192.getBatteryChargeCurrent();
  }
  if (powerState.discharging) {
    return -pmu192.getBattDischargeCurrent();
  }
  return 0.0f;
}

static float measuredVbusCurrentMa() {
  if (pmicModel == PmicModel::Axp192) {
    return pmu192.getVbusCurrent();
  }
  return 0.0f;
}

static float measuredPmuTemperatureC() {
  if (pmicModel == PmicModel::Axp2101) {
    return pmu2101.getTemperature();
  }
  if (pmicModel == PmicModel::Axp192) {
    return pmu192.getTemperature();
  }
  return 0.0f;
}

static void samplePowerTelemetry() {
  if (!pmu) {
    return;
  }
  powerState.batteryPresent = pmu->isBatteryConnect();
  powerState.charging = powerState.batteryPresent && pmu->isCharging();
  powerState.discharging = powerState.batteryPresent && pmu->isDischarge();
  powerState.vbusPresent = pmu->isVbusIn();
  powerState.batteryMv = pmu->getBattVoltage();
  powerState.vbusMv = pmu->getVbusVoltage();
  powerState.vbusCurrentMa = measuredVbusCurrentMa();
  powerState.systemMv = pmu->getSystemVoltage();
  powerState.temperatureC = measuredPmuTemperatureC();
  powerState.batteryPercent = powerState.batteryPresent ? pmu->getBatteryPercent() : -1;
  powerState.batteryCurrentMa = measuredBatteryCurrentMa();
}

static bool batteryDischargingBelow(uint16_t thresholdMv) {
  return powerState.batteryPresent && powerState.discharging &&
         powerState.batteryMv > 0 && powerState.batteryMv <= thresholdMv;
}

static void updatePowerWarning(bool wasWarning) {
  powerState.warning = batteryDischargingBelow(settings.warningVoltageMv);
  if (powerState.warning && !wasWarning) {
    wakeOledForPowerWarning();
  }
}

static void updatePowerCutoff() {
  if (!batteryDischargingBelow(settings.cutoffVoltageMv)) {
    lowBatterySinceMs = 0;
    powerState.cutoffPending = false;
    return;
  }

  if (lowBatterySinceMs == 0) {
    lowBatterySinceMs = millis();
  }
  powerState.cutoffPending = true;
  if (millis() - lowBatterySinceMs > BATTERY_CUTOFF_DEBOUNCE_MS) {
    Serial.println("Battery below cutoff while discharging; requesting PMU shutdown");
    if (pmu) {
      pmu->shutdown();
    }
  }
}

static void pollPower() {
  if (!powerPollDue()) {
    return;
  }
  lastPowerPollMs = millis();
  bool wasWarning = powerState.warning;

  samplePowerTelemetry();
  updatePowerWarning(wasWarning);
  updatePowerCutoff();
}

static bool beforeDeadline(uint32_t deadlineMs) {
  return deadlineMs != 0 && static_cast<int32_t>(millis() - deadlineMs) < 0;
}

static void pauseOledAutoCycle() {
  oledAutoCyclePausedUntilMs = millis() + OLED_MANUAL_CYCLE_PAUSE_MS;
  lastOledCycleMs = millis();
}

static void handlePmuIrq() {
  if (!powerState.online || !pmuIrqPending) {
    return;
  }
  pmuIrqPending = false;
  pmu->getIrqStatus();
  if (pmu->isPekeyShortPressIrq()) {
    bool wokeDisplay = oledSleeping;
    wakeOled();
    if (!wokeDisplay) {
      oledPage = (oledPage + 1) % OLED_PAGE_COUNT;
    }
    pauseOledAutoCycle();
    lastOledActivityMs = millis();
    lastOledUpdateMs = 0;
  }
  pmu->clearIrqStatus();
}

static void scanI2c() {
  if (millis() - lastI2cScanMs < 300000 && i2cDevices.length() > 0) {
    return;
  }
  lastI2cScanMs = millis();
  String out;
  for (uint8_t addr = 1; addr < 127; ++addr) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      char buf[8];
      snprintf(buf, sizeof(buf), "0x%02X", addr);
      if (out.length() > 0) {
        out += ", ";
      }
      out += buf;
    }
  }
  i2cDevices = out.length() ? out : "none";
}

static bool hostMatchesNtpList(String host) {
  host = normalizeHost(host);
  for (size_t i = 0; i < settings.ntpHostCount; ++i) {
    if (host == settings.ntpHosts[i]) {
      return true;
    }
  }
  return false;
}

static bool isCaptiveProbeHost(const String &host) {
  static const char *probes[] = {
      "captive.apple.com", "www.apple.com", "connectivitycheck.gstatic.com",
      "clients3.google.com", "www.google.com", "www.msftconnecttest.com",
      "msftconnecttest.com", "dns.msftncsi.com", "detectportal.firefox.com",
      "network-test.debian.org"};
  String normalized = normalizeHost(host);
  for (const char *probe : probes) {
    if (normalized == probe) {
      return true;
    }
  }
  return false;
}

static bool shouldAnswerDns(String host, bool &aliasHit) {
  host = normalizeHost(host);
  aliasHit = false;
  if (host.length() == 0) {
    return false;
  }
  if (host == sanitizedHostname() || host == sanitizedHostname() + ".local") {
    return true;
  }
  if (settings.dnsNtpAliases && hostMatchesNtpList(host)) {
    aliasHit = true;
    return true;
  }
  if (settings.dnsWildcardCaptive || isCaptiveProbeHost(host)) {
    return true;
  }
  return false;
}

static bool readDnsName(const uint8_t *packet, size_t len, size_t &offset, String &name) {
  name = "";
  while (offset < len) {
    uint8_t labelLen = packet[offset++];
    if (labelLen == 0) {
      return true;
    }
    if ((labelLen & 0xC0) != 0 || labelLen > 63 || offset + labelLen > len) {
      return false;
    }
    if (name.length() > 0) {
      name += ".";
    }
    for (uint8_t i = 0; i < labelLen; ++i) {
      name += static_cast<char>(packet[offset++]);
    }
  }
  return false;
}

struct DnsQuestion {
  String name;
  uint16_t type;
  uint16_t qclass;
  size_t endOffset;
};

static bool readDnsQuestion(const uint8_t *packet, size_t len, DnsQuestion &question) {
  size_t offset = DNS_HEADER_SIZE;
  if (!readDnsName(packet, len, offset, question.name) || offset + 4 > len) {
    return false;
  }

  question.type = (packet[offset] << 8) | packet[offset + 1];
  question.qclass = (packet[offset + 2] << 8) | packet[offset + 3];
  question.endOffset = offset + 4;
  return true;
}

static bool shouldAnswerDnsQuestion(const DnsQuestion &question, bool &aliasHit) {
  return question.type == 1 && question.qclass == 1 && shouldAnswerDns(question.name, aliasHit);
}

static void writeDnsResponseHeader(uint8_t *response, const uint8_t *request, size_t questionEnd, bool answer) {
  memcpy(response, request, questionEnd);
  response[2] = 0x81;
  response[3] = answer ? 0x80 : 0x83;
  response[4] = 0x00;
  response[5] = 0x01;
  response[6] = 0x00;
  response[7] = answer ? 0x01 : 0x00;
  response[8] = response[9] = response[10] = response[11] = 0x00;
}

static size_t appendDnsARecord(uint8_t *response, size_t responseLen, size_t responseCapacity) {
  if (responseLen + 16 > responseCapacity) {
    return responseLen;
  }

  response[responseLen++] = 0xC0;
  response[responseLen++] = 0x0C;
  response[responseLen++] = 0x00;
  response[responseLen++] = 0x01;
  response[responseLen++] = 0x00;
  response[responseLen++] = 0x01;
  response[responseLen++] = 0x00;
  response[responseLen++] = 0x00;
  response[responseLen++] = 0x00;
  response[responseLen++] = 0x3C;
  response[responseLen++] = 0x00;
  response[responseLen++] = 0x04;
  response[responseLen++] = settings.apIp[0];
  response[responseLen++] = settings.apIp[1];
  response[responseLen++] = settings.apIp[2];
  response[responseLen++] = settings.apIp[3];
  return responseLen;
}

static void sendDnsResponse(const uint8_t *response, size_t responseLen) {
  dnsUdp.beginPacket(dnsUdp.remoteIP(), dnsUdp.remotePort());
  dnsUdp.write(response, responseLen);
  dnsUdp.endPacket();
}

static void handleDnsPacket(const uint8_t *packet, size_t len) {
  if (len < DNS_HEADER_SIZE) {
    return;
  }
  dnsQueryCount++;

  DnsQuestion question;
  if (!readDnsQuestion(packet, len, question)) {
    return;
  }

  bool aliasHit = false;
  bool answer = shouldAnswerDnsQuestion(question, aliasHit);
  if (aliasHit) {
    dnsAliasHitCount++;
  }

  uint8_t response[DNS_RESPONSE_MAX_SIZE];
  memset(response, 0, sizeof(response));
  writeDnsResponseHeader(response, packet, question.endOffset, answer);
  size_t responseLen = answer ? appendDnsARecord(response, question.endOffset, sizeof(response)) : question.endOffset;
  sendDnsResponse(response, responseLen);
}

static void processDns() {
  if (!dnsOnline) {
    return;
  }
  for (uint8_t handled = 0; handled < 4; ++handled) {
    int packetSize = dnsUdp.parsePacket();
    if (packetSize <= 0) {
      return;
    }

    uint8_t packet[DNS_PACKET_MAX_SIZE];
    int len = dnsUdp.read(packet, sizeof(packet));
    handleDnsPacket(packet, len > 0 ? static_cast<size_t>(len) : 0);
  }
}

static void encodeNtpTimestamp(uint8_t *out, uint64_t unixUsec) {
  uint64_t seconds = unixUsec / 1000000ULL + UNIX_TO_NTP_SECONDS;
  uint64_t fraction = ((unixUsec % 1000000ULL) << 32) / 1000000ULL;
  uint32_t sec32 = static_cast<uint32_t>(seconds);
  uint32_t frac32 = static_cast<uint32_t>(fraction);
  out[0] = sec32 >> 24;
  out[1] = sec32 >> 16;
  out[2] = sec32 >> 8;
  out[3] = sec32;
  out[4] = frac32 >> 24;
  out[5] = frac32 >> 16;
  out[6] = frac32 >> 8;
  out[7] = frac32;
}

static void encodeNtpShort(uint8_t *out, float seconds) {
  uint32_t value = static_cast<uint32_t>(seconds * 65536.0f + 0.5f);
  out[0] = value >> 24;
  out[1] = value >> 16;
  out[2] = value >> 8;
  out[3] = value;
}

static int readNtpRequest(uint8_t *request, size_t requestSize) {
  memset(request, 0, requestSize);
  int len = ntpUdp.read(request, requestSize);
  while (ntpUdp.available()) {
    ntpUdp.read();
  }
  return len;
}

static bool ntpServingClockHealthy(uint64_t &recvUsec) {
  bool synced = getClockUnixUsec(recvUsec);
  // Fail closed: PPS can maintain phase briefly, but fresh NMEA proves the GPS
  // receiver is still reporting valid absolute UTC rather than stale holdover.
  return synced && hasFreshGpsTime();
}

static uint8_t sanitizedNtpVersion(const uint8_t *request) {
  uint8_t version = (request[0] >> 3) & 0x07;
  return version < 3 || version > 4 ? 4 : version;
}

static void buildNtpResponse(const uint8_t *request, uint8_t *response, uint64_t recvUsec) {
  memset(response, 0, NTP_PACKET_SIZE);
  bool ppsLocked = hasRecentPps();
  response[0] = (0 << 6) | (sanitizedNtpVersion(request) << 3) | 4;
  response[1] = 1;
  response[2] = request[2] ? request[2] : 6;
  response[3] = ppsLocked ? static_cast<uint8_t>(-20) : static_cast<uint8_t>(-10);
  encodeNtpShort(response + 4, 0.0f);
  encodeNtpShort(response + 8, ppsLocked ? 0.001f : 0.250f);
  response[12] = 'G';
  response[13] = 'P';
  response[14] = 'S';
  response[15] = ppsLocked ? 0x00 : 'N';
  memcpy(response + 24, request + 40, 8);

  uint64_t refUsec = static_cast<uint64_t>(clockAnchorEpoch) * 1000000ULL;
  uint64_t txUsec = recvUsec;
  getClockUnixUsec(txUsec);
  encodeNtpTimestamp(response + 16, refUsec);
  encodeNtpTimestamp(response + 32, recvUsec);
  encodeNtpTimestamp(response + 40, txUsec);
}

static void sendNtpResponse(const uint8_t *response, size_t responseSize) {
  ntpUdp.beginPacket(ntpUdp.remoteIP(), ntpUdp.remotePort());
  ntpUdp.write(response, responseSize);
  ntpUdp.endPacket();
}

static void handleNtpPacket() {
  uint8_t request[NTP_PACKET_SIZE];
  if (readNtpRequest(request, sizeof(request)) < static_cast<int>(NTP_PACKET_SIZE)) {
    return;
  }

  ntpRequestCount++;
  uint64_t recvUsec = 0;
  if (!ntpServingClockHealthy(recvUsec)) {
    ntpSuppressedCount++;
    return;
  }

  uint8_t response[NTP_PACKET_SIZE];
  buildNtpResponse(request, response, recvUsec);
  sendNtpResponse(response, sizeof(response));
  ntpResponseCount++;
}

static void processNtp() {
  if (!ntpOnline) {
    return;
  }
  for (uint8_t handled = 0; handled < 8; ++handled) {
    int packetSize = ntpUdp.parsePacket();
    if (packetSize <= 0) {
      return;
    }

    handleNtpPacket();
  }
}

static void configureDhcpOptions() {
  ip_addr_t dnsServer;
  IP_ADDR4(&dnsServer, settings.apIp[0], settings.apIp[1], settings.apIp[2], settings.apIp[3]);
  dhcps_dns_setserver(&dnsServer);
  dhcps_offer_t dnsOffer = OFFER_DNS;
  dhcps_set_option_info(OFFER_DNS, &dnsOffer, sizeof(dnsOffer));

  if (settings.dhcpOption42) {
    ip4_addr_t ntpServer;
    IP4_ADDR(&ntpServer, settings.apIp[0], settings.apIp[1], settings.apIp[2], settings.apIp[3]);
    dhcps_set_option_info(NETWORK_TIME_PROTOCOL_SERVERS, &ntpServer, sizeof(ntpServer));
    Serial.println("DHCP option 42 enabled for SoftAP clients");
  } else {
    Serial.println("DHCP option 42 disabled");
  }
}

static void startDnsIfNeeded() {
  if (dnsOnline) {
    dnsUdp.stop();
    dnsOnline = false;
  }
  if (networkMode == NetworkMode::ApFallback &&
      (settings.dnsNtpAliases || settings.dnsWildcardCaptive)) {
    dnsOnline = dnsUdp.begin(DNS_PORT);
    Serial.printf("DNS server %s on %s\n", dnsOnline ? "started" : "failed", ipToString(settings.apIp).c_str());
  }
}

static void startMdns() {
  if (mdnsOnline) {
    MDNS.end();
    mdnsOnline = false;
  }
  String host = sanitizedHostname();
  if (MDNS.begin(host.c_str())) {
    MDNS.addService("ntp", "udp", NTP_PORT);
    MDNS.addService("http", "tcp", 80);
    mdnsOnline = true;
  }
}

static void startNtp() {
  if (!ntpOnline) {
    ntpOnline = ntpUdp.begin(NTP_PORT);
    Serial.printf("NTP server %s on UDP/%u\n", ntpOnline ? "started" : "failed", NTP_PORT);
  }
}

static bool softApIsOpen() {
  return settings.apSecurityMode == ApSecurityMode::Open;
}

static String effectiveSoftApPassword() {
  if (softApIsOpen()) {
    return "";
  }
  return strlen(settings.apPassword) >= 8 ? String(settings.apPassword) : String(DEFAULT_AP_PASSWORD);
}

static bool softApNeedsSecurityOverride() {
  return settings.apSecurityMode != ApSecurityMode::Open &&
         settings.apSecurityMode != ApSecurityMode::Wpa2;
}

static wifi_cipher_type_t softApPairwiseCipher() {
  return settings.apSecurityMode == ApSecurityMode::WpaWpa2
             ? WIFI_CIPHER_TYPE_TKIP_CCMP
             : WIFI_CIPHER_TYPE_CCMP;
}

static bool readSoftApConfig(wifi_config_t &conf) {
  esp_err_t err = esp_wifi_get_config(WIFI_IF_AP, &conf);
  if (err == ESP_OK) {
    return true;
  }
  Serial.printf("AP security read failed: %d\n", err);
  return false;
}

static bool applySoftApSecurityOverride() {
  wifi_config_t conf;
  if (!readSoftApConfig(conf)) {
    return false;
  }

  conf.ap.authmode = apWifiAuthMode(settings.apSecurityMode);
  conf.ap.pairwise_cipher = softApPairwiseCipher();
  esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &conf);
  if (err != ESP_OK) {
    Serial.printf("AP security mode %s failed: %d\n", apSecurityModeName(settings.apSecurityMode), err);
    return false;
  }
  return true;
}

static bool startConfiguredSoftAp(const String &ssid) {
  bool openAp = softApIsOpen();
  String password = effectiveSoftApPassword();
  bool ok = WiFi.softAP(ssid.c_str(), openAp ? nullptr : password.c_str(), settings.apChannel, 0, settings.apMaxClients);
  if (!ok) {
    return false;
  }
  if (!softApNeedsSecurityOverride()) {
    return true;
  }
  return applySoftApSecurityOverride();
}

static void startApMode() {
  networkMode = NetworkMode::ApFallback;
  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect(false, false);
  WiFi.setSleep(false);

  IPAddress leaseStart = settings.apIp;
  leaseStart[3] = min<uint8_t>(settings.apIp[3] + 1, 250);
  WiFi.softAPConfig(settings.apIp, settings.apIp, settings.apSubnet, leaseStart);

  String ssid = expandedApSsid();
  bool ok = startConfiguredSoftAp(ssid);
  WiFi.softAPsetHostname(sanitizedHostname().c_str());

  Serial.printf("AP mode %s: SSID=%s security=%s IP=%s maxClients=%u\n",
                ok ? "started" : "failed", ssid.c_str(),
                apSecurityModeName(settings.apSecurityMode),
                WiFi.softAPIP().toString().c_str(), settings.apMaxClients);
  configureDhcpOptions();
  startDnsIfNeeded();
  startMdns();
}

static bool startStaMode() {
  if (strlen(settings.staSsid) == 0) {
    return false;
  }

  networkMode = NetworkMode::Sta;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname(sanitizedHostname().c_str());

  if (settings.staDhcp) {
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
  } else {
    WiFi.config(settings.staIp, settings.staGateway, settings.staSubnet, settings.staDns1, settings.staDns2);
  }

  Serial.printf("Connecting to WiFi SSID '%s'\n", settings.staSsid);
  WiFi.begin(settings.staSsid, settings.staPassword);
  uint32_t deadline = millis() + settings.staConnectTimeoutSec * 1000UL;
  while (millis() < deadline && WiFi.status() != WL_CONNECTED) {
    consumeGps();
    handlePps();
    delay(100);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("STA connection failed; falling back to AP mode");
    WiFi.disconnect(false, false);
    return false;
  }

  if (dnsOnline) {
    dnsUdp.stop();
    dnsOnline = false;
  }
  Serial.printf("STA connected: IP=%s hostname=%s\n",
                WiFi.localIP().toString().c_str(), sanitizedHostname().c_str());
  startMdns();
  return true;
}

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

static void drawLine(int y, const String &text) {
  display.drawString(0, y, text.substring(0, 22));
}

static void drawCentered(int y, const String &text, const uint8_t *font) {
  display.setFont(font);
  int x = max(0, (128 - display.getStringWidth(text)) / 2);
  display.drawString(x, y, text);
}

static void drawText(int y, const String &text, const uint8_t *font) {
  display.setFont(font);
  display.drawString(0, y, text);
}

static void drawTimePage(const String &title, const String &timestamp, bool servingTime) {
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  drawLine(0, title);
  if (servingTime) {
    drawCentered(17, timestamp.substring(11), ArialMT_Plain_24);
    display.setFont(ArialMT_Plain_10);
    drawCentered(48, timestamp.substring(0, 10), ArialMT_Plain_10);
  } else {
    drawCentered(20, "NO GPS", ArialMT_Plain_24);
    drawCentered(48, "TIME", ArialMT_Plain_10);
  }
}

static String apPasswordForDisplay() {
  if (settings.apSecurityMode == ApSecurityMode::Open || strlen(settings.apPassword) == 0) {
    return "(open)";
  }
  return String(settings.apPassword);
}

static bool oledTextExceedsWidth(const String &text, const uint8_t *font) {
  display.setFont(font);
  return display.getStringWidth(text) > 128;
}

static void drawScrollingText(int y, const String &text, const uint8_t *font) {
  display.setFont(font);
  int width = display.getStringWidth(text);
  if (width <= 128) {
    display.drawString(0, y, text);
    return;
  }

  constexpr int gapPx = 28;
  uint32_t span = static_cast<uint32_t>(width + gapPx);
  int offset = static_cast<int>((millis() / 90UL) % span);
  display.drawString(-offset, y, text);
  display.drawString(width + gapPx - offset, y, text);
}

static bool networkPageHasScrollingText() {
  if (oledPage != 5) {
    return false;
  }
  String networkName = networkMode == NetworkMode::Sta ? WiFi.SSID() : expandedApSsid();
  return oledTextExceedsWidth(networkName, ArialMT_Plain_10) ||
         oledTextExceedsWidth("Pass " + apPasswordForDisplay(), ArialMT_Plain_10);
}

static void ensureOledAwakeForPrompt() {
  if (!oledOnline) {
    return;
  }
  if (oledSleeping) {
    display.displayOn();
    oledSleeping = false;
  }
  splashUntilMs = 0;
  lastOledActivityMs = millis();
  lastOledUpdateMs = millis();
}

static void showPromptScreen(const String &title, const String &line1, const String &line2) {
  if (!oledOnline) {
    return;
  }
  ensureOledAwakeForPrompt();
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  drawCentered(0, title, ArialMT_Plain_16);
  drawCentered(25, line1, ArialMT_Plain_10);
  drawCentered(39, line2, ArialMT_Plain_10);
  display.display();
}

static void showBootSplash() {
  if (!oledOnline) {
    return;
  }
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  drawCentered(0, "T-Beam", ArialMT_Plain_24);
  drawCentered(28, "NTP Server", ArialMT_Plain_16);
  drawCentered(51, FIRMWARE_VERSION, ArialMT_Plain_10);
  display.display();
  splashUntilMs = millis() + 2500UL;
}

static void showBootButtonPrompt() {
  showPromptScreen("Button Held", "Hold 10s: reset", "Release: AP mode");
}

static void showBootForceApPrompt() {
  showPromptScreen("AP Mode", "Forced for", "this boot");
}

static void showResetConfirmPrompt() {
  showPromptScreen("Reset?", "Hold button", "10s again");
}

static void showFactoryResetPrompt() {
  showPromptScreen("Resetting", "Settings cleared", "Restarting");
}

static void wakeOled() {
  if (!oledOnline) {
    return;
  }
  if (oledSleeping) {
    display.displayOn();
    oledSleeping = false;
  }
  lastOledActivityMs = millis();
  lastOledUpdateMs = 0;
}

static void sleepOled() {
  if (!oledOnline || oledSleeping) {
    return;
  }
  display.clear();
  display.display();
  display.displayOff();
  oledSleeping = true;
}

static void wakeOledForPowerWarning() {
  if (!oledOnline) {
    return;
  }
  if (oledSleeping) {
    display.displayOn();
    oledSleeping = false;
    lastOledActivityMs = millis();
  }
  lastOledUpdateMs = 0;
}

static void drawLowBatteryOverlay() {
  if (!powerState.warning || ((millis() / 500UL) & 1UL) != 0) {
    return;
  }

  const String label = "LOW BATTERY";
  display.setFont(ArialMT_Plain_10);
  int width = min(128, display.getStringWidth(label) + 6);
  int x = 128 - width;
  int y = 51;
  display.setColor(WHITE);
  display.fillRect(x, y, width, 13);
  display.setColor(BLACK);
  display.drawString(x + 3, y + 1, label);
  display.setColor(WHITE);
}

static void drawOledSummaryPage(uint64_t nowUsec, bool servingTime, bool gpsFix) {
  drawLine(0, networkMode == NetworkMode::Sta ? "WiFi STA " + WiFi.localIP().toString() : "AP " + WiFi.softAPIP().toString());
  drawLine(12, servingTime ? "UTC " + formatUtc(nowUsec).substring(11) : "NTP gated: no GPS");
  drawLine(24, servingTime ? "LOC " + formatLocal(nowUsec).substring(11) : "LOC not synced");
  drawLine(36, String("GPS ") + (gpsFix ? "fix" : "no fix") + " PPS " + (hasRecentPps() ? "lock" : "wait"));
  drawLine(48, String("NTP ") + ntpRequestCount + "/" + ntpSuppressedCount + " DNS " + dnsQueryCount);
}

static void drawOledGridPage(bool gpsFix) {
  String grid = gpsFix ? maidenhead(gps.location.lat(), gps.location.lng()) : "";
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  drawLine(0, "Grid Square");
  drawCentered(17, grid.length() ? grid : "--", ArialMT_Plain_24);
  display.setFont(ArialMT_Plain_10);
  drawCentered(48, gpsFix ? "GPS fix" : "waiting for fix", ArialMT_Plain_10);
}

static void drawOledGpsPage(bool gpsFix) {
  drawText(0, String("Sat ") + (gps.satellites.isValid() ? gps.satellites.value() : 0) + " DOP " + gpsDopString(), ArialMT_Plain_16);
  drawText(16, gpsFix ? "Lat " + String(gps.location.lat(), 4) : "Lat --", ArialMT_Plain_16);
  drawText(32, gpsFix ? "Lon " + String(gps.location.lng(), 4) : "Lon --", ArialMT_Plain_16);
  drawText(48, gpsFix && gps.altitude.isValid() ? "Alt " + String(gps.altitude.meters(), 0) + "m" : "Alt --", ArialMT_Plain_16);
}

static void drawOledNetworkPage() {
  drawLine(0, networkMode == NetworkMode::Sta ? "Network client" : "Standalone AP");
  drawScrollingText(12, networkMode == NetworkMode::Sta ? WiFi.SSID() : expandedApSsid(), ArialMT_Plain_10);
  drawLine(24, networkMode == NetworkMode::Sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString());
  drawLine(36, String("Clients ") + (networkMode == NetworkMode::ApFallback ? WiFi.softAPgetStationNum() : 0) + "/" + settings.apMaxClients);
  drawScrollingText(48, "Pass " + apPasswordForDisplay(), ArialMT_Plain_10);
}

static void drawOledPowerPage() {
  drawText(0, powerState.batteryPresent ? String("Bat ") + powerState.batteryMv + "mV" : "No battery", ArialMT_Plain_16);
  drawText(16, String("I ") + String(powerState.batteryCurrentMa, 0) + "mA", ArialMT_Plain_16);
  drawText(32, powerState.vbusPresent ? String("USB ") + powerState.vbusMv + "mV" : "USB absent", ArialMT_Plain_16);
  drawText(48, powerState.warning ? String("Bat warning") : String("Temp ") + String(powerState.temperatureC, 0) + "C", ArialMT_Plain_16);
}

static void drawOledPage(uint64_t nowUsec, bool servingTime, bool gpsFix) {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);

  switch (oledPage) {
  case 0:
    drawOledSummaryPage(nowUsec, servingTime, gpsFix);
    break;
  case 1:
    drawTimePage("UTC", servingTime ? formatUtc(nowUsec) : "", servingTime);
    break;
  case 2:
    drawTimePage("Local Time", servingTime ? formatLocal(nowUsec) : "", servingTime);
    break;
  case 3:
    drawOledGridPage(gpsFix);
    break;
  case 4:
    drawOledGpsPage(gpsFix);
    break;
  case 5:
    drawOledNetworkPage();
    break;
  default:
    drawOledPowerPage();
    break;
  }

  drawLowBatteryOverlay();
  display.display();
}

static bool skipOledForSplash() {
  if (splashUntilMs == 0) {
    return false;
  }
  if (millis() < splashUntilMs) {
    return true;
  }
  splashUntilMs = 0;
  return false;
}

static void trackGpsFixForDisplay(bool gpsFix) {
  if (gpsFix == lastDisplayGpsFix) {
    return;
  }
  lastDisplayGpsFix = gpsFix;
  wakeOled();
}

static bool applyOledScreensaver() {
  if (lastOledActivityMs == 0) {
    lastOledActivityMs = millis();
  }
  if (!oledSleeping && settings.oledScreensaverTimeoutSec > 0 &&
      millis() - lastOledActivityMs >= static_cast<uint32_t>(settings.oledScreensaverTimeoutSec) * 1000UL) {
    sleepOled();
    return true;
  }
  return oledSleeping;
}

static void updateOledAutoCycle() {
  if (!settings.oledAutoCycle || settings.oledCycleSeconds == 0 ||
      beforeDeadline(oledAutoCyclePausedUntilMs) ||
      millis() - lastOledCycleMs < static_cast<uint32_t>(settings.oledCycleSeconds) * 1000UL) {
    return;
  }
  oledPage = (oledPage + 1) % OLED_PAGE_COUNT;
  lastOledCycleMs = millis();
  lastOledUpdateMs = 0;
}

static bool oledRefreshDue() {
  uint32_t refreshMs = (powerState.warning || networkPageHasScrollingText()) ? 250UL : 1000UL;
  if (millis() - lastOledUpdateMs < refreshMs) {
    return false;
  }
  lastOledUpdateMs = millis();
  return true;
}

static void factoryResetSettings(const char *reason) {
  Serial.printf("Factory reset requested: %s\n", reason ? reason : "user button");
  showFactoryResetPrompt();
  if (littleFsFileExists("/settings.json")) {
    LittleFS.remove("/settings.json");
  }
  delay(250);
  ESP.restart();
}

static bool userButtonPressed() {
  return digitalRead(PIN_USER_BUTTON) == LOW;
}

static void forceApForCurrentBoot() {
  bootForceApMode = true;
  showBootForceApPrompt();
  delay(900);
}

static bool waitForContinuousButtonHold(uint32_t holdMs) {
  uint32_t start = millis();
  while (millis() - start < holdMs) {
    if (!userButtonPressed()) {
      return false;
    }
    delay(50);
  }
  return true;
}

static bool waitForButtonRelease(uint32_t timeoutMs) {
  uint32_t deadline = millis() + timeoutMs;
  while (userButtonPressed() && beforeDeadline(deadline)) {
    delay(50);
  }
  return !userButtonPressed();
}

static void waitForResetConfirmationHold() {
  uint32_t confirmUntilMs = millis() + FACTORY_RESET_CONFIRM_WINDOW_MS;
  while (beforeDeadline(confirmUntilMs)) {
    if (!userButtonPressed()) {
      delay(50);
      continue;
    }

    if (waitForContinuousButtonHold(USER_BUTTON_FACTORY_RESET_MS)) {
      factoryResetSettings("boot confirmed button hold");
    }

    Serial.println("Reset confirmation press released early; forcing AP mode for this boot");
    forceApForCurrentBoot();
    return;
  }

  Serial.println("Reset confirmation timed out; forcing AP mode for this boot");
  forceApForCurrentBoot();
}

static void checkBootFactoryReset() {
  if (!userButtonPressed()) {
    return;
  }

  Serial.println("User button held at boot; release before 10 seconds to force AP mode");
  showBootButtonPrompt();
  if (!waitForContinuousButtonHold(USER_BUTTON_FACTORY_RESET_MS)) {
    Serial.println("Boot button released before reset threshold; forcing AP mode for this boot");
    forceApForCurrentBoot();
    return;
  }

  Serial.println("Factory reset threshold reached; waiting for confirmation hold");
  showResetConfirmPrompt();
  if (!waitForButtonRelease(FACTORY_RESET_CONFIRM_WINDOW_MS)) {
    Serial.println("Reset confirmation canceled; button was not released");
    forceApForCurrentBoot();
    return;
  }

  waitForResetConfirmationHold();
}

static void updateOled() {
  if (!oledOnline) {
    return;
  }

  if (skipOledForSplash()) {
    return;
  }

  uint64_t nowUsec = 0;
  bool synced = getClockUnixUsec(nowUsec);
  bool servingTime = synced && hasFreshGpsTime();
  bool gpsFix = hasFreshGpsFix();

  trackGpsFixForDisplay(gpsFix);
  if (applyOledScreensaver()) {
    return;
  }

  updateOledAutoCycle();
  if (!oledRefreshDue()) {
    return;
  }

  drawOledPage(nowUsec, servingTime, gpsFix);
}

static void handleUserButton() {
  static bool lastRawState = true;
  static bool stableState = true;
  static uint32_t lastEdgeMs = 0;
  static uint32_t pressedAtMs = 0;
  static bool pressWokeDisplay = false;

  bool rawState = digitalRead(PIN_USER_BUTTON);
  if (rawState != lastRawState) {
    lastRawState = rawState;
    lastEdgeMs = millis();
  }
  if (millis() - lastEdgeMs < 60) {
    return;
  }

  if (rawState != stableState) {
    stableState = rawState;
    if (stableState == LOW) {
      pressedAtMs = millis();
      pressWokeDisplay = oledSleeping;
      wakeOled();
      pauseOledAutoCycle();
    } else {
      uint32_t pressDuration = pressedAtMs ? millis() - pressedAtMs : 0;
      if (!pressWokeDisplay && pressDuration < USER_BUTTON_FACTORY_RESET_MS) {
        oledPage = (oledPage + 1) % OLED_PAGE_COUNT;
        pauseOledAutoCycle();
        lastOledUpdateMs = 0;
      }
      pressedAtMs = 0;
    }
  }
}

static void checkStaHealth() {
  if (networkMode != NetworkMode::Sta) {
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    staLostSinceMs = 0;
    return;
  }
  if (staLostSinceMs == 0) {
    staLostSinceMs = millis();
  }
  if (millis() - staLostSinceMs > 60000) {
    Serial.println("STA lost for 60 seconds; falling back to AP mode");
    startApMode();
  }
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);
  Serial.println();
  Serial.println("T-Beam GPS disciplined NTP server booting");

#if CONFIG_BT_ENABLED
  esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
#endif

  WiFi.persistent(false);
  pinMode(PIN_USER_BUTTON, INPUT);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  }
  loadSettings();
  refreshLittleFsPresenceFlags();

  beginPower();
  delay(100);
  oledOnline = display.init();
  if (oledOnline) {
    display.flipScreenVertically();
    showBootSplash();
    lastOledActivityMs = millis();
    lastOledCycleMs = millis();
  }
  checkBootFactoryReset();

  pinMode(PIN_GPS_PPS, INPUT);
  attachInterrupt(PIN_GPS_PPS, onPps, RISING);
  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

  scanI2c();
  if (bootForceApMode ||
      settings.networkStartMode == NetworkStartMode::StandaloneAp ||
      !startStaMode()) {
    startApMode();
  }
  startNtp();
  beginWeb();
  Serial.println("HTTP portal started");
}

void loop() {
  consumeGps();
  handlePps();
  handlePmuIrq();
  pollPower();
  scanI2c();
  handleUserButton();
  server.handleClient();
  processDns();
  processNtp();
  updateOled();
  checkStaHealth();
  delay(1);
}
