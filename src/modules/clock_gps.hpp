// GPS parsing, PPS discipline, UTC/local time, timezone, and Maidenhead helpers.

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

