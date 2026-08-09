/*
Here is the **complete, optimized and stable code**.

This version combines all earlier improvements:

1. **Fully non-blocking (`millis()`)**: No `delay()` calls, so the ESP does not freeze while ringing.
2. **Button interrupt**: A button press is captured at hardware level, even while the ESP8266 processes network or web tasks.
3. **Hardware conflict resolved**: `Serial.begin()` is removed so GPIO 1 (TX) and GPIO 3 (RX) can be used reliably for LED and button input.
4. **Crash and hang protection**: `client.setTimeout(3000)` and `yield()` are used in the Telegram function to reduce WDT resets on unstable WiFi.

```cpp
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <time.h>
#include <stdarg.h>
#include "wifi_secrets.h"

/* ===================== CONFIG ===================== */
#define DEBUG_SERIAL 0

static const int PIN_RELAY_OLD = 0;
static const int PIN_RELAY_NEW = 2;
static const int PIN_BUTTON    = 3; // GPIO 3 / RX pin (safe when Serial is disabled)
static const int PIN_LED       = 1; // GPIO 1 / TX Pin

static const bool RELAY_ACTIVE_LOW = true;

static uint32_t RING_PULSE_OLD_MS = 2000;
static uint32_t RING_PULSE_NEW_MS = 500;

static const char *TZ_INFO = "CET-1CEST,M3.5.0/02,M10.5.0/03";

/* Heap monitoring thresholds (ESP8266): warn early, panic later */
static const uint32_t HEAP_CHECK_INTERVAL_MS = 15000;
static const uint32_t HEAP_WARN_BYTES = 12000;
static const uint32_t HEAP_CRIT_BYTES = 9000;
static const uint32_t HEAP_RECOVER_BYTES = 14000;
static const uint32_t HEAP_ALERT_COOLDOWN_MS = 30UL * 60UL * 1000UL;

static const uint32_t SETTINGS_MAGIC = 0x4442554CUL;
static const uint16_t SETTINGS_VERSION = 1;
static const uint16_t SETTINGS_EEPROM_SIZE = 128;
static bool gSettingsStorageReady = false;

/* ===================== STATE ===================== */
static bool gRelayOldEnabled = true;
static bool gRelayNewEnabled = true;
static bool gTelegramEnabled = true;

static uint32_t muteOldUntilMs = 0;
static uint32_t muteNewUntilMs = 0;
static uint32_t muteTelegramUntilMs = 0;

/* Non-blocking relay timers */
static uint32_t relayOldOffTimeMs = 0;
static uint32_t relayNewOffTimeMs = 0;

/* Button status via interrupt */
volatile bool gButtonPressed = false;
static uint32_t lastDebounceTime = 0;
static const uint32_t DEBOUNCE_DELAY_MS = 35; // Accept only if button is still low after 35ms
static const uint32_t BUTTON_RETRIGGER_COOLDOWN_MS = 500; // Time between two valid ring presses
static bool gButtonPressArmed = true;
static bool gButtonValidationPending = false;
static uint32_t gButtonValidationDueMs = 0;

/* Per-component night mode: default off */
static bool nightModeOldEnabled = false;
static bool nightModeNewEnabled = false;
static bool nightModeTelegramEnabled = false;

static uint8_t nightOldStartHour = 22;
static uint8_t nightOldEndHour   = 7;
static uint8_t nightNewStartHour = 22;
static uint8_t nightNewEndHour   = 7;
static uint8_t nightTgStartHour  = 22;
static uint8_t nightTgEndHour    = 7;

enum HeapLevel : uint8_t { HEAP_OK = 0, HEAP_WARN = 1, HEAP_CRIT = 2 };
static HeapLevel gHeapLevel = HEAP_OK;
static uint32_t gMinHeapSeen = 0xFFFFFFFFUL;
static uint32_t gLastHeapCheckMs = 0;
static uint32_t gLastHeapAlertMs = 0;

static bool gDebugUiEnabled = false;

struct DebugEvent {
  uint32_t ms = 0;
  uint32_t epoch = 0;
  char text[96];
};

static DebugEvent gDebugEvents[40];
static uint8_t gDebugHead = 0;
static uint8_t gDebugSize = 0;

struct PersistentSettings {
  uint32_t magic;
  uint16_t version;
  uint8_t relayOldEnabled;
  uint8_t relayNewEnabled;
  uint8_t telegramEnabled;
  uint8_t nightModeOldEnabled;
  uint8_t nightModeNewEnabled;
  uint8_t nightModeTelegramEnabled;
  uint8_t nightOldStartHour;
  uint8_t nightOldEndHour;
  uint8_t nightNewStartHour;
  uint8_t nightNewEndHour;
  uint8_t nightTgStartHour;
  uint8_t nightTgEndHour;
  uint8_t debugUiEnabled;
  uint8_t reserved[8];
  uint32_t checksum;
};

/* Forward declarations */
static bool isTelegramMuted();
static bool getEpoch(uint32_t &outEpoch);
static bool telegramSendText(const String &msg, bool bypassMute = false);
static void heapMonitorTick();
static bool isInNightWindow(uint8_t startHour, uint8_t endHour);
static bool isOldNightMuted();
static bool isNewNightMuted();
static bool isTelegramNightMuted();
static void debugLog(const char *fmt, ...);
static void debugLogVerbose(const char *fmt, ...);
static void debugClear();
static String buildDebugText(uint8_t maxLines, bool includeHeader);
static bool sendDebugLogToTelegram();
static uint8_t clampHour(int value);
static bool loadPersistentSettings();
static bool savePersistentSettings();

/* ===================== QUEUE ===================== */
struct RingEvent {
  uint32_t epoch = 0;
};

static RingEvent queue[32];
static uint8_t qHead = 0, qTail = 0, qSize = 0;

static void queueClear() {
  qHead = qTail = qSize = 0;
}

static void debugClear() {
  gDebugHead = 0;
  gDebugSize = 0;
}

static String formatEpochWithSeconds(uint32_t epoch) {
  if (!epoch) return "unknown";
  time_t t = epoch;
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[20];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
  return String(buf);
}

static String debugTimeLabel(const DebugEvent &e) {
  if (e.epoch) return formatEpochWithSeconds(e.epoch);
  return String("t+") + String(e.ms / 1000UL) + "s";
}

static void debugLog(const char *fmt, ...) {
  char buf[96];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  uint8_t idx = (gDebugHead + gDebugSize) % 40;
  if (gDebugSize >= 40) {
    gDebugHead = (gDebugHead + 1) % 40;
    idx = (gDebugHead + gDebugSize - 1) % 40;
  } else {
    gDebugSize++;
  }

  gDebugEvents[idx].ms = millis();
  gDebugEvents[idx].epoch = 0;
  (void)getEpoch(gDebugEvents[idx].epoch);
  snprintf(gDebugEvents[idx].text, sizeof(gDebugEvents[idx].text), "%s", buf);
}

static void debugLogVerbose(const char *fmt, ...) {
  if (!gDebugUiEnabled) return;

  char buf[96];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  debugLog("%s", buf);
}

static String buildDebugText(uint8_t maxLines, bool includeHeader) {
  String out;
  out.reserve(1600);

  if (includeHeader) {
    out += "Doorbell debug log\n";
    out += "Total lines: ";
    out += String(gDebugSize);
    out += "\n\n";
  }

  if (gDebugSize == 0) {
    out += "(no actions)\n";
    return out;
  }

  uint8_t lines = gDebugSize;
  if (maxLines > 0 && lines > maxLines) lines = maxLines;
  uint8_t start = (uint8_t)(gDebugSize - lines);

  for (uint8_t i = 0; i < lines; i++) {
    uint8_t idx = (gDebugHead + start + i) % 40;
    out += "[";
    out += debugTimeLabel(gDebugEvents[idx]);
    out += "] ";
    out += gDebugEvents[idx].text;
    out += "\n";
  }

  return out;
}

static bool sendDebugLogToTelegram() {
  String msg = buildDebugText(20, true);

  // Telegram limiet is ~4096 chars; houd marge voor veiligheid.
  if (msg.length() > 3200) {
    msg.remove(3200);
    msg += "\n... truncated";
  }

  return telegramSendText(msg, true);
}

static uint32_t calcSettingsChecksum(const PersistentSettings &cfg) {
  const uint8_t *p = reinterpret_cast<const uint8_t*>(&cfg);
  size_t len = sizeof(PersistentSettings) - sizeof(cfg.checksum);

  // Kleine, snelle FNV-1a checksum voor integriteitscontrole.
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < len; i++) {
    hash ^= p[i];
    hash *= 16777619UL;
  }
  return hash;
}

static void applyPersistentSettings(const PersistentSettings &cfg) {
  gRelayOldEnabled = cfg.relayOldEnabled != 0;
  gRelayNewEnabled = cfg.relayNewEnabled != 0;
  gTelegramEnabled = cfg.telegramEnabled != 0;

  nightModeOldEnabled = cfg.nightModeOldEnabled != 0;
  nightModeNewEnabled = cfg.nightModeNewEnabled != 0;
  nightModeTelegramEnabled = cfg.nightModeTelegramEnabled != 0;

  nightOldStartHour = clampHour(cfg.nightOldStartHour);
  nightOldEndHour = clampHour(cfg.nightOldEndHour);
  nightNewStartHour = clampHour(cfg.nightNewStartHour);
  nightNewEndHour = clampHour(cfg.nightNewEndHour);
  nightTgStartHour = clampHour(cfg.nightTgStartHour);
  nightTgEndHour = clampHour(cfg.nightTgEndHour);

  gDebugUiEnabled = cfg.debugUiEnabled != 0;
}

static void fillPersistentSettings(PersistentSettings &cfg) {
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic = SETTINGS_MAGIC;
  cfg.version = SETTINGS_VERSION;

  cfg.relayOldEnabled = gRelayOldEnabled ? 1 : 0;
  cfg.relayNewEnabled = gRelayNewEnabled ? 1 : 0;
  cfg.telegramEnabled = gTelegramEnabled ? 1 : 0;

  cfg.nightModeOldEnabled = nightModeOldEnabled ? 1 : 0;
  cfg.nightModeNewEnabled = nightModeNewEnabled ? 1 : 0;
  cfg.nightModeTelegramEnabled = nightModeTelegramEnabled ? 1 : 0;

  cfg.nightOldStartHour = nightOldStartHour;
  cfg.nightOldEndHour = nightOldEndHour;
  cfg.nightNewStartHour = nightNewStartHour;
  cfg.nightNewEndHour = nightNewEndHour;
  cfg.nightTgStartHour = nightTgStartHour;
  cfg.nightTgEndHour = nightTgEndHour;

  cfg.debugUiEnabled = gDebugUiEnabled ? 1 : 0;
  cfg.checksum = calcSettingsChecksum(cfg);
}

static bool loadPersistentSettings() {
  if (!gSettingsStorageReady) return false;

  PersistentSettings cfg;
  EEPROM.get(0, cfg);

  if (cfg.magic != SETTINGS_MAGIC) return false;
  if (cfg.version != SETTINGS_VERSION) return false;
  if (cfg.checksum != calcSettingsChecksum(cfg)) return false;

  applyPersistentSettings(cfg);
  return true;
}

static bool savePersistentSettings() {
  if (!gSettingsStorageReady) return false;

  PersistentSettings cfg;
  fillPersistentSettings(cfg);
  EEPROM.put(0, cfg);
  return EEPROM.commit();
}

static void queuePush() {
  if (!gTelegramEnabled || isTelegramMuted() || isTelegramNightMuted()) return;
  if (qSize >= 32) return;

  uint32_t epoch = 0;
  getEpoch(epoch);

  queue[qTail] = { epoch };
  qTail = (qTail + 1) % 32;
  qSize++;
}

/* ===================== HELPERS ===================== */
static bool getEpoch(uint32_t &outEpoch) {
  time_t now = time(nullptr);
  if (now < 1700000000) return false;
  outEpoch = (uint32_t)now;
  return true;
}

static String formatTime(uint32_t epoch) {
  if (!epoch) return "unknown";
  time_t t = epoch;
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
  return String(buf);
}

static bool isMuted(uint32_t untilMs) {
  return untilMs && (int32_t)(millis() - untilMs) < 0;
}

static bool isOldMuted() { return isMuted(muteOldUntilMs); }
static bool isNewMuted() { return isMuted(muteNewUntilMs); }
static bool isTelegramMuted() { return isMuted(muteTelegramUntilMs); }

static bool isInNightWindow(uint8_t startHour, uint8_t endHour) {
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  if (startHour < endHour)
    return tm.tm_hour >= startHour && tm.tm_hour < endHour;
  return tm.tm_hour >= startHour || tm.tm_hour < endHour;
}

static bool isOldNightMuted() {
  if (!nightModeOldEnabled) return false;
  return isInNightWindow(nightOldStartHour, nightOldEndHour);
}

static bool isNewNightMuted() {
  if (!nightModeNewEnabled) return false;
  return isInNightWindow(nightNewStartHour, nightNewEndHour);
}

static bool isTelegramNightMuted() {
  if (!nightModeTelegramEnabled) return false;
  return isInNightWindow(nightTgStartHour, nightTgEndHour);
}

static void setRelay(int pin, bool on) {
  digitalWrite(pin, RELAY_ACTIVE_LOW ? !on : on);
}

/* ===================== INTERRUPT SERVICE ROUTINE ===================== */
// This ISR runs immediately when the button line falls to GND.
void IRAM_ATTR handleButtonInterrupt() {
  gButtonPressed = true;
}

/* ===================== RING ===================== */
static void startRing() {
  uint32_t now = millis();
  
  bool oldAllowed = gRelayOldEnabled && !isOldMuted() && !isOldNightMuted();
  bool newAllowed = gRelayNewEnabled && !isNewMuted() && !isNewNightMuted();

  if (oldAllowed) {
    setRelay(PIN_RELAY_OLD, true);
    relayOldOffTimeMs = now + RING_PULSE_OLD_MS; // Schedule relay off time.
  }

  if (newAllowed) {
    setRelay(PIN_RELAY_NEW, true);
    relayNewOffTimeMs = now + RING_PULSE_NEW_MS; // Schedule relay off time.
  }

  debugLog("Ring action: old=%s new=%s", oldAllowed ? "on" : "off", newAllowed ? "on" : "off");

  queuePush();
}

// Checks whether active relays should be switched off.
static void checkRelayTimers() {
  uint32_t now = millis();
  
  if (relayOldOffTimeMs && (int32_t)(now - relayOldOffTimeMs) >= 0) {
    setRelay(PIN_RELAY_OLD, false);
    relayOldOffTimeMs = 0;
  }
  
  if (relayNewOffTimeMs && (int32_t)(now - relayNewOffTimeMs) >= 0) {
    setRelay(PIN_RELAY_NEW, false);
    relayNewOffTimeMs = 0;
  }
}

/* ===================== TELEGRAM ===================== */
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

static bool telegramSendText(const String &msg, bool bypassMute) {
  if (!gTelegramEnabled) return false;
  if (!bypassMute && isTelegramMuted()) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(3000);

  if (!client.connect("api.telegram.org", 443)) {
    return false;
  }

  String body = "chat_id=" + String(TG_CHAT_ID) + "&text=" + urlEncode(msg);
  client.print(
    "POST /bot" + String(TG_BOT_TOKEN) + "/sendMessage HTTP/1.1\r\n"
    "Host: api.telegram.org\r\n"
    "Content-Type: application/x-www-form-urlencoded\r\n"
    "Content-Length: " + String(body.length()) + "\r\n\r\n" + body
  );

  unsigned long startWait = millis();
  while (client.connected() && millis() - startWait < 500) {
    if (client.available()) break;
    yield();
  }

  return true;
}

static void telegramSend() {
  if (!gTelegramEnabled || isTelegramMuted() || isTelegramNightMuted() || qSize == 0) return;

  // Rate limiting: send at most 1 Telegram message every 10 seconds.
  static uint32_t lastTelegramSentMs = 0;
  uint32_t now = millis();
  if (lastTelegramSentMs && (now - lastTelegramSentMs) < 10000UL) {
    return; // Wait and batch multiple presses in the queue.
  }

  String msg = "🔔 Doorbell\nCount: " + String(qSize) + "\nLast: ";
  msg += formatTime(queue[(qTail + 31) % 32].epoch);

  if (!telegramSendText(msg, false)) return;

  // Update last-send only after a successful send attempt.
  lastTelegramSentMs = millis();
  debugLog("Telegram sent (%u events)", qSize);
  queueClear();
}

static const char* heapLevelLabel(HeapLevel lvl) {
  if (lvl == HEAP_CRIT) return "critical";
  if (lvl == HEAP_WARN) return "low";
  return "ok";
}

static const char* heapLevelClass(HeapLevel lvl) {
  return (lvl == HEAP_OK) ? "badge-on" : "badge-off";
}

static void heapMonitorTick() {
  uint32_t now = millis();
  if (gLastHeapCheckMs && (now - gLastHeapCheckMs) < HEAP_CHECK_INTERVAL_MS) return;
  gLastHeapCheckMs = now;

  uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < gMinHeapSeen) gMinHeapSeen = freeHeap;

  HeapLevel newLevel = gHeapLevel;
  if (freeHeap <= HEAP_CRIT_BYTES) newLevel = HEAP_CRIT;
  else if (freeHeap <= HEAP_WARN_BYTES) newLevel = HEAP_WARN;
  else if (freeHeap >= HEAP_RECOVER_BYTES) newLevel = HEAP_OK;

  bool needAlert = false;
  if (newLevel != gHeapLevel) {
    needAlert = true;
  } else if (newLevel == HEAP_CRIT && (!gLastHeapAlertMs || (now - gLastHeapAlertMs) >= HEAP_ALERT_COOLDOWN_MS)) {
    needAlert = true;
  }

  if (!needAlert) {
    gHeapLevel = newLevel;
    return;
  }

  String msg;
  msg.reserve(220);
  if (newLevel == HEAP_OK && gHeapLevel != HEAP_OK) {
    msg = "✅ ESP recovered\nHeap stable again: " + String(freeHeap) + " bytes\nMin seen: " + String(gMinHeapSeen) + " bytes";
  } else if (newLevel == HEAP_CRIT) {
    msg = "🚨 ESP memory critical\nFree heap: " + String(freeHeap) + " bytes\nMin seen: " + String(gMinHeapSeen) + " bytes\nAdvice: reboot or reduce web requests";
  } else if (newLevel == HEAP_WARN) {
    msg = "⚠️ ESP memory low\nFree heap: " + String(freeHeap) + " bytes\nMin seen: " + String(gMinHeapSeen) + " bytes";
  }

  // Critical system warnings may bypass mute, unless Telegram is fully disabled.
  if (telegramSendText(msg, true)) {
    gLastHeapAlertMs = now;
    debugLog("Heap alert: %s (%lu bytes)", heapLevelLabel(newLevel), (unsigned long)freeHeap);
  }

  gHeapLevel = newLevel;
}

/* ===================== WEB ===================== */
ESP8266WebServer web(80);

static String muteStatus(uint32_t untilMs) {
  if (!untilMs) return "active";
  if (!isMuted(untilMs)) return "active";
  uint32_t e;
  if (!getEpoch(e)) return "muted";
  return "muted until " + formatTime(e + (untilMs - millis()) / 1000);
}

static String nightModeStatus(bool enabled, uint8_t startHour, uint8_t endHour, bool activeNow) {
  if (!enabled) return "off";
  String s = "on (";
  s += String(startHour);
  s += ":00-";
  s += String(endHour);
  s += ":00";
  s += activeNow ? ", active now)" : ", inactive now)";
  return s;
}

static String aanUitStatus(bool enabled) {
  return enabled ? "on" : "off";
}

static void handleRoot() {
  bool wifiUp = WiFi.status() == WL_CONNECTED;
  String ip = wifiUp ? WiFi.localIP().toString() : "offline";

  // Stream in small chunks to lower peak heap usage.
  web.setContentLength(CONTENT_LENGTH_UNKNOWN);
  web.send(200, "text/html; charset=utf-8", "");

  web.sendContent(
    F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width, initial-scale=1'>"
      "<title>Doorbell control</title>"
      "<style>"
      "*{box-sizing:border-box;margin:0;padding:0;}"
      "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);min-height:100vh;padding:20px;color:#333;}"
      ".container{max-width:880px;margin:0 auto;background:#fff;border-radius:16px;box-shadow:0 20px 40px rgba(0,0,0,.15);overflow:hidden;}"
      ".header{background:linear-gradient(45deg,#36a661,#2f8f53);color:#fff;padding:28px;text-align:center;}"
      ".header h1{font-size:2.2em;margin-bottom:8px;}"
      ".header p{opacity:.95;}"
      ".content{padding:24px;}"
      ".status-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:14px;margin-bottom:22px;}"
      ".status-card{background:#f8f9fb;border-left:4px solid #36a661;border-radius:10px;padding:14px;}"
      ".status-card h3{color:#2f8f53;font-size:.95em;margin-bottom:6px;}"
      ".status-value{font-size:1.15em;font-weight:700;}"
      ".section{margin-bottom:24px;}"
      ".section h2{color:#2f8f53;border-bottom:2px solid #e7eaee;padding-bottom:8px;margin-bottom:14px;}"
      ".mute-block{background:#f8f9fb;border-radius:10px;padding:14px;margin-bottom:12px;}"
      ".mute-title{font-weight:700;margin-bottom:6px;}"
      ".mute-status{margin-bottom:10px;color:#45505e;}"
      ".btn{display:inline-block;text-decoration:none;background:linear-gradient(45deg,#36a661,#2f8f53);color:#fff;border-radius:8px;padding:9px 14px;margin:3px;font-size:.95em;}"
      ".btn:hover{filter:brightness(1.05);}"
      ".btn-off{background:linear-gradient(45deg,#6b7280,#555f6d);}"
      ".badge-on{color:#188645;}"
      ".badge-off{color:#bf2d2d;}"
      ".log-wrap{background:#111827;color:#e5e7eb;border-radius:10px;padding:10px;max-height:240px;overflow:auto;font-family:Consolas,'Courier New',monospace;font-size:.85em;}"
      ".log-row{padding:4px 0;border-bottom:1px solid rgba(255,255,255,.08);}"
      ".footer{background:#f4f6f8;border-top:1px solid #e7eaee;color:#647181;text-align:center;padding:16px;}"
      "@media(max-width:640px){body{padding:10px;}.header{padding:20px;}.header h1{font-size:1.7em;}.content{padding:16px;}}"
          "</style></head><body><div class='container'><div class='header'><h1>🔔 Doorbell control</h1><p>ESP01S status and settings</p><p>Default on boot: components ON, night mode OFF.</p></div><div class='content'><div class='status-grid'>")
  );

  String chunk;
  chunk.reserve(256);

  chunk = "<div class='status-card'><h3>IP address</h3><div class='status-value'>" + ip + "</div></div>";
  web.sendContent(chunk);

  chunk = "<div class='status-card'><h3>WiFi</h3><div class='status-value ";
  chunk += (wifiUp ? "badge-on'>connected" : "badge-off'>offline");
  chunk += "</div></div>";
  web.sendContent(chunk);

  chunk = "<div class='status-card'><h3>Night mode active</h3><div class='status-value ";
  chunk += ((isOldNightMuted() || isNewNightMuted() || isTelegramNightMuted()) ? "badge-off'>yes" : "badge-on'>no");
  chunk += "</div></div>";
  web.sendContent(chunk);

  chunk = "<div class='status-card'><h3>Queue</h3><div class='status-value'>" + String(qSize) + " events</div></div>";
  web.sendContent(chunk);

  chunk = "<div class='status-card'><h3>Memory</h3><div class='status-value ";
  chunk += heapLevelClass(gHeapLevel);
  chunk += "'>";
  chunk += String(ESP.getFreeHeap());
  chunk += " bytes (";
  chunk += heapLevelLabel(gHeapLevel);
  chunk += ")</div></div>";
  web.sendContent(chunk);

  web.sendContent(F("</div><div class='section'><h2>🧪 Debugging</h2>"));
  chunk = "<div class='mute-block'><div class='mute-title'>Debug mode</div><div class='mute-status'>Status: ";
  chunk += gDebugUiEnabled ? "on" : "off";
  chunk += " (base log always active, on = extra details)</div><a class='btn' href='/debug?en=1'>Debug on</a><a class='btn btn-off' href='/debug?en=0'>Debug off</a><a class='btn btn-off' href='/debug?clear=1'>Clear log</a><a class='btn' href='/debug?export=1'>Export .txt</a><a class='btn' href='/debug?sendtg=1'>Send to Telegram</a></div>";
  web.sendContent(chunk);

  web.sendContent(F("<div class='mute-block'><div class='mute-title'>Action log (no idle status)</div><div class='log-wrap'>"));
  if (gDebugSize == 0) {
    web.sendContent(F("<div class='log-row'>No actions recorded yet.</div>"));
  } else {
    for (uint8_t i = 0; i < gDebugSize; i++) {
      uint8_t idx = (gDebugHead + i) % 40;
      String row = "<div class='log-row'>[";
      row += debugTimeLabel(gDebugEvents[idx]);
      row += "] ";
      row += gDebugEvents[idx].text;
      row += "</div>";
      web.sendContent(row);
    }
  }
  web.sendContent(F("</div></div>"));

  web.sendContent(F("</div><div class='section'><h2>⏻ Components on/off</h2>"));

  chunk = "<div class='mute-block'><div class='mute-title'>Legacy bell</div><div class='mute-status'>Status: ";
  chunk += aanUitStatus(gRelayOldEnabled);
  chunk += "</div><a class='btn' href='/part?ch=old&en=1'>Fully on</a><a class='btn btn-off' href='/part?ch=old&en=0'>Fully off</a></div>";
  web.sendContent(chunk);

  chunk = "<div class='mute-block'><div class='mute-title'>New bell</div><div class='mute-status'>Status: ";
  chunk += aanUitStatus(gRelayNewEnabled);
  chunk += "</div><a class='btn' href='/part?ch=new&en=1'>Fully on</a><a class='btn btn-off' href='/part?ch=new&en=0'>Fully off</a></div>";
  web.sendContent(chunk);

  chunk = "<div class='mute-block'><div class='mute-title'>Telegram</div><div class='mute-status'>Status: ";
  chunk += aanUitStatus(gTelegramEnabled);
  chunk += "</div><a class='btn' href='/part?ch=tg&en=1'>Fully on</a><a class='btn btn-off' href='/part?ch=tg&en=0'>Fully off</a></div>";
  web.sendContent(chunk);

  web.sendContent(F("</div><div class='section'><h2>🔕 Temporary mute</h2>"));

  chunk = "<div class='mute-block'><div class='mute-title'>Legacy bell</div><div class='mute-status'>Status: ";
  chunk += muteStatus(muteOldUntilMs);
  chunk += "</div><a class='btn' href='/mute?old=1800'>30 min</a><a class='btn' href='/mute?old=3600'>1 hour</a><a class='btn' href='/mute?old=7200'>2 hours</a><a class='btn' href='/mute?old=14400'>4 hours</a><a class='btn btn-off' href='/mute?old=0'>Cancel</a></div>";
  web.sendContent(chunk);

  chunk = "<div class='mute-block'><div class='mute-title'>New bell</div><div class='mute-status'>Status: ";
  chunk += muteStatus(muteNewUntilMs);
  chunk += "</div><a class='btn' href='/mute?new=1800'>30 min</a><a class='btn' href='/mute?new=3600'>1 hour</a><a class='btn' href='/mute?new=7200'>2 hours</a><a class='btn' href='/mute?new=14400'>4 hours</a><a class='btn btn-off' href='/mute?new=0'>Cancel</a></div>";
  web.sendContent(chunk);

  chunk = "<div class='mute-block'><div class='mute-title'>Telegram</div><div class='mute-status'>Status: ";
  chunk += muteStatus(muteTelegramUntilMs);
  chunk += "</div><a class='btn' href='/mute?tg=1800'>30 min</a><a class='btn' href='/mute?tg=3600'>1 hour</a><a class='btn' href='/mute?tg=7200'>2 hours</a><a class='btn' href='/mute?tg=14400'>4 hours</a><a class='btn btn-off' href='/mute?tg=0'>Cancel</a></div>";
  web.sendContent(chunk);

  web.sendContent(F("</div><div class='section'><h2>🌙 Night mode</h2>"));

  chunk = "<div class='mute-block'><div class='mute-title'>Legacy bell</div><div class='mute-status'>Status: ";
  chunk += nightModeStatus(nightModeOldEnabled, nightOldStartHour, nightOldEndHour, isOldNightMuted());
  chunk += "</div><a class='btn' href='/night?ch=old&en=1'>Enable</a><a class='btn btn-off' href='/night?ch=old&en=0'>Disable</a> ";
  chunk += "<a class='btn' href='/night?ch=old&start=22&end=7'>22-07</a><a class='btn' href='/night?ch=old&start=23&end=6'>23-06</a><a class='btn' href='/night?ch=old&start=0&end=0'>24 hours</a><br><br>";
  chunk += "<form action='/night' method='get'>";
  chunk += "<input type='hidden' name='ch' value='old'>";
  chunk += "Start <input type='number' name='start' min='0' max='23' value='" + String(nightOldStartHour) + "' style='width:60px;'> ";
  chunk += "End <input type='number' name='end' min='0' max='23' value='" + String(nightOldEndHour) + "' style='width:60px;'> ";
  chunk += "<button class='btn' type='submit'>Save times</button></form></div>";
  web.sendContent(chunk);

  chunk = "<div class='mute-block'><div class='mute-title'>New bell</div><div class='mute-status'>Status: ";
  chunk += nightModeStatus(nightModeNewEnabled, nightNewStartHour, nightNewEndHour, isNewNightMuted());
  chunk += "</div><a class='btn' href='/night?ch=new&en=1'>Enable</a><a class='btn btn-off' href='/night?ch=new&en=0'>Disable</a> ";
  chunk += "<a class='btn' href='/night?ch=new&start=22&end=7'>22-07</a><a class='btn' href='/night?ch=new&start=23&end=6'>23-06</a><a class='btn' href='/night?ch=new&start=0&end=0'>24 hours</a><br><br>";
  chunk += "<form action='/night' method='get'>";
  chunk += "<input type='hidden' name='ch' value='new'>";
  chunk += "Start <input type='number' name='start' min='0' max='23' value='" + String(nightNewStartHour) + "' style='width:60px;'> ";
  chunk += "End <input type='number' name='end' min='0' max='23' value='" + String(nightNewEndHour) + "' style='width:60px;'> ";
  chunk += "<button class='btn' type='submit'>Save times</button></form></div>";
  web.sendContent(chunk);

  chunk = "<div class='mute-block'><div class='mute-title'>Telegram</div><div class='mute-status'>Status: ";
  chunk += nightModeStatus(nightModeTelegramEnabled, nightTgStartHour, nightTgEndHour, isTelegramNightMuted());
  chunk += "</div><a class='btn' href='/night?ch=tg&en=1'>Enable</a><a class='btn btn-off' href='/night?ch=tg&en=0'>Disable</a> ";
  chunk += "<a class='btn' href='/night?ch=tg&start=22&end=7'>22-07</a><a class='btn' href='/night?ch=tg&start=23&end=6'>23-06</a><a class='btn' href='/night?ch=tg&start=0&end=0'>24 hours</a><br><br>";
  chunk += "<form action='/night' method='get'>";
  chunk += "<input type='hidden' name='ch' value='tg'>";
  chunk += "Start <input type='number' name='start' min='0' max='23' value='" + String(nightTgStartHour) + "' style='width:60px;'> ";
  chunk += "End <input type='number' name='end' min='0' max='23' value='" + String(nightTgEndHour) + "' style='width:60px;'> ";
  chunk += "<button class='btn' type='submit'>Save times</button></form></div></div><div class='footer'>Uptime: ";
  chunk += String(millis() / 1000);
  chunk += " seconds • Lowest heap: ";
  if (gMinHeapSeen == 0xFFFFFFFFUL) chunk += "n/a";
  else chunk += String(gMinHeapSeen) + " bytes";
  chunk += "</div></div></body></html>";
  web.sendContent(chunk);
}

static uint8_t clampHour(int value) {
  if (value < 0) return 0;
  if (value > 23) return 23;
  return (uint8_t)value;
}

static void handleNight() {
  String ch = web.arg("ch");

  bool *enabled = nullptr;
  uint8_t *start = nullptr;
  uint8_t *end = nullptr;

  if (ch == "old") {
    enabled = &nightModeOldEnabled;
    start = &nightOldStartHour;
    end = &nightOldEndHour;
  } else if (ch == "new") {
    enabled = &nightModeNewEnabled;
    start = &nightNewStartHour;
    end = &nightNewEndHour;
  } else if (ch == "tg") {
    enabled = &nightModeTelegramEnabled;
    start = &nightTgStartHour;
    end = &nightTgEndHour;
  }

  if (enabled && start && end) {
    bool changed = false;

    if (web.hasArg("en")) {
      bool newEnabled = web.arg("en").toInt() != 0;
      if (*enabled != newEnabled) {
        *enabled = newEnabled;
        changed = true;
      }
    }
    if (web.hasArg("start")) {
      uint8_t newStart = clampHour(web.arg("start").toInt());
      if (*start != newStart) {
        *start = newStart;
        changed = true;
      }
    }
    if (web.hasArg("end")) {
      uint8_t newEnd = clampHour(web.arg("end").toInt());
      if (*end != newEnd) {
        *end = newEnd;
        changed = true;
      }
    }

    debugLog("Night mode %s: en=%s start=%u end=%u",
             ch.c_str(),
             *enabled ? "aan" : "off",
             (unsigned)*start,
             (unsigned)*end);

    if (changed) {
      savePersistentSettings();
    }
  }

  web.sendHeader("Location", "/");
  web.send(302);
}

static void handleDebug() {
  if (web.hasArg("export")) {
    String txt = buildDebugText(0, true);
    web.sendHeader("Content-Disposition", "attachment; filename=debug-log.txt");
    web.send(200, "text/plain; charset=utf-8", txt);
    debugLog("Debug log exported via web");
    return;
  }

  if (web.hasArg("sendtg")) {
    bool ok = sendDebugLogToTelegram();
    debugLog("Debug log to Telegram: %s", ok ? "success" : "failed");
    web.sendHeader("Location", "/");
    web.send(302);
    return;
  }

  if (web.hasArg("en")) {
    bool enable = web.arg("en").toInt() != 0;
    bool changed = false;
    if (enable && !gDebugUiEnabled) {
      gDebugUiEnabled = true;
      changed = true;
      debugLog("Debug mode enabled");
    } else if (!enable && gDebugUiEnabled) {
      debugLog("Debug mode disabled");
      gDebugUiEnabled = false;
      changed = true;
    }

    if (changed) {
      savePersistentSettings();
    }
  }

  if (web.hasArg("clear")) {
    debugClear();
  }

  web.sendHeader("Location", "/");
  web.send(302);
}

static void handlePartPower() {
  String ch = web.arg("ch");
  bool enable = web.arg("en").toInt() != 0;
  bool changed = false;

  if (ch == "old") {
    if (gRelayOldEnabled != enable) {
      gRelayOldEnabled = enable;
      changed = true;
    }
  } else if (ch == "new") {
    if (gRelayNewEnabled != enable) {
      gRelayNewEnabled = enable;
      changed = true;
    }
  } else if (ch == "tg") {
    if (gTelegramEnabled != enable) {
      gTelegramEnabled = enable;
      changed = true;
    }
    if (!enable) {
      // When fully disabling Telegram, clear queue immediately.
      queueClear();
    }
  }

  debugLog("Component %s -> %s", ch.c_str(), enable ? "on" : "off");

  if (changed) {
    savePersistentSettings();
  }

  web.sendHeader("Location", "/");
  web.send(302);
}

static void handleMute() {
  uint32_t now = millis();
  if (web.hasArg("old")) {
    int sec = web.arg("old").toInt();
    muteOldUntilMs = now + sec * 1000UL;
    debugLog("Mute legacy bell: %d sec", sec);
  }
  if (web.hasArg("new")) {
    int sec = web.arg("new").toInt();
    muteNewUntilMs = now + sec * 1000UL;
    debugLog("Mute new bell: %d sec", sec);
  }
  if (web.hasArg("tg")) {
    int sec = web.arg("tg").toInt();
    muteTelegramUntilMs = now + sec * 1000UL;
    debugLog("Mute Telegram: %d sec", sec);
  }
  web.sendHeader("Location", "/");
  web.send(302);
}

/* ===================== SETUP / LOOP ===================== */
void setup() {
  // Serial.begin(115200); // Fully disabled to free GPIO 1 and 3 for LED/button

  pinMode(PIN_RELAY_OLD, OUTPUT);
  pinMode(PIN_RELAY_NEW, OUTPUT);
  
  // Ensure both relays are off at startup.
  setRelay(PIN_RELAY_OLD, false);
  setRelay(PIN_RELAY_NEW, false);

  // Configure button pin with internal pull-up
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  
  // Attach hardware interrupt to button (triggers when pin is pulled to GND)
  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), handleButtonInterrupt, FALLING);

  pinMode(PIN_LED, OUTPUT);

  EEPROM.begin(SETTINGS_EEPROM_SIZE);
  gSettingsStorageReady = true;
  if (gSettingsStorageReady) {
    if (loadPersistentSettings()) {
      debugLog("Persistent settings loaded");
    } else {
      savePersistentSettings();
      debugLog("Default settings saved");
    }
  } else {
    debugLog("Warning: EEPROM not available");
  }

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, "pool.ntp.org");
  setenv("TZ", TZ_INFO, 1);
  tzset();

  web.on("/", handleRoot);
  web.on("/debug", handleDebug);
  web.on("/mute", handleMute);
  web.on("/night", handleNight);
  web.on("/part", handlePartPower);
  web.begin();
}


void loop() {
  uint32_t now = millis();

  // Re-arm only after the button is physically released.
  if (!gButtonPressArmed && digitalRead(PIN_BUTTON) == HIGH) {
    gButtonPressArmed = true;
    debugLogVerbose("Button released: trigger re-armed");
  }

  // Interrupt seen: start validation window only when trigger is armed.
  if (gButtonPressed) {
    gButtonPressed = false;
    if (gButtonPressArmed && !gButtonValidationPending) {
      gButtonValidationPending = true;
      gButtonValidationDueMs = now + DEBOUNCE_DELAY_MS;
    }
  }

  // Only trigger a ring when button is still low after debounce.
  if (gButtonValidationPending && (int32_t)(now - gButtonValidationDueMs) >= 0) {
    gButtonValidationPending = false;

    if (digitalRead(PIN_BUTTON) == LOW && gButtonPressArmed) {
      // ONESHOT COOLDOWN: accept button only if at least
      // 500 milliseconds (0.5 seconds) passed since the previous ring.
      if (lastDebounceTime == 0 || (now - lastDebounceTime) > BUTTON_RETRIGGER_COOLDOWN_MS) {
        startRing();
        lastDebounceTime = now;
        gButtonPressArmed = false;
      } else {
        debugLogVerbose("Press ignored: cooldown active (%lums)", (unsigned long)(now - lastDebounceTime));
      }
    } else {
      debugLogVerbose("Ghost trigger ignored: button not stably low");
    }
  }

  checkRelayTimers(); // Handles non-blocking relay switch-off.
  telegramSend();     // Processes and sends the Telegram message
  heapMonitorTick();  // Automatically warns on low/critical memory
  web.handleClient(); // Handles incoming webserver requests
  
  yield();            // Gives ESP8266 WiFi background tasks execution time
}

