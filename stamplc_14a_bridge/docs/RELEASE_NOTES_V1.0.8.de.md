# 14a Bridge V1.0.8

## Neu

- Vier unabhängig editierbare RSE-Leistungswerte für 100 %, 60 %, 30 % und
  0 %. Die Startwerte werden aus der installierten PV-Leistung berechnet und
  auf die bestätigte Wechselrichterleistung begrenzt; freigegebene Rundungs-
  oder Standortwerte können anschließend manuell eingetragen werden.
- Separate Option **Feedin Enable** für jede RSE-Stufe.
- Persistentes Schema 4. Vorhandene Schema-1/2/3-Konfigurationen werden unter
  Erhalt der bestehenden Einstellungen automatisch migriert.

## Geändert und korrigiert

- Jeder erforderliche Schreibzugriff auf P17-Register `0x04E5` wird per FC03
  zurückgelesen. Die Prüfung endet nach höchstens drei Versuchen.
- Nach drei fehlgeschlagenen Leistungsprüfungen sendet die Steuerung den
  P17-Befehl zur Einspeisesperre (`0xDFFF`) an Register `0x0007`, Bit 13, und
  prüft anschließend den Bitzustand.
- Im Normalbetrieb wird Register `0x0007` zuerst gelesen und nur geschrieben,
  wenn sein bestätigter Zustand von der gewünschten Feedin-Enable-Einstellung
  abweicht. Ein nicht lesbarer Zustand bleibt unberührt; ausgenommen ist die
  begrenzte Sicherheitsabschaltung nach fehlgeschlagener Leistungsprüfung.
- Bei deaktiviertem Feedin Enable zeigen StampPLC und GUI effektiv 0 W an,
  während `0x04E5` weiterhin mit dem konfigurierten Wert programmiert wird.
- Die periodische Zustandsprüfung liest Leistung und Einspeisefreigabe, ohne
  unveränderte Werte ständig neu zu schreiben.

> **Betriebswarnung:** Eine Änderung von Register `0x0007`, Bit 13 kann die
> Netzeinspeisung des Wechselrichters stoppen oder neu starten. Jede RSE-Stufe
> ist beaufsichtigt und mit den freigegebenen Standortwerten in Betrieb zu
> nehmen.

## Prüfung vor Freigabe

- PlatformIO-Produktionsbuild bestanden.
- OTA-Signatur, Dateigröße und SHA-256 erfolgreich geprüft.
- Interne Logik- und UI-Selbsttests der Windows-GUI bestanden.
- Physischer DI-Test der StampPLC V1.0.8 für IN1–IN4 einschließlich Rückkehr
  zum offenen Zustand bestanden.
- RS485-FC03-Test zu Wechselrichter-ID2: 20/20 erfolgreiche Lesevorgänge bei
  19200 Baud, ohne Timeout oder CRC-Fehler.
