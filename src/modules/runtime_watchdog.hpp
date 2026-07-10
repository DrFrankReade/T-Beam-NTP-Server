// Runtime network health monitoring before Arduino setup and loop.

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

