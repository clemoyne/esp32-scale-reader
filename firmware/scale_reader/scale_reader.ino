// ESP32 reader for a Renpho / QN bathroom scale.
//
// Scans for the scale, connects when it wakes, does the same handshake as
// pi/scale_weight.py (config to FFE3, time to FFE4), and POSTs each stable
// weight as JSON to REPORT_HOST:REPORT_PORT/REPORT_PATH over Wi-Fi.
// Readings that can't be sent yet are kept in RAM and retried.
//
// http://<HOSTNAME>.local/ shows status and the recent log (including the
// scale's RSSI each time it advertises, for placing the board). Updates can
// be flashed over Wi-Fi (ArduinoOTA) once the first USB flash is done.
//
// Needs: Arduino-ESP32 core 3.x, NimBLE-Arduino 2.x. Copy config.example.h
// to config.h and fill it in. See the README at the top of the repository.

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <NimBLEDevice.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <time.h>

#include "config.h"
#include "scale_types.h"

#ifndef LED_PIN
#define LED_PIN 2
#endif
#ifndef LED_ON_LEVEL
#define LED_ON_LEVEL HIGH
#endif

// -------------------------------------------------------------------- LED
// Fast blink: connecting / handshake. Solid: step on now.
// Three slow flashes: stable reading taken. Off: idle.
// Runs in its own task so it keeps blinking through blocking BLE calls.

enum LedMode { LED_OFF, LED_CONNECTING, LED_READY, LED_DONE };
static volatile LedMode ledMode = LED_OFF;

static void ledWrite(bool on) { digitalWrite(LED_PIN, on ? LED_ON_LEVEL : !LED_ON_LEVEL); }

static void setLed(LedMode m) { ledMode = m; }

static void ledTask(void*) {
  for (;;) {
    switch (ledMode) {
      case LED_OFF:
        ledWrite(false);
        vTaskDelay(pdMS_TO_TICKS(50));
        break;
      case LED_CONNECTING:
        ledWrite(true);
        vTaskDelay(pdMS_TO_TICKS(100));
        ledWrite(false);
        vTaskDelay(pdMS_TO_TICKS(100));
        break;
      case LED_READY:
        ledWrite(true);
        vTaskDelay(pdMS_TO_TICKS(50));
        break;
      case LED_DONE:
        for (int i = 0; i < 3; i++) {
          ledWrite(true);
          vTaskDelay(pdMS_TO_TICKS(400));
          ledWrite(false);
          vTaskDelay(pdMS_TO_TICKS(400));
        }
        if (ledMode == LED_DONE) ledMode = LED_OFF;
        break;
    }
  }
}

// ---------------------------------------------------------------- logging

// The log and the current stage live in RTC memory, which survives a
// watchdog or crash restart (not a power cut), so the status page can show
// what was happening just before one.
static const int LOG_LINES = 40;
static const int LOG_WIDTH = 100;
static const uint32_t LOG_MAGIC = 0x5ca1e001;
RTC_NOINIT_ATTR static uint32_t logMagic;
RTC_NOINIT_ATTR static char logBuf[LOG_LINES][LOG_WIDTH];
RTC_NOINIT_ATTR static int logNext;
RTC_NOINIT_ATTR static int logCount;
RTC_NOINIT_ATTR static char rtcStage[24];
static char bootNote[100] = "";

// Name what the loop is about to block on, for the watchdog's benefit.
static void setStage(const char* s) {
  strncpy(rtcStage, s, sizeof rtcStage - 1);
  rtcStage[sizeof rtcStage - 1] = 0;
}

static const char* resetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout (power dip)";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default: return "other";
  }
}

static void initLog() {
  esp_reset_reason_t r = esp_reset_reason();
  bool keep = logMagic == LOG_MAGIC && r != ESP_RST_POWERON && logNext >= 0 &&
              logNext < LOG_LINES && logCount >= 0 && logCount <= LOG_LINES;
  if (keep) {
    rtcStage[sizeof rtcStage - 1] = 0;
    snprintf(bootNote, sizeof bootNote, "restarted by %s while in \"%s\"", resetName(r), rtcStage);
  } else {
    logMagic = LOG_MAGIC;
    logNext = logCount = 0;
    snprintf(bootNote, sizeof bootNote, "started (%s)", resetName(r));
  }
  setStage("boot");
}

static bool timeValid() { return time(nullptr) > 1700000000; }

static void stamp(char* out, size_t n) {
  if (timeValid()) {
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(out, n, "%m-%d %H:%M:%S", &tmv);
  } else {
    snprintf(out, n, "+%lus", (unsigned long)(millis() / 1000));
  }
}

// Only call from the Arduino loop task (not from BLE callbacks).
static void logf(const char* fmt, ...) {
  char msg[LOG_WIDTH - 16];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  char ts[20];
  stamp(ts, sizeof ts);
  snprintf(logBuf[logNext], LOG_WIDTH, "%s %s", ts, msg);
  Serial.println(logBuf[logNext]);
  logNext = (logNext + 1) % LOG_LINES;
  if (logCount < LOG_LINES) logCount++;
}

static void hex(const uint8_t* d, size_t n, char* out, size_t outLen) {
  size_t o = 0;
  for (size_t i = 0; i < n && o + 3 <= outLen; i++) o += snprintf(out + o, outLen - o, "%02x", d[i]);
  if (outLen) out[o < outLen ? o : outLen - 1] = 0;
}

// --------------------------------------------------------------- readings

static const int MAX_PENDING = 32;
static Reading pending[MAX_PENDING];
static int pendingCount = 0;
static Reading lastReading = {0, 0, 0, 0};
static bool haveLastReading = false;
static uint32_t lastPostTryMs = 0;
static IPAddress reportIp;
static uint32_t reportIpAtMs = 0;

static const float LB_PER_KG = 2.2046226218f;

static void queueReading(float kg, int rssi) {
  Reading r = {kg, millis(), timeValid() ? time(nullptr) : 0, rssi};
  lastReading = r;
  haveLastReading = true;
  if (pendingCount == MAX_PENDING) {  // drop the oldest
    memmove(pending, pending + 1, sizeof(Reading) * (MAX_PENDING - 1));
    pendingCount--;
  }
  pending[pendingCount++] = r;
  lastPostTryMs = 0;  // try straight away
}

static time_t readingEpoch(const Reading& r) {
  if (r.epoch) return r.epoch;
  if (!timeValid()) return 0;
  return time(nullptr) - (time_t)((millis() - r.atMs) / 1000);
}

// Resolve REPORT_HOST, using mDNS for .local names (plain DNS can't).
static bool resolveReportHost(IPAddress& ip) {
  String host = REPORT_HOST;
  if (reportIp != IPAddress() && millis() - reportIpAtMs < 10 * 60 * 1000UL) {
    ip = reportIp;
    return true;
  }
  if (host.endsWith(".local")) {
    setStage("mdns lookup");
    ip = MDNS.queryHost(host.substring(0, host.length() - 6), 2000);
  } else if (!WiFi.hostByName(host.c_str(), ip)) {
    ip = IPAddress();
  }
  if (ip == IPAddress()) return false;
  reportIp = ip;
  reportIpAtMs = millis();
  return true;
}

static bool postReading(const Reading& r) {
  IPAddress ip;
  if (!resolveReportHost(ip)) {
    logf("post: can't resolve %s", REPORT_HOST);
    return false;
  }
  char body[200];
  snprintf(body, sizeof body,
           "{\"kg\":%.2f,\"lb\":%.1f,\"time\":%ld,\"rssi\":%d,\"reader\":\"%s\"}",
           r.kg, r.kg * LB_PER_KG, (long)readingEpoch(r), r.rssi, HOSTNAME);
  HTTPClient http;
  http.setTimeout(5000);
  String url = String("http://") + ip.toString() + ":" + REPORT_PORT + REPORT_PATH;
  if (!http.begin(url)) return false;
  http.addHeader("Content-Type", "application/json");
  setStage("http post");
  int code = http.POST((uint8_t*)body, strlen(body));
  http.end();
  if (code >= 200 && code < 300) {
    logf("posted %.2f kg to %s", r.kg, REPORT_HOST);
    return true;
  }
  logf("post to %s failed (%d)", url.c_str(), code);
  reportIp = IPAddress();  // re-resolve next time
  return false;
}

static void postPending() {
  if (!pendingCount || WiFi.status() != WL_CONNECTED) return;
  if (lastPostTryMs && millis() - lastPostTryMs < 30000) return;
  lastPostTryMs = millis();
  while (pendingCount) {
    if (!postReading(pending[0])) return;
    memmove(pending, pending + 1, sizeof(Reading) * (pendingCount - 1));
    pendingCount--;
  }
}

// -------------------------------------------------------------------- BLE

enum State { SCANNING, IN_SESSION };
static State state = SCANNING;

static NimBLEClient* client = nullptr;
static NimBLERemoteCharacteristic* chrConfig = nullptr;
static NimBLERemoteCharacteristic* chrTime = nullptr;

// Written by the BLE host task in the scan callback, read by loop().
static volatile bool connectWanted = false;
static NimBLEAddress wantedAddr;
static volatile int lastAdvRssi = 0;
static volatile uint32_t lastAdvMs = 0;
static volatile uint32_t advCount = 0;
static volatile uint32_t cooldownUntilMs = 0;
static String targetAddr;  // lower case, or "" to match by name

static QueueHandle_t packets;

// Session state (loop task only)
static uint32_t sessionStartMs = 0;
static uint32_t stableAtMs = 0;
static uint32_t lastSettleLogMs = 0;
static bool closing = false;
static bool cfgSent = false;
static bool timeSent = false;
static float weightDiv = 100.0f;
static int sessionRssi = 0;
static uint32_t connectFailures = 0;
static uint32_t sessions = 0;

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    bool match;
    if (targetAddr.length()) {
      match = targetAddr.equalsIgnoreCase(dev->getAddress().toString().c_str());
    } else {
      match = dev->haveName() && dev->getName() == SCALE_NAME;
    }
    if (!match) return;
    lastAdvRssi = dev->getRSSI();
    lastAdvMs = millis();
    advCount = advCount + 1;
    if (state != SCANNING || connectWanted) return;
    if ((int32_t)(millis() - cooldownUntilMs) < 0) return;
    wantedAddr = dev->getAddress();
    connectWanted = true;
  }
} scanCallbacks;

static void onNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  Packet p;
  p.len = len > sizeof p.data ? sizeof p.data : len;
  memcpy(p.data, data, p.len);
  xQueueSend(packets, &p, 0);
}

static void startScan() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan->isScanning()) return;
  // Scan in 30 s chunks: restarting clears the duplicate filter's memory.
  setStage("scan start");
  scan->start(30000, false, true);
}

static void stopScan() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  setStage("scan stop");
  if (scan->isScanning()) scan->stop();
}

static void writeChr(NimBLERemoteCharacteristic* c, const uint8_t* v, size_t n) {
  char h[64];
  hex(v, n, h, sizeof h);
  setStage("ble write");
  bool ok = c->writeValue(v, n, c->canWrite());  // with response if supported
  logf("write %s: %s%s", c->getUUID().toString().c_str(), h, ok ? "" : " FAILED");
}

static bool subscribe(NimBLERemoteService* svc, const char* uuid) {
  NimBLERemoteCharacteristic* c = svc->getCharacteristic(uuid);
  if (!c || !(c->canNotify() || c->canIndicate())) return false;
  setStage("ble subscribe");
  bool ok = c->subscribe(c->canNotify(), onNotify, true);
  logf("subscribe %s %s", uuid, ok ? "ok" : "FAILED");
  return ok;
}

static bool setupSession() {
  setStage("ble discovery");
  NimBLERemoteService* svc = client->getService("ffe0");
  bool any = false;
  if (svc) {
    chrConfig = svc->getCharacteristic("ffe3");
    chrTime = svc->getCharacteristic("ffe4");
    any |= subscribe(svc, "ffe1");
    any |= subscribe(svc, "ffe2");
  } else if ((svc = client->getService("fff0"))) {  // other QN variant
    chrConfig = svc->getCharacteristic("fff2");
    chrTime = nullptr;
    any |= subscribe(svc, "fff1");
  } else {
    logf("no FFE0/FFF0 service");
    return false;
  }
  return any;
}

static void connectToScale() {
  NimBLEAddress addr = wantedAddr;
  logf("scale advertising (%d dBm), connecting", lastAdvRssi);
  setLed(LED_CONNECTING);
  stopScan();
  xQueueReset(packets);
  uint32_t t0 = millis();
  setStage("ble connect");
  if (!client->connect(addr, true)) {
    connectFailures++;
    logf("connect failed after %lu ms (%d)", (unsigned long)(millis() - t0),
         client->getLastError());
    setLed(LED_OFF);
    connectWanted = false;  // the next advertisement tries again
    return;
  }
  sessionRssi = lastAdvRssi;
  logf("connected in %lu ms", (unsigned long)(millis() - t0));
  chrConfig = chrTime = nullptr;
  cfgSent = timeSent = closing = false;
  weightDiv = 100.0f;
  stableAtMs = 0;
  lastSettleLogMs = 0;
  if (!setupSession()) {
    logf("handshake setup failed, disconnecting");
    setLed(LED_OFF);
    client->disconnect();
    connectWanted = false;
    return;
  }
  sessions++;
  sessionStartMs = millis();
  state = IN_SESSION;
  connectWanted = false;
}

static void handlePacket(const Packet& p) {
  const uint8_t* d = p.data;
  char h[70];
  hex(d, p.len, h, sizeof h);
  if (!p.len) return;
  uint8_t kind = d[0];

  if (kind == 0x10 && p.len > 5) {
    float kg = ((d[3] << 8) | d[4]) / weightDiv;
    bool stable = d[5] == 1;
    if (stable && !stableAtMs) {
      logf("notify %s", h);
      logf("WEIGHT %.2f kg = %.1f lb STABLE", kg, kg * LB_PER_KG);
      stableAtMs = millis();
      setLed(LED_DONE);
      queueReading(kg, sessionRssi);
    } else if (!stable && millis() - lastSettleLogMs > 1000) {
      lastSettleLogMs = millis();
      logf("weight %.2f kg (settling)", kg);
    }
    return;
  }

  logf("notify %s", h);
  if (kind == 0x12 && p.len > 10) {
    weightDiv = d[10] == 1 ? 100.0f : 10.0f;
    if (chrConfig && !cfgSent) {
      uint8_t cfg[9] = {0x13, 0x09, 0x15, (uint8_t)(DISPLAY_UNIT_LB ? 0x02 : 0x01),
                        0x10, 0x00, 0x00, 0x00, 0x00};
      uint8_t sum = 0;
      for (int i = 0; i < 8; i++) sum += cfg[i];
      cfg[8] = sum;
      cfgSent = true;
      writeChr(chrConfig, cfg, sizeof cfg);
      if (!chrTime) {  // FFF0 variant: no time step, so this ends the handshake
        logf("handshake done, step on now");
        setLed(LED_READY);
      }
    }
  } else if (kind == 0x14 && chrTime && !timeSent) {
    // Seconds since 2000-01-01 UTC, as pi/scale_weight.py sends.
    uint32_t secs = timeValid() ? (uint32_t)(time(nullptr) - 946684800) : 0;
    uint8_t t[5] = {0x02, (uint8_t)secs, (uint8_t)(secs >> 8), (uint8_t)(secs >> 16),
                    (uint8_t)(secs >> 24)};
    timeSent = true;
    writeChr(chrTime, t, sizeof t);
    logf("handshake done, step on now");
    if (!stableAtMs) setLed(LED_READY);
  }
}

static void runSession() {
  Packet p;
  while (xQueueReceive(packets, &p, 0) == pdTRUE) handlePacket(p);

  if (!client->isConnected()) {
    if (!closing) logf("link dropped after %lu s", (unsigned long)((millis() - sessionStartMs) / 1000));
    else logf("disconnected");
    if (ledMode != LED_DONE) setLed(LED_OFF);
    state = SCANNING;
    return;
  }
  if (closing) return;
  if (stableAtMs && millis() - stableAtMs > 2000) {
    closing = true;
    cooldownUntilMs = millis() + COOLDOWN_S * 1000UL;
    client->disconnect();
  } else if (millis() - sessionStartMs > SESSION_MAX_S * 1000UL) {
    logf("no stable weight in %d s, giving up", SESSION_MAX_S);
    closing = true;
    cooldownUntilMs = millis() + 10000;
    client->disconnect();
  }
}

// ------------------------------------------------------------ web status

static WebServer server(80);
static bool otaActive = false;

static void handleStatus() {
  String s;
  s.reserve(LOG_LINES * 90 + 800);
  char line[160];
  char ts[20];
  stamp(ts, sizeof ts);
  snprintf(line, sizeof line, "%s  now %s  up %lu s\n", HOSTNAME, ts, (unsigned long)(millis() / 1000));
  s += line;
  snprintf(line, sizeof line, "last boot: %s\n", bootNote);
  s += line;
  snprintf(line, sizeof line, "wifi %s  %d dBm  %s\n", WiFi.SSID().c_str(), WiFi.RSSI(),
           WiFi.localIP().toString().c_str());
  s += line;
  snprintf(line, sizeof line, "state %s  sessions %lu  connect failures %lu\n",
           state == SCANNING ? "scanning" : "connected", (unsigned long)sessions,
           (unsigned long)connectFailures);
  s += line;
  if (lastAdvMs) {
    snprintf(line, sizeof line, "scale last heard %lu s ago at %d dBm (%lu adverts)\n",
             (unsigned long)((millis() - lastAdvMs) / 1000), lastAdvRssi, (unsigned long)advCount);
  } else {
    snprintf(line, sizeof line, "scale not heard yet\n");
  }
  s += line;
  if (haveLastReading) {
    snprintf(line, sizeof line, "last reading %.2f kg = %.1f lb, %lu s ago\n", lastReading.kg,
             lastReading.kg * LB_PER_KG, (unsigned long)((millis() - lastReading.atMs) / 1000));
    s += line;
  }
  snprintf(line, sizeof line, "waiting to send %d  ->  %s:%d%s\n\n", pendingCount, REPORT_HOST,
           REPORT_PORT, REPORT_PATH);
  s += line;
  for (int i = 0; i < logCount; i++) {
    int idx = (logNext - logCount + i + LOG_LINES) % LOG_LINES;
    s += logBuf[idx];
    s += '\n';
  }
  server.send(200, "text/plain; charset=utf-8", s);
}

// ------------------------------------------------------------------ setup

static void setupWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  // Modem sleep stays on: the ESP32 needs it to share the radio with BLE.
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com");
}

static void setupOta() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    otaActive = true;
    stopScan();
    if (client && client->isConnected()) client->disconnect();
  });
  ArduinoOTA.onProgress([](unsigned int, unsigned int) { esp_task_wdt_reset(); });
  ArduinoOTA.onError([](ota_error_t e) {
    otaActive = false;
    logf("OTA error %d", (int)e);
  });
  ArduinoOTA.begin();  // also starts mDNS as HOSTNAME.local
}

void setup() {
  Serial.begin(115200);
  delay(200);
  initLog();
  // Restart if loop() stalls: the longest legitimate block is a 5 s connect
  // or HTTP post (OTA feeds the watchdog as it goes).
  esp_task_wdt_config_t wdt = {};
  wdt.timeout_ms = 30000;
  wdt.idle_core_mask = 0;
  wdt.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) esp_task_wdt_init(&wdt);
  esp_task_wdt_add(nullptr);
  targetAddr = SCALE_ADDRESS;
  targetAddr.toLowerCase();
  packets = xQueueCreate(24, sizeof(Packet));
  pinMode(LED_PIN, OUTPUT);
  ledWrite(false);
  xTaskCreate(ledTask, "led", 2048, nullptr, 1, nullptr);
  // Two quick flashes at boot, so you can tell the LED is the one we drive.
  setLed(LED_CONNECTING);
  delay(400);
  setLed(LED_OFF);

  setupWifi();
  logf("--- %s ---", bootNote);
  logf("%s starting, looking for %s", HOSTNAME, targetAddr.length() ? targetAddr.c_str() : SCALE_NAME);

  NimBLEDevice::init(HOSTNAME);
  NimBLEDevice::setPower(9);  // +9 dBm, the most a classic ESP32 does
  client = NimBLEDevice::createClient();
  // Interval 15-30 ms, latency 0, supervision timeout in 10 ms units.
  client->setConnectionParams(12, 24, 0, SUPERVISION_TIMEOUT_MS / 10);
  client->setConnectTimeout(CONNECT_TIMEOUT_MS);

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, true);  // want every advert, for RSSI
  scan->setActiveScan(targetAddr.length() == 0);  // the name may be in the scan response
  scan->setMaxResults(0);  // callbacks only; don't keep every device in RAM
  scan->setInterval(160);  // 100 ms
  scan->setWindow(96);     // 60 ms, leaving air time for Wi-Fi
  startScan();
}

void loop() {
  esp_task_wdt_reset();
  setStage("loop");
  static bool wifiWasUp = false;
  static uint32_t lastAdvLogged = 0;

  bool wifiUp = WiFi.status() == WL_CONNECTED;
  if (wifiUp && !wifiWasUp) {
    logf("wifi up, %s, %d dBm", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    static bool servicesStarted = false;
    if (!servicesStarted) {
      setupOta();
      MDNS.addService("http", "tcp", 80);
      server.on("/", handleStatus);
      server.begin();
      servicesStarted = true;
    }
  } else if (!wifiUp && wifiWasUp) {
    logf("wifi down");
  }
  wifiWasUp = wifiUp;

  if (wifiUp) {
    setStage("ota check");
    ArduinoOTA.handle();
    setStage("web server");
    server.handleClient();
  }
  if (otaActive) return;

  // Note the scale's signal now and then, to help place the board.
  if (lastAdvMs && lastAdvMs != lastAdvLogged && millis() - lastAdvLogged > 10000 &&
      state == SCANNING && !connectWanted) {
    lastAdvLogged = lastAdvMs;
    logf("scale heard at %d dBm", lastAdvRssi);
  }

  if (state == SCANNING) {
    if (connectWanted) {
      lastAdvLogged = millis();
      connectToScale();
    } else {
      postPending();
      startScan();
    }
  } else {
    runSession();
  }
  delay(5);
}
