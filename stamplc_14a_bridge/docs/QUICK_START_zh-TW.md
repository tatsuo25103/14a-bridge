# 14A Bridge 快速安裝與調適指南

> 適用：SmartPLC 韌體 `V1.0.2`、Windows USB Configurator `V1.0.2`

照順序完成以下 7 個步驟，即可把 RSE 的 `100% / 60% / 30% / 0%` 指令送到最多 6 台逆變器。

> [!CAUTION]
> RSE 若輸出 230 V，不可直接接 StampPLC。必須使用中間繼電器或隔離模組。StampPLC 數位輸入只接受 DC 5～36 V。

## 1. 接線

![系統接線](user_manual_assets/system_wiring.png)

| 來源 | StampPLC | 功能 |
|---|---|---|
| 12 V + | VIN | 主電源正極 |
| 12 V 0 V | GND | 主電源負極 |
| 12 V 0 V | COM | 數位輸入共同參考 |
| 12 V + | RSE 公共端 | 接點閉合時送出 +12 V |
| K1 / K2 / K3 / K4 | IN1 / IN2 / IN3 / IN4 | 100% / 60% / 30% / 0% |

RS485：`A → A/D+`、`B → B/D−`。PWR485 的 `VIN` 不要接逆變器。

**成功：** StampPLC 開機並顯示 `V1.0.2`。

## 2. USB 連線

1. 接上 USB Type-C 資料線。
2. GUI 按 **Scan**。
3. 選擇新出現的 COM。
4. 按 **Connect**。
5. 按 **Read SmartPLC settings**。

**成功：** 顯示 `CONNECTED COMx`，韌體版本為 `V1.0.2`。

## 3. 掃描逆變器

1. 逆變器上電並設定不同的 Modbus ID。
2. 打開 **COMMISSIONING**。
3. 按 **Scan all IDs**。

**成功：** 實際存在的 ID 顯示 `FOUND`。這是 FC03 唯讀掃描，不會寫逆變器。

## 4. 設定功率

1. 回到 **SETTINGS**。
2. 勾選實際要控制的 ID。
3. 10 kW 填 `10000`；15 kW 填 `15000`。
4. 保持 `19200 baud`、`0x04E5`，除非逆變器文件另有規定。
5. 按 **Save inverter settings**。

**成功：** 顯示 `OK`、`PENDING` 或 `LIMITED`。若 15 kW 逆變器拒絕 20 kW，GUI 會自動改回 15,000 W。

## 5. 測試 RSE

1. 確認 Mode 為 `DRY-RUN`。
2. 依序閉合 K1、K2、K3、K4；每次只能一個接點閉合。
3. 依序按 `100% test`、`60% test`、`30% test`、`0% test`。

| 額定功率 | 100% | 60% | 30% | 0% |
|---:|---:|---:|---:|---:|
| 15 kW | 15.0 kW | 9.0 kW | 4.5 kW | 0 kW |
| 10 kW | 10.0 kW | 6.0 kW | 3.0 kW | 0 kW |

**成功：** RSE 與計算值都正確，沒有 `INVALID`。

## 6. 啟用 LIVE

1. 先讓 RSE 停在 `100%`。
2. 按 **Enable LIVE** 並確認。
3. 等 GUI 重新讀取實體 RSE。
4. 測試 `100% → 60% → 30% → 0% → 100%`。

**成功：** Mode 顯示 `LIVE`；虛線是 RSE 目標，液位與中央 kW 是逆變器回讀。

> [!NOTE]
> RSE 沒有變化時不會持續寫入逆變器。背景只做 FC03 唯讀健康檢查；寫入失敗最多重試 3 次。

## 7. Wi-Fi 與 OTA（選配）

1. **Refresh PC Wi-Fi** → 選 2.4 GHz SSID。
2. 輸入密碼 → **Save connect**。
3. 需要自動更新時勾選 **Enable automatic OTA**。

**成功：** Wi-Fi 顯示 `Connected`，自動更新顯示 `AUTO OTA: ON`。

## 出問題時

| 問題 | 處理順序 |
|---|---|
| 找不到 COM | 換 USB 資料線 → 換 USB 埠 → 重開 GUI |
| 全部 NO_RESPONSE | 逆變器上電 → baud → ID → A/B |
| 偶爾 RETRY | 等下一輪 → 檢查線型、屏蔽、終端與第二主站 |
| ERROR | A/B → ID → baud → 0x04E5 → 逆變器供電 |
| INVALID 0x00 | RSE 公共端 +12 V、COM 0 V、確認一個接點閉合 |
| 多接點 INVALID | K1～K4 任何時候只能一個閉合 |
| AUTO OTA OFF | Connect → Read SmartPLC settings → 再勾選 |

## 交機確認

- 只勾選實際存在的 ID。
- 每台最大功率正確。
- 四種 RSE 輸入正確。
- LIVE 寫入與 FC03 回讀成功。
- 能從限制狀態恢復到 100%。
- RS485 斷線與恢復可正確顯示。
