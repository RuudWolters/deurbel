# ESP01S Doorbell Controller / Deurbel Controller

EN: Smart ESP-01S (ESP8266) doorbell controller with relay control, web interface, Telegram notifications, per-component mute and night mode, built-in debug log, and persistent settings.

NL: Slimme ESP-01S (ESP8266) deurbelcontroller met relaissturing, webinterface, Telegram meldingen, demping en nachtmodus per onderdeel, ingebouwde debuglog en persistente instellingen.

---

## English

### What this project does

This firmware controls both a legacy doorbell and new wireless doorbells through separate relays. A physical button is handled using an interrupt for reliable triggering, while the web interface provides live status and runtime control.

Main features:
- Non-blocking timing using millis
- Reliable button handling with interrupt and retrigger cooldown
- Two relay outputs (legacy bell and new bells)
- Telegram notifications with queueing and rate limiting
- Temporary mute per component (30 min, 1 hour, 2 hours, 4 hours)
- Night mode per component with configurable time windows
- Full on or off control per component
- Debug section with action log in the web UI
- Debug log export as txt from the web UI
- Send debug log to Telegram from the web UI
- Persistent settings stored in flash across reboot
- Heap monitoring with warning and critical alerts

### File structure

- Main sketch: [20260808_arduino_deurbel_gemini.ino](20260808_arduino_deurbel_gemini.ino)
- English sketch: [20260808_arduino_doorbell_gemini_en.ino](20260808_arduino_doorbell_gemini_en.ino)
- Local credentials (do not commit): [wifi_secrets.h](wifi_secrets.h)
- Credential template: [wifi_secrets.example.h](wifi_secrets.example.h)
- Git ignore rules: [.gitignore](.gitignore)

### Credentials and security

Credentials are stored outside the main sketch in a local header file.

Steps:
1. Copy [wifi_secrets.example.h](wifi_secrets.example.h) to wifi_secrets.h
2. Fill in your WiFi and Telegram settings
3. Confirm that wifi_secrets.h is listed in [.gitignore](.gitignore)

### Hardware used

Hardware list from this build:
- ESP-01S ESP8266 serial module
- 12V DC to 5V DC converter
- RC absorption or snubber circuit module for relay contact protection
- ESP8266 ESP-01 or ESP-01S 1-channel or 2-channel relay module
- CACAZI wireless waterproof doorbell set

How it is used:
- Legacy bell is switched through a relay with the snubber network for contact protection.
- New bells are triggered by connecting the CACAZI push-button circuit through one relay output.
- No snubber network is required for the new bell push-button circuit in this setup.

Default pin mapping in the sketch:
- GPIO0: legacy bell relay
- GPIO2: new bells relay
- GPIO3 (RX): physical button input (INPUT_PULLUP)
- GPIO1 (TX): status LED

Important notes:
- GPIO0 and GPIO2 are ESP-01S boot pins.
- Use a suitable relay driver and stable power supply.

### Build and flash

Use Arduino IDE or PlatformIO with ESP8266 board support.

Arduino IDE steps:
1. Install the ESP8266 board package.
2. Open [20260808_arduino_deurbel_gemini.ino](20260808_arduino_deurbel_gemini.ino).
3. Verify wifi_secrets.h values.
4. Select the correct board and serial port.
5. Compile and upload.

### Web interface

Available controls:
- Full on or off per component
- Temporary mute per component
- Night mode per component (on or off plus custom hours)
- Debug mode and action log
- Debug log export as txt
- Send debug log to Telegram

Debug behavior:
- Base logging is always active.
- Enabling debug adds extra verbose lines.

Persistent settings:
- Automatically stored: component on or off states, night mode on or off states, night hours, and debug mode on or off.
- Temporary by design: mute duration timers (30 min, 1 hour, 2 hours, 4 hours).

### Project goal

This project focuses on a practical, stable, and manageable doorbell controller with emphasis on:
- Reliability
- Predictable behavior
- Easy operation
- Safe sharing on GitHub without exposing secrets

### Possible next improvements

- OTA updates
- Additional notification channels

---

## Nederlands

### Wat dit project doet

Deze firmware stuurt zowel een oude deurbel als nieuwe draadloze bellen aan via aparte relais. Een fysieke drukknop wordt via interrupt verwerkt voor betrouwbare detectie, terwijl de webinterface live status en bediening geeft.

Belangrijkste functies:
- Non-blocking timing met millis
- Betrouwbare knopafhandeling met interrupt en retrigger cooldown
- Twee relaisuitgangen (oude bel en nieuwe bellen)
- Telegram meldingen met wachtrij en rate limiting
- Tijdelijke demping per onderdeel (30 min, 1 uur, 2 uur, 4 uur)
- Nachtmodus per onderdeel met instelbaar tijdvenster
- Volledig aan of uit per onderdeel
- Debugsectie met actielog in de webinterface
- Debuglog export als txt via de webinterface
- Debuglog doorsturen naar Telegram via de webinterface
- Persistente instellingen in flash na reboot
- Heap-bewaking met waarschuwing en kritieke meldingen

### Bestandsstructuur

- Hoofdsketch: [20260808_arduino_deurbel_gemini.ino](20260808_arduino_deurbel_gemini.ino)
- Engelse sketch: [20260808_arduino_doorbell_gemini_en.ino](20260808_arduino_doorbell_gemini_en.ino)
- Lokale credentials (niet committen): [wifi_secrets.h](wifi_secrets.h)
- Voorbeeldbestand credentials: [wifi_secrets.example.h](wifi_secrets.example.h)
- Git ignore regels: [.gitignore](.gitignore)

### Credentials en veiligheid

Credentials staan buiten de hoofdsketch in een lokaal headerbestand.

Stappen:
1. Kopieer [wifi_secrets.example.h](wifi_secrets.example.h) naar wifi_secrets.h
2. Vul je WiFi- en Telegram-gegevens in
3. Controleer dat wifi_secrets.h in [.gitignore](.gitignore) staat

### Gebruikte hardware

Hardwarelijst van deze build:
- ESP-01S ESP8266 seriele module
- 12V DC naar 5V DC omzetter
- RC absorptie of snubber circuit module voor relaiscontactbescherming
- ESP8266 ESP-01 of ESP-01S 1-kanaals of 2-kanaals relaismodule
- CACAZI draadloze waterdichte deurbel set

Gebruik in deze opstelling:
- De oude bel wordt geschakeld via een relais met snubbernetwerk voor contactbescherming.
- De nieuwe bellen worden getriggerd door het CACAZI drukknopcircuit via een relaisuitgang te verbinden.
- Voor het nieuwe bel-drukknopcircuit is in deze opstelling geen snubbernetwerk nodig.

Standaard pinmapping in de sketch:
- GPIO0: relais oude bel
- GPIO2: relais nieuwe bellen
- GPIO3 (RX): fysieke knop ingang (INPUT_PULLUP)
- GPIO1 (TX): status led

Belangrijke opmerkingen:
- GPIO0 en GPIO2 zijn boot-pinnen van de ESP-01S.
- Gebruik een geschikte relaisdriver en stabiele voeding.

### Build en flash

Gebruik Arduino IDE of PlatformIO met ESP8266 board support.

Arduino IDE stappen:
1. Installeer het ESP8266 board package.
2. Open [20260808_arduino_deurbel_gemini.ino](20260808_arduino_deurbel_gemini.ino).
3. Controleer de waarden in wifi_secrets.h.
4. Selecteer het juiste board en de juiste seriele poort.
5. Compileer en upload.

### Webinterface

Beschikbare bediening:
- Volledig aan of uit per onderdeel
- Tijdelijke demping per onderdeel
- Nachtmodus per onderdeel (aan of uit plus aangepaste uren)
- Debugmodus en actielog
- Debuglog export als txt
- Debuglog doorsturen naar Telegram

Debuggedrag:
- Basislogging is altijd actief.
- Bij debug aan komen extra detailregels erbij.

Persistente instellingen:
- Automatisch opgeslagen: onderdeel aan of uit, nachtmodus aan of uit, nachturen en debugmodus aan of uit.
- Bewust tijdelijk: demptimers (30 min, 1 uur, 2 uur, 4 uur).

### Projectdoel

Dit project richt zich op een praktische, stabiele en goed beheersbare deurbelcontroller met focus op:
- Betrouwbaarheid
- Voorspelbaar gedrag
- Eenvoudige bediening
- Veilig delen op GitHub zonder secrets

### Mogelijke volgende verbeteringen

- OTA updates
- Extra notificatiekanalen
