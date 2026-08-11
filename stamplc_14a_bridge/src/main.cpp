#include <Arduino.h>
#include <M5StamPLC.h>
#include <Preferences.h>
#include <SD.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <time.h>

#include "AppConfig.h"
#include "EventLog.h"
#include "ModbusRtuMaster.h"
#include "OtaManager.h"

namespace {
constexpr int8_t RS485_TX = 0;
constexpr int8_t RS485_RX = 39;
constexpr int8_t RS485_DIR = 46;
constexpr uint8_t INVERTER_COUNT = 6;
constexpr uint8_t HEALTH_FAILURE_THRESHOLD = 3;
constexpr uint32_t PERIODIC_READ_SLOT_MS = 2000;
constexpr uint32_t MODBUS_DEVICE_GAP_MS = 500;
constexpr uint32_t DISPLAY_FRAME_MS = 33;

AppConfig config;
HardwareSerial rs485(1);
ModbusRtuMaster modbus(rs485, RS485_RX, RS485_TX, RS485_DIR);
EventLog eventLog;

String usbLine;
String lastResult = "starting";
uint8_t rawRseMask = 0;
uint8_t stableRseMask = 0;
int16_t activePercent = -1;
bool outputHealthy = false;
uint32_t rawChangedAt = 0;
uint32_t lastDisplayAt = 0;
String otaDisplayStage;
int16_t otaDisplayPercent = -1;
uint32_t otaDisplayUpdatedAt = 0;
uint32_t lastRequested[INVERTER_COUNT] = {};
uint32_t lastReadback[INVERTER_COUNT] = {};
bool inverterHealthy[INVERTER_COUNT] = {};
bool inverterHasReadback[INVERTER_COUNT] = {};
bool inverterAtTarget[INVERTER_COUNT] = {};
uint8_t inverterFailureStreak[INVERTER_COUNT] = {};
uint8_t periodicReadCursor = 0;
uint32_t lastPeriodicReadAt = 0;
float displayedInvPercent[INVERTER_COUNT] = {};
float displayedResPercent = 0.0f;
bool applyInProgress = false;
uint8_t applyProgress = 0;
uint8_t applyTotal = 0;
bool otaBootValidationPending = false;
uint32_t otaBootValidationDeadline = 0;
bool inverterControlStarted = false;

void updateDisplay(bool force = false);
uint8_t readRseMask();
void probeEnabledInverters(const String& event);
void handleStableRseState(uint8_t mask, const String& reason);
void startInverterControl();

void clearOtaBootState() {
    Preferences p;
    if (!p.begin("bridgeboot", false)) return;
    p.clear();
    p.end();
}

void rollbackToPreviousApp(uint8_t previousSubtype, const String& reason) {
    const esp_partition_t* previous = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        static_cast<esp_partition_subtype_t>(previousSubtype), nullptr);
    Serial.println("OTA ROLLBACK DETAIL=" + reason);
    if (previous && esp_ota_set_boot_partition(previous) == ESP_OK) {
        clearOtaBootState();
        Serial.flush();
        delay(300);
        ESP.restart();
    }
    // Never loop forever if the previous partition cannot be selected.  Keep
    // the current application running and expose the error over USB instead.
    clearOtaBootState();
    otaBootValidationPending = false;
    Serial.println("OTA ROLLBACK ERROR=previous application unavailable");
}

void prepareOtaBootValidation() {
    Preferences p;
    if (!p.begin("bridgeboot", false)) return;
    const String target = p.getString("target", "");
    if (target != APP_VERSION) {
        p.end();
        return;
    }
    const uint8_t previousSubtype = p.getUChar("previous", 0xFF);
    const uint8_t attempts = p.getUChar("attempts", 0);
    p.putUChar("attempts", attempts < 255 ? attempts + 1 : attempts);
    p.end();
    if (attempts >= 1) {
        rollbackToPreviousApp(previousSubtype,
                              "new firmware restarted before validation");
        return;
    }
    otaBootValidationPending = true;
    otaBootValidationDeadline = millis() + 15000;
    Serial.println("OTA BOOT STATUS=PENDING_VERIFY VERSION=" + String(APP_VERSION));
}

void serviceOtaBootValidation() {
    if (!otaBootValidationPending ||
        static_cast<int32_t>(millis() - otaBootValidationDeadline) < 0)
        return;
    const esp_partition_t* running = esp_ota_get_running_partition();
    const bool configValid = (config.modbusQuantity == 1 || config.modbusQuantity == 2) &&
                             config.modbusBaud >= 1200 &&
                             config.responseTimeoutMs >= 100 &&
                             config.verifyRetries >= 1 &&
                             config.verifyRetries <= MAX_WRITE_VERIFY_RETRIES;
    if (!running || !configValid) {
        Preferences p;
        uint8_t previousSubtype = 0xFF;
        if (p.begin("bridgeboot", true)) {
            previousSubtype = p.getUChar("previous", 0xFF);
            p.end();
        }
        rollbackToPreviousApp(previousSubtype,
                              "new firmware self-test failed");
        return;
    }
    const esp_err_t validation = esp_ota_mark_app_valid_cancel_rollback();
    if (validation != ESP_OK) {
        Serial.printf("OTA BOOT WARNING=validation state error 0x%X\r\n",
                      static_cast<unsigned>(validation));
    }
    clearOtaBootState();
    otaBootValidationPending = false;
    Serial.println("OTA BOOT STATUS=VALID VERSION=" + String(APP_VERSION));
}

void startInverterControl() {
    if (inverterControlStarted) return;
    rawRseMask = readRseMask();
    stableRseMask = rawRseMask;
    rawChangedAt = millis();
    // The first inverter access after an OTA boot is deliberately deferred
    // until the new application has passed its rollback self-test.
    probeEnabledInverters("startup_probe");
    handleStableRseState(stableRseMask, "initial_state");
    inverterControlStarted = true;
}

struct TileLayoutAnimation {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
    float targetX = 0;
    float targetY = 0;
    float targetWidth = 0;
    float targetHeight = 0;
    bool targetEnabled = false;
};

TileLayoutAnimation tileLayout[INVERTER_COUNT];
bool tileLayoutInitialized = false;

constexpr uint16_t COLOR_NAVY = 0x088E;
constexpr uint16_t COLOR_PANEL = 0x18E3;
constexpr uint16_t COLOR_GREEN = 0x2589;
constexpr uint16_t COLOR_AMBER = 0xFD20;
constexpr uint16_t COLOR_RED = 0xC986;
constexpr uint16_t COLOR_CYAN = 0x04FF;
constexpr uint16_t COLOR_MUTED = 0x9CF3;
constexpr uint16_t COLOR_WATERMARK = 0x4A49;

constexpr int16_t MES_LOGO_W = 80;
constexpr int16_t MES_LOGO_H = 25;
constexpr int16_t MES_LOGO_SCALE = 3;
const uint8_t MES_LOGO_BITS[] PROGMEM = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0xC0,0x01,0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x60,0x03,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x20,0x03,0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x78,0x30,0x06,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0xD8,0x10,0x06,0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0xCC,0x18,0x0C,0x00,0x00,0x00,0x00,0x08,0x00,
    0x00,0x8C,0x09,0x0C,0xF0,0x78,0x7C,0x1C,0xC3,0x00, 0x00,0x86,0x0D,0x18,0xF0,0x78,0x3C,0x90,0x83,0x01,
    0x00,0x06,0x0F,0x18,0xF0,0x7C,0x3C,0x80,0x07,0x01, 0x00,0x07,0x07,0x10,0xF0,0x7D,0x3C,0x84,0x0F,0x00,
    0x00,0x0F,0x06,0x30,0xF0,0x7D,0x7C,0x07,0x3F,0x00, 0x80,0x19,0x06,0x60,0xD0,0x7B,0x3C,0x04,0xFC,0x00,
    0x80,0x31,0x0C,0x60,0xD0,0x7B,0x3C,0x04,0xF0,0x01, 0xC0,0xE0,0x0D,0x40,0xD0,0x7B,0x3C,0x00,0xE0,0x03,
    0xC0,0x80,0x09,0xC0,0x90,0x7B,0x3C,0xA0,0xC0,0x03, 0xC0,0xCF,0xDF,0xFF,0x90,0x79,0x7C,0x98,0xC1,0x01,
    0x80,0xCF,0xDF,0x7F,0x38,0xFD,0xFE,0x9F,0xE7,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};

String timestampNow() {
    struct tm t {};
    M5StamPLC.getRtcTime(&t);
    char buffer[32];
    if (t.tm_year + 1900 < 2024) {
        snprintf(buffer, sizeof(buffer), "uptime:%lu", millis() / 1000UL);
    } else {
        strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &t);
    }
    return buffer;
}

uint8_t readRseMask() {
    uint8_t mask = 0;
    for (uint8_t channel = 0; channel < 4; ++channel) {
        const bool physical = M5StamPLC.readPlcInput(channel);
        const bool active = config.inputActiveHigh ? physical : !physical;
        if (active) mask |= (1U << channel);
    }
    return mask;
}

int16_t decodePercent(uint8_t mask) {
    // One-hot mapping: DI1=100%, DI2=60%, DI3=30%, DI4=0%.
    switch (mask) {
        case 0x01: return 100;
        case 0x02: return 60;
        case 0x04: return 30;
        case 0x08: return 0;
        default: return -1;
    }
}

uint32_t wattsForPercent(uint32_t maximum, uint8_t percent) {
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(maximum) * percent + 50ULL) / 100ULL);
}

void recordSystem(const String& event, const String& result) {
    eventLog.append(timestampNow(), event, stableRseMask, activePercent,
                    0, 0, 0, 0, result);
}

void updateInverterHealth(uint8_t index, const ModbusResult& result) {
    if (result.ok) {
        lastReadback[index] = result.value;
        inverterHasReadback[index] = true;
        inverterFailureStreak[index] = 0;
        inverterHealthy[index] = true;
        return;
    }
    // A Modbus exception or a write/readback mismatch is not a good value
    // either.  Count it just like a timeout, but only promote it to ERROR
    // after the same three-consecutive-failure threshold.
    if (inverterFailureStreak[index] < 255) ++inverterFailureStreak[index];
    if (inverterFailureStreak[index] >= HEALTH_FAILURE_THRESHOLD) {
        inverterHealthy[index] = false;
    }
}

bool applyLevel(uint8_t percent, const String& reason) {
    bool allOk = true;
    uint8_t enabledCount = 0;
    for (uint8_t i = 0; i < INVERTER_COUNT; ++i) {
        if (config.inverters[i].enabled) ++enabledCount;
    }
    // Show the new RSE command and progress before the blocking Modbus work.
    activePercent = percent;
    applyInProgress = enabledCount > 0;
    applyProgress = 0;
    applyTotal = enabledCount;
    lastResult = "Applying RSE " + String(percent) + "%";
    updateDisplay(true);
    modbus.begin(config.modbusBaud);

    for (uint8_t i = 0; i < INVERTER_COUNT; ++i) {
        if (!config.inverters[i].enabled) continue;
        ++applyProgress;
        updateDisplay(true);
        const uint8_t slaveId = i + 1;
        const uint32_t requested = wattsForPercent(
            config.inverters[i].maxPvPowerW, percent);
        lastRequested[i] = requested;

        String detail;
        if (config.dryRun) {
            detail = "DRY-RUN: no Modbus write";
        } else {
            const ModbusResult result = modbus.writeAndVerify(
                slaveId, config.modbusRegister, config.modbusQuantity,
                requested, config.responseTimeoutMs, config.verifyRetries);
            updateInverterHealth(i, result);
            inverterAtTarget[i] = result.ok && result.value == requested;
            lastReadback[i] = result.value;
            inverterHasReadback[i] = result.communicationOk;
            detail = result.detail;
            allOk = allOk && result.ok;
            Serial.printf(
                "WRITE ID=%u TARGET=%lu READBACK=%lu STATUS=%s DETAIL=%s\r\n",
                slaveId, requested, lastReadback[i],
                result.ok ? "OK" : "ERROR", result.detail.c_str());
        }
        if (config.dryRun) allOk = allOk && inverterHealthy[i];
        eventLog.append(timestampNow(), reason, stableRseMask, percent,
                        slaveId, config.inverters[i].maxPvPowerW,
                        requested, lastReadback[i], detail);
        // Keep the RS485 quiet between different devices, but do not add an
        // unnecessary final half-second pause after the last transaction.
        bool hasNextEnabled = false;
        for (uint8_t next = i + 1; next < INVERTER_COUNT; ++next) {
            if (config.inverters[next].enabled) {
                hasNextEnabled = true;
                break;
            }
        }
        if (hasNextEnabled) delay(MODBUS_DEVICE_GAP_MS);
    }

    outputHealthy = enabledCount > 0 && allOk;
    applyInProgress = false;
    if (enabledCount == 0) {
        lastResult = "No inverter IDs are enabled";
        recordSystem(reason, lastResult);
    } else {
        lastResult = config.dryRun
            ? "DRY-RUN level " + String(percent) + "% logged"
            : (allOk ? "All enabled inverters verified at " + String(percent) + "%"
                     : "One or more inverter writes failed");
    }
    Serial.println("RESULT " + lastResult);
    if (!outputHealthy) M5StamPLC.tone(1800, 250);
    updateDisplay(true);
    return outputHealthy;
}

void handleStableRseState(uint8_t mask, const String& reason) {
    const int16_t decoded = decodePercent(mask);
    if (decoded < 0) {
        activePercent = -1;
        outputHealthy = false;
        char message[96];
        snprintf(message, sizeof(message),
                 "Invalid RSE one-hot mask 0x%02X; outputs unchanged", mask);
        lastResult = message;
        recordSystem(reason, lastResult);
        Serial.println("ALARM " + lastResult);
        M5StamPLC.tone(2200, 500);
        return;
    }
    applyLevel(static_cast<uint8_t>(decoded), reason);
}

void refreshOutputHealthFromCommunication();

bool probeInverter(uint8_t slaveId, const String& event = "usb_probe") {
    modbus.begin(config.modbusBaud);
    const ModbusResult result = modbus.readRaw(
        slaveId, config.modbusRegister, config.modbusQuantity,
        config.responseTimeoutMs);
    const uint8_t index = slaveId - 1;
    updateInverterHealth(index, result);
    String detail = result.detail;
    if (!result.ok) {
        detail += " (failure " + String(inverterFailureStreak[index]) + "/" +
                  String(HEALTH_FAILURE_THRESHOLD) + ")";
    }
    const char* status = result.ok ? "OK"
        : (inverterFailureStreak[index] >= HEALTH_FAILURE_THRESHOLD
               ? "ERROR" : "RETRY");
    Serial.printf(
        "PROBE ID=%u REGISTER=0x%04X VALUE=%lu STATUS=%s DETAIL=%s\r\n",
        slaveId, config.modbusRegister,
        result.ok ? result.value : lastReadback[index],
        status, detail.c_str());
    eventLog.append(timestampNow(), event, stableRseMask,
                    activePercent, slaveId,
                    config.inverters[index].maxPvPowerW, 0,
                    lastReadback[index], detail);
    return result.ok;
}

void probeEnabledInverters(const String& event = "usb_probe") {
    uint8_t count = 0;
    uint8_t passed = 0;
    Serial.println(event == "startup_probe"
        ? "STARTUP PROBE (read-only FC03; no values will be written)"
        : "PROBE START (read-only FC03; no values will be written)");
    for (uint8_t id = 1; id <= INVERTER_COUNT; ++id) {
        if (!config.inverters[id - 1].enabled) continue;
        ++count;
        if (probeInverter(id, event)) ++passed;
        delay(MODBUS_DEVICE_GAP_MS);
    }
    if (count == 0) {
        Serial.println("PROBE SUMMARY enabled=0 passed=0 failed=0");
    } else {
        Serial.printf("PROBE SUMMARY enabled=%u passed=%u failed=%u\r\n",
                      count, passed, count - passed);
    }
    refreshOutputHealthFromCommunication();
}

void scanAllInverters() {
    uint8_t found = 0;
    Serial.println("SCAN START (read-only FC03; IDs 1-6)");
    modbus.begin(config.modbusBaud);
    for (uint8_t id = 1; id <= INVERTER_COUNT; ++id) {
        const uint8_t index = id - 1;
        const ModbusResult result = modbus.readRaw(
            id, config.modbusRegister, config.modbusQuantity,
            config.responseTimeoutMs);
        updateInverterHealth(index, result);
        if (result.ok) {
            ++found;
            Serial.printf("SCAN ID=%u VALUE=%lu STATUS=FOUND DETAIL=FC03 readback verified\r\n",
                          id, result.value);
        } else {
            const char* status = result.communicationOk ? "ERROR" : "NO_RESPONSE";
            Serial.printf("SCAN ID=%u VALUE=0 STATUS=%s DETAIL=%s\r\n",
                          id, status, result.detail.c_str());
        }
        eventLog.append(timestampNow(), "usb_scan", stableRseMask, activePercent,
                        id, config.inverters[index].maxPvPowerW, 0,
                        result.ok ? result.value : lastReadback[index], result.detail);
        if (id < INVERTER_COUNT) delay(MODBUS_DEVICE_GAP_MS);
    }
    Serial.printf("SCAN SUMMARY found=%u missing=%u\r\n", found,
                  INVERTER_COUNT - found);
    refreshOutputHealthFromCommunication();
}

void refreshOutputHealthFromCommunication() {
    if (activePercent < 0) return;
    bool anyEnabled = false;
    bool allHealthy = true;
    for (uint8_t i = 0; i < INVERTER_COUNT; ++i) {
        if (!config.inverters[i].enabled) continue;
        anyEnabled = true;
        const bool controlOk = config.dryRun || inverterAtTarget[i];
        allHealthy = allHealthy && inverterHealthy[i] && controlOk;
    }
    outputHealthy = anyEnabled && allHealthy;
}

void pollNextEnabledInverter() {
    if (millis() - lastPeriodicReadAt < PERIODIC_READ_SLOT_MS) return;
    lastPeriodicReadAt = millis();

    for (uint8_t checked = 0; checked < INVERTER_COUNT; ++checked) {
        const uint8_t index = periodicReadCursor;
        periodicReadCursor = (periodicReadCursor + 1) % INVERTER_COUNT;
        if (!config.inverters[index].enabled) continue;

        modbus.begin(config.modbusBaud);
        const uint8_t previousFailures = inverterFailureStreak[index];
        const bool wasConfirmedFault =
            previousFailures >= HEALTH_FAILURE_THRESHOLD;
        const ModbusResult result = modbus.readRaw(
            index + 1, config.modbusRegister, config.modbusQuantity,
            config.responseTimeoutMs);
        updateInverterHealth(index, result);
        if (result.ok && !config.dryRun && activePercent >= 0) {
            const uint32_t expected = wattsForPercent(
                config.inverters[index].maxPvPowerW,
                static_cast<uint8_t>(activePercent));
            inverterAtTarget[index] = result.value == expected;
        }

        if (result.ok && previousFailures > 0) {
            const String detail = wasConfirmedFault
                ? "communication recovered" : "transient read failure cleared";
            Serial.printf("HEALTH ID=%u STATUS=OK DETAIL=%s\r\n",
                          index + 1, detail.c_str());
            eventLog.append(timestampNow(), "periodic_recovered", stableRseMask,
                            activePercent, index + 1,
                            config.inverters[index].maxPvPowerW,
                            lastRequested[index], result.value, detail);
        } else if (!result.ok &&
                   inverterFailureStreak[index] == HEALTH_FAILURE_THRESHOLD) {
            Serial.printf("HEALTH ID=%u STATUS=ERROR DETAIL=%s after %u consecutive failures\r\n",
                          index + 1, result.detail.c_str(),
                          HEALTH_FAILURE_THRESHOLD);
            eventLog.append(timestampNow(), "periodic_offline", stableRseMask,
                            activePercent, index + 1,
                            config.inverters[index].maxPvPowerW,
                            lastRequested[index], lastReadback[index],
                            result.detail + "; confirmed after " +
                                String(HEALTH_FAILURE_THRESHOLD) + " failures");
        }
        refreshOutputHealthFromCommunication();
        return;
    }
}

void printHelp() {
    Serial.println();
    Serial.println("14a Bridge - USB configuration");
    Serial.println("Commands:");
    Serial.println("  identify                     Identify this 14A Bridge SmartPLC");
    Serial.println("  show                         Show configuration and live state");
    Serial.println("  probe all                    FC03-read all enabled inverter IDs");
    Serial.println("  probe <1-6>                  FC03-read one inverter ID");
    Serial.println("  id <1-6> <on|off>            Enable/disable an inverter ID");
    Serial.println("  max <1-6> <watts>            Set maximum PV power for an ID");
    Serial.println("  time <YYYY-MM-DD HH:MM:SS>    Set the onboard RTC");
    Serial.println("  test <100|60|30|0>            Test level (safe in dry-run)");
    Serial.println("  test <level> CONFIRM          Test level when live");
    Serial.println("  apply                         Reapply current RSE level in dry-run");
    Serial.println("  apply CONFIRM                 Reapply current RSE level when live");
    Serial.println("  dryrun on                    Disable real Modbus writes");
    Serial.println("  dryrun off CONFIRM           Enable live inverter control");
    Serial.println("  baud <rate>                  Set RS485 baud rate");
    Serial.println("  reg <hex|decimal>             Set power-limit register");
    Serial.println("  wifi show                     Show Wi-Fi and OTA state");
    Serial.println("  wifi sethex <ssid> <password> Save UTF-8 values encoded as hexadecimal");
    Serial.println("  wifi connect                  Start/retry Wi-Fi connection");
    Serial.println("  wifi clear CONFIRM            Erase saved Wi-Fi credentials");
    Serial.println("  ota auto <on|off>             Enable/disable automatic OTA updates");
    Serial.println("  ota check                     Check the latest GitHub release");
    Serial.println("  ota update CONFIRM            Verify and install the latest firmware");
    Serial.println("  reset CONFIRM                Erase saved configuration and reboot");
    Serial.println("  help                         Show this help");
    Serial.println("All successful setting commands are saved automatically.");
    Serial.println();
}

void printOtaDiagnostic(const char* prefix) {
    const String status = otaManager.lastStatus();
    const String detailHex = otaManager.lastDetailHex();
    Serial.printf("%sOTA LAST=%s CHECK=%lu SUCCESS=%lu FAILS=%lu NEXT=%lu DETAILHEX=%s\r\n",
                  prefix, status.c_str(),
                  static_cast<unsigned long>(otaManager.lastCheckEpoch()),
                  static_cast<unsigned long>(otaManager.lastSuccessEpoch()),
                  static_cast<unsigned long>(otaManager.consecutiveFailures()),
                  static_cast<unsigned long>(otaManager.nextCheckSeconds()),
                  detailHex.c_str());
}

void printStatus() {
    Serial.println();
    Serial.println("=== RSE BRIDGE STATUS ===");
    Serial.printf("Time: %s\r\n", timestampNow().c_str());
    Serial.printf("RSE DI mask: 0x%02X  level: ", stableRseMask);
    if (activePercent >= 0) Serial.printf("%d%%\r\n", activePercent);
    else Serial.println("INVALID");
    Serial.printf("Mode: %s  RS485: %lu baud  register: 0x%04X  quantity: %u\r\n",
                  config.dryRun ? "DRY-RUN" : "LIVE",
                  config.modbusBaud, config.modbusRegister,
                  config.modbusQuantity);
    Serial.printf("Firmware: %s  WiFi saved: %s connected: %s auto OTA: %s IP: %s RSSI: %ld\r\n",
                  APP_VERSION, otaManager.hasCredentials() ? "yes" : "no",
                  otaManager.connected() ? "yes" : "no",
                  otaManager.automatic() ? "yes" : "no",
                  otaManager.ipAddress().c_str(), static_cast<long>(otaManager.rssi()));
    printOtaDiagnostic("");
    Serial.println("ID  Enabled  MaxPV(W)  100%    60%    30%     0%  LastReq  Readback  OK");
    for (uint8_t i = 0; i < INVERTER_COUNT; ++i) {
        const uint32_t maximum = config.inverters[i].maxPvPowerW;
        const bool statusOk = inverterHealthy[i] &&
            (config.dryRun || activePercent < 0 || inverterAtTarget[i]);
        Serial.printf("%u   %-7s %8lu %6lu %6lu %6lu %6u %8lu %9lu  %s\r\n",
                      i + 1, config.inverters[i].enabled ? "yes" : "no",
                      maximum, maximum, wattsForPercent(maximum, 60),
                      wattsForPercent(maximum, 30), 0,
                      lastRequested[i], lastReadback[i],
                      statusOk ? "yes" : "no");
    }
    Serial.printf("SD log: %s  Overall output: %s\r\n",
                  eventLog.available() ? "available" : "unavailable",
                  outputHealthy ? "OK" : "CHECK");
    Serial.println("Last result: " + lastResult);
    Serial.println("=========================\n");
}

void printGuiStatus() {
    Serial.printf("@ RSE DI mask: 0x%02X  level: ", stableRseMask);
    if (activePercent >= 0) Serial.printf("%d%%\r\n", activePercent);
    else Serial.println("INVALID");
    Serial.printf("@ Mode: %s  RS485: %lu baud  register: 0x%04X  quantity: %u\r\n",
                  config.dryRun ? "DRY-RUN" : "LIVE", config.modbusBaud,
                  config.modbusRegister, config.modbusQuantity);
    Serial.printf("@ WIFI VERSION=%s SAVED=%s CONNECTED=%s AUTO=%s SSIDHEX=%s IP=%s RSSI=%ld\r\n",
                  APP_VERSION, otaManager.hasCredentials() ? "yes" : "no",
                  otaManager.connected() ? "yes" : "no",
                  otaManager.automatic() ? "yes" : "no",
                  otaManager.ssidHex().c_str(), otaManager.ipAddress().c_str(),
                  static_cast<long>(otaManager.rssi()));
    printOtaDiagnostic("@ ");
    for (uint8_t i = 0; i < INVERTER_COUNT; ++i) {
        const uint32_t maximum = config.inverters[i].maxPvPowerW;
        const bool statusOk = inverterHealthy[i] &&
            (config.dryRun || activePercent < 0 || inverterAtTarget[i]);
        Serial.printf("@ %u   %-7s %8lu %6lu %6lu %6lu %6u %8lu %9lu  %s\r\n",
                      i + 1, config.inverters[i].enabled ? "yes" : "no",
                      maximum, maximum, wattsForPercent(maximum, 60),
                      wattsForPercent(maximum, 30), 0, lastRequested[i],
                      lastReadback[i], statusOk ? "yes" : "no");
    }
}

bool validId(int id) {
    if (id >= 1 && id <= INVERTER_COUNT) return true;
    Serial.println("ERROR ID must be between 1 and 6");
    return false;
}

bool saveConfig() {
    if (!config.save()) {
        Serial.println("ERROR configuration could not be saved");
        return false;
    }
    Serial.println("OK saved");
    recordSystem("configuration", "saved by USB");
    return true;
}

void processUsbCommand(String line) {
    line.trim();
    if (line.isEmpty()) return;
    if (!line.equalsIgnoreCase("gui") && !line.equalsIgnoreCase("identify"))
        Serial.println("> " + line);

    if (line.equalsIgnoreCase("identify")) {
        Serial.printf("IDENTITY PRODUCT=14A_BRIDGE MODEL=STAMPPLC VERSION=%s\r\n",
                      APP_VERSION);
        return;
    }

    if (line.equalsIgnoreCase("help") || line == "?") {
        printHelp();
        return;
    }
    if (line.equalsIgnoreCase("show")) {
        printStatus();
        return;
    }
    if (line.equalsIgnoreCase("gui")) {
        printGuiStatus();
        return;
    }

    if (line.equalsIgnoreCase("wifi show")) {
        Serial.printf("WIFI VERSION=%s SAVED=%s CONNECTED=%s AUTO=%s SSIDHEX=%s IP=%s RSSI=%ld\r\n",
                      APP_VERSION, otaManager.hasCredentials() ? "yes" : "no",
                      otaManager.connected() ? "yes" : "no",
                      otaManager.automatic() ? "yes" : "no",
                      otaManager.ssidHex().c_str(), otaManager.ipAddress().c_str(),
                      static_cast<long>(otaManager.rssi()));
        printOtaDiagnostic("");
        return;
    }
    if (line.startsWith("wifi sethex ")) {
        const int split = line.indexOf(' ', 12);
        if (split < 0) {
            Serial.println("WIFI STATUS=ERROR DETAIL=use: wifi sethex <ssid-hex> <password-hex-or-dash>");
            return;
        }
        const String ssidHex = line.substring(12, split);
        String passwordHex = line.substring(split + 1);
        if (passwordHex == "-") passwordHex = "";
        String detail;
        const bool ok = otaManager.saveWifiHex(ssidHex, passwordHex, detail);
        Serial.printf("WIFI STATUS=%s DETAIL=%s\r\n", ok ? "OK" : "ERROR", detail.c_str());
        return;
    }
    if (line.equalsIgnoreCase("wifi connect")) {
        if (!otaManager.hasCredentials()) {
            Serial.println("WIFI STATUS=ERROR DETAIL=no saved credentials");
        } else {
            otaManager.connectNow();
            Serial.println("WIFI STATUS=CONNECTING DETAIL=connection started");
        }
        return;
    }
    if (line.equalsIgnoreCase("wifi clear CONFIRM")) {
        Serial.printf("WIFI STATUS=%s DETAIL=credentials cleared\r\n",
                      otaManager.clearWifi() ? "OK" : "ERROR");
        return;
    }
    if (line.equalsIgnoreCase("ota auto on") || line.equalsIgnoreCase("ota auto off")) {
        const bool enabled = line.equalsIgnoreCase("ota auto on");
        const bool saved = otaManager.setAutomatic(enabled);
        Serial.printf("OTA AUTO=%s STATUS=%s DETAIL=%s\r\n",
                      otaManager.automatic() ? "yes" : "no",
                      saved ? "OK" : "ERROR",
                      saved ? "saved" : "NVS save failed");
        return;
    }
    if (line.equalsIgnoreCase("ota check")) {
        String version;
        String detail;
        const bool ok = otaManager.checkForUpdate(version, detail);
        Serial.printf("OTA STATUS=%s CURRENT=%s AVAILABLE=%s DETAIL=%s\r\n",
                      ok ? "OK" : "ERROR", APP_VERSION,
                      version.isEmpty() ? "-" : version.c_str(), detail.c_str());
        return;
    }
    if (line.equalsIgnoreCase("ota update CONFIRM")) {
        if (activePercent < 0 || applyInProgress) {
            Serial.println("OTA STATUS=ERROR DETAIL=RSE state must be valid and Modbus control idle");
            return;
        }
        String detail;
        const bool ok = otaManager.installUpdate(detail);
        Serial.printf("OTA STATUS=%s DETAIL=%s\r\n", ok ? "OK" : "ERROR", detail.c_str());
        return;
    }

    int commitId = 0;
    char commitState[16] = {};
    unsigned long commitWatts = 0;
    if (sscanf(line.c_str(), "commit %d %15s %lu", &commitId, commitState,
               &commitWatts) == 3) {
        if (!validId(commitId)) return;
        String requestedState = commitState;
        requestedState.toLowerCase();
        if ((requestedState != "on" && requestedState != "off") || commitWatts == 0) {
            Serial.println("COMMIT ERROR DETAIL=use: commit <1-6> <on|off> <watts>");
            return;
        }
        if (config.modbusQuantity == 1 && commitWatts > 65535UL) {
            Serial.printf("COMMIT ID=%d STATUS=ERROR CONFIG=%lu DETAIL=value exceeds one-register configuration\r\n",
                          commitId, config.inverters[commitId - 1].maxPvPowerW);
            return;
        }

        const bool enabled = requestedState == "on";
        const uint8_t index = static_cast<uint8_t>(commitId - 1);
        if (!enabled) {
            config.inverters[index].enabled = false;
            config.inverters[index].maxPvPowerW = commitWatts;
            saveConfig();
            Serial.printf("COMMIT ID=%d STATUS=OK CONFIG=%lu DETAIL=disabled ID saved without inverter check\r\n",
                          commitId, commitWatts);
            return;
        }

        // Validate the requested rating against the actual inverter: retain
        // its current value, write the candidate, read it back, then restore.
        modbus.begin(config.modbusBaud);
        ModbusResult previous = modbus.readRaw(commitId, config.modbusRegister,
                                               config.modbusQuantity,
                                               config.responseTimeoutMs);
        if (!previous.ok) {
            // Installation is often configured before the RS485 cable or the
            // inverter itself is online.  Preserve the customer's intended
            // setting and defer rating validation instead of discarding it.
            const InverterConfig previousConfig = config.inverters[index];
            config.inverters[index].enabled = true;
            config.inverters[index].maxPvPowerW = commitWatts;
            if (!saveConfig()) {
                config.inverters[index] = previousConfig;
                Serial.printf("COMMIT ID=%d STATUS=ERROR CONFIG=%lu DETAIL=inverter unavailable and configuration save failed\r\n",
                              commitId, previousConfig.maxPvPowerW);
                return;
            }
            Serial.printf("COMMIT ID=%d STATUS=PENDING CONFIG=%lu DETAIL=inverter unavailable; setting saved and validation deferred: %s\r\n",
                          commitId, commitWatts, previous.detail.c_str());
            return;
        }
        ModbusResult candidate = modbus.writeAndVerify(
            commitId, config.modbusRegister, config.modbusQuantity, commitWatts,
            config.responseTimeoutMs, 1);
        ModbusResult restored = modbus.writeAndVerify(
            commitId, config.modbusRegister, config.modbusQuantity, previous.value,
            config.responseTimeoutMs, 1);
        if (!restored.ok) {
            Serial.printf("COMMIT ID=%d STATUS=ERROR CONFIG=%lu DETAIL=restore failed: %s\r\n",
                          commitId, config.inverters[index].maxPvPowerW,
                          restored.detail.c_str());
            return;
        }
        lastReadback[index] = restored.value;
        inverterHasReadback[index] = true;
        const InverterConfig previousConfig = config.inverters[index];
        if (candidate.ok) {
            config.inverters[index].enabled = true;
            config.inverters[index].maxPvPowerW = commitWatts;
            if (!saveConfig()) {
                config.inverters[index] = previousConfig;
                Serial.printf("COMMIT ID=%d STATUS=ERROR CONFIG=%lu DETAIL=configuration save failed; previous configuration kept\r\n",
                              commitId, previousConfig.maxPvPowerW);
                return;
            }
            Serial.printf("COMMIT ID=%d STATUS=OK CONFIG=%lu DETAIL=write/readback verified; prior value restored\r\n",
                          commitId, commitWatts);
            return;
        }
        if (candidate.communicationOk && candidate.value > 0) {
            // The inverter has proven its actual ceiling by clamping the
            // temporary validation write.  Adopt that ceiling so a later RSE
            // transition can never request the rejected higher value.
            config.inverters[index].enabled = true;
            config.inverters[index].maxPvPowerW = candidate.value;
            if (!saveConfig()) {
                config.inverters[index] = previousConfig;
                Serial.printf("COMMIT ID=%d STATUS=ERROR CONFIG=%lu DETAIL=accepted limit %lu W but configuration save failed\r\n",
                              commitId, previousConfig.maxPvPowerW, candidate.value);
                return;
            }
            Serial.printf("COMMIT ID=%d STATUS=CLAMPED CONFIG=%lu DETAIL=requested %lu W; inverter accepted %lu W; configuration adjusted; prior value restored\r\n",
                          commitId, candidate.value, commitWatts, candidate.value);
            return;
        }
        Serial.printf("COMMIT ID=%d STATUS=ERROR CONFIG=%lu DETAIL=validation failed: %s\r\n",
                      commitId, config.inverters[index].maxPvPowerW,
                      candidate.detail.c_str());
        return;
    }

    if (line.equalsIgnoreCase("scan all")) {
        scanAllInverters();
        return;
    }

    if (line.equalsIgnoreCase("probe all")) {
        probeEnabledInverters();
        return;
    }

    int probeId = 0;
    if (sscanf(line.c_str(), "probe %d", &probeId) == 1) {
        if (!validId(probeId)) return;
        Serial.println("PROBE START (read-only FC03; no values will be written)");
        const bool ok = probeInverter(static_cast<uint8_t>(probeId));
        Serial.printf("PROBE SUMMARY enabled=1 passed=%u failed=%u\r\n",
                      ok ? 1 : 0, ok ? 0 : 1);
        return;
    }

    int id = 0;
    char state[16] = {};
    if (sscanf(line.c_str(), "id %d %15s", &id, state) == 2) {
        if (!validId(id)) return;
        String requestedState = state;
        requestedState.toLowerCase();
        if (requestedState != "on" && requestedState != "off") {
            Serial.println("ERROR use: id <1-6> <on|off>");
            return;
        }
        config.inverters[id - 1].enabled = requestedState == "on";
        saveConfig();
        return;
    }

    unsigned long watts = 0;
    if (sscanf(line.c_str(), "max %d %lu", &id, &watts) == 2) {
        if (!validId(id)) return;
        if (watts == 0) {
            Serial.println("ERROR maximum PV power must be greater than zero");
            return;
        }
        if (config.modbusQuantity == 1 && watts > 65535UL) {
            Serial.println("ERROR value exceeds a one-register configuration");
            return;
        }
        config.inverters[id - 1].maxPvPowerW = watts;
        saveConfig();
        return;
    }

    int year, month, day, hour, minute, second;
    if (sscanf(line.c_str(), "time %d-%d-%d %d:%d:%d",
               &year, &month, &day, &hour, &minute, &second) == 6) {
        if (year < 2024 || year > 2099 || month < 1 || month > 12 ||
            day < 1 || day > 31 || hour < 0 || hour > 23 ||
            minute < 0 || minute > 59 || second < 0 || second > 59) {
            Serial.println("ERROR invalid date/time");
            return;
        }
        struct tm t {};
        t.tm_year = year - 1900;
        t.tm_mon = month - 1;
        t.tm_mday = day;
        t.tm_hour = hour;
        t.tm_min = minute;
        t.tm_sec = second;
        M5StamPLC.setRtcTime(&t);
        Serial.println("OK RTC set to " + timestampNow());
        recordSystem("clock_set", "RTC set by USB");
        return;
    }

    int percent = -1;
    char confirmation[16] = {};
    const int testParts = sscanf(line.c_str(), "test %d %15s", &percent, confirmation);
    if (testParts >= 1) {
        if (percent != 100 && percent != 60 && percent != 30 && percent != 0) {
            Serial.println("ERROR test level must be 100, 60, 30, or 0");
            return;
        }
        String confirm = confirmation;
        confirm.toUpperCase();
        if (!config.dryRun && confirm != "CONFIRM") {
            Serial.println("ERROR live test requires: test <level> CONFIRM");
            return;
        }
        applyLevel(percent, "usb_manual_test");
        return;
    }

    if (line.equalsIgnoreCase("apply") ||
        line.equalsIgnoreCase("apply CONFIRM")) {
        if (!config.dryRun && !line.equalsIgnoreCase("apply CONFIRM")) {
            Serial.println("ERROR live apply requires: apply CONFIRM");
            return;
        }
        handleStableRseState(stableRseMask, "usb_apply_current");
        return;
    }

    if (line.equalsIgnoreCase("dryrun on")) {
        config.dryRun = true;
        saveConfig();
        return;
    }
    if (line.equalsIgnoreCase("dryrun off CONFIRM")) {
        config.dryRun = false;
        saveConfig();
        Serial.println("WARNING LIVE CONTROL ENABLED");
        // A manual test may have left activePercent at its test value.  As
        // soon as LIVE is armed, discard that temporary display/control state
        // and immediately evaluate the physical RSE inputs instead.
        rawRseMask = readRseMask();
        stableRseMask = rawRseMask;
        rawChangedAt = millis();
        handleStableRseState(stableRseMask, "live_enabled_actual_rse");
        return;
    }

    unsigned long baud = 0;
    if (sscanf(line.c_str(), "baud %lu", &baud) == 1) {
        if (baud < 1200 || baud > 1000000) {
            Serial.println("ERROR baud must be between 1200 and 1000000");
            return;
        }
        config.modbusBaud = baud;
        saveConfig();
        return;
    }

    if (line.startsWith("reg ")) {
        const String value = line.substring(4);
        char* end = nullptr;
        const unsigned long address = strtoul(value.c_str(), &end, 0);
        if (end == value.c_str() || *end != '\0' || address > 0xFFFF) {
            Serial.println("ERROR register must be 0..65535 or 0x0000..0xFFFF");
            return;
        }
        config.modbusRegister = address;
        saveConfig();
        return;
    }

    if (line.equalsIgnoreCase("reset CONFIRM")) {
        Preferences p;
        if (p.begin("rsebridge", false)) {
            p.clear();
            p.end();
            Serial.println("OK configuration cleared; rebooting");
            delay(250);
            ESP.restart();
        } else {
            Serial.println("ERROR reset failed");
        }
        return;
    }

    Serial.println("ERROR unknown command. Enter 'help'.");
}

void serviceUsb() {
    while (Serial.available()) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\r' || c == '\n') {
            if (!usbLine.isEmpty()) {
                processUsbCommand(usbLine);
                usbLine = "";
            }
        } else if (c == '\b' || c == 0x7F) {
            if (!usbLine.isEmpty()) usbLine.remove(usbLine.length() - 1);
        } else if (c >= 32 && c <= 126 && usbLine.length() < 319) {
            usbLine += c;
        }
    }
}

String kilowattsValue(uint32_t watts) {
    char text[10];
    snprintf(text, sizeof(text), "%lu.%lu",
             static_cast<unsigned long>(watts / 1000),
             static_cast<unsigned long>((watts % 1000) / 100));
    return text;
}

String compactKilowattsValue(uint32_t watts) {
    if (watts >= 100000) return String((watts + 500) / 1000);
    return kilowattsValue(watts);
}

uint8_t percentOfMaximum(uint32_t value, uint32_t maximum) {
    if (maximum == 0) return 0;
    const uint64_t rounded =
        (static_cast<uint64_t>(value) * 100ULL + maximum / 2) / maximum;
    return static_cast<uint8_t>(min<uint64_t>(rounded, 100));
}

uint16_t colorForLevel(int16_t percent) {
    if (percent < 0) return COLOR_RED;
    if (percent >= 100) return COLOR_GREEN;
    if (percent >= 60) return TFT_YELLOW;
    if (percent >= 30) return COLOR_AMBER;
    return COLOR_RED;
}

uint16_t blendRgb565(uint16_t from, uint16_t to, float amount) {
    amount = min(1.0f, max(0.0f, amount));
    const int16_t fromR = (from >> 11) & 0x1F;
    const int16_t fromG = (from >> 5) & 0x3F;
    const int16_t fromB = from & 0x1F;
    const int16_t toR = (to >> 11) & 0x1F;
    const int16_t toG = (to >> 5) & 0x3F;
    const int16_t toB = to & 0x1F;
    const uint16_t red = static_cast<uint16_t>(fromR + (toR - fromR) * amount);
    const uint16_t green = static_cast<uint16_t>(fromG + (toG - fromG) * amount);
    const uint16_t blue = static_cast<uint16_t>(fromB + (toB - fromB) * amount);
    return (red << 11) | (green << 5) | blue;
}

uint16_t animatedLevelColor(float percent) {
    if (percent <= 30.0f) return blendRgb565(TFT_RED, COLOR_AMBER, percent / 30.0f);
    if (percent <= 60.0f) return blendRgb565(COLOR_AMBER, TFT_YELLOW,
                                              (percent - 30.0f) / 30.0f);
    return blendRgb565(TFT_YELLOW, TFT_GREEN, (percent - 60.0f) / 40.0f);
}

void drawCentered(M5Canvas& canvas, const String& text, int32_t y,
                  uint8_t size, uint16_t color) {
    canvas.setTextSize(size);
    canvas.setTextColor(color);
    canvas.setTextDatum(top_center);
    canvas.drawString(text, canvas.width() / 2, y);
    canvas.setTextDatum(top_left);
}

void drawCellText(M5Canvas& canvas, const String& text, int32_t x, int32_t y,
                  float size, uint16_t color = TFT_WHITE) {
    canvas.setTextDatum(middle_center);
    canvas.setTextSize(size);
    canvas.setTextColor(TFT_BLACK);
    // A two-pixel dark outline keeps the values legible over every liquid
    // colour and over the MES watermark, without putting an opaque box over
    // the gauge animation.
    canvas.drawString(text, x - 1, y);
    canvas.drawString(text, x + 1, y);
    canvas.drawString(text, x, y - 1);
    canvas.drawString(text, x, y + 1);
    canvas.setTextColor(color);
    canvas.drawString(text, x, y);
}

void drawMesWatermark(M5Canvas& canvas, int16_t x, int16_t y) {
    // The bitmap was generated directly from the supplied MES source logo.
    // It is drawn before gauges, so liquid naturally covers it.
    constexpr int16_t bytesPerRow = MES_LOGO_W / 8;
    for (int16_t row = 0; row < MES_LOGO_H; ++row) {
        for (int16_t column = 0; column < MES_LOGO_W; ++column) {
            const uint8_t bits = pgm_read_byte(&MES_LOGO_BITS[
                row * bytesPerRow + column / 8]);
            if ((bits & (1U << (column & 7))) == 0) continue;
            canvas.fillRect(x + column * MES_LOGO_SCALE,
                            y + row * MES_LOGO_SCALE,
                            MES_LOGO_SCALE, MES_LOGO_SCALE, COLOR_WATERMARK);
        }
    }
}

void drawDashedLine(M5Canvas& canvas, int16_t x, int16_t y, int16_t width,
                    uint16_t color) {
    constexpr int16_t dash = 5;
    constexpr int16_t space = 3;
    for (int16_t offset = 0; offset < width; offset += dash + space) {
        canvas.drawFastHLine(x + offset, y, min<int16_t>(dash, width - offset),
                             color);
    }
}

void drawLiquidFill(M5Canvas& canvas, int16_t x, int16_t y, int16_t width,
                    int16_t height, float percent, uint16_t color,
                    uint8_t inverterIndex, uint32_t now) {
    const int16_t left = x + 2;
    const int16_t right = x + width - 3;
    const int16_t top = y + 2;
    const int16_t bottom = y + height - 3;
    const int16_t innerHeight = bottom - top + 1;
    if (percent <= 0.2f || innerHeight <= 0) return;

    const float clampedPercent = min(100.0f, max(0.0f, percent));
    const float baseSurface = bottom - (innerHeight * clampedPercent / 100.0f);
    const float phase = now * 0.0065f + inverterIndex * 1.37f;
    const float amplitude = height >= 70 ? 2.4f : 1.2f;
    const uint16_t surfaceColor = TFT_WHITE;

    for (int16_t pixelX = left; pixelX <= right; ++pixelX) {
        const float wave = sinf((pixelX - left) * 0.28f + phase) * amplitude +
            sinf((pixelX - left) * 0.13f - phase * 0.65f) * (amplitude * 0.45f);
        const int16_t surfaceY = constrain(
            static_cast<int16_t>(baseSurface + wave), top, bottom);
        canvas.drawFastVLine(pixelX, surfaceY, bottom - surfaceY + 1, color);
        canvas.drawPixel(pixelX, surfaceY, surfaceColor);
    }

    // Three deterministic bubbles rise inside the liquid; no random source is
    // used, so the animation remains smooth and repeatable.
    const int16_t liquidHeight = bottom - static_cast<int16_t>(baseSurface);
    if (liquidHeight < 12 || right - left < 14) return;
    const int16_t bubbleMinX = left + 4;
    const int16_t bubbleSpan = max<int16_t>(1, right - left - 8);
    const int16_t driftSpan = min<int16_t>(6, max<int16_t>(2, bubbleSpan / 10));
    for (uint8_t bubble = 0; bubble < 3; ++bubble) {
        const uint32_t cycleMs = 1700 + bubble * 430;
        const float progress = static_cast<float>((now + inverterIndex * 317 +
            bubble * 541) % cycleMs) / cycleMs;
        // Bubbles rise vertically in separate lanes and only drift gently.
        const float horizontalPhase = now * (0.00115f + bubble * 0.00017f) +
            inverterIndex * 1.71f + bubble * 2.12f;
        const int16_t laneX = bubbleMinX +
            static_cast<int16_t>((bubble + 1) * bubbleSpan / 4);
        const int16_t bubbleX = laneX + static_cast<int16_t>(
            sinf(horizontalPhase) * driftSpan);
        const int16_t bubbleY = bottom - 4 -
            static_cast<int16_t>(progress * max<int16_t>(1, liquidHeight - 7));
        if (bubbleY > top + 2 && bubbleY < bottom - 1) {
            canvas.drawCircle(bubbleX, bubbleY, bubble == 1 ? 2 : 1, surfaceColor);
        }
    }
}

void updateDisplay(bool force) {
    const uint32_t now = millis();
    if (!force && now - lastDisplayAt < DISPLAY_FRAME_MS) return;
    lastDisplayAt = now;
    auto& display = M5StamPLC.Display;
    static M5Canvas canvas(&display);
    static bool canvasReady = false;
    if (!canvasReady) {
        canvas.setColorDepth(16);
        canvasReady = canvas.createSprite(display.width(), display.height()) != nullptr;
    }
    M5Canvas& ui = canvas;
    ui.fillScreen(TFT_BLACK);

    const int16_t screenW = ui.width();
    const int16_t screenH = ui.height();

    if (!otaDisplayStage.isEmpty()) {
        ui.fillScreen(TFT_BLACK);
        ui.fillRect(0, 0, screenW, 22, COLOR_NAVY);
        ui.setTextDatum(middle_center);
        ui.setTextSize(1);
        ui.setTextColor(COLOR_CYAN);
        ui.drawString("SMARTPLC OTA  v" APP_VERSION, screenW / 2, 11);
        ui.setTextSize(2);
        ui.setTextColor(otaDisplayStage == "ERROR" ? COLOR_RED : TFT_WHITE);
        ui.drawString(otaDisplayStage, screenW / 2, 53);

        const int16_t barX = 20;
        const int16_t barY = 78;
        const int16_t barW = screenW - 40;
        ui.drawRoundRect(barX, barY, barW, 14, 5, COLOR_MUTED);
        if (otaDisplayPercent >= 0) {
            const int16_t filled = static_cast<int16_t>(
                (barW - 4) * constrain(otaDisplayPercent, 0, 100) / 100);
            if (filled > 0)
                ui.fillRoundRect(barX + 2, barY + 2, filled, 10, 4,
                                 otaDisplayStage == "ERROR" ? COLOR_RED : COLOR_CYAN);
            ui.setTextSize(1);
            ui.setTextColor(TFT_WHITE);
            ui.drawString(String(otaDisplayPercent) + "%", screenW / 2, 105);
        } else {
            const uint8_t activeDot = (millis() / 180) % 3;
            for (uint8_t dot = 0; dot < 3; ++dot)
                ui.fillCircle(screenW / 2 - 14 + dot * 14, 105, 3,
                              dot == activeDot ? COLOR_CYAN : COLOR_MUTED);
        }
        ui.setTextSize(1);
        ui.setTextColor(COLOR_AMBER);
        ui.drawString("KEEP POWER ON", screenW / 2, screenH - 10);
        ui.setTextDatum(top_left);
        ui.pushSprite(0, 0);
        return;
    }
    drawMesWatermark(ui, (screenW - MES_LOGO_W * MES_LOGO_SCALE) / 2, 31);

    // One global RSE command keeps six-inverter tiles uncluttered.
    ui.fillRect(0, 0, screenW, 18, COLOR_NAVY);
    ui.setTextDatum(middle_left);
    ui.setTextSize(1);
    ui.setTextColor(TFT_WHITE);
    const bool invalid = activePercent < 0;
    const float resTarget = invalid ? 0.0f : static_cast<float>(activePercent);
    const float resDifference = resTarget - displayedResPercent;
    if (applyInProgress) displayedResPercent = resTarget;
    else {
        displayedResPercent += resDifference * 0.13f;
        if (fabsf(resDifference) < 0.15f) displayedResPercent = resTarget;
    }
    const uint8_t displayedResValue = static_cast<uint8_t>(
        constrain(static_cast<int>(displayedResPercent + 0.5f), 0, 100));
    ui.drawString("14a  RSE", 5, 9);
    ui.setTextColor(animatedLevelColor(displayedResPercent));
    ui.drawString(invalid ? "--" : String(displayedResValue) + "%", 58, 9);
    ui.setTextColor(COLOR_MUTED);
    ui.drawString("kW", 91, 9);
    ui.setTextDatum(middle_right);
    ui.drawString("v" APP_VERSION, screenW - 55, 9);
    const uint16_t modeColor = config.dryRun ? COLOR_CYAN : COLOR_AMBER;
    ui.fillRoundRect(screenW - 48, 1, 44, 16, 4, modeColor);
    ui.setTextDatum(middle_center);
    ui.setTextColor(TFT_BLACK);
    ui.drawString(config.dryRun ? "DRY" : "LIVE", screenW - 26, 9);

    uint8_t enabledCount = 0;
    uint8_t healthyCount = 0;
    uint8_t enabledIds[INVERTER_COUNT] = {};
    for (uint8_t i = 0; i < INVERTER_COUNT; ++i) {
        if (!config.inverters[i].enabled) continue;
        enabledIds[enabledCount] = i;
        ++enabledCount;
        if (inverterHealthy[i]) ++healthyCount;
    }

    if (enabledCount == 0) {
        // Keep the layout state coherent when the last tile is removed.
        tileLayoutInitialized = false;
        ui.fillRoundRect(4, 21, screenW - 8, screenH - 38, 6, COLOR_PANEL);
        drawCentered(ui, "NO INVERTER", 48, 2, COLOR_AMBER);
        drawCentered(ui, "Configure via USB", 75, 1, COLOR_MUTED);
    } else {
        uint8_t columns;
        uint8_t rows;
        if (enabledCount <= 3) {
            columns = enabledCount;
            rows = 1;
        } else if (enabledCount == 4) {
            columns = 2;
            rows = 2;
        } else {
            columns = 3;
            rows = 2;
        }
        constexpr int16_t gridX = 4;
        constexpr int16_t gridY = 21;
        constexpr int16_t gap = 3;
        const int16_t gridW = screenW - 8;
        const int16_t gridH = screenH - gridY - 16;
        const int16_t cellW = (gridW - gap * (columns - 1)) / columns;
        const int16_t cellH = (gridH - gap * (rows - 1)) / rows;

        // Calculate the destination rectangle for every ID.  The rectangle is
        // animated rather than swapped immediately, so changing the selected
        // inverter count feels like a reflow instead of a flashing new screen.
        bool targetEnabled[INVERTER_COUNT] = {};
        float targetX[INVERTER_COUNT] = {};
        float targetY[INVERTER_COUNT] = {};
        for (uint8_t item = 0; item < enabledCount; ++item) {
            const uint8_t inverterIndex = enabledIds[item];
            targetEnabled[inverterIndex] = true;
            targetX[inverterIndex] = gridX + (item % columns) * (cellW + gap);
            targetY[inverterIndex] = gridY + (item / columns) * (cellH + gap);
        }
        for (uint8_t i = 0; i < INVERTER_COUNT; ++i) {
            TileLayoutAnimation& tile = tileLayout[i];
            const bool wasVisible = tile.width > 1.0f && tile.height > 1.0f;
            if (!tileLayoutInitialized) {
                tile.x = targetX[i];
                tile.y = targetY[i];
                tile.width = targetEnabled[i] ? cellW : 0;
                tile.height = targetEnabled[i] ? cellH : 0;
            } else if (targetEnabled[i] && !tile.targetEnabled && !wasVisible) {
                // A newly enabled inverter grows from the centre of its slot.
                tile.x = targetX[i] + cellW / 2;
                tile.y = targetY[i] + cellH / 2;
                tile.width = 0;
                tile.height = 0;
            }
            tile.targetEnabled = targetEnabled[i];
            tile.targetX = targetEnabled[i] ? targetX[i] : tile.x + tile.width / 2;
            tile.targetY = targetEnabled[i] ? targetY[i] : tile.y + tile.height / 2;
            tile.targetWidth = targetEnabled[i] ? cellW : 0;
            tile.targetHeight = targetEnabled[i] ? cellH : 0;
            tile.x += (tile.targetX - tile.x) * 0.18f;
            tile.y += (tile.targetY - tile.y) * 0.18f;
            tile.width += (tile.targetWidth - tile.width) * 0.18f;
            tile.height += (tile.targetHeight - tile.height) * 0.18f;
        }
        tileLayoutInitialized = true;

        // Shrinking cards are drawn first, underneath the active cards.
        for (uint8_t i = 0; i < INVERTER_COUNT; ++i) {
            const TileLayoutAnimation& tile = tileLayout[i];
            if (tile.targetEnabled || tile.width < 8 || tile.height < 8) continue;
            ui.drawRoundRect(static_cast<int16_t>(tile.x), static_cast<int16_t>(tile.y),
                             static_cast<int16_t>(tile.width),
                             static_cast<int16_t>(tile.height), 4, COLOR_MUTED);
        }
        for (uint8_t item = 0; item < enabledCount; ++item) {
            const uint8_t inverterIndex = enabledIds[item];
            const TileLayoutAnimation& tile = tileLayout[inverterIndex];
            const int16_t x = static_cast<int16_t>(tile.x);
            const int16_t y = static_cast<int16_t>(tile.y);
            const int16_t drawW = static_cast<int16_t>(tile.width);
            const int16_t drawH = static_cast<int16_t>(tile.height);
            if (drawW < 12 || drawH < 18) continue;
            // During the smooth grid reflow, a new card begins at zero size.
            // Do not draw its labels until it is at least as large as the
            // smallest normal (six-inverter) card.  This prevents text from
            // briefly spilling into a neighbouring card while it grows.
            const bool hasRoomForText = drawW >= 58 && drawH >= 40;
            const bool hasReadback = inverterHasReadback[inverterIndex];
            const bool confirmedFault =
                inverterFailureStreak[inverterIndex] >= HEALTH_FAILURE_THRESHOLD;
            const bool transientFailure = inverterFailureStreak[inverterIndex] > 0 &&
                                          !confirmedFault;
            // A single lost reply is a CHECK/amber condition, not a red
            // fault.  A confirmed readback that differs from the target is
            // still a real control fault and is red immediately.
            const bool controlFault = !config.dryRun && activePercent >= 0 &&
                                      !inverterAtTarget[inverterIndex] &&
                                      inverterHasReadback[inverterIndex];
            const bool tileFault = confirmedFault || controlFault;
            const uint32_t maximum = config.inverters[inverterIndex].maxPvPowerW;
            const float resPercent = invalid ? 0.0f : displayedResPercent;
            const uint8_t invPercent = hasReadback
                ? percentOfMaximum(lastReadback[inverterIndex], maximum) : 0;
            const uint16_t resColor = animatedLevelColor(resPercent);
            const float targetFill = hasReadback && !confirmedFault
                ? static_cast<float>(invPercent) : 0.0f;
            const float fillDifference = targetFill - displayedInvPercent[inverterIndex];
            displayedInvPercent[inverterIndex] += fillDifference * 0.16f;
            if (fabsf(fillDifference) < 0.15f) {
                displayedInvPercent[inverterIndex] = targetFill;
            }
            const uint16_t animatedInvColor = animatedLevelColor(
                displayedInvPercent[inverterIndex]);

            // Each tile is one gauge: INV is the filled level and RES is the
            // dashed setpoint. Faults deliberately dominate the complete tile.
            const int16_t radius = min<int16_t>(5, min<int16_t>(drawW / 4, drawH / 4));
            if (tileFault) {
                ui.fillRoundRect(x, y, drawW, drawH, radius, COLOR_RED);
            }
            if (hasReadback && !confirmedFault) {
                drawLiquidFill(ui, x, y, drawW, drawH,
                               displayedInvPercent[inverterIndex],
                               controlFault ? TFT_RED : animatedInvColor,
                               inverterIndex, now);
            }
            ui.drawRoundRect(x, y, drawW, drawH, radius,
                             tileFault ? TFT_RED
                                 : ((transientFailure || !hasReadback)
                                        ? COLOR_AMBER : animatedInvColor));

            if (!invalid) {
                const int16_t innerH = drawH - 4;
                const int16_t lineY = y + drawH - 2 -
                    static_cast<int16_t>((static_cast<int32_t>(innerH) *
                                          resPercent) / 100);
                drawDashedLine(ui, x + 3, lineY, drawW - 6,
                               confirmedFault ? TFT_WHITE : resColor);
            }

            if (!hasRoomForText) continue;

            const int16_t centerX = x + drawW / 2;
            const uint32_t animatedInvWatts = static_cast<uint32_t>(
                (static_cast<float>(maximum) * displayedInvPercent[inverterIndex]) /
                100.0f + 0.5f);
            const String invNumber = confirmedFault ? "ERR"
                : (hasReadback ? compactKilowattsValue(animatedInvWatts)
                               : "--");
            const bool largeText = enabledCount <= 2 && drawH >= 70;
            const uint8_t textSize = largeText ? 2 : 1;
            drawCellText(ui, (controlFault ? "!" : "") +
                             String("ID") + String(inverterIndex + 1), centerX,
                         y + (drawH < 60 ? 6 : 10), textSize);
            if (!confirmedFault) {
                const int16_t valueCenter = y + drawH / 2 +
                    (drawH < 60 ? 2 : 4);
                const int16_t valueGap = largeText ? 15 : 7;
                drawCellText(ui, "R: " + (invalid ? String("--")
                                                     : String(displayedResValue) + "%"),
                             centerX, valueCenter - valueGap, textSize);
                // Keep the unit visible in every layout.  The compact number
                // formatting keeps this readable even in the six-tile view.
                const String invText = invNumber + "kW";
                drawCellText(ui, invText, centerX,
                             valueCenter + valueGap, largeText ? 2.0f : 1.25f);
            } else {
                drawCellText(ui, "ERROR", centerX, y + drawH / 2,
                             largeText ? 3 : 1);
            }
        }
    }

    ui.fillRect(0, screenH - 13, screenW, 13, COLOR_PANEL);
    ui.setTextDatum(middle_left);
    ui.setTextColor(COLOR_MUTED);
    ui.drawString(applyInProgress
                      ? "SYNC " + String(applyProgress) + "/" + String(applyTotal)
                      : "R=RES  I=INV",
                  5, screenH - 7);
    ui.setTextDatum(middle_right);
    ui.setTextColor(enabledCount > 0 && healthyCount == enabledCount
                        ? COLOR_GREEN : COLOR_AMBER);
    ui.drawString("485 " + String(healthyCount) + "/" + String(enabledCount) +
                  (eventLog.available() ? "  SD" : ""), screenW - 5,
                  screenH - 7);
    ui.pushSprite(0, 0);

    M5StamPLC.setStatusLight(invalid || !outputHealthy ? 255 : 0,
                             !invalid && outputHealthy ? 255 : 0,
                             config.dryRun ? 255 : 0);
}
}  // namespace

void setup() {
    Serial.begin(115200);
    delay(300);
    config.load();

    auto boardConfig = M5StamPLC.config();
    boardConfig.enableModbusSlave = false;
    boardConfig.enableSdCard = true;
    M5StamPLC.config(boardConfig);
    M5StamPLC.begin();
    M5StamPLC.setBacklight(true);
    prepareOtaBootValidation();
    otaManager.begin();

    eventLog.begin();
    rawRseMask = readRseMask();
    stableRseMask = rawRseMask;
    rawChangedAt = millis();
    otaManager.setSafetyCheck([] {
        return !applyInProgress && readRseMask() == stableRseMask;
    });
    otaManager.setProgressCallback([](const String& stage, int percent) {
        otaDisplayStage = stage;
        otaDisplayPercent = percent;
        otaDisplayUpdatedAt = millis();
        updateDisplay(true);
    });
    recordSystem("boot", "firmware started");
    if (!otaBootValidationPending) {
        startInverterControl();
    } else {
        recordSystem("ota_boot", "inverter access held until self-test passes");
        updateDisplay(true);
    }
    printHelp();
    printStatus();
}

void loop() {
    M5StamPLC.update();
    serviceOtaBootValidation();
    if (!inverterControlStarted) {
        if (!otaBootValidationPending) startInverterControl();
        // Do not accept USB commissioning commands or touch RS485 while a new
        // OTA image is still inside its rollback validation window.
        updateDisplay();
        delay(2);
        return;
    }
    serviceUsb();

    const uint8_t now = readRseMask();
    if (now != rawRseMask) {
        rawRseMask = now;
        rawChangedAt = millis();
    }
    if (rawRseMask != stableRseMask &&
        millis() - rawChangedAt >= config.debounceMs) {
        stableRseMask = rawRseMask;
        handleStableRseState(stableRseMask, "rse_transition");
    }
    // The round-robin FC03 poll below is the only periodic inverter access:
    // one enabled ID every two seconds, read-only, and never an auto-write.
    pollNextEnabledInverter();
    otaManager.service(activePercent >= 0 && !applyInProgress &&
                       millis() - rawChangedAt >= config.debounceMs + 2000);
    if (!otaDisplayStage.isEmpty() &&
        (otaDisplayStage == "ERROR" || otaDisplayStage == "UP TO DATE" ||
         otaDisplayStage == "UPDATE FOUND") &&
        millis() - otaDisplayUpdatedAt > 3000) {
        otaDisplayStage = "";
        otaDisplayPercent = -1;
    }
    updateDisplay();
    delay(2);
}
