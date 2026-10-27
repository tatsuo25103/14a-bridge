# 14a Bridge V1.0.9

## Neu

- Zwei getrennte RS485-Segmente verhindern die bisher mögliche
  Master-Kollision zwischen PlexLog und SmartPLC:
  - PlexLog-Wechselrichterport → externer RS485-UART-Wandler → StampPLC
    **PORT.C** (`GPIO4/RX`, `GPIO5/TX`)
  - StampPLC **PWR485** → P17-Karten / Wechselrichter-IDs `2-7`
- Option **Enable PlexLog gateway** auf der Settings-Seite. Sie ist nach der
  Schema-5-Migration standardmäßig aktiv und kann gespeichert abgeschaltet
  werden.
- Separate PlexLog-Diagnose je Wechselrichter-ID sowie Zähler für Weiterleitung,
  Cache, Warteschlange, Zeitüberschreitungen, Ablehnung und maximale Latenz.

## Kommunikationslogik

- Im Normalbetrieb hat PlexLog Vorrang; Hintergrundabfragen der SmartPLC
  beginnen nur in einer erkannten PlexLog-Sendepause.
- Bei einer RSE-Änderung hat die SmartPLC Vorrang, schreibt den neuen Grenzwert
  und prüft ihn per FC03-Rücklesen. Zwischen Wechselrichtern bleibt je ein
  begrenztes PlexLog-Servicefenster.
- Erfolgreiche FC03/FC04-Leseantworten werden exakt nach ID, Funktionscode,
  Adresse und Anzahl zwischengespeichert. Schreibantworten und Modbus-Ausnahmen
  werden nie erfunden oder aus dem Cache beantwortet.
- Nur strukturell gültige Anfragen für Wechselrichter-IDs `2-7` werden zum
  P17-Bus weitergeleitet. Zähler-ID `1`, Broadcasts und nicht unterstützte
  Telegramme bleiben gesperrt.
- Die Leistungsprüfung beginnt nach 140 ms. Ist ein gültiger Rücklesewert noch
  nicht aktualisiert, erfolgt eine zweite rein lesende Prüfung, bevor ein
  weiterer Schreibversuch zulässig ist. Die Grenze von drei Schreibversuchen
  bleibt unverändert.

## Wichtige Verdrahtungsänderung

PlexLog darf nicht mehr parallel mit der SmartPLC am P17-Bus liegen. Der
PlexLog-Wechselrichterport wird ausschließlich über einen RS485-Wandler mit
automatischer Richtungsumschaltung an PORT.C angeschlossen. Der separate
PlexLog-Zählerport bleibt direkt mit dem Zähler (ID 1) verbunden. Es gibt kein
Bypass-Relais; bei Ausfall oder Abschaltung der SmartPLC meldet PlexLog die
Wechselrichterkommunikation daher als unterbrochen.

## Prüfung vor Freigabe

- PlatformIO-Produktionsbuild bestanden: RAM 27,1 %, Flash 40,6 %.
- 14/14 automatisierte Firmware-/GUI-/Kapazitätstests bestanden.
- Ein-Wechselrichter-Dauertest: 191 PlexLog-Anfragen in 175 Sekunden, ohne
  neue Downstream-Fehler, Drops oder Zeitüberschreitungen.
- Zwei vollständige, absichtlich mit PlexLog kollidierende RSE-Runden
  `100 → 60 → 30 → 0 → 100`: 10/10 Schreib-/Rückleseprüfungen und 100/100
  PlexLog-Antworten, ohne Timeout, Drop oder Downstream-Fehler.

## Hinweis zum Windows-Download

Das V1.0.9-Windows-Setup besitzt keine Authenticode-Signatur. Windows
SmartScreen kann deshalb vor einem unbekannten Herausgeber warnen. Nur vom
offiziellen GitHub Release laden und vor der Installation mit
`SHA256SUMS_V1.0.9.txt` prüfen.

> Der reale Sechs-Wechselrichter-Abnahmetest bleibt vor einer Installation mit
> sechs Geräten erforderlich. Die Software-Kapazitätsprüfung ersetzt diesen
> Standorttest nicht.
