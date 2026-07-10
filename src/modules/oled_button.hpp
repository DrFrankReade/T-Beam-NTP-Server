// OLED screen rendering, screensaver behavior, boot button reset flow, and runtime user button handling.

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

