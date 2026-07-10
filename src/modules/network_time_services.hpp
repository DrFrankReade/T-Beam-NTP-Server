// I2C scan, DNS, NTP, DHCP options, mDNS, AP mode, and STA mode network services.

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

