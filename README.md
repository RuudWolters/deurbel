# ESP01S Deurbel Controller

Slimme deurbelcontroller voor ESP-01S (ESP8266) met relaissturing, webinterface, Telegram-notificaties, dempstanden, nachtmodus per onderdeel en ingebouwde debuglog.

## Wat dit project doet

Deze firmware bestuurt een oude en nieuwe bel via twee relais en reageert op een fysieke drukknop via interrupt.
De webinterface geeft live statusinformatie en laat je instellingen wijzigen zonder herstart.

Belangrijkste eigenschappen:
- Non-blocking timing op basis van millis (geen blokkerende delays in de hoofdlogica)
- Betrouwbare knopdetectie met interrupt + cooldown
- Aansturing van 2 relais (oude bel en nieuwe bel)
- Telegram-berichten met wachtrij en rate limiting
- Tijdelijk dempen per onderdeel (30 min, 1 uur, 2 uur, 4 uur)
- Nachtmodus per onderdeel met eigen tijdvenster
- Volledig aan/uit per onderdeel
- Debugsectie met actielog op de webpagina
- Debuglog exporteren als .txt via webinterface
- Debuglog direct doorsturen naar Telegram
- Geheugenbewaking met waarschuwingen

## Bestandsstructuur

- Hoofdsketch: [20260808_arduino_deurbel_gemini.ino](20260808_arduino_deurbel_gemini.ino)
- Lokale credentials (niet committen): [wifi_secrets.h](wifi_secrets.h)
- Voorbeeld credentials: [wifi_secrets.example.h](wifi_secrets.example.h)
- Git ignore-regels: [.gitignore](.gitignore)

## Veilig omgaan met credentials

Credentials staan buiten de sketch in een apart lokaal bestand.

Stappen:
1. Kopieer [wifi_secrets.example.h](wifi_secrets.example.h) naar wifi_secrets.h
2. Vul je eigen WiFi en Telegram gegevens in
3. Controleer dat wifi_secrets.h in [.gitignore](.gitignore) staat

## Hardware

Doelplatform:
- ESP-01S (ESP8266)

Standaard pinindeling in de sketch:
- GPIO0: Relais oude bel
- GPIO2: Relais nieuwe bel
- GPIO3 (RX): Drukknop (INPUT_PULLUP)
- GPIO1 (TX): LED

Let op:
- GPIO0 en GPIO2 zijn boot-pinnen op ESP-01S
- Gebruik geschikte relaisdriver en voeding

## Build en flash

Gebruik Arduino IDE of PlatformIO met ESP8266 board support.

Algemene Arduino IDE stappen:
1. Installeer ESP8266 board package
2. Open [20260808_arduino_deurbel_gemini.ino](20260808_arduino_deurbel_gemini.ino)
3. Controleer wifi_secrets.h
4. Selecteer het juiste board en poort
5. Compileer en upload

## Webinterface functies

Beschikbare bediening:
- Onderdelen volledig aan/uit
- Tijdelijk dempen per onderdeel
- Nachtmodus per onderdeel (aan/uit + tijden)
- Debugmodus en actielog
- Debuglog exporteren (.txt)
- Debuglog doorsturen naar Telegram

Debuggedrag:
- Basislog is altijd actief
- Debug aan geeft extra detailregels

## Projectdoel

Dit project is gemaakt voor een praktische, stabiele en goed beheersbare deurbeloplossing met focus op:
- Betrouwbaarheid
- Uitlegbaar gedrag
- Eenvoudige bediening
- Veilig delen op GitHub zonder secrets

## Verbeterideeën

Mogelijke volgende stappen:
- Instellingen persistent opslaan na reboot
- OTA updates
- Extra notificatiekanalen
