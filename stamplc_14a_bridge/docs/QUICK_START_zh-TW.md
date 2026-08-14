# 14A Bridge 快速安裝與調適指南

> 適用：SmartPLC 韌體 `V1.0.6`、Windows USB Configurator `V1.0.6`

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

**成功：** StampPLC 開機並顯示 `V1.0.6`。

## 2. USB 連線

1. 接上 USB Type-C 資料線。
2. GUI 按 **Scan**。
3. 選擇新出現的 COM。
4. 按 **Connect**。
5. 按 **Read SmartPLC settings**。

**成功：** GUI 先顯示偵測到的 `[SMARTPLC] COMx V1.0.6`，連線後顯示 `CONNECTED COMx`。

## 3. 掃描逆變器

1. 逆變器上電並設定不同的 Modbus ID。
2. 打開 **COMMISSIONING**。
3. 按 **Scan all IDs**。

**成功：** 實際存在的 ID 顯示 `FOUND`。這是 FC03 唯讀掃描，不會寫逆變器。

## 4. 設定功率

1. 回到 **SETTINGS**。
2. 勾選實際要控制的 ID。
3. 填寫太陽能板總裝機功率，不是逆變器銘牌功率。例如15 kW逆變器連接18 kWp太陽能板時填 `18000`。
4. 保持 `19200 baud`、`0x04E5`，除非逆變器文件另有規定。
5. 依電網營運商的書面要求選擇 **RSE profile**，不可用試誤方式猜測。
6. 按 **Save inverter settings**。

| RSE profile | 接點規則 |
|---|---|
| Strict 4-contact (legacy) | DI1/DI2/DI3/DI4 分別為 100/60/30/0%；無接點或多接點為無效 |
| Westnetz 4-contact | K1 優先釋放為 100%；沒有 K1 時由 K2/K3/K4 中最嚴格者生效 |
| EWE 4-contact (hold last) | 無接點或多接點時保持最後一次有效指令 |
| VDE FNN / Netze BW 3-contact | DI2/DI3/DI4 分別為 60/30/0%；無接點為 100%；DI1 不使用 |

**成功：** 顯示 `OK`、`PENDING` 或 `PV > INV`。`PENDING` 表示設定已保存，但該 ID 暫不參與控制；必須在逆變器在線、LIVE 已啟用且實體 RSE 為 100% 時完成驗證。`PV > INV` 是允許的黃框警告：PV裝機功率會保留，只有實際寫入值受逆變器上限限制。18 kWp PV搭配15 kW逆變器時，100/60/30/0%分別為15/10.8/5.4/0 kW。

## 5. 測試 RSE

1. 將實體 RSE 設為 `100%`，再按 **Enable LIVE**。
2. 依所選 RSE profile 與電網營運商的真值表操作接點。
3. 依序按 `100% test`、`60% test`、`30% test`、`0% test`。Mode 會變成黃色 `TEST`，五分鐘後自動回到實體 RSE。

| PV裝機功率／逆變器上限 | 100% | 60% | 30% | 0% |
|---:|---:|---:|---:|---:|
| 15 kW | 15.0 kW | 9.0 kW | 4.5 kW | 0 kW |
| 10 kW | 10.0 kW | 6.0 kW | 3.0 kW | 0 kW |
| 18 kWp／15 kW | 15.0 kW | 10.8 kW | 5.4 kW | 0 kW |

**成功：** RSE 與計算值都正確，沒有 `INVALID`。

LIVE 模式下，GUI 測試只能設定成比實體 RSE 更嚴格或相同的值，不能繞過實體降載指令；測試最長五分鐘後會自動回到實體 LIVE。

## 6. 啟用 LIVE

1. 先讓 RSE 停在 `100%`。
2. 按 **Enable LIVE** 並確認。
3. 等 GUI 清除暫時的 `TEST` 值並重新讀取實體 RSE；也可以用此按鈕在五分鐘到期前結束測試。
4. 測試 `100% → 60% → 30% → 0% → 100%`。

**成功：** Mode 顯示 `LIVE`；虛線是 RSE 目標，液位與中央 kW 是逆變器回讀。

> [!NOTE]
> RSE 沒有變化時不會持續寫入逆變器。背景只做 FC03 唯讀健康檢查；寫入失敗最多重試 3 次。

## 7. Wi-Fi 與 OTA（選配）

1. **Refresh PC Wi-Fi** → 選 2.4 GHz SSID。
2. 輸入密碼 → **Save connect**。
3. 需要自動更新時勾選 **Enable automatic OTA**。
4. 設定 **OTA time**（預設 `01:00`），再按 **Save OTA time**。
5. 沒有 Wi-Fi 或需要立即校正時，按 **Sync clock from PC**。

**成功：** Wi-Fi 顯示 `Connected`，自動更新顯示 `AUTO OTA: ON`。

自動 OTA 只會在每天設定的 60 分鐘維護時段內執行，預設為 `01:00–01:59`。只有在 Wi-Fi 已連線、RTC 時間有效、RSE 狀態有效且 Modbus 閒置時才會安裝；錯過時段就等隔天。Wi-Fi/NTP 會在開機連線後及每天一次，以 Europe/Berlin 當地時間校正 RTC。這只更新 RTC 暫存器，不會反覆寫入 ESP Flash 或逆變器記憶體。

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
- 逆變器ID為2–7，且每台PV裝機功率與模組文件一致。
- 每台已驗證逆變器上限正確；PV較大時黃框屬正常提醒。
- 四種 RSE 輸入正確。
- LIVE 寫入與 FC03 回讀成功。
- 能從限制狀態恢復到 100%。
- RS485 斷線與恢復可正確顯示。
