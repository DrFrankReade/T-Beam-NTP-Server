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
static constexpr size_t MAX_NTP_HOSTS = 64;
static constexpr size_t POSIX_TZ_MAX_LEN = 96;
static constexpr uint64_t UNIX_TO_NTP_SECONDS = 2208988800ULL;
static constexpr uint8_t OLED_PAGE_COUNT = 7;
static constexpr uint32_t USER_BUTTON_FACTORY_RESET_MS = 10000;
static constexpr uint32_t FACTORY_RESET_CONFIRM_WINDOW_MS = 30000;
static constexpr uint32_t OLED_MANUAL_CYCLE_PAUSE_MS = 30000;
static constexpr const char *FIRMWARE_VERSION = "v0.1.8";
static constexpr const char *DEFAULT_POSIX_TZ = "PST8PDT,M3.2.0/2,M11.1.0/2";
static constexpr const char *DEFAULT_IANA_TIME_ZONE = "America/Los_Angeles";
static constexpr const char *DEFAULT_AP_PASSWORD = "tbeam-ntp";
static constexpr const char *TIMEZONE_DB_PATH = "/timezones.current.tsv";

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

static DeviceSettings settings;
static PowerState powerState;
static WebServer server(80);
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
static XPowersPMU pmu;

static NetworkMode networkMode = NetworkMode::ApFallback;
static bool oledOnline = false;
static bool ntpOnline = false;
static bool dnsOnline = false;
static bool mdnsOnline = false;
static uint32_t ntpRequestCount = 0;
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
    if (c >= 33 && c <= 126) {
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
    bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') || c == '/' || c == '_' ||
                   c == '-' || c == '+';
    if (allowed) {
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

static void setHardDefaults() {
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

  copyText(settings.apSsid, sizeof(settings.apSsid), "TBeam-NTP-{mac}");
  copyText(settings.apPassword, sizeof(settings.apPassword), DEFAULT_AP_PASSWORD);
  settings.apSecurityMode = ApSecurityMode::Wpa2;
  settings.apIp = IPAddress(192, 168, 4, 1);
  settings.apSubnet = IPAddress(255, 255, 255, 0);
  settings.apChannel = 6;
  settings.apMaxClients = 8;

  settings.dhcpOption42 = true;
  settings.dnsNtpAliases = true;
  settings.dnsWildcardCaptive = true;
  addDefaultNtpHosts();

  settings.autoLocalOffset = false;
  settings.manualOffsetMinutes = 0;
  settings.observeDst = true;
  settings.localTimeMode = LocalTimeMode::Iana;
  copyIanaTimeZone(settings.ianaTimeZone, sizeof(settings.ianaTimeZone), DEFAULT_IANA_TIME_ZONE);
  copyPosixTimezone(settings.posixTimezone, sizeof(settings.posixTimezone), DEFAULT_POSIX_TZ);

  settings.oledAutoCycle = true;
  settings.oledCycleSeconds = 5;
  settings.oledScreensaverTimeoutSec = 300;

  settings.chargeEnabled = true;
  settings.chargeCurrentMa = 280;
  settings.chargeTargetMv = 4100;
  settings.vbusCurrentLimitMa = 500;
  settings.warningVoltageMv = 3500;
  settings.cutoffVoltageMv = 3200;
  settings.sysPowerDownMv = 3000;
  settings.powerKeyOffSeconds = 6;
}

static bool applySettingsJson(JsonDocument &doc) {
  bool explicitNetworkStartMode = false;
  if (doc["networkMode"].is<const char *>()) {
    settings.networkStartMode = parseNetworkStartMode(doc["networkMode"], settings.networkStartMode);
    explicitNetworkStartMode = true;
  }

  if (doc["hostname"].is<const char *>()) {
    copyText(settings.hostname, sizeof(settings.hostname), doc["hostname"]);
  }

  JsonObject wifi = doc["wifi"].as<JsonObject>();
  if (!wifi.isNull()) {
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

  JsonObject ap = doc["ap"].as<JsonObject>();
  if (!ap.isNull()) {
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

  if (!explicitNetworkStartMode && strlen(settings.staSsid) > 0) {
    settings.networkStartMode = NetworkStartMode::ClientWithApFallback;
  }

  if (settings.apSecurityMode != ApSecurityMode::Open && strlen(settings.apPassword) < 8) {
    copyText(settings.apPassword, sizeof(settings.apPassword), DEFAULT_AP_PASSWORD);
  }

  JsonObject standalone = doc["standalone"].as<JsonObject>();
  if (!standalone.isNull()) {
    settings.dhcpOption42 = standalone["dhcpOption42"] | settings.dhcpOption42;
    settings.dnsNtpAliases = standalone["dnsNtpAliases"] | settings.dnsNtpAliases;
    settings.dnsWildcardCaptive = standalone["dnsWildcardCaptive"] | settings.dnsWildcardCaptive;
    if (standalone["ntpHosts"].is<JsonArray>()) {
      settings.ntpHostCount = 0;
      for (JsonVariant value : standalone["ntpHosts"].as<JsonArray>()) {
        if (value.is<const char *>()) {
          addNtpHost(value.as<const char *>());
        }
      }
    }
  }

  JsonObject time = doc["time"].as<JsonObject>();
  if (!time.isNull()) {
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

  JsonObject displaySettings = doc["display"].as<JsonObject>();
  if (!displaySettings.isNull()) {
    settings.oledAutoCycle = displaySettings["autoCycle"] | settings.oledAutoCycle;
    settings.oledCycleSeconds = clampU16(displaySettings["cycleSeconds"] | settings.oledCycleSeconds, 2, 60);
    settings.oledScreensaverTimeoutSec = clampU16(displaySettings["screensaverTimeoutSec"] | settings.oledScreensaverTimeoutSec, 0, 3600);
  }

  JsonObject power = doc["power"].as<JsonObject>();
  if (!power.isNull()) {
    settings.chargeEnabled = power["chargeEnabled"] | settings.chargeEnabled;
    settings.chargeCurrentMa = clampU16(power["chargeCurrentMa"] | settings.chargeCurrentMa, 100, 700);
    settings.chargeTargetMv = clampU16(power["chargeTargetMv"] | settings.chargeTargetMv, 4100, 4200);
    settings.vbusCurrentLimitMa = power["vbusCurrentLimitMa"] | settings.vbusCurrentLimitMa;
    if (settings.vbusCurrentLimitMa != 100 && settings.vbusCurrentLimitMa != 500 && settings.vbusCurrentLimitMa != 0) {
      settings.vbusCurrentLimitMa = 500;
    }
    settings.warningVoltageMv = clampU16(power["warningVoltageMv"] | settings.warningVoltageMv, 3200, 3900);
    settings.cutoffVoltageMv = clampU16(power["cutoffVoltageMv"] | settings.cutoffVoltageMv, 3000, 3600);
    settings.sysPowerDownMv = clampU16(power["sysPowerDownMv"] | settings.sysPowerDownMv, 2600, 3300);
    settings.powerKeyOffSeconds = power["powerKeyOffSeconds"] | settings.powerKeyOffSeconds;
    if (settings.powerKeyOffSeconds != 4 && settings.powerKeyOffSeconds != 6 &&
        settings.powerKeyOffSeconds != 8 && settings.powerKeyOffSeconds != 10) {
      settings.powerKeyOffSeconds = 6;
    }
  }

  return true;
}

static String expandedApSsid();

static void settingsToJson(JsonDocument &doc) {
  doc["networkMode"] = networkStartModeName(settings.networkStartMode);
  doc["hostname"] = settings.hostname;

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

  JsonObject ap = doc.createNestedObject("ap");
  ap["ssid"] = settings.apSsid;
  ap["password"] = settings.apPassword;
  ap["security"] = apSecurityModeName(settings.apSecurityMode);
  ap["ip"] = ipToString(settings.apIp);
  ap["subnet"] = ipToString(settings.apSubnet);
  ap["channel"] = settings.apChannel;
  ap["maxClients"] = settings.apMaxClients;
  ap["expandedSsid"] = expandedApSsid();

  JsonObject standalone = doc.createNestedObject("standalone");
  standalone["dhcpOption42"] = settings.dhcpOption42;
  standalone["dnsNtpAliases"] = settings.dnsNtpAliases;
  standalone["dnsWildcardCaptive"] = settings.dnsWildcardCaptive;
  JsonArray hosts = standalone.createNestedArray("ntpHosts");
  for (size_t i = 0; i < settings.ntpHostCount; ++i) {
    hosts.add(settings.ntpHosts[i]);
  }

  JsonObject time = doc.createNestedObject("time");
  time["mode"] = localTimeModeName(settings.localTimeMode);
  time["autoLocalOffset"] = settings.autoLocalOffset;
  time["manualOffsetMinutes"] = settings.manualOffsetMinutes;
  time["observeDst"] = settings.observeDst;
  time["ianaTimeZone"] = settings.ianaTimeZone;
  time["posixTimezone"] = settings.posixTimezone;
  time["databasePath"] = TIMEZONE_DB_PATH;

  JsonObject displaySettings = doc.createNestedObject("display");
  displaySettings["autoCycle"] = settings.oledAutoCycle;
  displaySettings["cycleSeconds"] = settings.oledCycleSeconds;
  displaySettings["screensaverTimeoutSec"] = settings.oledScreensaverTimeoutSec;

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

static String sanitizedHostname() {
  String host = settings.hostname;
  host.trim();
  host.toLowerCase();
  String clean;
  for (size_t i = 0; i < host.length() && clean.length() < 31; ++i) {
    char c = host.charAt(i);
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
      clean += c;
    } else if (c == '_' || c == ' ' || c == '.') {
      clean += '-';
    }
  }
  clean.trim();
  while (clean.startsWith("-")) {
    clean.remove(0, 1);
  }
  while (clean.endsWith("-")) {
    clean.remove(clean.length() - 1);
  }
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

static uint8_t chargeCurrentOption(uint16_t ma) {
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

static uint8_t chargeVoltageOption(uint16_t mv) {
  if (mv <= 4100) {
    return XPOWERS_AXP192_CHG_VOL_4V1;
  }
  if (mv <= 4150) {
    return XPOWERS_AXP192_CHG_VOL_4V15;
  }
  return XPOWERS_AXP192_CHG_VOL_4V2;
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

static void applyPowerSettings() {
  if (!powerState.online) {
    return;
  }

  uint16_t sysDown = settings.sysPowerDownMv;
  sysDown = (sysDown / 100) * 100;
  sysDown = clampU16(sysDown, 2600, 3300);

  pmu.setSysPowerDownVoltage(sysDown);
  pmu.setVbusVoltageLimit(XPOWERS_AXP192_VBUS_VOL_LIM_4V5);
  if (settings.vbusCurrentLimitMa == 100) {
    pmu.setVbusCurrentLimit(XPOWERS_AXP192_VBUS_CUR_LIM_100MA);
  } else if (settings.vbusCurrentLimitMa == 0) {
    pmu.setVbusCurrentLimit(XPOWERS_AXP192_VBUS_CUR_LIM_OFF);
  } else {
    pmu.setVbusCurrentLimit(XPOWERS_AXP192_VBUS_CUR_LIM_500MA);
  }

  pmu.setChargerConstantCurr(chargeCurrentOption(settings.chargeCurrentMa));
  pmu.setChargeTargetVoltage(chargeVoltageOption(settings.chargeTargetMv));
  pmu.setChargerTerminationCurr(XPOWERS_AXP192_CHG_ITERM_LESS_10_PERCENT);

  if (settings.chargeEnabled) {
    pmu.enableCharge();
  } else {
    pmu.disableCharge();
  }

  pmu.setPowerKeyPressOffTime(powerOffOption(settings.powerKeyOffSeconds));
  pmu.setPowerKeyPressOnTime(XPOWERS_POWERON_128MS);
}

static void beginPower() {
  powerState.online = pmu.begin(Wire, AXP192_SLAVE_ADDRESS, PIN_I2C_SDA, PIN_I2C_SCL);
  if (!powerState.online) {
    Serial.println("AXP192 PMU not detected; continuing without battery telemetry");
    return;
  }

  Serial.printf("AXP192 PMU online, chip ID 0x%02X\n", pmu.getChipID());
  pmu.setChargingLedMode(XPOWERS_CHG_LED_CTRL_CHG);
  pmu.disableTSPinMeasure();

  pmu.setProtectedChannel(XPOWERS_DCDC3);
  pmu.setProtectedChannel(XPOWERS_DCDC1);

  pmu.setDC1Voltage(3300);
  pmu.enableDC1();

  pmu.setLDO3Voltage(3300);
  pmu.enableLDO3();

  pmu.setLDO2Voltage(3300);
  pmu.disableLDO2();
  pmu.disableDC2();

  pmu.enableBattDetection();
  pmu.enableVbusVoltageMeasure();
  pmu.enableBattVoltageMeasure();
  pmu.enableSystemVoltageMeasure();

  pmu.disableIRQ(XPOWERS_AXP192_ALL_IRQ);
  pmu.clearIrqStatus();
  pmu.enableIRQ(XPOWERS_AXP192_VBUS_REMOVE_IRQ |
                XPOWERS_AXP192_VBUS_INSERT_IRQ |
                XPOWERS_AXP192_BAT_CHG_DONE_IRQ |
                XPOWERS_AXP192_BAT_CHG_START_IRQ |
                XPOWERS_AXP192_BAT_REMOVE_IRQ |
                XPOWERS_AXP192_BAT_INSERT_IRQ |
                XPOWERS_AXP192_PKEY_SHORT_IRQ |
                XPOWERS_AXP192_PKEY_LONG_IRQ);

  pinMode(PIN_PMU_IRQ, INPUT);
  attachInterrupt(PIN_PMU_IRQ, onPmuIrq, FALLING);
  applyPowerSettings();
}

static void pollPower() {
  if (!powerState.online || millis() - lastPowerPollMs < 1000) {
    return;
  }
  lastPowerPollMs = millis();
  bool wasWarning = powerState.warning;

  powerState.batteryPresent = pmu.isBatteryConnect();
  powerState.charging = powerState.batteryPresent && pmu.isCharging();
  powerState.discharging = powerState.batteryPresent && pmu.isDischarge();
  powerState.vbusPresent = pmu.isVbusIn();
  powerState.batteryMv = pmu.getBattVoltage();
  powerState.vbusMv = pmu.getVbusVoltage();
  powerState.vbusCurrentMa = pmu.getVbusCurrent();
  powerState.systemMv = pmu.getSystemVoltage();
  powerState.temperatureC = pmu.getTemperature();
  powerState.batteryPercent = powerState.batteryPresent ? pmu.getBatteryPercent() : -1;

  if (powerState.charging) {
    powerState.batteryCurrentMa = pmu.getBatteryChargeCurrent();
  } else if (powerState.discharging) {
    powerState.batteryCurrentMa = -pmu.getBattDischargeCurrent();
  } else {
    powerState.batteryCurrentMa = 0.0f;
  }

  powerState.warning = powerState.batteryPresent && powerState.discharging &&
                       powerState.batteryMv > 0 && powerState.batteryMv <= settings.warningVoltageMv;
  if (powerState.warning && !wasWarning) {
    wakeOledForPowerWarning();
  }

  bool belowCutoff = powerState.batteryPresent && powerState.discharging &&
                     powerState.batteryMv > 0 && powerState.batteryMv <= settings.cutoffVoltageMv;
  if (belowCutoff) {
    if (lowBatterySinceMs == 0) {
      lowBatterySinceMs = millis();
    }
    powerState.cutoffPending = true;
    if (millis() - lowBatterySinceMs > 30000) {
      Serial.println("Battery below cutoff while discharging; requesting PMU shutdown");
      pmu.shutdown();
    }
  } else {
    lowBatterySinceMs = 0;
    powerState.cutoffPending = false;
  }
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
  pmu.getIrqStatus();
  if (pmu.isPekeyShortPressIrq()) {
    bool wokeDisplay = oledSleeping;
    wakeOled();
    if (!wokeDisplay) {
      oledPage = (oledPage + 1) % OLED_PAGE_COUNT;
    }
    pauseOledAutoCycle();
    lastOledActivityMs = millis();
    lastOledUpdateMs = 0;
  }
  pmu.clearIrqStatus();
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

static void processDns() {
  if (!dnsOnline) {
    return;
  }
  for (uint8_t handled = 0; handled < 4; ++handled) {
    int packetSize = dnsUdp.parsePacket();
    if (packetSize <= 0) {
      return;
    }

    uint8_t packet[512];
    int len = dnsUdp.read(packet, sizeof(packet));
    if (len < 12) {
      continue;
    }
    dnsQueryCount++;

    size_t offset = 12;
    String qname;
    bool validName = readDnsName(packet, len, offset, qname);
    if (!validName || offset + 4 > static_cast<size_t>(len)) {
      continue;
    }
    uint16_t qtype = (packet[offset] << 8) | packet[offset + 1];
    uint16_t qclass = (packet[offset + 2] << 8) | packet[offset + 3];
    offset += 4;
    size_t questionEnd = offset;

    bool aliasHit = false;
    bool answer = qtype == 1 && qclass == 1 && shouldAnswerDns(qname, aliasHit);
    if (aliasHit) {
      dnsAliasHitCount++;
    }

    uint8_t response[576];
    memset(response, 0, sizeof(response));
    memcpy(response, packet, questionEnd);
    response[2] = 0x81;
    response[3] = answer ? 0x80 : 0x83;
    response[4] = 0x00;
    response[5] = 0x01;
    response[6] = 0x00;
    response[7] = answer ? 0x01 : 0x00;
    response[8] = response[9] = response[10] = response[11] = 0x00;

    size_t responseLen = questionEnd;
    if (answer && responseLen + 16 <= sizeof(response)) {
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
    }

    dnsUdp.beginPacket(dnsUdp.remoteIP(), dnsUdp.remotePort());
    dnsUdp.write(response, responseLen);
    dnsUdp.endPacket();
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

static void processNtp() {
  if (!ntpOnline) {
    return;
  }
  for (uint8_t handled = 0; handled < 8; ++handled) {
    int packetSize = ntpUdp.parsePacket();
    if (packetSize <= 0) {
      return;
    }

    uint8_t request[48];
    memset(request, 0, sizeof(request));
    int len = ntpUdp.read(request, sizeof(request));
    while (ntpUdp.available()) {
      ntpUdp.read();
    }
    if (len < 48) {
      continue;
    }

    ntpRequestCount++;
    uint64_t recvUsec = 0;
    bool synced = getClockUnixUsec(recvUsec);
    // Fail closed: PPS can maintain phase briefly, but fresh NMEA proves the GPS
    // receiver is still reporting valid absolute UTC rather than stale holdover.
    bool healthy = synced && hasFreshGpsTime();
    if (!healthy) {
      ntpSuppressedCount++;
      continue;
    }

    uint8_t response[48];
    memset(response, 0, sizeof(response));
    uint8_t version = (request[0] >> 3) & 0x07;
    if (version < 3 || version > 4) {
      version = 4;
    }
    bool ppsLocked = hasRecentPps();
    response[0] = (0 << 6) | (version << 3) | 4;
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

    ntpUdp.beginPacket(ntpUdp.remoteIP(), ntpUdp.remotePort());
    ntpUdp.write(response, sizeof(response));
    ntpUdp.endPacket();
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

static bool startConfiguredSoftAp(const String &ssid) {
  bool openAp = settings.apSecurityMode == ApSecurityMode::Open;
  String password = settings.apPassword;
  if (!openAp && password.length() < 8) {
    password = DEFAULT_AP_PASSWORD;
  }

  bool ok = WiFi.softAP(ssid.c_str(), openAp ? nullptr : password.c_str(), settings.apChannel, 0, settings.apMaxClients);
  if (!ok || openAp || settings.apSecurityMode == ApSecurityMode::Wpa2) {
    return ok;
  }

  wifi_config_t conf;
  esp_err_t err = esp_wifi_get_config(WIFI_IF_AP, &conf);
  if (err != ESP_OK) {
    Serial.printf("AP security read failed: %d\n", err);
    return false;
  }

  conf.ap.authmode = apWifiAuthMode(settings.apSecurityMode);
  conf.ap.pairwise_cipher = settings.apSecurityMode == ApSecurityMode::WpaWpa2
                                ? WIFI_CIPHER_TYPE_TKIP_CCMP
                                : WIFI_CIPHER_TYPE_CCMP;
  err = esp_wifi_set_config(WIFI_IF_AP, &conf);
  if (err != ESP_OK) {
    Serial.printf("AP security mode %s failed: %d\n", apSecurityModeName(settings.apSecurityMode), err);
    return false;
  }
  return true;
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

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>T-Beam NTP Server</title>
<style>
:root{color-scheme:light;--bg:#f6f7f9;--ink:#17202a;--muted:#5d6875;--line:#d8dde5;--panel:#fff;--accent:#0f766e;--accent2:#1d4ed8;--warn:#b45309;--bad:#b91c1c}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.35 system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif}
header{background:#101820;color:#fff;padding:18px 20px}header h1{font-size:20px;margin:0 0 4px}header .meta{color:#b8c2cc}
main{max-width:1120px;margin:0 auto;padding:18px;display:grid;gap:14px}
section,details.panel{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:14px}
details.panel>summary{cursor:pointer;font-weight:800;font-size:15px;margin:-2px 0 12px;list-style:none;display:flex;align-items:center;justify-content:space-between;gap:12px}details.panel>summary::after{content:"\25BE";color:var(--muted);font-size:13px;line-height:1}details.panel[open]>summary::after{content:"\25B4"}details.panel>summary::-webkit-details-marker{display:none}details.panel:not([open])>summary{margin-bottom:0}
.status-panel{background:#f7fbff}.network-panel{background:#f4fbf7}.ap-panel{background:#fffaf0}.timezone-panel{background:#f6f0ff}.display-panel{background:#f0f8ff}.power-panel{background:#fff4f6}.actions-panel{background:#f3fbf6}
h2{font-size:15px;margin:0 0 12px}.grid{display:grid;grid-template-columns:repeat(12,1fr);gap:10px 12px}.span3{grid-column:span 3}.span4{grid-column:span 4}.span6{grid-column:span 6}.span8{grid-column:span 8}.span12{grid-column:span 12}
label{display:block;font-weight:650;margin-bottom:4px}input,select,textarea,button{width:100%;font:inherit;border:1px solid var(--line);border-radius:6px;background:#fff;color:var(--ink);padding:9px 10px}textarea{min-height:160px;resize:vertical;font-family:ui-monospace,SFMono-Regular,Consolas,monospace}
input[type=checkbox]{width:auto;margin-right:8px}.check{display:flex;align-items:center;min-height:39px;color:var(--ink);font-weight:650}.actions{display:flex;gap:10px;flex-wrap:wrap}.actions button{width:auto;min-width:120px}
.radio-row{display:flex;gap:14px;flex-wrap:wrap;align-items:center;min-height:39px}.radio-row label{display:flex;gap:6px;align-items:center;margin:0;font-weight:650}.radio-row input{width:auto;margin:0}
.offset-row{display:grid;grid-template-columns:140px 1fr 1fr;gap:10px;align-items:end}.time-mode-body.hidden{display:none}.battery-strip{display:flex;align-items:center;gap:12px;min-height:42px}.battery-shell{width:58px;height:24px;border:2px solid var(--line);border-radius:5px;position:relative;background:#fff}.battery-shell::after{content:"";position:absolute;right:-7px;top:6px;width:5px;height:10px;background:var(--line);border-radius:0 2px 2px 0}.battery-fill{height:100%;width:0;border-radius:2px;background:var(--accent);transition:width .2s}.battery-fill.warn{background:var(--warn)}.battery-fill.bad{background:var(--bad)}.battery-meta{display:flex;gap:8px;flex-wrap:wrap;align-items:baseline}.battery-meta strong{font-size:16px}.power-indicator{font-weight:800}.status-battery-row{display:flex;align-items:center;gap:10px;flex-wrap:wrap}.status-battery-row .battery-shell{flex:0 0 auto}.status-battery-percent{font-size:16px;font-weight:800}.status-battery-state{font-size:14px;font-weight:800;white-space:nowrap}.charge-bolt{color:#f97316;font-size:17px;font-weight:900;line-height:1}
button{background:#e8eef6;cursor:pointer;font-weight:700}button.primary{background:var(--accent);border-color:var(--accent);color:#fff}button.secondary{background:#eaf0ff;border-color:#c6d4ff;color:#173b87}button.danger{background:#fff0f0;border-color:#ffc7c7;color:var(--bad)}
.status{display:grid;grid-template-columns:repeat(4,1fr);gap:10px}.metric{border:1px solid var(--line);border-radius:8px;padding:10px;background:#fbfcfd}.metric b{display:block;font-size:12px;color:var(--muted);margin-bottom:3px}.metric span{font-size:15px}
.ok{color:var(--accent)}.warn{color:var(--warn)}.bad{color:var(--bad)}.muted,.hint{color:var(--muted);font-size:12px}.hint{margin-top:5px;line-height:1.3}.hidden{display:none}
@media(max-width:820px){.grid{grid-template-columns:1fr}.span3,.span4,.span6,.span8,.span12{grid-column:span 1}.status{grid-template-columns:1fr 1fr}.actions button{width:100%}.offset-row{grid-template-columns:1fr 1fr 1fr}}
</style>
</head>
<body>
<header><h1 title="GPS/PPS disciplined NTP server status and configuration">T-Beam NTP Server</h1><div class="meta" id="topline">Loading</div></header>
<main>
<details class="panel status-panel" open><summary title="Live clock, GPS, network, power, and memory state">Status</summary><div class="status" id="status"></div></details>
<details class="panel network-panel"><summary title="Settings used when the T-Beam joins an existing WiFi network">Network Client</summary><div class="grid">
<div class="span12"><label>Network mode</label><div class="radio-row"><label for="networkModeAp"><input name="networkMode" id="networkModeAp" type="radio" value="standaloneAp">Standalone AP Mode</label><label for="networkModeClient"><input name="networkMode" id="networkModeClient" type="radio" value="clientWithApFallback">Client Mode with AP Fallback</label></div></div>
<div class="span6"><label for="staSsid">Network Client SSID</label><input id="staSsid" maxlength="32" placeholder="Type SSID or choose a scan result"></div>
<div class="span3"><label for="staPass">Network Client Password</label><input id="staPass" type="password" maxlength="64"></div>
<div class="span3"><label>&nbsp;</label><button class="secondary" id="scanBtn" type="button">Scan</button></div>
<div class="span12"><label for="scanSelect">Scan results</label><select id="scanSelect" disabled><option value="">Scan to find networks</option></select></div>
<div class="span4"><label for="hostname">Hostname</label><input id="hostname" maxlength="31"></div>
<div class="span4 check"><input id="staDhcp" type="checkbox">Use DHCP</div>
<div class="span4"><label for="connectTimeout">Connect timeout seconds</label><input id="connectTimeout" type="number" min="5" max="90"></div>
<div class="span3"><label for="staticIp">Static IP</label><input id="staticIp"></div>
<div class="span3"><label for="gateway">Gateway</label><input id="gateway"></div>
<div class="span3"><label for="subnet">Subnet</label><input id="subnet"></div>
<div class="span3"><label for="dns1">DNS 1</label><input id="dns1"></div>
</div></details>
<details class="panel ap-panel"><summary title="Settings used when the T-Beam is its own off-grid WiFi network">Standalone AP</summary><div class="grid">
<div class="span4"><label for="apSsid">AP SSID</label><input id="apSsid" maxlength="32" aria-describedby="apSsidNote"><div class="hint" id="apSsidNote">Use {mac} to insert this unit's last six MAC hex digits.</div></div>
<div class="span4"><label for="apPass">AP password</label><input id="apPass" type="password" maxlength="64"></div>
<div class="span4"><label for="apSecurity">AP security</label><select id="apSecurity"><option value="wpa2">WPA2-Personal</option><option value="wpa2-wpa3">WPA2/WPA3-Personal</option><option value="wpa3">WPA3-Personal</option><option value="wpa-wpa2">WPA/WPA2-Personal legacy</option><option value="open">Open</option></select></div>
<div class="span2"><label for="apChannel">Channel</label><input id="apChannel" type="number" min="1" max="13"></div>
<div class="span2"><label for="apMax">Max clients</label><input id="apMax" type="number" min="1" max="10"></div>
<div class="span3"><label for="apIp">AP IP</label><input id="apIp"></div>
<div class="span3"><label for="apSubnet">AP subnet</label><input id="apSubnet"></div>
<div class="span3 check"><input id="opt42" type="checkbox">DHCP option 42</div>
<div class="span3 check"><input id="dnsAliases" type="checkbox">DNS NTP aliases</div>
<div class="span3 check"><input id="dnsWildcard" type="checkbox">Captive DNS wildcard</div>
<div class="span12"><label for="ntpHosts">NTP host aliases</label><textarea id="ntpHosts" spellcheck="false"></textarea></div>
</div></details>
<details class="panel timezone-panel"><summary title="Local display timezone settings">Local Time Zone</summary><div class="grid">
<div class="span12"><label>Local time mode</label><div class="radio-row"><label for="timeModeOffset"><input name="timeMode" id="timeModeOffset" type="radio" value="offset">UTC offset</label><label for="timeModeIana"><input name="timeMode" id="timeModeIana" type="radio" value="iana">Time zone</label><label for="timeModePosix"><input name="timeMode" id="timeModePosix" type="radio" value="posix">POSIX rule</label></div></div>
<div class="span12 time-mode-body" data-time-mode="offset" id="offsetModeFields"><div class="offset-row"><div><label for="offsetSign">Offset sign</label><select id="offsetSign"><option value="1">UTC+</option><option value="-1">UTC-</option></select></div><div><label for="offsetHours">Offset hours</label><input id="offsetHours" type="number" min="0" max="14"></div><div><label for="offsetMinutes">Offset minutes</label><input id="offsetMinutes" type="number" min="0" max="59"></div></div></div>
<div class="span6 time-mode-body" data-time-mode="iana" id="ianaModeFields"><label for="ianaTimeZone">Time zone</label><select id="ianaTimeZone"><option value="">Loading timezone file</option></select></div>
<div class="span6 time-mode-body" data-time-mode="posix" id="posixModeFields"><label for="posixTimezone">POSIX TZ rule</label><input id="posixTimezone" maxlength="95"></div>
<input id="manualOffset" type="hidden"><input id="observeDst" type="hidden"><input id="autoOffset" type="hidden">
</div></details>
<details class="panel display-panel"><summary title="OLED display behavior">Display</summary><div class="grid">
<div class="span3 check"><input id="oledCycle" type="checkbox">Cycle OLED screens</div>
<div class="span3"><label for="oledCycleSeconds">OLED cycle seconds</label><input id="oledCycleSeconds" type="number" min="2" max="60"></div>
<div class="span3"><label for="screenTimeout">Screensaver seconds</label><input id="screenTimeout" type="number" min="0" max="3600"></div>
</div></details>
<details class="panel power-panel"><summary title="AXP192 battery and USB power settings">Power</summary><div class="grid">
<div class="span12 battery-strip" id="batteryMeter"><div class="battery-shell"><div class="battery-fill" id="batteryFill"></div></div><div class="battery-meta"><strong id="batteryPct">--</strong><span class="power-indicator" id="batteryState">Waiting for power data</span><span class="muted" id="batteryDetails"></span></div></div>
<div class="span3 check"><input id="chargeEnabled" type="checkbox">Charging enabled</div>
<div class="span3"><label for="chargeCurrent">Charge current mA</label><select id="chargeCurrent"><option>100</option><option>190</option><option selected>280</option><option>360</option><option>450</option><option>550</option><option>630</option><option>700</option></select></div>
<div class="span3"><label for="chargeVoltage">Charge target mV</label><select id="chargeVoltage"><option>4100</option><option>4150</option><option>4200</option></select></div>
<div class="span3"><label for="vbusLimit">VBUS limit mA</label><select id="vbusLimit"><option value="100">100</option><option value="500">500</option><option value="0">Off</option></select></div>
<div class="span3"><label for="warnVoltage">Warning mV</label><input id="warnVoltage" type="number" min="3200" max="3900" step="10"></div>
<div class="span3"><label for="cutoffVoltage">Cutoff mV</label><input id="cutoffVoltage" type="number" min="3000" max="3600" step="10"></div>
<div class="span3"><label for="sysDown">PMU power-down mV</label><input id="sysDown" type="number" min="2600" max="3300" step="100"></div>
<div class="span3"><label for="powerKey">Power key off seconds</label><select id="powerKey"><option>4</option><option>6</option><option>8</option><option>10</option></select></div>
</div></details>
<section class="panel actions-panel"><h2 title="Save settings or restart the T-Beam">Actions</h2><div class="actions"><button class="primary" id="saveBtn">Save</button><button class="danger" id="rebootBtn">Reboot</button><span class="muted" id="message"></span></div></section>
</main>
<script>
const $=id=>document.getElementById(id);
let current={};
let timeZoneRows=[];
const tips={
topline:'Live summary of network mode, device IP, UTC/local time, and connected AP clients.',
status:'Live operational metrics. Each metric tile also has its own tooltip.',
networkModeAp:'Start directly as the off-grid access point and skip network-client connection attempts.',
networkModeClient:'Try the configured network-client SSID first, then fall back to standalone AP mode if connection fails or is later lost.',
staSsid:'SSID of the existing WiFi network to join when Client Mode with AP Fallback is selected. Type hidden SSIDs here, or choose a scanned network below.',
scanBtn:'Scan nearby WiFi networks and refresh Scan results. Scanning does not overwrite the SSID field.',
staPass:'Password for the selected network-client WiFi network. It is saved only when you press Save.',
scanSelect:'Nearby WiFi networks discovered by the last scan. Choosing one copies that SSID into the Network Client SSID field.',
hostname:'Hostname used on the LAN and for mDNS services. Keep it short, unique, and DNS-safe.',
staDhcp:'Use the router DHCP lease in LAN mode. Turn this off only when you want a fixed IPv4 address.',
connectTimeout:'How long to try joining home WiFi before falling back to standalone AP mode.',
staticIp:'Fixed IPv4 address to use when DHCP is disabled.',
gateway:'IPv4 gateway for static LAN mode. Usually the router address.',
subnet:'Subnet mask for static LAN mode, normally 255.255.255.0 on small networks.',
dns1:'DNS server used by the T-Beam in LAN mode when static addressing is selected.',
apSsid:'Standalone AP network name. The literal token {mac} is replaced with the last six MAC hex digits for this unit before the AP starts.',
apSsidNote:'Shows the AP SSID after {mac} expansion, using the value reported by the firmware.',
apPass:'Standalone AP password. Default is tbeam-ntp. Secure AP modes require at least 8 characters.',
apSecurity:'Standalone AP security mode. WEP is not offered because ESP32 SoftAP mode does not support WEP.',
apChannel:'2.4 GHz WiFi channel for standalone AP mode. Use 1, 6, or 11 when avoiding overlap.',
apMax:'Maximum SoftAP stations allowed. Higher values increase management and buffer pressure.',
apIp:'IPv4 address of the T-Beam in standalone AP mode. DHCP, DNS, portal, and NTP use this address.',
apSubnet:'Subnet mask handed to standalone AP clients by the DHCP server.',
opt42:'Advertise the T-Beam AP IP as the NTP server with DHCP option 42.',
dnsAliases:'Answer configured NTP hostnames with the T-Beam AP IP so clients already pointed at public time servers still work off-grid.',
dnsWildcard:'Answer other DNS names with the T-Beam AP IP for captive portal discovery and easier onboarding.',
ntpHosts:'One NTP hostname per line. These names are answered locally by DNS in standalone AP mode.',
timeModeOffset:'Use a fixed UTC offset entered as hours and minutes. This mode does not apply daylight-saving changes.',
timeModeIana:'Choose a current timezone preset loaded from the editable timezone file.',
timeModePosix:'Use the POSIX TZ rule string exactly as entered below.',
offsetModeFields:'Fixed local offset from UTC, entered without doing minute math by hand.',
ianaModeFields:'Preset timezone selector for current daylight-saving behavior.',
posixModeFields:'Manual POSIX timezone rule entry for advanced or custom local-time behavior.',
offsetSign:'Direction of the local offset from UTC. UTC- is used west of Greenwich, such as US time zones.',
offsetHours:'Hour portion of the fixed UTC offset. The firmware converts this to minutes internally.',
offsetMinutes:'Minute portion of the fixed UTC offset. Use values like 30 or 45 for half-hour and quarter-hour zones.',
ianaTimeZone:'Timezone preset name. If the timezone file is removed, this mode falls back to the POSIX rule field.',
posixTimezone:'Manual POSIX timezone rule for local display time. Example: PST8PDT,M3.2.0/2,M11.1.0/2.',
observeDst:'Compatibility field retained for older saved settings.',
autoOffset:'Compatibility field retained for older saved settings.',
manualOffset:'Internal fixed UTC offset in minutes, generated from the hour/minute controls.',
batteryMeter:'Live battery gauge from the AXP192. Shows USB-only operation cleanly when no battery is installed.',
batteryPct:'Battery charge estimate from the AXP192 fuel gauge.',
batteryState:'Charging, discharging, idle, USB-only, or PMU-offline state.',
batteryDetails:'Battery voltage/current detail and USB input telemetry when available.',
chargeEnabled:'Allow AXP192 battery charging using the configured current and voltage limits.',
chargeCurrent:'Li-Ion charge current limit. Conservative values reduce heat and battery stress.',
chargeVoltage:'Li-Ion charge termination voltage. 4100 mV is conservative; 4200 mV maximizes capacity.',
vbusLimit:'USB/VBUS input current limit. Use 500 mA for normal USB ports, 100 mA for weak sources, Off to disable the limiter.',
warnVoltage:'Battery voltage that marks the power state as warning while discharging.',
cutoffVoltage:'Battery voltage that requests PMU shutdown while discharging after the debounce interval.',
sysDown:'AXP192 system power-down threshold. Keep below the battery cutoff setting.',
powerKey:'AXP192 long-press duration for PMU power-off behavior.',
oledCycle:'Automatically rotate OLED status screens.',
oledCycleSeconds:'Seconds each OLED screen remains visible while auto-cycle is enabled.',
screenTimeout:'Seconds of no display activity before the OLED turns off. Use 0 to disable the screensaver.',
saveBtn:'Write the displayed settings to LittleFS. The firmware avoids flash writes until this is pressed.',
rebootBtn:'Restart the T-Beam. Network mode and AP settings take effect cleanly after reboot.',
message:'Save, scan, and reboot status messages appear here.'
};
const metricTips={
Mode:'Current network mode: AP for standalone service or STA for LAN client mode.',
Version:'Firmware version running on this T-Beam.',
IP:'IPv4 address currently serving the portal and NTP.',
UTC:'GPS-derived UTC time. NTP uses UTC and ignores local timezone settings.',
Local:'Display-only local time after POSIX TZ/DST or fallback offset handling.',
TZ:'Timezone abbreviation, daylight-saving state, or numeric fallback offset.',
PPS:'Whether recent GPS PPS edges are disciplining the firmware clock.',
GPS:'Fix quality summary with satellite count and grid square when available.',
Location:'Current GPS latitude and longitude when a fix is fresh.',
Altitude:'GPS altitude in meters when available.',
DOP:'Dilution of precision reported by the GPS receiver.',
Battery:'AXP192 battery presence, voltage, and charge/discharge current.',
VBUS:'USB/VBUS voltage and current reported by the AXP192.',
'NTP/DNS':'NTP requests, suppressed NTP requests, and DNS query count.',
Heap:'Available ESP32 heap and PSRAM for stability monitoring.'
};
function escAttr(s){return String(s??'').replace(/&/g,'&amp;').replace(/"/g,'&quot;').replace(/</g,'&lt;').replace(/>/g,'&gt;')}
function metric(k,v,c='',tip=''){return `<div class="metric" title="${escAttr(tip||metricTips[k]||k)}"><b>${k}</b><span class="${c}">${v??''}</span></div>`}
function applyTooltips(){Object.keys(tips).forEach(id=>{const el=$(id);if(!el)return;el.title=tips[id];const label=document.querySelector(`label[for="${id}"]`);if(label)label.title=tips[id];const wrap=el.closest('.check');if(wrap)wrap.title=tips[id]})}
function offsetLabel(m){const s=m>=0?'+':'-';const a=Math.abs(m||0);return `UTC${s}${String(Math.floor(a/60)).padStart(2,'0')}:${String(a%60).padStart(2,'0')}`}
async function getJson(url,opts){const r=await fetch(url,opts);if(!r.ok)throw new Error(await r.text());return r.json()}
function setMsg(t,c=''){const el=$('message');el.textContent=t;el.className='muted '+c}
function setOffsetControls(minutes){const m=Number(minutes)||0;$('offsetSign').value=m<0?'-1':'1';const a=Math.abs(m);$('offsetHours').value=Math.floor(a/60);$('offsetMinutes').value=a%60;$('manualOffset').value=m}
function offsetFromControls(){const sign=+$('offsetSign').value||1;const h=Math.min(14,Math.max(0,+$('offsetHours').value||0));const m=Math.min(59,Math.max(0,+$('offsetMinutes').value||0));return sign*(h*60+m)}
function getNetworkMode(){const el=document.querySelector('input[name="networkMode"]:checked');return el?el.value:'standaloneAp'}
function setNetworkMode(mode){const id=mode==='clientWithApFallback'?'networkModeClient':'networkModeAp';($(id)||$('networkModeAp')).checked=true}
function getTimeMode(){const el=document.querySelector('input[name="timeMode"]:checked');return el?el.value:'iana'}
function updateTimeModeVisibility(){const mode=getTimeMode();document.querySelectorAll('.time-mode-body').forEach(el=>el.classList.toggle('hidden',el.dataset.timeMode!==mode))}
function wireTimeModeControls(){document.querySelectorAll('input[name="timeMode"]').forEach(el=>{el.onchange=updateTimeModeVisibility})}
function setTimeMode(mode){const id='timeMode'+String(mode||'iana').replace(/^./,c=>c.toUpperCase());const el=$(id)||$('timeModeIana');el.checked=true;updateTimeModeVisibility()}
function populateTimeZones(selected){const sel=$('ianaTimeZone');sel.innerHTML='';if(!timeZoneRows.length){const o=document.createElement('option');o.value=selected||'';o.textContent='Timezone file missing';sel.appendChild(o);sel.disabled=true;return}sel.disabled=false;timeZoneRows.forEach(z=>{const o=document.createElement('option');o.value=z.name;o.textContent=z.name;sel.appendChild(o)});sel.value=selected||'America/Los_Angeles';if(sel.value!==selected&&selected){const o=document.createElement('option');o.value=selected;o.textContent=selected+' (not in file)';sel.insertBefore(o,sel.firstChild);sel.value=selected}}
async function loadTimeZones(){try{const r=await fetch('/api/timezones');if(!r.ok)throw new Error('missing');const text=await r.text();timeZoneRows=text.split(/\r?\n/).map(x=>x.trim()).filter(x=>x&&!x.startsWith('#')).map(x=>{const p=x.split('\t');return{name:p[0],posix:p.slice(1).join('\t')}}).filter(x=>x.name&&x.posix)}catch(e){timeZoneRows=[]}}
function setScanPrompt(text,disabled=true){const sel=$('scanSelect');if(!sel)return;sel.innerHTML='';const o=document.createElement('option');o.value='';o.textContent=text;o.selected=true;sel.appendChild(o);sel.disabled=disabled}
function scanOptionLabel(n){return `${n.ssid} (${n.rssi} dBm, ch ${n.channel}, ${n.auth||'unknown'})`}
function updateApSecurityUi(){const open=$('apSecurity').value==='open';$('apPass').disabled=open;$('apPass').placeholder=open?'not used in open mode':''}
function batteryPercentValue(p){const raw=Number(p&&p.batteryPercent);return Number.isFinite(raw)&&raw>=0?Math.min(100,Math.max(0,Math.round(raw))):null}
function batteryFillClass(p,percent){return 'battery-fill '+((p&&p.warning)||percent<=10?'bad':percent<=25?'warn':'')}
function updateBatteryMeter(p){const fill=$('batteryFill'),pct=$('batteryPct'),state=$('batteryState'),details=$('batteryDetails');if(!fill||!pct||!state||!details)return;if(!p||!p.online){fill.style.width='0%';fill.className='battery-fill bad';pct.textContent='--';state.textContent='PMU offline';details.textContent='No AXP192 telemetry';return}if(!p.batteryPresent){fill.style.width='0%';fill.className='battery-fill';pct.textContent='USB';state.textContent=p.vbusPresent?'\u26A1 USB power':'No battery';details.textContent=p.vbusPresent?`${Number(p.vbusMv||0).toFixed(0)} mV ${Number(p.vbusCurrentMa||0).toFixed(0)} mA VBUS`:'Battery not detected';return}const raw=Number(p.batteryPercent);const percent=Number.isFinite(raw)&&raw>=0?Math.min(100,Math.max(0,Math.round(raw))):0;fill.style.width=percent+'%';fill.className='battery-fill '+(p.warning||percent<=10?'bad':percent<=25?'warn':'');pct.textContent=Number.isFinite(raw)&&raw>=0?percent+'%':'--';state.textContent=p.charging?'\u26A1 Charging':(p.discharging?'\u25BE Discharging':'Idle');details.textContent=`${Number(p.batteryMv||0).toFixed(0)} mV ${Number(p.batteryCurrentMa||0).toFixed(0)} mA`}
function statusBatteryMetric(p){const title='Compact battery charge indicator. Lightning means charging.';if(!p||!p.online)return `<div class="metric" title="${title}"><b>Battery</b><span class="bad">PMU offline</span></div>`;if(!p.batteryPresent)return `<div class="metric" title="${title}"><b>Battery</b><div class="status-battery-row"><div class="battery-shell"><div class="battery-fill" style="width:0%"></div></div><span class="status-battery-percent">USB</span></div></div>`;const percent=batteryPercentValue(p);const shown=percent===null?0:percent;const label=percent===null?'--':percent+'%';const state=p.charging?'<span class="charge-bolt" title="Charging">\u26A1</span><span class="status-battery-state">Charging</span>':(p.discharging?'<span class="status-battery-state">Discharging</span>':'<span class="status-battery-state">Idle</span>');return `<div class="metric" title="${title}"><b>Battery</b><div class="status-battery-row"><div class="battery-shell"><div class="${batteryFillClass(p,shown)}" style="width:${shown}%"></div></div><span class="status-battery-percent">${label}</span>${state}</div></div>`}
function fillSettings(s){
current=s;const d=s.display||{},t=s.time||{};
$('hostname').value=s.hostname||'';setNetworkMode(s.networkMode||((s.wifi.ssid||'')?'clientWithApFallback':'standaloneAp'));$('staSsid').value=s.wifi.ssid||'';$('staPass').value=s.wifi.password||'';$('staDhcp').checked=!!s.wifi.dhcp;$('staticIp').value=s.wifi.staticIp;$('gateway').value=s.wifi.gateway;$('subnet').value=s.wifi.subnet;$('dns1').value=s.wifi.dns1;$('connectTimeout').value=s.wifi.connectTimeoutSec;$('apSsid').value=s.ap.ssid;$('apPass').value=s.ap.password;$('apSecurity').value=s.ap.security||'wpa2';updateApSecurityUi();$('apIp').value=s.ap.ip;$('apSubnet').value=s.ap.subnet;$('apChannel').value=s.ap.channel;$('apMax').value=s.ap.maxClients;$('opt42').checked=!!s.standalone.dhcpOption42;$('dnsAliases').checked=!!s.standalone.dnsNtpAliases;$('dnsWildcard').checked=!!s.standalone.dnsWildcardCaptive;$('ntpHosts').value=(s.standalone.ntpHosts||[]).join('\n');setTimeMode(t.mode||((t.observeDst===false)?'offset':'posix'));setOffsetControls(t.manualOffsetMinutes??0);populateTimeZones(t.ianaTimeZone||'America/Los_Angeles');$('posixTimezone').value=t.posixTimezone||'PST8PDT,M3.2.0/2,M11.1.0/2';$('observeDst').value='';$('autoOffset').value='';$('chargeEnabled').checked=!!s.power.chargeEnabled;$('chargeCurrent').value=s.power.chargeCurrentMa;$('chargeVoltage').value=s.power.chargeTargetMv;$('vbusLimit').value=s.power.vbusCurrentLimitMa;$('warnVoltage').value=s.power.warningVoltageMv;$('cutoffVoltage').value=s.power.cutoffVoltageMv;$('sysDown').value=s.power.sysPowerDownMv;$('powerKey').value=s.power.powerKeyOffSeconds;$('oledCycle').checked=d.autoCycle!==false;$('oledCycleSeconds').value=d.cycleSeconds||5;$('screenTimeout').value=d.screensaverTimeoutSec??300;const ex=s.ap.expandedSsid||s.ap.ssid||'';$('apSsidNote').textContent=`Use {mac} to insert this unit's last six MAC hex digits. Current AP SSID: ${ex}.`}
function readSettings(){const mode=getTimeMode(),offset=offsetFromControls();return{networkMode:getNetworkMode(),hostname:$('hostname').value,wifi:{ssid:$('staSsid').value,password:$('staPass').value,dhcp:$('staDhcp').checked,staticIp:$('staticIp').value,gateway:$('gateway').value,subnet:$('subnet').value,dns1:$('dns1').value,dns2:(current.wifi&&current.wifi.dns2)||'1.1.1.1',connectTimeoutSec:+$('connectTimeout').value},ap:{ssid:$('apSsid').value,password:$('apPass').value,security:$('apSecurity').value,ip:$('apIp').value,subnet:$('apSubnet').value,channel:+$('apChannel').value,maxClients:+$('apMax').value},standalone:{dhcpOption42:$('opt42').checked,dnsNtpAliases:$('dnsAliases').checked,dnsWildcardCaptive:$('dnsWildcard').checked,ntpHosts:$('ntpHosts').value.split(/\r?\n/).map(x=>x.trim()).filter(Boolean)},time:{mode,ianaTimeZone:$('ianaTimeZone').value,posixTimezone:$('posixTimezone').value.trim(),autoLocalOffset:false,observeDst:mode!=='offset',manualOffsetMinutes:offset},display:{autoCycle:$('oledCycle').checked,cycleSeconds:+$('oledCycleSeconds').value,screensaverTimeoutSec:+$('screenTimeout').value},power:{chargeEnabled:$('chargeEnabled').checked,chargeCurrentMa:+$('chargeCurrent').value,chargeTargetMv:+$('chargeVoltage').value,vbusCurrentLimitMa:+$('vbusLimit').value,warningVoltageMv:+$('warnVoltage').value,cutoffVoltageMv:+$('cutoffVoltage').value,sysPowerDownMv:+$('sysDown').value,powerKeyOffSeconds:+$('powerKey').value}}}
async function refreshStatus(){try{const s=await getJson('/api/status');updateBatteryMeter(s.power);$('topline').textContent=`${s.mode} ${s.ip} | UTC ${s.utc||'not synced'} | Local ${s.local||'not synced'} | clients ${s.clients}`;let gps=s.gps.fix?'ok':(s.gps.seen?'warn':'bad');let pwr=s.power.online?(s.power.warning?'warn':'ok'):'warn';let tz=s.timeZone?`${s.timeZone} ${s.dstActive?'DST':'STD'}`:offsetLabel(s.localOffsetMinutes);$('status').innerHTML=metric('Mode',s.mode)+metric('Version',s.version||'')+metric('IP',s.ip)+metric('UTC',s.utc||'not synced',s.clock.synced?'ok':'bad')+metric('Local',s.local||'not synced',s.clock.synced?'ok':'bad')+metric('TZ',tz)+metric('PPS',s.clock.pps?'locked':'waiting',s.clock.pps?'ok':'warn')+metric('GPS',`${s.gps.sats} sats ${s.gps.grid||''}`,gps)+metric('Location',s.gps.fix?`${s.gps.lat.toFixed(6)}, ${s.gps.lon.toFixed(6)}`:'no fix')+metric('Altitude',s.gps.fix?`${s.gps.alt.toFixed(1)} m`:'')+metric('DOP',s.gps.dop||'')+metric('Battery',s.power.online?(s.power.batteryPresent?`${s.power.batteryMv} mV ${s.power.batteryCurrentMa.toFixed(0)} mA`:'no battery'):'PMU offline',pwr)+metric('VBUS',s.power.online?`${s.power.vbusMv} mV ${s.power.vbusCurrentMa.toFixed(0)} mA`:'')+metric('NTP/DNS',`${s.ntpRequests}/${s.ntpSuppressed} / ${s.dnsQueries}`)+metric('Heap',`${s.heap.free} free, PSRAM ${s.heap.psramFree}`)+statusBatteryMetric(s.power)}catch(e){setMsg(e.message,'bad')}}
async function load(){applyTooltips();wireTimeModeControls();$('apSecurity').onchange=updateApSecurityUi;await loadTimeZones();fillSettings(await getJson('/api/settings'));refreshStatus();setInterval(refreshStatus,2000)}
$('saveBtn').onclick=async()=>{try{setMsg('Saving');await getJson('/api/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(readSettings())});setMsg('Saved. Reboot to apply network changes.','ok')}catch(e){setMsg(e.message,'bad')}};
$('rebootBtn').onclick=async()=>{if(confirm('Reboot now?')){await fetch('/api/reboot',{method:'POST'});setMsg('Rebooting')}};
$('scanSelect').onchange=()=>{const sel=$('scanSelect');if(sel.value){$('staSsid').value=sel.value;setMsg(`Selected ${sel.value}. Press Save to store it.`,'ok')}};
$('scanBtn').onclick=async()=>{const btn=$('scanBtn');try{btn.disabled=true;btn.textContent='Scanning';setScanPrompt('Scanning...',true);setMsg('Scanning');const s=await getJson('/api/scan');if(s.error)throw new Error(s.error);const networks=(s.networks||[]).slice().sort((a,b)=>(Number(b.rssi)||-999)-(Number(a.rssi)||-999));const named=networks.filter(n=>n.ssid);const sel=$('scanSelect');sel.innerHTML='';const prompt=document.createElement('option');prompt.value='';prompt.textContent=named.length?`Select a network - ${named.length} found`:'No named networks found';prompt.selected=true;sel.appendChild(prompt);named.forEach(n=>{const o=document.createElement('option');o.value=n.ssid;o.textContent=scanOptionLabel(n);sel.appendChild(o)});sel.disabled=!named.length;$('staSsid').placeholder=named.length?'Type SSID or choose a scan result':'Type hidden SSID manually';if(named.length){setMsg(`Found ${named.length} named networks${s.count>networks.length?' (showing strongest 40)':''}. Choose one or type a hidden SSID.`,'ok')}else{setMsg('No named networks found. Type a hidden SSID manually if needed.','warn')}}catch(e){setScanPrompt('Scan failed',true);setMsg(e.message,'bad')}finally{btn.disabled=false;btn.textContent='Scan'}};
load();
</script>
</body>
</html>
)HTML";

static void sendJson(JsonDocument &doc) {
  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

static void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

static void handleSettingsGet() {
  PsramJsonDocument doc(12288);
  settingsToJson(doc);
  sendJson(doc);
}

static void handleTimezones() {
  File file = LittleFS.open(TIMEZONE_DB_PATH, "r");
  if (!file) {
    server.send(404, "text/plain", "timezone preset file not found");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.streamFile(file, "text/tab-separated-values");
  file.close();
}

static void handleSettingsSave() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing body\"}");
    return;
  }
  PsramJsonDocument doc(12288);
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  if (error) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid json\"}");
    return;
  }
  applySettingsJson(doc);
  applyPowerSettings();
  if (!saveSettings()) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"save failed\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true,\"rebootRecommended\":true}");
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
  server.send(200, "application/json", "{\"ok\":true}");
  delay(250);
  ESP.restart();
}

static void handleStatus() {
  uint64_t nowUsec = 0;
  bool synced = getClockUnixUsec(nowUsec);
  bool servingTime = synced && hasFreshGpsTime();
  int16_t localOffset = servingTime ? currentLocalOffsetMinutes(nowUsec) : currentLocalOffsetMinutes();
  bool gpsFix = hasFreshGpsFix();

  PsramJsonDocument doc(12288);
  doc["mode"] = networkMode == NetworkMode::Sta ? "STA" : "AP";
  doc["networkMode"] = networkStartModeName(settings.networkStartMode);
  doc["version"] = FIRMWARE_VERSION;
  doc["ip"] = networkMode == NetworkMode::Sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  doc["hostname"] = sanitizedHostname();
  doc["clients"] = networkMode == NetworkMode::ApFallback ? WiFi.softAPgetStationNum() : 0;
  doc["apSecurity"] = apSecurityModeName(settings.apSecurityMode);
  doc["utc"] = servingTime ? formatUtc(nowUsec) : "";
  doc["local"] = servingTime ? formatLocal(nowUsec) : "";
  doc["localOffsetMinutes"] = localOffset;
  doc["timeZone"] = servingTime ? localTimezoneName(nowUsec) : "";
  doc["localTimeMode"] = localTimeModeName(settings.localTimeMode);
  doc["ianaTimeZone"] = settings.ianaTimeZone;
  doc["timeZoneDatabasePresent"] = littleFsFileExists(TIMEZONE_DB_PATH);
  doc["dstActive"] = servingTime && isLocalDstActive(nowUsec);
  doc["ntpRequests"] = ntpRequestCount;
  doc["ntpSuppressed"] = ntpSuppressedCount;
  doc["dnsQueries"] = dnsQueryCount;
  doc["dnsAliasHits"] = dnsAliasHitCount;

  JsonObject clock = doc.createNestedObject("clock");
  clock["synced"] = servingTime;
  clock["hasClock"] = synced;
  clock["pps"] = hasRecentPps();
  clock["lastSyncMs"] = lastClockSyncMs;
  clock["lastGpsTimeAgeMs"] = lastNmeaUpdateMs ? static_cast<int32_t>(millis() - lastNmeaUpdateMs) : -1;
  clock["lastPpsAgeMs"] = lastPpsMs ? static_cast<int32_t>(millis() - lastPpsMs) : -1;

  JsonObject gpsJson = doc.createNestedObject("gps");
  gpsJson["seen"] = gps.charsProcessed() > 0;
  gpsJson["fix"] = gpsFix;
  gpsJson["sats"] = gps.satellites.isValid() ? gps.satellites.value() : 0;
  gpsJson["lat"] = gpsFix ? gps.location.lat() : 0.0;
  gpsJson["lon"] = gpsFix ? gps.location.lng() : 0.0;
  gpsJson["alt"] = gps.altitude.isValid() ? gps.altitude.meters() : 0.0;
  gpsJson["dop"] = gpsDopString();
  gpsJson["grid"] = gpsFix ? maidenhead(gps.location.lat(), gps.location.lng()) : "";
  gpsJson["chars"] = gps.charsProcessed();
  gpsJson["sentences"] = gps.sentencesWithFix();

  JsonObject power = doc.createNestedObject("power");
  power["online"] = powerState.online;
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

  JsonObject heap = doc.createNestedObject("heap");
  heap["free"] = ESP.getFreeHeap();
  heap["minFree"] = ESP.getMinFreeHeap();
  heap["psramSize"] = ESP.getPsramSize();
  heap["psramFree"] = ESP.getFreePsram();

  doc["i2c"] = i2cDevices;
  JsonObject displaySettings = doc.createNestedObject("display");
  displaySettings["page"] = oledPage;
  displaySettings["sleeping"] = oledSleeping;
  sendJson(doc);
}

static void handleNotFound() {
  if (networkMode == NetworkMode::ApFallback) {
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "Not found");
  }
}

static void beginWeb() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/settings", HTTP_GET, handleSettingsGet);
  server.on("/api/timezones", HTTP_GET, handleTimezones);
  server.on("/api/save", HTTP_POST, handleSettingsSave);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/generate_204", HTTP_GET, handleRoot);
  server.on("/hotspot-detect.html", HTTP_GET, handleRoot);
  server.on("/connecttest.txt", HTTP_GET, handleRoot);
  server.on("/ncsi.txt", HTTP_GET, handleRoot);
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

static void factoryResetSettings(const char *reason) {
  Serial.printf("Factory reset requested: %s\n", reason ? reason : "user button");
  showFactoryResetPrompt();
  if (littleFsFileExists("/settings.json")) {
    LittleFS.remove("/settings.json");
  }
  delay(250);
  ESP.restart();
}

static void checkBootFactoryReset() {
  if (digitalRead(PIN_USER_BUTTON) != LOW) {
    return;
  }
  Serial.println("User button held at boot; release before 10 seconds to force AP mode");
  showBootButtonPrompt();
  uint32_t start = millis();
  while (millis() - start < USER_BUTTON_FACTORY_RESET_MS) {
    if (digitalRead(PIN_USER_BUTTON) != LOW) {
      Serial.println("Boot button released before reset threshold; forcing AP mode for this boot");
      bootForceApMode = true;
      showBootForceApPrompt();
      delay(900);
      return;
    }
    delay(50);
  }

  Serial.println("Factory reset threshold reached; waiting for confirmation hold");
  showResetConfirmPrompt();
  uint32_t releaseDeadline = millis() + FACTORY_RESET_CONFIRM_WINDOW_MS;
  while (digitalRead(PIN_USER_BUTTON) == LOW && beforeDeadline(releaseDeadline)) {
    delay(50);
  }
  if (digitalRead(PIN_USER_BUTTON) == LOW) {
    Serial.println("Reset confirmation canceled; button was not released");
    bootForceApMode = true;
    showBootForceApPrompt();
    delay(900);
    return;
  }

  uint32_t confirmUntilMs = millis() + FACTORY_RESET_CONFIRM_WINDOW_MS;
  while (beforeDeadline(confirmUntilMs)) {
    if (digitalRead(PIN_USER_BUTTON) == LOW) {
      uint32_t confirmStart = millis();
      while (digitalRead(PIN_USER_BUTTON) == LOW) {
        if (millis() - confirmStart >= USER_BUTTON_FACTORY_RESET_MS) {
          factoryResetSettings("boot confirmed button hold");
        }
        delay(50);
      }
      Serial.println("Reset confirmation press released early; forcing AP mode for this boot");
      bootForceApMode = true;
      showBootForceApPrompt();
      delay(900);
      return;
    }
    delay(50);
  }

  Serial.println("Reset confirmation timed out; forcing AP mode for this boot");
  bootForceApMode = true;
  showBootForceApPrompt();
  delay(900);
}

static void updateOled() {
  if (!oledOnline) {
    return;
  }

  if (splashUntilMs != 0 && millis() < splashUntilMs) {
    return;
  }
  splashUntilMs = 0;

  uint64_t nowUsec = 0;
  bool synced = getClockUnixUsec(nowUsec);
  bool servingTime = synced && hasFreshGpsTime();
  bool gpsFix = hasFreshGpsFix();

  if (gpsFix != lastDisplayGpsFix) {
    lastDisplayGpsFix = gpsFix;
    wakeOled();
  }

  if (lastOledActivityMs == 0) {
    lastOledActivityMs = millis();
  }

  if (!oledSleeping && settings.oledScreensaverTimeoutSec > 0 &&
      millis() - lastOledActivityMs >= static_cast<uint32_t>(settings.oledScreensaverTimeoutSec) * 1000UL) {
    sleepOled();
    return;
  }

  if (oledSleeping) {
    return;
  }

  if (settings.oledAutoCycle && settings.oledCycleSeconds > 0 &&
      !beforeDeadline(oledAutoCyclePausedUntilMs) &&
      millis() - lastOledCycleMs >= static_cast<uint32_t>(settings.oledCycleSeconds) * 1000UL) {
    oledPage = (oledPage + 1) % OLED_PAGE_COUNT;
    lastOledCycleMs = millis();
    lastOledUpdateMs = 0;
  }

  uint32_t refreshMs = (powerState.warning || networkPageHasScrollingText()) ? 250UL : 1000UL;
  if (millis() - lastOledUpdateMs < refreshMs) {
    return;
  }
  lastOledUpdateMs = millis();

  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);

  if (oledPage == 0) {
    drawLine(0, networkMode == NetworkMode::Sta ? "WiFi STA " + WiFi.localIP().toString() : "AP " + WiFi.softAPIP().toString());
    drawLine(12, servingTime ? "UTC " + formatUtc(nowUsec).substring(11) : "NTP gated: no GPS");
    drawLine(24, servingTime ? "LOC " + formatLocal(nowUsec).substring(11) : "LOC not synced");
    drawLine(36, String("GPS ") + (gpsFix ? "fix" : "no fix") + " PPS " + (hasRecentPps() ? "lock" : "wait"));
    drawLine(48, String("NTP ") + ntpRequestCount + "/" + ntpSuppressedCount + " DNS " + dnsQueryCount);
  } else if (oledPage == 1) {
    drawTimePage("UTC", servingTime ? formatUtc(nowUsec) : "", servingTime);
  } else if (oledPage == 2) {
    drawTimePage("Local Time", servingTime ? formatLocal(nowUsec) : "", servingTime);
  } else if (oledPage == 3) {
    String grid = gpsFix ? maidenhead(gps.location.lat(), gps.location.lng()) : "";
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.setFont(ArialMT_Plain_10);
    drawLine(0, "Grid Square");
    drawCentered(17, grid.length() ? grid : "--", ArialMT_Plain_24);
    display.setFont(ArialMT_Plain_10);
    drawCentered(48, gpsFix ? "GPS fix" : "waiting for fix", ArialMT_Plain_10);
  } else if (oledPage == 4) {
    drawText(0, String("Sat ") + (gps.satellites.isValid() ? gps.satellites.value() : 0) + " DOP " + gpsDopString(), ArialMT_Plain_16);
    drawText(16, gpsFix ? "Lat " + String(gps.location.lat(), 4) : "Lat --", ArialMT_Plain_16);
    drawText(32, gpsFix ? "Lon " + String(gps.location.lng(), 4) : "Lon --", ArialMT_Plain_16);
    drawText(48, gpsFix && gps.altitude.isValid() ? "Alt " + String(gps.altitude.meters(), 0) + "m" : "Alt --", ArialMT_Plain_16);
  } else if (oledPage == 5) {
    drawLine(0, networkMode == NetworkMode::Sta ? "Network client" : "Standalone AP");
    drawScrollingText(12, networkMode == NetworkMode::Sta ? WiFi.SSID() : expandedApSsid(), ArialMT_Plain_10);
    drawLine(24, networkMode == NetworkMode::Sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString());
    drawLine(36, String("Clients ") + (networkMode == NetworkMode::ApFallback ? WiFi.softAPgetStationNum() : 0) + "/" + settings.apMaxClients);
    drawScrollingText(48, "Pass " + apPasswordForDisplay(), ArialMT_Plain_10);
  } else {
    drawText(0, powerState.batteryPresent ? String("Bat ") + powerState.batteryMv + "mV" : "No battery", ArialMT_Plain_16);
    drawText(16, String("I ") + String(powerState.batteryCurrentMa, 0) + "mA", ArialMT_Plain_16);
    drawText(32, powerState.vbusPresent ? String("USB ") + powerState.vbusMv + "mV" : "USB absent", ArialMT_Plain_16);
    drawText(48, powerState.warning ? String("Bat warning") : String("Temp ") + String(powerState.temperatureC, 0) + "C", ArialMT_Plain_16);
  }

  drawLowBatteryOverlay();
  display.display();
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
