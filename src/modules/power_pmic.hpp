// PMIC detection, rail setup, charger settings, battery telemetry, and power-button IRQ handling.

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

