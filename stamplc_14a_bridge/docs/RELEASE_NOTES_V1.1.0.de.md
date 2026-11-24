# 14a Bridge V1.1.0

## Neu

- **Winter-/Notbetrieb je Wechselrichter:** Die SmartPLC steuert Register
  `0x0007` Bit 14 und Register `0x011F` mit sicherer Reihenfolge und
  Rückleseprüfung.
- **Taste A fünf Sekunden:** Normalbetrieb anfordern.
- **Taste C fünf Sekunden:** Winterbetrieb anfordern.
- **A+C fünf Sekunden:** unverändert lokale Rückkehr zur vorherigen,
  verifizierten Firmware. Taste B bricht jeden Haltevorgang ab.
- LCD- und Windows-Anzeige mit animiertem Schneemann für jeden Wechselrichter,
  auf dem der Winterbetrieb tatsächlich bestätigt wurde.
- Sichtbarer Fortschritt während Lesen, Schreiben und Rücklesen. Die Anzeige
  trennt jetzt eindeutig `INVERTER 1/2` von `STEP 1/6`.

## Steuerfolge

| Ziel | Sichere Reihenfolge pro Wechselrichter |
|---|---|
| **Winter** | Register 7 und 287 lesen → `0x011F = 1000` (100,0 A) schreiben und prüfen → Register 7 Bit 14 aktivieren und prüfen |
| **Normal** | Register 7 und 287 lesen → Register 7 Bit 14 deaktivieren und prüfen → `0x011F = 30` (3,0 A) schreiben und prüfen |

Bereits korrekte Werte werden nur gelesen und **nicht erneut geschrieben**.
Fällt bei mehreren Wechselrichtern nur ein Gerät aus, behalten erfolgreich
bestätigte Geräte ihren Zustand und ihre Anzeige. Dieselbe Taste wiederholen,
um nur die noch nicht bestätigten Geräte erneut zu prüfen.

## Zuverlässigkeit und Sicherheit

- Register 7 wird nur geändert, wenn der gelesene Zustand vom Ziel abweicht.
- Jede Änderung wird begrenzt wiederholt und anschließend zurückgelesen.
- Ein fehlgeschlagener Winter-Wechselrichter wird, soweit erreichbar, einzeln
  in den sicheren Normalzustand zurückgeführt. Erfolgreiche Geräte werden
  dabei nicht zurückgesetzt.
- Gespeicherte Wechselrichter-, PV-, RS485-, Wi-Fi-, RSE- und OTA-Einstellungen
  bleiben beim Update erhalten.
- Lange Tastengesten werden erst freigegeben, nachdem A, B und C nach dem
  Einschalten einmal losgelassen erkannt wurden.

## Wichtiger USB-Hinweis

> [!CAUTION]
> SolarPower oder andere Programme zur automatischen COM-Port-Suche vollständig
> schließen, bevor der 14a-Bridge-Konfigurator verwendet wird. Öffnet ein
> Fremdprogramm den SmartPLC-COM-Port und schaltet DTR/RTS, kann die SmartPLC
> zurückgesetzt werden. SmartPLC und SolarPower dürfen nicht denselben
> USB-COM-Port verwenden.

## Vor Freigabe geprüft

- PlatformIO-Produktionsbuild: RAM 27,2 %, Flash 41,0 %.
- Windows-GUI Selbsttest bestanden.
- V1.1.0 per USB auf reale SmartPLC geschrieben und Identität zurückgelesen.
- Gespeicherte Wechselrichterkonfiguration nach dem Flash erhalten.
- Normalbetrieb an 10-kW-Gerät bestätigt: AC Charge deaktiviert,
  `0x011F = 30`.
- Winterbetrieb an 10-kW-Gerät bestätigt: AC Charge aktiviert,
  `0x011F = 1000`.

Das Windows-Setup ist nicht Authenticode-signiert. Nur vom offiziellen
GitHub-Release laden und `SHA256SUMS_V1.1.0.txt` prüfen.
