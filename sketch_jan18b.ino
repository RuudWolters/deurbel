//Dit is de code voor een deurbel aangestuurd door een esp01S met een 2tal relais.
//Op zich werkt de code prima, echter zou ik graag alle responses (zoals de webpagina en de telegram berichten in het Nederlands willen hebben.
//Ook zou ik de mogelijkheid willen hebben om voor een bepaalde tijd bepaalde uitgangen op stil te zetten.
//Bijv een 0,5 uur, 1 uur of 1,5 uur geen oude bel/nieuwe bel of geen oude bel ook wil ik dit kunnen annuleren.
//ook zou ik als ik telegram uit zet willen dat het ook niet gecached wordt om later te versturen.
//Goede ideeën en verbeteringen zijn altijd welkom. Maar houdt ook rekening met het geheugen van de esp01S


#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266WebServer.h>
#include <time.h>
#include <stdarg.h>
#include "wifi_secrets.h"

// Forward declare the type so any auto-generated prototypes can "see" it.
struct RingEvent;

// (Optional but safe) Forward declare queue functions too.
static bool queuePush(const RingEvent &e);
static bool queuePop(RingEvent &out);
static bool queuePeek(uint8_t indexFromHead, RingEvent &out);

// Now define the type and queue storage.
struct RingEvent {
  uint32_t t_ms;
  uint32_t epoch;   // 0 if unknown
  uint16_t count;
};

static const uint8_t EVENT_QUEUE_CAP = 48;
static RingEvent eventQ[EVENT_QUEUE_CAP];
static uint8_t qHead = 0;
static uint8_t qTail = 0;
static uint8_t qSize = 0;

// Then define the functions.
static bool queuePush(const RingEvent &e) {
  if (qSize >= EVENT_QUEUE_CAP) return false;
  eventQ[qTail] = e;
  qTail = (uint8_t)((qTail + 1) % EVENT_QUEUE_CAP);
  qSize++;
  return true;
}

static bool queuePop(RingEvent &out) {
  if (qSize == 0) return false;
  out = eventQ[qHead];
  qHead = (uint8_t)((qHead + 1) % EVENT_QUEUE_CAP);
  qSize--;
  return true;
}

static bool queuePeek(uint8_t indexFromHead, RingEvent &out) {
  if (indexFromHead >= qSize) return false;
  uint8_t idx = (uint8_t)((qHead + indexFromHead) % EVENT_QUEUE_CAP);
  out = eventQ[idx];
  return true;
}

// ===================== Compile-time debug (boot logs) =====================
#define DEBUG_SERIAL 1  // 1 = on, 0 = off

#if DEBUG_SERIAL
  #define DBG_BEGIN(baud) Serial.begin(baud)
  #define DBG_BOOT_PRINTLN(x)  Serial.println(x)
  #define DBG_BOOT_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
  #define DBG_BEGIN(baud) do {} while (0)
  #define DBG_BOOT_PRINTLN(x)  do {} while (0)
  #define DBG_BOOT_PRINTF(...) do {} while (0)
#endif

// ===================== GPIO config voor ESP01 =====================
static const int PIN_RELAY_OLD = 0;   // GPIO0 (let op: boot pin)
static const int PIN_RELAY_NEW = 2;   // GPIO2 (let op: boot pin)
static const int PIN_BUTTON    = 3;   // GPIO3 (RX pin)

static const int PIN_LED = 1; // GPIO1 (TX pin) - onboard LED

// ===================== Behavior =====================
static const bool RELAY_ACTIVE_LOW = true;
static uint32_t RING_PULSE_OLD_MS = 500;  // Afzonderlijke duur voor oude relais
static uint32_t RING_PULSE_NEW_MS = 500;  // Afzonderlijke duur voor nieuwe relais
static const uint32_t DEBOUNCE_MS = 35;

// ===================== WiFi + NTP =====================
static const bool WIFI_ENABLED = true;


static const char *NTP_SERVER_1 = "pool.ntp.org";
static const char *NTP_SERVER_2 = "time.nist.gov";
static const char *TZ_INFO = "CET-1CEST,M3.5.0/02,M10.5.0/03";

static const uint32_t WIFI_RETRY_MS = 10000;
static const uint32_t NTP_RETRY_MS  = 15000;

// ===================== Telegram =====================
static const bool TG_INSECURE_TLS = true;

static const uint32_t TG_MIN_SEND_INTERVAL_MS = 2000;
static const uint32_t TG_RETRY_MS = 8000;

static const uint8_t TG_MAX_EVENTS_PER_MESSAGE = 15;
static const uint8_t TG_MAX_LISTED_TIMES = 10;

// ===================== Web UI =====================
static const bool WEB_ENABLED = true;
static const uint16_t WEB_PORT = 80;
static ESP8266WebServer web(WEB_PORT);

static bool gWebStarted = false;

// ===================== Runtime toggles =====================
static bool gDebugRuntime = true;
static bool gRelayOldEnabled = true;
static bool gRelayNewEnabled = true;
static bool gTelegramEnabled = true;

static inline void DBG_RUN_PRINTLN(const String &s) {
#if DEBUG_SERIAL
  if (gDebugRuntime) Serial.println(s);
#else
  (void)s;
#endif
}

static inline void DBG_RUN_PRINTF(const char *fmt, ...) {
#if DEBUG_SERIAL
  if (!gDebugRuntime) return;
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.print(buf);
#else
  (void)fmt;
#endif
}

// ===================== State =====================
static bool lastButtonRaw = true;
static bool buttonStable = true;
static uint32_t lastDebounceChangeMs = 0;

static bool ringingOld = false;
static bool ringingNew = false;
static uint32_t ringOldUntilMs = 0;
static uint32_t ringNewUntilMs = 0;

enum class WifiState : uint8_t { Disabled, Connecting, Connected, Failed };
static WifiState wifiState = WifiState::Disabled;

static bool timeSynced = false;
static uint32_t lastWifiAttemptMs = 0;
static uint32_t lastNtpAttemptMs  = 0;

static uint32_t lastTgAttemptMs = 0;
static uint32_t lastTgSuccessMs = 0;

// ===================== Helpers =====================
static inline void setRelay(int pin, bool on) {
  if (RELAY_ACTIVE_LOW) digitalWrite(pin, on ? LOW : HIGH);
  else                 digitalWrite(pin, on ? HIGH : LOW);
}

static bool getEpochNow(uint32_t &outEpoch) {
  time_t now = time(nullptr);
  if (now < 1700000000) return false;
  outEpoch = (uint32_t)now;
  return true;
}

static void formatEpoch(uint32_t epoch, char *buf, size_t bufLen) {
  if (epoch == 0) {
    snprintf(buf, bufLen, "unknown");
    return;
  }
  time_t t = (time_t)epoch;
  struct tm tmLocal;
  localtime_r(&t, &tmLocal);
  snprintf(buf, bufLen, "%04d-%02d-%02d %02d:%02d:%02d",
           tmLocal.tm_year + 1900, tmLocal.tm_mon + 1, tmLocal.tm_mday,
           tmLocal.tm_hour, tmLocal.tm_min, tmLocal.tm_sec);
}

static void queueLogRing(uint32_t tMs) {
  RingEvent e{};
  e.t_ms = tMs;
  e.count = 1;

  uint32_t epoch;
  e.epoch = (getEpochNow(epoch) ? epoch : 0);

  if (!queuePush(e)) {
    RingEvent dropped;
    (void)queuePop(dropped);
    (void)queuePush(e);
    DBG_RUN_PRINTLN("Queue: full, dropped oldest");
  }

  char ts[32];
  formatEpoch(e.epoch, ts, sizeof(ts));
  DBG_RUN_PRINTF("Queue: size=%u (epoch=%lu %s)\n", qSize, (unsigned long)e.epoch, ts);
}

static void startRing() {
  uint32_t now = millis();

  ringingOld = gRelayOldEnabled;
  ringingNew = gRelayNewEnabled;
  
  if (ringingOld) {
    ringOldUntilMs = now + RING_PULSE_OLD_MS;
    setRelay(PIN_RELAY_OLD, true);
  }
  
  if (ringingNew) {
    ringNewUntilMs = now + RING_PULSE_NEW_MS;
    setRelay(PIN_RELAY_NEW, true);
  }

  queueLogRing(now);
  DBG_RUN_PRINTLN("Doorbell: RING start");
}

static void stopRingIfDue() {
  uint32_t now = millis();
  
  if (ringingOld && (int32_t)(now - ringOldUntilMs) >= 0) {
    setRelay(PIN_RELAY_OLD, false);
    ringingOld = false;
    DBG_RUN_PRINTLN("Doorbell: RING OLD stop");
  }
  
  if (ringingNew && (int32_t)(now - ringNewUntilMs) >= 0) {
    setRelay(PIN_RELAY_NEW, false);
    ringingNew = false;
    DBG_RUN_PRINTLN("Doorbell: RING NEW stop");
  }
}

static void handleButton() {
  bool raw = digitalRead(PIN_BUTTON); // INPUT_PULLUP: pressed = LOW

  if (raw != lastButtonRaw) {
    lastButtonRaw = raw;
    lastDebounceChangeMs = millis();
  }

  if (millis() - lastDebounceChangeMs >= DEBOUNCE_MS) {
    if (raw != buttonStable) {
      buttonStable = raw;
      if (buttonStable == LOW) startRing();
    }
  }
}

static void wifiTick() {
  if (!WIFI_ENABLED) {
    wifiState = WifiState::Disabled;
    return;
  }

  wl_status_t st = WiFi.status();
  uint32_t now = millis();

  if (st == WL_CONNECTED) {
    if (wifiState != WifiState::Connected) {
      wifiState = WifiState::Connected;
      DBG_RUN_PRINTF("WiFi: connected, IP=%s\n", WiFi.localIP().toString().c_str());
    }
    return;
  }

  if (wifiState == WifiState::Connected) {
    wifiState = WifiState::Failed;
    DBG_RUN_PRINTLN("WiFi: lost connection");
  }

  if (now - lastWifiAttemptMs < WIFI_RETRY_MS) return;
  lastWifiAttemptMs = now;

  wifiState = WifiState::Connecting;
  DBG_RUN_PRINTF("WiFi: connecting to %s\n", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

static void ntpTick() {
  if (!WIFI_ENABLED) return;

  if (WiFi.status() != WL_CONNECTED) {
    timeSynced = false;
    return;
  }
  if (timeSynced) return;

  uint32_t now = millis();
  if (now - lastNtpAttemptMs < NTP_RETRY_MS) return;
  lastNtpAttemptMs = now;

  DBG_RUN_PRINTLN("NTP: configuring time");
  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);
  setenv("TZ", TZ_INFO, 1);
  tzset();

  uint32_t epoch;
  if (getEpochNow(epoch)) {
    timeSynced = true;
    DBG_RUN_PRINTF("NTP: synced (epoch=%lu)\n", (unsigned long)epoch);
  }
}

static void updateLed() {
  if (!WIFI_ENABLED) {
    digitalWrite(PIN_LED, LOW);
    return;
  }

  if (WiFi.status() == WL_CONNECTED && timeSynced) {
    digitalWrite(PIN_LED, HIGH);
    return;
  }

  uint32_t periodMs = 800;
  if (wifiState == WifiState::Connecting) periodMs = 1200;
  else if (wifiState == WifiState::Failed) periodMs = 250;

  static uint32_t lastToggleMs = 0;
  static bool ledState = false;

  uint32_t now = millis();
  if (now - lastToggleMs >= (periodMs / 2)) {
    lastToggleMs = now;
    ledState = !ledState;
    digitalWrite(PIN_LED, ledState ? HIGH : LOW);
  }
}

// ===================== Telegram: URL encoding + HTTP =====================
static bool isUnreserved(char c) {
  if (c >= 'a' && c <= 'z') return true;
  if (c >= 'A' && c <= 'Z') return true;
  if (c >= '0' && c <= '9') return true;
  if (c == '-' || c == '_' || c == '.' || c == '~') return true;
  return false;
}

static String urlEncode(const String &s) {
  static const char *hex = "0123456789ABCDEF";
  String out;
  out.reserve(s.length() * 3);

  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isUnreserved(c)) out += c;
    else if (c == ' ') out += "%20";
    else if (c == '\n') out += "%0A";
    else {
      out += '%';
      out += hex[(uint8_t)c >> 4];
      out += hex[(uint8_t)c & 0x0F];
    }
  }
  return out;
}

static bool telegramSendMessage(const String &text) {
  if (!gTelegramEnabled) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  if (TG_INSECURE_TLS) client.setInsecure();

  const char *host = "api.telegram.org";
  const uint16_t port = 443;

  if (!client.connect(host, port)) {
    DBG_RUN_PRINTLN("Telegram: connect failed");
    return false;
  }

  String path = "/bot";
  path += TG_BOT_TOKEN;
  path += "/sendMessage";

  String body = "chat_id=";
  body += TG_CHAT_ID;
  body += "&text=";
  body += urlEncode(text);

  String req;
  req.reserve(512 + body.length());
  req += "POST " + path + " HTTP/1.1\r\n";
  req += "Host: " + String(host) + "\r\n";
  req += "User-Agent: esp01-doorbell\r\n";
  req += "Connection: close\r\n";
  req += "Content-Type: application/x-www-form-urlencoded\r\n";
  req += "Content-Length: " + String(body.length()) + "\r\n\r\n";
  req += body;

  client.print(req);

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();

  bool ok = statusLine.startsWith("HTTP/1.1 200") || statusLine.startsWith("HTTP/1.0 200");

  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break;
  }
  client.stop();

  DBG_RUN_PRINTF("Telegram: %s\n", ok ? "send ok" : "send failed");
  return ok;
}

static String buildNiceTelegramMessage(uint8_t maxToSend) {
  uint8_t n = (qSize < maxToSend) ? qSize : maxToSend;

  uint32_t firstEpoch = 0;
  uint32_t lastEpoch = 0;

  for (uint8_t i = 0; i < n; i++) {
    RingEvent e;
    if (!queuePeek(i, e)) break;
    if (e.epoch != 0) {
      if (firstEpoch == 0) firstEpoch = e.epoch;
      lastEpoch = e.epoch;
    }
  }

  char firstBuf[32];
  char lastBuf[32];
  formatEpoch(firstEpoch, firstBuf, sizeof(firstBuf));
  formatEpoch(lastEpoch, lastBuf, sizeof(lastBuf));

  String msg;
  msg.reserve(800);

  msg += "🔔 Doorbell\n";
  msg += "Events: ";
  msg += String(n);
  if (qSize > n) {
    msg += " (";
    msg += String(qSize);
    msg += " queued)";
  }
  msg += "\n";

  msg += "Range: ";
  msg += firstBuf;
  msg += " -> ";
  msg += lastBuf;
  msg += "\n";

  msg += "WiFi: ";
  msg += (WiFi.status() == WL_CONNECTED) ? "connected" : "offline";
  msg += "\n";

  msg += "Time: ";
  msg += timeSynced ? "synced" : "not synced";
  msg += "\n\n";

  msg += "Times:\n";
  uint8_t listed = 0;
  for (uint8_t i = 0; i < n && listed < TG_MAX_LISTED_TIMES; i++) {
    RingEvent e;
    if (!queuePeek(i, e)) break;
    char ts[32];
    formatEpoch(e.epoch, ts, sizeof(ts));
    msg += "  ";
    msg += String(i + 1);
    msg += ") ";
    msg += ts;
    msg += "\n";
    listed++;
  }

  if (n > listed) msg += "  ...\n";
  return msg;
}

static void telegramTick() {
  if (!gTelegramEnabled) return;
  if (qSize == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;

  uint32_t now = millis();
  if (now - lastTgSuccessMs < TG_MIN_SEND_INTERVAL_MS) return;
  if (now - lastTgAttemptMs < TG_RETRY_MS) return;
  lastTgAttemptMs = now;

  String msg = buildNiceTelegramMessage(TG_MAX_EVENTS_PER_MESSAGE);
  bool ok = telegramSendMessage(msg);
  if (!ok) return;

  uint8_t toDrop = (qSize < TG_MAX_EVENTS_PER_MESSAGE) ? qSize : TG_MAX_EVENTS_PER_MESSAGE;
  for (uint8_t i = 0; i < toDrop; i++) {
    RingEvent tmp;
    (void)queuePop(tmp);
  }
  lastTgSuccessMs = now;
}

// ===================== Web UI CSS en HTML helpers =====================
static String getCSS() {
  return R"(
<style>
* { box-sizing: border-box; margin: 0; padding: 0; }
body { 
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; 
  background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
  min-height: 100vh;
  padding: 20px;
  color: #333;
}
.container { 
  max-width: 800px; 
  margin: 0 auto; 
  background: white; 
  border-radius: 15px;
  box-shadow: 0 20px 40px rgba(0,0,0,0.1);
  overflow: hidden;
}
.header { 
  background: linear-gradient(45deg, #4CAF50, #45a049); 
  color: white; 
  padding: 30px; 
  text-align: center;
}
.header h1 { font-size: 2.5em; margin-bottom: 10px; }
.header p { opacity: 0.9; font-size: 1.1em; }
.content { padding: 30px; }
.status-grid { 
  display: grid; 
  grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); 
  gap: 20px; 
  margin-bottom: 30px; 
}
.status-card { 
  background: #f8f9fa; 
  padding: 20px; 
  border-radius: 10px;
  border-left: 4px solid #4CAF50;
  box-shadow: 0 2px 10px rgba(0,0,0,0.05);
}
.status-card h3 { color: #4CAF50; margin-bottom: 10px; }
.status-card .value { font-size: 1.4em; font-weight: bold; color: #333; }
.section { margin-bottom: 30px; }
.section h2 { 
  color: #4CAF50; 
  border-bottom: 2px solid #e9ecef; 
  padding-bottom: 10px; 
  margin-bottom: 20px; 
}
.form-grid { 
  display: grid; 
  grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); 
  gap: 30px; 
}
.form-group { margin-bottom: 20px; }
.form-group label { 
  display: flex; 
  align-items: center; 
  font-weight: 500; 
  margin-bottom: 10px;
  cursor: pointer;
}
.form-group input[type="checkbox"] { 
  width: 20px; 
  height: 20px; 
  margin-right: 10px; 
  accent-color: #4CAF50;
}
.form-group input[type="number"] { 
  width: 100%; 
  padding: 12px; 
  border: 2px solid #e9ecef; 
  border-radius: 8px; 
  font-size: 16px;
  transition: border-color 0.3s;
}
.form-group input[type="number"]:focus { 
  outline: none; 
  border-color: #4CAF50; 
}
.btn { 
  background: linear-gradient(45deg, #4CAF50, #45a049); 
  color: white; 
  border: none; 
  padding: 12px 25px; 
  border-radius: 8px; 
  font-size: 16px; 
  cursor: pointer; 
  transition: transform 0.2s;
  text-decoration: none;
  display: inline-block;
  margin: 5px;
}
.btn:hover { transform: translateY(-2px); box-shadow: 0 5px 15px rgba(76, 175, 80, 0.4); }
.btn-secondary { 
  background: linear-gradient(45deg, #6c757d, #5a6268); 
}
.btn-secondary:hover { box-shadow: 0 5px 15px rgba(108, 117, 125, 0.4); }
.actions { text-align: center; }
.online { color: #28a745; }
.offline { color: #dc3545; }
.footer { 
  background: #f8f9fa; 
  padding: 20px; 
  text-align: center; 
  color: #6c757d; 
  border-top: 1px solid #e9ecef;
}
@media (max-width: 600px) {
  .form-grid { grid-template-columns: 1fr; }
  .status-grid { grid-template-columns: 1fr; }
  body { padding: 10px; }
  .header { padding: 20px; }
  .header h1 { font-size: 2em; }
  .content { padding: 20px; }
}
</style>
)";
}

static String htmlEscape(const String &s) {
  String o;
  o.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

static void handleRoot() {
  String ip = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("offline");
  String wifiStatus = (WiFi.status() == WL_CONNECTED) ? "online" : "offline";
  String wifiClass = (WiFi.status() == WL_CONNECTED) ? "online" : "offline";
  String timeStatus = timeSynced ? "online" : "offline";
  String timeClass = timeSynced ? "online" : "offline";

  String page;
  page.reserve(4000);
  page += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  page += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  page += "<title>ESP01 Doorbell Control</title>";
  page += getCSS();
  page += "</head><body>";
  
  page += "<div class='container'>";
  page += "<div class='header'>";
  page += "<h1>🔔 Doorbell Control</h1>";
  page += "<p>ESP01 Doorbell Management System</p>";
  page += "</div>";
  
  page += "<div class='content'>";
  
  // Status cards
  page += "<div class='status-grid'>";
  page += "<div class='status-card'>";
  page += "<h3>IP Address</h3>";
  page += "<div class='value " + wifiClass + "'>" + htmlEscape(ip) + "</div>";
  page += "</div>";
  
  page += "<div class='status-card'>";
  page += "<h3>Queue</h3>";
  page += "<div class='value'>" + String(qSize) + " events</div>";
  page += "</div>";
  
  page += "<div class='status-card'>";
  page += "<h3>WiFi</h3>";
  page += "<div class='value " + wifiClass + "'>" + wifiStatus + "</div>";
  page += "</div>";
  
  page += "<div class='status-card'>";
  page += "<h3>Time Sync</h3>";
  page += "<div class='value " + timeClass + "'>" + (timeSynced ? "synced" : "not synced") + "</div>";
  page += "</div>";
  page += "</div>";

  // Configuration section
  page += "<div class='section'>";
  page += "<h2>⚙️ Configuration</h2>";
  page += "<form action='/set' method='get'>";
  page += "<div class='form-grid'>";
  
  // Settings column
  page += "<div>";
  page += "<h3>Relay Settings</h3>";
  page += "<div class='form-group'>";
  page += "<label><input type='checkbox' name='old' value='1' ";
  page += (gRelayOldEnabled ? "checked" : "");
  page += "> 🔌 Enable Relay Old (GPIO0)</label>";
  page += "</div>";

  page += "<div class='form-group'>";
  page += "<label><input type='checkbox' name='new' value='1' ";
  page += (gRelayNewEnabled ? "checked" : "");
  page += "> 🔌 Enable Relay New (GPIO2)</label>";
  page += "</div>";

  page += "<div class='form-group'>";
  page += "<label><input type='checkbox' name='tg' value='1' ";
  page += (gTelegramEnabled ? "checked" : "");
  page += "> 📱 Enable Telegram</label>";
  page += "</div>";

  page += "<div class='form-group'>";
  page += "<label><input type='checkbox' name='dbg' value='1' ";
  page += (gDebugRuntime ? "checked" : "");
  page += "> 🐛 Enable Debug</label>";
  page += "</div>";
  page += "</div>";
  
  // Timing column
  page += "<div>";
  page += "<h3>Pulse Duration</h3>";
  page += "<div class='form-group'>";
  page += "<label for='oldDuration'>Old Relay Duration (ms)</label>";
  page += "<input type='number' id='oldDuration' name='oldDuration' value='" + String(RING_PULSE_OLD_MS) + "' min='100' max='5000' step='50'>";
  page += "</div>";
  
  page += "<div class='form-group'>";
  page += "<label for='newDuration'>New Relay Duration (ms)</label>";
  page += "<input type='number' id='newDuration' name='newDuration' value='" + String(RING_PULSE_NEW_MS) + "' min='100' max='5000' step='50'>";
  page += "</div>";
  page += "</div>";
  
  page += "</div>";
  page += "<div class='actions'>";
  page += "<button type='submit' class='btn'>💾 Save Settings</button>";
  page += "</div>";
  page += "</form>";
  page += "</div>";

  // Actions section
  page += "<div class='section'>";
  page += "<h2>🎮 Actions</h2>";
  page += "<div class='actions'>";
  page += "<a href='/testring' class='btn'>🔔 Test Ring</a>";
  page += "<a href='/sendnow' class='btn btn-secondary'>📤 Send Telegram Now</a>";
  page += "</div>";
  page += "</div>";
  
  page += "</div>";
  page += "<div class='footer'>";
  page += "<p>ESP01 Doorbell System • Uptime: " + String(millis() / 1000) + "s</p>";
  page += "</div>";
  page += "</div>";
  page += "</body></html>";

  web.send(200, "text/html; charset=utf-8", page);
}

static void handleSet() {
  gRelayOldEnabled = web.hasArg("old");
  gRelayNewEnabled = web.hasArg("new");
  gTelegramEnabled = web.hasArg("tg");
  gDebugRuntime    = web.hasArg("dbg");
  
  // Update durations
  if (web.hasArg("oldDuration")) {
    int oldDur = web.arg("oldDuration").toInt();
    if (oldDur >= 100 && oldDur <= 5000) {
      RING_PULSE_OLD_MS = oldDur;
    }
  }
  
  if (web.hasArg("newDuration")) {
    int newDur = web.arg("newDuration").toInt();
    if (newDur >= 100 && newDur <= 5000) {
      RING_PULSE_NEW_MS = newDur;
    }
  }

  DBG_RUN_PRINTF("Web: set old=%d(%dms) new=%d(%dms) tg=%d dbg=%d\n",
                 gRelayOldEnabled ? 1 : 0, RING_PULSE_OLD_MS,
                 gRelayNewEnabled ? 1 : 0, RING_PULSE_NEW_MS,
                 gTelegramEnabled ? 1 : 0,
                 gDebugRuntime ? 1 : 0);

  web.sendHeader("Location", "/");
  web.send(302, "text/plain", "Settings saved");
}

static void handleTestRing() {
  startRing();
  web.sendHeader("Location", "/");
  web.send(302, "text/plain", "Ring test started");
}

static void handleSendNow() {
  lastTgAttemptMs = 0;
  lastTgSuccessMs = 0;
  telegramTick();
  web.sendHeader("Location", "/");
  web.send(302, "text/plain", "Telegram send attempted");
}

static void webStartIfNeeded() {
  if (!WEB_ENABLED) return;
  if (gWebStarted) return;

  web.on("/", handleRoot);
  web.on("/set", handleSet);
  web.on("/testring", handleTestRing);
  web.on("/sendnow", handleSendNow);

  web.begin();
  gWebStarted = true;
}

static void webStopIfRunning() {
  if (!WEB_ENABLED) return;
  if (!gWebStarted) return;

  web.stop();
  gWebStarted = false;
}

static void webTick() {
  if (!WEB_ENABLED) return;
  if (!WIFI_ENABLED) return;

  if (WiFi.status() == WL_CONNECTED) {
    webStartIfNeeded();
    web.handleClient();
  } else {
    webStopIfRunning();
  }
}

// ===================== Setup/Loop =====================
void setup() {
  DBG_BEGIN(115200);
  delay(200);
  DBG_BOOT_PRINTLN("Boot: ESP01 Doorbell with separate relay durations");

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  pinMode(PIN_RELAY_OLD, OUTPUT);
  pinMode(PIN_RELAY_NEW, OUTPUT);
  setRelay(PIN_RELAY_OLD, false);
  setRelay(PIN_RELAY_NEW, false);

  pinMode(PIN_BUTTON, INPUT_PULLUP);

  if (WIFI_ENABLED) {
    wifiState = WifiState::Failed;
    lastWifiAttemptMs = 0;
    lastNtpAttemptMs  = 0;
  } else {
    wifiState = WifiState::Disabled;
  }

  DBG_BOOT_PRINTF("GPIO: Old relay=%d, New relay=%d, Button=%d\n", 
                  PIN_RELAY_OLD, PIN_RELAY_NEW, PIN_BUTTON);
}

void loop() {
  handleButton();
  stopRingIfDue();

  wifiTick();
  ntpTick();
  telegramTick();

  webTick();
  updateLed();
}
