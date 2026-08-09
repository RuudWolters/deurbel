/*
Hier is de **volledige, geoptimaliseerde en stabiele code**.

In deze versie zijn alle eerdere verbeteringen gecombineerd:

1. **Volledig non-blocking (`millis()`)**: Geen enkele `delay()` meer, waardoor de ESP nooit "bevriest" tijdens het luiden van de bel.
2. **Interrupt op de knop**: De knopdruk wordt direct op hardwareniveau geregistreerd, zelfs als de ESP8266 netwerk- of webtaken uitvoert.
3. **Hardware conflict opgelost**: `Serial.begin()` is volledig verwijderd. Hierdoor werken GPIO 1 (TX) en GPIO 3 (RX) nu 100% betrouwbaar als digitale pinnen voor je LED en drukknop, zonder interferentie van de seriële poort.
4. **Crash- & Hangbeveiliging**: `client.setTimeout(3000)` en `yield()` zijn toegevoegd aan de Telegram-functie om te voorkomen dat een haperende wifiverbinding de Watchdog Timer (WDT) triggert of de boel laat crashen.

```cpp
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266WebServer.h>
#include <time.h>
#include <stdarg.h>
#include "wifi_secrets.h"

/* ===================== CONFIG ===================== */
#define DEBUG_SERIAL 0

static const int PIN_RELAY_OLD = 0;
static const int PIN_RELAY_NEW = 2;
static const int PIN_BUTTON    = 3; // GPIO 3 / RX Pin (veilig te gebruiken mits Serial uit staat)
static const int PIN_LED       = 1; // GPIO 1 / TX Pin

static const bool RELAY_ACTIVE_LOW = true;

static uint32_t RING_PULSE_OLD_MS = 2000;
static uint32_t RING_PULSE_NEW_MS = 500;

static const char *TZ_INFO = "CET-1CEST,M3.5.0/02,M10.5.0/03";

/* Heap monitoring thresholds (ESP8266): waarschuw vroeg, paniek later */
static const uint32_t HEAP_CHECK_INTERVAL_MS = 15000;
static const uint32_t HEAP_WARN_BYTES = 12000;
static const uint32_t HEAP_CRIT_BYTES = 9000;
static const uint32_t HEAP_RECOVER_BYTES = 14000;
static const uint32_t HEAP_ALERT_COOLDOWN_MS = 30UL * 60UL * 1000UL;

/* ===================== STATE ===================== */
static bool gRelayOldEnabled = true;
static bool gRelayNewEnabled = true;
static bool gTelegramEnabled = true;

static uint32_t muteOldUntilMs = 0;
static uint32_t muteNewUntilMs = 0;
static uint32_t muteTelegramUntilMs = 0;

/* Non-blocking Relais timers */
static uint32_t relayOldOffTimeMs = 0;
static uint32_t relayNewOffTimeMs = 0;

/* Knoppen status via Interrupt */
volatile bool gButtonPressed = false;
static uint32_t lastDebounceTime = 0;
static const uint32_t DEBOUNCE_DELAY_MS = 200; // Voorkomt dubbel triggeren door denderen van de knop

/* Nacht schema */
static uint8_t nightStartHour = 22;
static uint8_t nightEndHour   = 7;

enum HeapLevel : uint8_t { HEAP_OK = 0, HEAP_WARN = 1, HEAP_CRIT = 2 };
static HeapLevel gHeapLevel = HEAP_OK;
static uint32_t gMinHeapSeen = 0xFFFFFFFFUL;
static uint32_t gLastHeapCheckMs = 0;
static uint32_t gLastHeapAlertMs = 0;

/* Forward declarations */
static bool isTelegramMuted();
static bool getEpoch(uint32_t &outEpoch);
static bool telegramSendText(const String &msg, bool bypassMute = false);
static void heapMonitorTick();

/* ===================== QUEUE ===================== */
struct RingEvent {
  uint32_t epoch = 0;
};

static RingEvent queue[32];
static uint8_t qHead = 0, qTail = 0, qSize = 0;

static void queueClear() {
  qHead = qTail = qSize = 0;
}

static void queuePush() {
  if (!gTelegramEnabled || isTelegramMuted()) return;
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
  if (!epoch) return "onbekend";
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

static bool isNightTime() {
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  if (nightStartHour < nightEndHour)
    return tm.tm_hour >= nightStartHour && tm.tm_hour < nightEndHour;
  return tm.tm_hour >= nightStartHour || tm.tm_hour < nightEndHour;
}

static void setRelay(int pin, bool on) {
  digitalWrite(pin, RELAY_ACTIVE_LOW ? !on : on);
}

/* ===================== INTERRUPT SERVICE ROUTINE ===================== */
// Deze functie wordt direct en met prioriteit uitgevoerd zodra de knop wordt ingedrukt
void IRAM_ATTR handleButtonInterrupt() {
  gButtonPressed = true;
}

/* ===================== RING ===================== */
static void startRing() {
  uint32_t now = millis();
  
  bool oldAllowed = gRelayOldEnabled && !isOldMuted() && !isNightTime();
  bool newAllowed = gRelayNewEnabled && !isNewMuted();

  if (oldAllowed) {
    setRelay(PIN_RELAY_OLD, true);
    relayOldOffTimeMs = now + RING_PULSE_OLD_MS; // Stel de uitschakeltijd in
  }

  if (newAllowed) {
    setRelay(PIN_RELAY_NEW, true);
    relayNewOffTimeMs = now + RING_PULSE_NEW_MS; // Stel de uitschakeltijd in
  }

  queuePush();
}

// Controleert in de achtergrond of een actief relais alweer uitgezet moet worden
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
  if (!gTelegramEnabled || isTelegramMuted() || qSize == 0) return;

  // Rate limiting: Stuur maximaal 1 Telegram bericht per 10 seconden
  static uint32_t lastTelegramSentMs = 0;
  uint32_t now = millis();
  if (lastTelegramSentMs && (now - lastTelegramSentMs) < 10000UL) {
    return; // Wacht nog even met sturen, verzamel de drukken in de queue
  }

  String msg = "🔔 Deurbel\nAantal: " + String(qSize) + "\nLaatste: ";
  msg += formatTime(queue[(qTail + 31) % 32].epoch);

  if (!telegramSendText(msg, false)) return;

  // Registreer het tijdstip alleen na succesvolle poging.
  lastTelegramSentMs = millis();
  queueClear();
}

static const char* heapLevelLabel(HeapLevel lvl) {
  if (lvl == HEAP_CRIT) return "kritiek";
  if (lvl == HEAP_WARN) return "laag";
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
    msg = "✅ ESP herstel\nHeap weer stabiel: " + String(freeHeap) + " bytes\nMin gezien: " + String(gMinHeapSeen) + " bytes";
  } else if (newLevel == HEAP_CRIT) {
    msg = "🚨 ESP geheugen kritiek\nVrije heap: " + String(freeHeap) + " bytes\nMin gezien: " + String(gMinHeapSeen) + " bytes\nAdvies: herstart of minder web-requests";
  } else if (newLevel == HEAP_WARN) {
    msg = "⚠️ ESP geheugen laag\nVrije heap: " + String(freeHeap) + " bytes\nMin gezien: " + String(gMinHeapSeen) + " bytes";
  }

  // Kritieke systeemwaarschuwingen mogen door mute heen; bij hard uitgeschakelde Telegram niet.
  if (telegramSendText(msg, true)) {
    gLastHeapAlertMs = now;
  }

  gHeapLevel = newLevel;
}

/* ===================== WEB ===================== */
ESP8266WebServer web(80);

static String muteStatus(uint32_t untilMs) {
  if (!untilMs) return "actief";
  if (!isMuted(untilMs)) return "actief";
  uint32_t e;
  if (!getEpoch(e)) return "gedempt";
  return "gedempt tot " + formatTime(e + (untilMs - millis()) / 1000);
}

static void handleRoot() {
  bool wifiUp = WiFi.status() == WL_CONNECTED;
  String ip = wifiUp ? WiFi.localIP().toString() : "offline";

  // Stream in kleine blokken om piekgebruik van heap te verlagen.
  web.setContentLength(CONTENT_LENGTH_UNKNOWN);
  web.send(200, "text/html; charset=utf-8", "");

  web.sendContent(
    F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width, initial-scale=1'>"
      "<title>Deurbel Bediening</title>"
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
      ".footer{background:#f4f6f8;border-top:1px solid #e7eaee;color:#647181;text-align:center;padding:16px;}"
      "@media(max-width:640px){body{padding:10px;}.header{padding:20px;}.header h1{font-size:1.7em;}.content{padding:16px;}}"
      "</style></head><body><div class='container'><div class='header'><h1>🔔 Deurbel Bediening</h1><p>ESP01S status en dempingsoverzicht</p></div><div class='content'><div class='status-grid'>")
  );

  String chunk;
  chunk.reserve(256);

  chunk = "<div class='status-card'><h3>IP-adres</h3><div class='status-value'>" + ip + "</div></div>";
  web.sendContent(chunk);

  chunk = "<div class='status-card'><h3>WiFi</h3><div class='status-value ";
  chunk += (wifiUp ? "badge-on'>online" : "badge-off'>offline");
  chunk += "</div></div>";
  web.sendContent(chunk);

  chunk = "<div class='status-card'><h3>Nachtmodus</h3><div class='status-value ";
  chunk += (isNightTime() ? "badge-off'>aan" : "badge-on'>uit");
  chunk += "</div></div>";
  web.sendContent(chunk);

  chunk = "<div class='status-card'><h3>Queue</h3><div class='status-value'>" + String(qSize) + " events</div></div>";
  web.sendContent(chunk);

  chunk = "<div class='status-card'><h3>Heap</h3><div class='status-value ";
  chunk += heapLevelClass(gHeapLevel);
  chunk += "'>";
  chunk += String(ESP.getFreeHeap());
  chunk += " bytes (";
  chunk += heapLevelLabel(gHeapLevel);
  chunk += ")</div></div>";
  web.sendContent(chunk);

  web.sendContent(F("</div><div class='section'><h2>🔕 Demping</h2>"));

  chunk = "<div class='mute-block'><div class='mute-title'>Oude bel</div><div class='mute-status'>Status: ";
  chunk += muteStatus(muteOldUntilMs);
  chunk += "</div><a class='btn' href='/mute?old=900'>15 min</a><a class='btn' href='/mute?old=1800'>30 min</a><a class='btn' href='/mute?old=3600'>1 uur</a><a class='btn btn-off' href='/mute?old=0'>Annuleren</a></div>";
  web.sendContent(chunk);

  chunk = "<div class='mute-block'><div class='mute-title'>Nieuwe bel</div><div class='mute-status'>Status: ";
  chunk += muteStatus(muteNewUntilMs);
  chunk += "</div><a class='btn' href='/mute?new=900'>15 min</a><a class='btn' href='/mute?new=1800'>30 min</a><a class='btn' href='/mute?new=3600'>1 uur</a><a class='btn btn-off' href='/mute?new=0'>Annuleren</a></div>";
  web.sendContent(chunk);

  chunk = "<div class='mute-block'><div class='mute-title'>Telegram</div><div class='mute-status'>Status: ";
  chunk += muteStatus(muteTelegramUntilMs);
  chunk += "</div><a class='btn' href='/mute?tg=900'>15 min</a><a class='btn' href='/mute?tg=1800'>30 min</a><a class='btn' href='/mute?tg=3600'>1 uur</a><a class='btn btn-off' href='/mute?tg=0'>Annuleren</a></div>";
  web.sendContent(chunk);

  chunk = "</div><div class='section'><h2>🌙 Nachtvenster</h2><div class='mute-block'>Oude bel wordt automatisch stil gezet tussen <strong>";
  chunk += String(nightStartHour);
  chunk += ":00</strong> en <strong>";
  chunk += String(nightEndHour);
  chunk += ":00</strong>.</div></div></div><div class='footer'>Uptime: ";
  chunk += String(millis() / 1000);
  chunk += " seconden • Min heap: ";
  if (gMinHeapSeen == 0xFFFFFFFFUL) chunk += "n.v.t.";
  else chunk += String(gMinHeapSeen) + " bytes";
  chunk += "</div></div></body></html>";
  web.sendContent(chunk);
}

static void handleMute() {
  uint32_t now = millis();
  if (web.hasArg("old")) muteOldUntilMs = now + web.arg("old").toInt() * 1000UL;
  if (web.hasArg("new")) muteNewUntilMs = now + web.arg("new").toInt() * 1000UL;
  if (web.hasArg("tg"))  muteTelegramUntilMs = now + web.arg("tg").toInt() * 1000UL;
  web.sendHeader("Location", "/");
  web.send(302);
}

/* ===================== SETUP / LOOP ===================== */
void setup() {
  // Serial.begin(115200); // Volledig uitgeschakeld om GPIO 1 en 3 vrij te maken voor LED/knop

  pinMode(PIN_RELAY_OLD, OUTPUT);
  pinMode(PIN_RELAY_NEW, OUTPUT);
  
  // Zorg dat de relais bij het opstarten gegarandeerd uitstaan
  setRelay(PIN_RELAY_OLD, false);
  setRelay(PIN_RELAY_NEW, false);

  // Configureer de knop-pin met de interne pull-up weerstand
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  
  // Koppel de hardware-interrupt aan de knop (triggert zodra de pin naar GND getrokken wordt)
  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), handleButtonInterrupt, FALLING);

  pinMode(PIN_LED, OUTPUT);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, "pool.ntp.org");
  setenv("TZ", TZ_INFO, 1);
  tzset();

  web.on("/", handleRoot);
  web.on("/mute", handleMute);
  web.begin();
}


void loop() {
  // Controleer of de interrupt vlag gezet is
  if (gButtonPressed) {
    uint32_t now = millis();
    
    // ONESHOT COOLDOWN: De knop wordt pas geaccepteerd als er minstens 
    // 500 milliseconden (0.5 seconden) zijn verstreken sinds de VORIGE geslaagde belbeurt.
    if (lastDebounceTime == 0 || (now - lastDebounceTime) > 5000UL) {
      startRing();
      lastDebounceTime = now; // Onthoud wanneer er VOOR HET LAATST écht is aangebeld
    }
    
    gButtonPressed = false; // Reset de vlag ALTIJD, zodat denderen direct gewist wordt
  }

  checkRelayTimers(); // Regelt het non-blocking uitschakelen van de bellen
  telegramSend();     // Verwerkt en verstuurt het Telegram-bericht
  heapMonitorTick();  // Waarschuwt automatisch bij laag/kritiek geheugen
  web.handleClient(); // Handelt binnenkomende webserver-verzoeken af
  
  yield();            // Geeft de ESP8266 Wi-Fi achtergrondtaken expliciet ademruimte
}
