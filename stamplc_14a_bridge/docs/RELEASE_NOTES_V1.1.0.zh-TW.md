# 14a Bridge V1.1.0

[Deutsch（預設）](https://github.com/tatsuo25103/14a-bridge/blob/main/stamplc_14a_bridge/docs/RELEASE_NOTES_V1.1.0.de.md) · [English](https://github.com/tatsuo25103/14a-bridge/blob/main/stamplc_14a_bridge/docs/RELEASE_NOTES_V1.1.0.md) · **繁體中文**

## 新增功能

- **每台逆變器獨立的冬季／緊急模式：**SmartPLC 依安全順序控制 Register
  `0x0007` Bit 14 與 Register `0x011F`，每次修改都會讀回確認。
- **長按 A 五秒：**要求切換至一般模式。
- **長按 C 五秒：**要求切換至冬季模式。
- **長按 A+C 五秒：**維持原本功能，回復上一個已驗證韌體；B 鍵會取消任何
  長按操作。
- SmartPLC 螢幕與 Windows 上位機新增雪人動畫；只有已確認成功進入冬季模式
  的逆變器會顯示雪人。
- 寫入期間顯示讀取、寫入與回讀進度，並明確區分
  `INVERTER 1/2` 與 `STEP 1/6`。

## 控制順序

| 目標模式 | 每台逆變器的安全順序 |
|---|---|
| **冬季模式** | 讀取 Register 7、287 → 寫入並確認 `0x011F = 1000`（100.0 A）→ 啟用 Register 7 Bit 14 並確認 |
| **一般模式** | 讀取 Register 7、287 → 停用 Register 7 Bit 14 並確認 → 寫入並確認 `0x011F = 30`（3.0 A） |

若讀取結果已經符合目標，SmartPLC 只確認而**不重複寫入**。多台逆變器中若
只有一台失敗，其他成功設備會保留正確狀態與雪人；再次長按同一模式按鍵即可
重試尚未確認的設備。

## 穩定性與安全

- 只有 Register 7 讀值與目標不同時才修改 Register 7。
- 所有修改都有次數上限，且必須讀回確認。
- 進入冬季模式失敗時，只針對該台仍可通訊的逆變器恢復安全的一般模式，
  不會重寫其他已成功的設備。
- 更新後保留逆變器、PV、RS485、Wi-Fi、RSE Profile 與 OTA 設定。
- SmartPLC 開機後必須先確認 A、B、C 三鍵都已放開，才會啟用長按功能。

## USB 重要注意事項

> [!CAUTION]
> 使用 14a Bridge 上位機前，必須完整關閉 SolarPower 及其他自動掃描 COM Port
> 的程式。第三方程式開啟 SmartPLC COM Port 並切換 DTR／RTS 時，可能造成
> SmartPLC 重置。SolarPower 與 SmartPLC 不可共用同一個 USB COM Port。

## 發布前驗證

- PlatformIO 正式版建置通過：RAM 27.2%、Flash 41.0%。
- Windows 上位機自我測試通過。
- V1.1.0 已透過 USB 寫入實體 SmartPLC，並成功讀回版本。
- 寫入後原有逆變器設定仍保留。
- 10 kW 逆變器一般模式驗證：AC Charge 停用、`0x011F = 30`。
- 10 kW 逆變器冬季模式驗證：AC Charge 啟用、`0x011F = 1000`。

Windows 安裝檔沒有 Authenticode 簽章。請只從官方 GitHub Release 下載，
並核對 `SHA256SUMS_V1.1.0.txt`。
