# V1.0.7

**Deutsch** · [English](RELEASE_NOTES_V1.0.7.md) ·
[繁體中文](RELEASE_NOTES_V1.0.7.zh-TW.md)

## Gehärtetes OTA-Rollback

- Der Produktionsbuild verlangt aktivierte ESP32-Bootloader-Rollback-Unterstützung.
- Fehler bei Download, Signatur, Größe, SHA-256, Sicherheitsprüfung,
  Partitionsschreiben oder Aktivierung lassen die laufende Firmware aktiv.
- Während der 15-sekündigen Erststartprüfung bleibt der gesamte
  Wechselrichter-RS485-Zugriff gesperrt.
- Neustart vor der Validierung, fehlerhafter Selbsttest oder fehlgeschlagene
  Aktivierung führen automatisch zur vorherigen gültigen Anwendung.
- Wechselrichter-, PV-, RS485-, RSE-, WLAN- und OTA-Einstellungen bleiben in
  NVS erhalten.

## Lokale Wiederherstellung

- Nach erfolgreichem OTA wird die vorherige Partition mit ihrer unveränderlichen
  ELF-SHA-256 gespeichert. Veraltete oder überschriebene Einträge werden
  abgelehnt.
- A+C fünf Sekunden halten; B bricht ab und sperrt bis zur vollständigen
  Tastenfreigabe.
- Rückkehr ist nur bei stabilem physischem 100 %, LIVE, freiem Modbus und allen
  bestätigten Wechselrichtern zulässig. Unsichere Anfragen starten nicht neu.
- Nach erfolgreicher Rückkehr wird AUTO OTA deaktiviert.
- Das LCD zeigt einen kreisförmigen Countdown mit Segmenten, Scannerpunkt und
  großer Sekundenanzeige.

## Weitere Korrekturen

- Kein Update wird als `AVAILABLE=-` gemeldet; Signaturdiagnose bleibt erhalten.
- Serielle Prüfwerkzeuge öffnen COM ohne DTR/RTS-bedingten ESP32-Neustart.
- Konfigurationen bestehender Installationen und das Standardprofil
  Strict 4-contact bleiben unverändert.

## Validierung

RSE 100/60/30/0, ID3 mit 10-kW-Wechselrichter, signiertes OTA,
Fehler-Injektion, automatisches und lokales Rollback, B-Abbruch, fehlende
Sicherung, Windows-Installer und GUI-Selbsttests wurden auf echter Hardware
geprüft.
