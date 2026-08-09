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

/* Forward declarations */
static bool isTelegramMuted();
static bool getEpoch(uint32_t &outEpoch);

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
static void telegramSend() {
  if (!gTelegramEnabled || isTelegramMuted() || qSize == 0) return;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(3000); // Voorkomt oneindig hangen bij netwerkproblemen

  if (!client.connect("api.telegram.org", 443)) {
    // Bij falen van de verbinding bewaren we de queue voor de volgende poging
    return; 
  }

  String msg = "🔔 Deurbel\nAantal: " + String(qSize) + "\nLaatste: ";
  msg += formatTime(queue[(qTail + 31) % 32].epoch);

  String body = "chat_id=" + String(TG_CHAT_ID) + "&text=" + msg;
  client.print(
    "POST /bot" + String(TG_BOT_TOKEN) + "/sendMessage HTTP/1.1\r\n"
    "Host: api.telegram.org\r\n"
    "Content-Type: application/x-www-form-urlencoded\r\n"
    "Content-Length: " + String(body.length()) + "\r\n\r\n" + body
  );

  // Geef de netwerk-stack kort de tijd om de data te verwerken zonder de boel te blokkeren
  unsigned long startWait = millis();
  while (client.connected() && millis() - startWait < 500) {
    if (client.available()) break;
    yield(); // Voed de ingebouwde Watchdog Timer (WDT) om resets te voorkomen
  }

  queueClear();
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
  String page = "<html><body><h1>🔔 Deurbel</h1>";

  page += "<p>Oude bel: " + muteStatus(muteOldUntilMs) + "</p>";
  page += "<a href='/mute?old=900'>15 min</a> ";
  page += "<a href='/mute?old=3600'>1 uur</a><br>";

  page += "<p>Nieuwe bel: " + muteStatus(muteNewUntilMs) + "</p>";
  page += "<a href='/mute?new=900'>15 min</a> ";
  page += "<a href='/mute?new=3600'>1 uur</a><br>";

  page += "<p>Telegram: " + muteStatus(muteTelegramUntilMs) + "</p>";
  page += "<a href='/mute?tg=900'>15 min</a> ";
  page += "<a href='/mute?tg=3600'>1 uur</a><br>";

  page += "<p>Nachtmodus: ";
  page += String(nightStartHour) + ":00 - " + String(nightEndHour) + ":00</p>";

  page += "</body></html>";
  web.send(200, "text/html", page);
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
  // Evalueer de interrupt-vlag en pas software-debouncing toe
  if (gButtonPressed) {
    uint32_t now = millis();
    if ((now - lastDebounceTime) > DEBOUNCE_DELAY_MS) {
      startRing();
      lastDebounceTime = now;
    }
    gButtonPressed = false; // Reset de vlag voor de volgende druk
  }

  checkRelayTimers(); // Regelt het non-blocking uitschakelen van de bellen
  telegramSend();     // Verwerkt en verstuurt de Telegram-berichten indien nodig
  web.handleClient(); // Handelt binnenkomende webserver-verzoeken af
  
  yield();            // Geeft de ESP8266 Wi-Fi achtergrondtaken expliciet ademruimte
}
