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
static constexpr const char *FIRMWARE_VERSION = "v0.1.10";
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
    bool callYield = false;

    if (!acceptClient()) {
      return;
    }

    if (!handleCurrentClient(callYield)) {
      resetCurrentClient();
    }

    if (callYield) {
      yield();
    }
  }

private:
  static constexpr uint32_t CLIENT_DATA_WAIT_MS = 250;

  bool acceptClient() {
    if (_currentStatus != HC_NONE) {
      return true;
    }

    _currentClient = _server.available();
    if (!_currentClient) {
      if (_nullDelay) {
        delay(1);
      }
      return false;
    }

    _currentStatus = HC_WAIT_READ;
    _statusChange = millis();
    return true;
  }

  bool handleCurrentClient(bool &callYield) {
    if (!_currentClient.connected()) {
      return false;
    }

    switch (_currentStatus) {
      case HC_NONE:
        return false;
      case HC_WAIT_READ:
        return handleWaitRead(callYield);
      case HC_WAIT_CLOSE:
        callYield = true;
        return false;
    }

    return false;
  }

  bool handleWaitRead(bool &callYield) {
    if (_currentClient.available()) {
      handleReadableRequest();
      return false;
    }

    callYield = true;
    return millis() - _statusChange <= CLIENT_DATA_WAIT_MS;
  }

  void handleReadableRequest() {
    if (!_parseRequest(_currentClient)) {
      return;
    }

    _currentClient.setTimeout(1);
    _contentLength = CONTENT_LENGTH_NOT_SET;
    _handleRequest();
  }

  void resetCurrentClient() {
    _currentClient = WiFiClient();
    _currentStatus = HC_NONE;
    _currentUpload.reset();
    _currentRaw.reset();
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

#include "modules/settings_storage.hpp"

#include "modules/clock_gps.hpp"

#include "modules/power_pmic.hpp"

#include "modules/network_time_services.hpp"

#include "modules/portal_status.hpp"

#include "modules/oled_button.hpp"

#include "modules/runtime_watchdog.hpp"

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
