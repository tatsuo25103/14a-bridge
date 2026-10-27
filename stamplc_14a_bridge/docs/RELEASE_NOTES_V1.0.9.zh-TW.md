# 14a Bridge V1.0.9

## 新增功能

- 使用兩段獨立的 RS485，避免 PlexLog 與 SmartPLC 同時成為 P17 Bus
  Master 而發生碰撞：
  - PlexLog 逆變器通訊埠 → 自動方向 RS485/UART 模組 → StampPLC
    **PORT.C**（`GPIO4/RX`、`GPIO5/TX`）
  - StampPLC **PWR485** → P17 Card／逆變器 ID `2-7`
- Settings 新增 **Enable PlexLog gateway**。Schema 5 遷移後預設啟用，也可
  關閉並保存。
- 每台逆變器有獨立 PlexLog 診斷，並記錄轉送、快取、佇列、逾時、拒絕及
  最大延遲。

## 通訊邏輯

- 平時 PlexLog 優先；SmartPLC 只有在偵測到 PlexLog 空檔時才進行背景健康
  讀取。
- RSE 改變時 SmartPLC 優先，先寫入並以 FC03 回讀確認新限制；多台逆變器
  之間仍保留一個有上限的 PlexLog 服務窗口。
- 只有成功的 FC03／FC04，而且 ID、功能碼、位址及數量完全相同時才可使用
  快取。寫入回覆與 Modbus Exception 絕不由快取假造。
- 只有逆變器 ID `2-7` 的有效請求能進入 P17 Bus；電表 ID `1`、Broadcast
  與不支援的封包會被拒絕。
- 功率寫入後 140 ms 開始回讀。如果回讀仍是舊值，只會再做一次唯讀確認，
  不會立刻增加一次寫入；最多三次寫入的限制維持不變。

## 必須修改的接線

PlexLog 不可再與 SmartPLC 並聯在同一條 P17 Bus。PlexLog 的逆變器通訊埠
只能經由自動方向 RS485 模組接到 PORT.C；PlexLog 的獨立電表通訊埠維持直接
連接電表（ID 1）。系統不使用旁路 Relay；SmartPLC 斷電或關閉 gateway 時，
PlexLog 會如實顯示逆變器通訊中斷。

## 發布前驗證

- PlatformIO 正式韌體建置通過：RAM 27.1%、Flash 40.6%。
- 14/14 韌體、GUI 與容量自動測試通過。
- 單台逆變器監控：175 秒內 191 筆 PlexLog 請求，沒有新增下游失敗、丟包
  或逾時。
- 刻意撞擊 PlexLog 通訊窗口，完成兩輪
  `100 → 60 → 30 → 0 → 100`：10/10 次逆變器寫入及回讀成功，100/100 筆
  PlexLog 請求都有回覆，逾時、丟包、下游失敗皆為 0。

## Windows 下載提醒

V1.0.9 Windows 安裝檔未使用 Authenticode 簽章，因此 Windows SmartScreen
可能顯示未知發行者警告。請只從官方 GitHub Release 下載，安裝前使用
`SHA256SUMS_V1.0.9.txt` 核對檔案。

> 六台逆變器的實體驗收仍必須在六台設備案場執行；軟體容量推算不能取代
> 實際現場測試。
