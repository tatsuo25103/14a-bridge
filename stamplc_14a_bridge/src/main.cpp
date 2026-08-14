#include <Arduino.h>
#include <M5StamPLC.h>
#include <Preferences.h>
#include <SD.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <sdkconfig.h>
#include <time.h>
#include <cstring>

#include "AppConfig.h"
#include "ControlPolicy.h"
#include "EventLog.h"
#include "ModbusRtuMaster.h"
#include "OtaManager.h"

#ifndef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "Production OTA requires ESP32 bootloader application rollback support"
#endif

namespace {
constexpr int8_t RS485_TX = 0;
constexpr int8_t RS485_RX = 39;
constexpr int8_t RS485_DIR = 46;
constexpr uint8_t INVERTER_COUNT = 6;
constexpr uint8_t FIRST_INVERTER_ID = 2;
constexpr uint8_t LAST_INVERTER_ID =
    FIRST_INVERTER_ID + INVERTER_COUNT - 1;

constexpr uint8_t slaveIdForIndex(uint8_t index) {
    return FIRST_INVERTER_ID + index;
}

constexpr uint8_t indexForSlaveId(uint8_t slaveId) {
    return slaveId - FIRST_INVERTER_ID;
}
static_assert(slaveIdForIndex(0) == 2 && slaveIdForIndex(5) == 7,
              "configured inverter IDs must remain 2-7");
static_assert(indexForSlaveId(2) == 0 && indexForSlaveId(7) == 5,
              "Modbus ID to configuration index mapping changed");
constexpr uint8_t HEALTH_FAILURE_THRESHOLD = 3;
constexpr uint32_t PERIODIC_READ_SLOT_MS = 2000;
constexpr uint32_t MODBUS_DEVICE_GAP_MS = 500;
constexpr uint32_t DISPLAY_FRAME_MS = 33;
constexpr uint32_t MANUAL_TEST_TIMEOUT_MS = 5UL * 60UL * 1000UL;
constexpr uint32_t MANUAL_ROLLBACK_HOLD_MS = 5000;

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
uint32_t lastAnimationAt = 0;
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
uint32_t displayedInvWatts[INVERTER_COUNT] = {};
float displayedResPercent = 0.0f;
bool applyInProgress = false;
uint8_t applyProgress = 0;
uint8_t applyTotal = 0;
bool otaBootValidationPending = false;
uint32_t otaBootValidationDeadline = 0;
bool inverterControlStarted = false;
bool manualTestActive = false;
uint32_t manualTestDeadline = 0;
bool watchdogSubscribed = false;
bool controlReapplyRequired = false;
uint32_t lastCriticalInputPollAt = 0;
bool manualRollbackHoldActive = false;
bool manualRollbackGestureBlocked = false;
uint32_t manualRollbackHoldStartedAt = 0;
String manualRollbackTargetVersion;
String manualRollbackNotice;
uint32_t manualRollbackNoticeUntil = 0;

void updateDisplay(bool force = false);
void responsiveDelay(uint32_t durationMs);
void serviceTimeCriticalInputs();
uint8_t readRseMask();
void probeEnabledInverters(const String& event);
void handleStableRseState(uint8_t mask, const String& reason);
void startInverterControl();
void returnToPhysicalRse(const String& reason);
bool safeForFirmwareUpdate();
void serviceManualRollbackGesture();

const char* rseProfileText(RseProfile profile) {
    switch (profile) {
        case RseProfile::StrictOneHot4: return "STRICT_4";
        case RseProfile::Westnetz4: return "WESTNETZ_4";
        case RseProfile::EweHold4: return "EWE_HOLD_4";
        case RseProfile::FnnEza3: return "FNN_EZA_3";
        default: return "INVALID";
    }
}

bool parseRseProfile(String value, RseProfile& profile) {
    value.trim();
    value.toLowerCase();
    if (value == "strict" || value == "strict_4") {
        profile = RseProfile::StrictOneHot4;
    } else if (value == "westnetz" || value == "westnetz_4") {
        profile = RseProfile::Westnetz4;
    } else if (value == "ewe" || value == "ewe_hold_4") {
        profile = RseProfile::EweHold4;
    } else if (value == "fnn" || value == "fnn_eza_3" ||
               value == "netze_bw") {
        profile = RseProfile::FnnEza3;
    } else {
        return false;
    }
    return true;
}

void clearOtaBootState() {
    Preferences p;
    if (!p.begin("bridgeboot", false)) return;
    p.clear();
    p.end();
}

bool loadManualRollbackTarget(uint8_t& subtype, String& version,
                              const esp_partition_t*& partition) {
    subtype = 0xFF;
    version = "";
    partition = nullptr;
    Preferences p;
    // Open read/write so a brand-new unit creates the empty namespace once
    // instead of emitting an NVS NOT_FOUND error on every status request.
    if (!p.begin("bridgeprev", false)) return false;
    if (!p.isKey("subtype") || !p.isKey("version")) {
        p.end();
        return false;
    }
    subtype = p.getUChar("subtype", 0xFF);
    version = p.getString("version", "");
    uint8_t recordedSha[32] {};
    const bool shaValid = p.isKey("sha") &&
        p.getBytesLength("sha") == sizeof(recordedSha) &&
        p.getBytes("sha", recordedSha, sizeof(recordedSha)) ==
            sizeof(recordedSha);
    p.end();
    if (subtype == 0xFF || version.isEmpty() || !shaValid) return false;

    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running || static_cast<uint8_t>(running->subtype) == subtype)
        return false;
    partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        static_cast<esp_partition_subtype_t>(subtype), nullptr);
    if (!partition) return false;

    esp_app_desc_t description {};
    if (esp_ota_get_partition_description(partition, &description) != ESP_OK)
        return false;
    // APP_VERSION is our product version, while esp_app_desc_t::version is
    // supplied by the Arduino framework build. Bind the rollback record to
    // the immutable ELF SHA instead, so a stale record cannot select a
    // partition that a later OTA has overwritten.
    if (memcmp(recordedSha, description.app_elf_sha256,
               sizeof(recordedSha)) != 0 || version == APP_VERSION)
        return false;
    return true;
}

void showManualRollbackNotice(const String& message) {
    manualRollbackNotice = message;
    manualRollbackNoticeUntil = millis() + 3000;
    updateDisplay(true);
}

void performManualRollback() {
    uint8_t subtype = 0xFF;
    String version;
    const esp_partition_t* previous = nullptr;
    if (!loadManualRollbackTarget(subtype, version, previous)) {
        Serial.println("MANUAL ROLLBACK STATUS=ERROR DETAIL=no verified previous firmware");
        M5StamPLC.tone(1200, 300);
        showManualRollbackNotice("NO VALID BACKUP");
        return;
    }
    if (!safeForFirmwareUpdate()) {
        Serial.println("MANUAL ROLLBACK STATUS=ERROR DETAIL=requires stable physical 100%, LIVE, idle Modbus, and all enabled inverters ready");
        M5StamPLC.tone(1200, 300);
        showManualRollbackNotice("UNSAFE - USE LIVE 100%");
        return;
    }

    // Prevent the reverted device from automatically reinstalling the version
    // that the local operator just rejected. It can be enabled again in GUI.
    const bool autoDisabled = !otaManager.automatic() ||
        otaManager.setAutomatic(false);
    if (esp_ota_set_boot_partition(previous) != ESP_OK) {
        Serial.println("MANUAL ROLLBACK STATUS=ERROR DETAIL=backup image rejected");
        M5StamPLC.tone(1200, 300);
        showManualRollbackNotice("BACKUP REJECTED");
        return;
    }

    inverterControlStarted = false;
    otaDisplayStage = "ROLLBACK";
    otaDisplayPercent = -1;
    otaDisplayUpdatedAt = millis();
    updateDisplay(true);
    Serial.printf("MANUAL ROLLBACK STATUS=REBOOTING VERSION=%s AUTO_OTA=%s\r\n",
                  version.c_str(), autoDisabled ? "OFF" : "SAVE_ERROR");
    Serial.flush();
    M5StamPLC.tone(1800, 180);
    delay(350);
    ESP.restart();
}

void serviceManualRollbackGesture() {
    if (!manualRollbackNotice.isEmpty() &&
        static_cast<int32_t>(millis() - manualRollbackNoticeUntil) >= 0) {
        manualRollbackNotice = "";
        updateDisplay(true);
    }

    const bool aPressed = M5StamPLC.BtnA.isPressed();
    const bool bPressed = M5StamPLC.BtnB.isPressed();
    const bool cPressed = M5StamPLC.BtnC.isPressed();

    // B cancels and locks the gesture until all three buttons are released.
    // This makes A+B+C, or pressing B midway, incapable of causing rollback.
    if (bPressed) {
        manualRollbackHoldActive = false;
        manualRollbackGestureBlocked = true;
        return;
    }
    if (manualRollbackGestureBlocked) {
        if (!aPressed && !bPressed && !cPressed)
            manualRollbackGestureBlocked = false;
        return;
    }
    if (otaBootValidationPending || applyInProgress || !otaDisplayStage.isEmpty()) {
        manualRollbackHoldActive = false;
        return;
    }

    if (!(aPressed && cPressed)) {
        manualRollbackHoldActive = false;
        manualRollbackHoldStartedAt = 0;
        manualRollbackTargetVersion = "";
        return;
    }
    if (!manualRollbackHoldActive) {
        manualRollbackHoldActive = true;
        manualRollbackHoldStartedAt = millis();
        uint8_t subtype = 0xFF;
        const esp_partition_t* partition = nullptr;
        if (!loadManualRollbackTarget(subtype, manualRollbackTargetVersion,
                                      partition))
            manualRollbackTargetVersion = "UNAVAILABLE";
        updateDisplay(true);
        return;
    }
    if (millis() - manualRollbackHoldStartedAt >= MANUAL_ROLLBACK_HOLD_MS) {
        manualRollbackHoldActive = false;
        manualRollbackGestureBlocked = true;
        performManualRollback();
    }
}

void rollbackToPreviousApp(uint8_t previousSubtype, const String& reason) {
    otaDisplayStage = "ROLLBACK";
    otaDisplayPercent = -1;
    otaDisplayUpdatedAt = millis();
    updateDisplay(true);
    Serial.println("OTA ROLLBACK DETAIL=" + reason);

    // Prefer the ESP-IDF bootloader rollback state machine. If the new image
    // is still PENDING_VERIFY this marks it invalid and reboots directly into
    // the last valid application. On success this call does not return.
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t runningState = ESP_OTA_IMG_UNDEFINED;
    if (running &&
        esp_ota_get_state_partition(running, &runningState) == ESP_OK &&
        runningState == ESP_OTA_IMG_PENDING_VERIFY) {
        clearOtaBootState();
        Serial.println("OTA ROLLBACK STATUS=BOOTLOADER");
        Serial.flush();
        const esp_err_t nativeRollback =
            esp_ota_mark_app_invalid_rollback_and_reboot();
        Serial.printf("OTA ROLLBACK WARNING=native rollback returned 0x%X; using recorded partition\r\n",
                      static_cast<unsigned>(nativeRollback));
    }

    // Defensive fallback for a damaged or unexpected OTA state: select the
    // exact application partition recorded before Update.end() activated the
    // new image.
    const esp_partition_t* previous = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        static_cast<esp_partition_subtype_t>(previousSubtype), nullptr);
    if (previous && esp_ota_set_boot_partition(previous) == ESP_OK) {
        clearOtaBootState();
        Serial.println("OTA ROLLBACK STATUS=RECORDED_PARTITION");
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
    uint8_t recordedPreviousSubtype = 0xFF;
    String recordedPreviousVersion;
    uint8_t recordedPreviousSha[32] {};
    bool recordedPreviousShaValid = false;
    Preferences bootRecord;
    if (bootRecord.begin("bridgeboot", true)) {
        recordedPreviousSubtype = bootRecord.getUChar("previous", 0xFF);
        recordedPreviousVersion = bootRecord.getString("prevver", "");
        recordedPreviousShaValid =
            bootRecord.getBytesLength("prevsha") ==
                sizeof(recordedPreviousSha) &&
            bootRecord.getBytes("prevsha", recordedPreviousSha,
                                sizeof(recordedPreviousSha)) ==
                sizeof(recordedPreviousSha);
        bootRecord.end();
    }
    bool configValid = (config.modbusQuantity == 1 || config.modbusQuantity == 2) &&
                             config.modbusBaud >= 1200 &&
                             config.responseTimeoutMs >= 100 &&
                             config.verifyRetries >= 1 &&
                             config.verifyRetries <= MAX_WRITE_VERIFY_RETRIES;
#if defined(OTA_FAULT_INJECT_SELF_TEST_FAILURE)
    configValid = false;
    Serial.println("OTA FAULT INJECTION=SELF_TEST_FAILURE");
#endif
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
        Preferences p;
        uint8_t previousSubtype = 0xFF;
        if (p.begin("bridgeboot", true)) {
            previousSubtype = p.getUChar("previous", 0xFF);
            p.end();
        }
        rollbackToPreviousApp(
            previousSubtype,
            "could not mark new firmware valid: 0x" + String(validation, HEX));
        return;
    }
    bool previousSaved = false;
    if (recordedPreviousSubtype != 0xFF &&
        !recordedPreviousVersion.isEmpty() && recordedPreviousShaValid) {
        Preferences previous;
        if (previous.begin("bridgeprev", false)) {
            previous.clear();
            previousSaved = previous.putUChar("subtype", recordedPreviousSubtype) > 0;
            previousSaved = previous.putString("version", recordedPreviousVersion) > 0 &&
                previousSaved;
            previousSaved = previous.putBytes(
                "sha", recordedPreviousSha,
                sizeof(recordedPreviousSha)) == sizeof(recordedPreviousSha) &&
                previousSaved;
            if (!previousSaved) previous.clear();
            previous.end();
        }
    }
    clearOtaBootState();
    otaBootValidationPending = false;
    Serial.println("OTA BOOT STATUS=VALID VERSION=" + String(APP_VERSION));
    Serial.printf("MANUAL ROLLBACK RECORD=%s VERSION=%s\r\n",
                  previousSaved ? "SAVED" : "UNAVAILABLE",
                  recordedPreviousVersion.c_str());
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
constexpr uint16_t COLOR_BLUE = 0x249F;
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
        snprintf(buffer, sizeof(buffer), "uptime:%llu",
                 static_cast<unsigned long long>(esp_timer_get_time() / 1000000ULL));
    } else {
        strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &t);
    }
    return buffer;
}

uint32_t calendarDayNumber(int year, unsigned month, unsigned day) {
    const int a = (14 - static_cast<int>(month)) / 12;
    const int y = year + 4800 - a;
    const int m = static_cast<int>(month) + 12 * a - 3;
    return static_cast<uint32_t>(day + (153 * m + 2) / 5 + 365 * y +
                                 y / 4 - y / 100 + y / 400 - 32045);
}

bool readLocalRtc(uint32_t& dayNumber, uint16_t& minuteOfDay,
                  uint8_t& second) {
    struct tm t {};
    M5StamPLC.getRtcTime(&t);
    const int year = t.tm_year + 1900;
    const unsigned month = static_cast<unsigned>(t.tm_mon + 1);
    const unsigned day = static_cast<unsigned>(t.tm_mday);
    if (year < 2024 || month < 1 || month > 12 || day < 1 || day > 31 ||
        t.tm_hour < 0 || t.tm_hour > 23 || t.tm_min < 0 || t.tm_min > 59 ||
        t.tm_sec < 0 || t.tm_sec > 59)
        return false;
    dayNumber = calendarDayNumber(year, month, day);
    minuteOfDay = static_cast<uint16_t>(t.tm_hour * 60 + t.tm_min);
    second = static_cast<uint8_t>(t.tm_sec);
    return true;
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
    return decodeRsePercent(mask, config.rseProfile);
}

uint32_t wattsForPercent(const InverterConfig& inverter, uint8_t percent) {
    return calculateFeedInLimit(inverter.maxPvPowerW,
                                inverter.inverterLimitW, percent);
}

bool controlEnabled(uint8_t index) {
    return index < INVERTER_COUNT && mayControlInverter(
        config.inverters[index].enabled,
        config.inverters[index].ratingVerified);
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
    controlReapplyRequired = false;
    bool allOk = true;
    bool superseded = false;
    uint8_t enabledCount = 0;
    uint8_t configuredCount = 0;
    uint8_t pendingCount = 0;
    for (uint8_t i = 0; i < INVERTER_COUNT; ++i) {
        if (config.inverters[i].enabled) {
            ++configuredCount;
            if (!config.inverters[i].ratingVerified) ++pendingCount;
        }
        if (controlEnabled(i)) ++enabledCount;
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
        if (!controlEnabled(i)) continue;
        serviceTimeCriticalInputs();
        if (rawRseMask != stableRseMask &&
            elapsedAtLeast(millis(), rawChangedAt, config.debounceMs)) {
            superseded = true;
            break;
        }
        ++applyProgress;
        updateDisplay(true);
        const uint8_t slaveId = slaveIdForIndex(i);
        const uint32_t requested = wattsForPercent(config.inverters[i], percent);
        lastRequested[i] = requested;

        String detail;
        if (config.dryRun) {
            detail = "SAFE LOCK: no Modbus write";
        } else {
            // Never trust a cached readback when deciding whether a write can
            // be skipped: another controller may have changed the inverter.
            // A fresh FC03 avoids unnecessary FC16 writes without sacrificing
            // correctness.
            ModbusResult result = modbus.readRaw(
                slaveId, config.modbusRegister, config.modbusQuantity,
                config.responseTimeoutMs);
            serviceTimeCriticalInputs();
            const bool rseSuperseded = rawRseMask != stableRseMask &&
                elapsedAtLeast(millis(), rawChangedAt, config.debounceMs);
            if (rseSuperseded) {
                superseded = true;
                detail = "RSE changed during pre-write verification";
            } else if (result.ok && result.value == requested) {
                inverterAtTarget[i] = true;
                detail = "fresh readback already at target; write skipped";
            } else {
                result = modbus.writeAndVerify(
                    slaveId, config.modbusRegister, config.modbusQuantity,
                    requested, config.responseTimeoutMs, config.verifyRetries);
                detail = result.detail;
            }
            updateInverterHealth(i, result);
            inverterAtTarget[i] = result.ok && result.value == requested;
            // Preserve the last verified value across transient timeouts. A
            // timeout is not evidence that the inverter suddenly contains 0.
            if (result.ok) {
                lastReadback[i] = result.value;
                inverterHasReadback[i] = true;
            }
            allOk = allOk && result.ok && !superseded;
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
            if (controlEnabled(next)) {
                hasNextEnabled = true;
                break;
            }
        }
        if (hasNextEnabled) responsiveDelay(MODBUS_DEVICE_GAP_MS);
        if (rawRseMask != stableRseMask &&
            elapsedAtLeast(millis(), rawChangedAt, config.debounceMs)) {
            superseded = true;
            break;
        }
    }

    outputHealthy = !superseded && configuredCount > 0 &&
                    pendingCount == 0 && enabledCount == configuredCount && allOk;
    applyInProgress = false;
    if (superseded) {
        // The old batch may have updated only some IDs. Force a complete pass
        // for whichever physical RSE state is stable when control returns to
        // the main loop, even if the input briefly changed and came back.
        controlReapplyRequired = true;
        manualTestActive = false;
        manualTestDeadline = 0;
        lastResult = "RSE command superseded; applying newer physical input";
    } else if (enabledCount == 0) {
        lastResult = pendingCount > 0
            ? String(pendingCount) + " configured inverter(s) pending validation"
            : "No inverter IDs are enabled";
        recordSystem(reason, lastResult);
    } else {
        lastResult = pendingCount > 0
            ? String(enabledCount) + " controllable; " + String(pendingCount) +
                  " pending validation"
            : config.dryRun
            ? "SAFE LOCK level " + String(percent) + "% logged"
            : (allOk ? "All enabled inverters verified at " + String(percent) + "%"
                     : "One or more inverter writes failed");
    }
    Serial.println("RESULT " + lastResult);
    // Pending commissioning is an amber condition, not an audible hard
    // fault. Sound only when a verified inverter transaction actually fails.
    if (!allOk && !superseded) M5StamPLC.tone(1800, 250);
    updateDisplay(true);
    return outputHealthy;
}

void handleStableRseState(uint8_t mask, const String& reason) {
    const int16_t decoded = decodePercent(mask);
    if (decoded == RSE_HOLD_LAST) {
        lastResult = "RSE profile holds last valid output for mask 0x";
        if (mask < 0x10) lastResult += "0";
        lastResult += String(mask, HEX);
        recordSystem(reason, lastResult);
        Serial.println("RSE HOLD " + lastResult);
        updateDisplay(true);
        return;
    }
    if (decoded == RSE_INVALID) {
        activePercent = -1;
        outputHealthy = false;
        char message[96];
        snprintf(message, sizeof(message),
                 "Invalid RSE mask 0x%02X for profile %s; outputs unchanged",
                 mask, rseProfileText(config.rseProfile));
        lastResult = message;
        recordSystem(reason, lastResult);
        Serial.println("ALARM " + lastResult);
        M5StamPLC.tone(2200, 500);
        return;
    }
    if (rseMaskNeedsWarning(mask, config.rseProfile)) {
        Serial.printf("RSE WARNING PROFILE=%s MASK=0x%02X DETAIL=multiple contacts; profile priority applied\r\n",
                      rseProfileText(config.rseProfile), mask);
        recordSystem("rse_multi_contact",
                     "multiple contacts; profile priority applied");
    }
    applyLevel(static_cast<uint8_t>(decoded), reason);
}

const char* controlModeText() {
    if (manualTestActive) return "TEST";
    // Kept only as a legacy commissioning safety lock. Normal customer
    // operation uses LIVE, TEST, and the dedicated OTA screen.
    if (config.dryRun) return "SAFE";
    return "LIVE";
}

void returnToPhysicalRse(const String& reason) {
    manualTestActive = false;
    manualTestDeadline = 0;
    rawRseMask = readRseMask();
    stableRseMask = rawRseMask;
    rawChangedAt = millis();
    handleStableRseState(stableRseMask, reason);
}

void refreshOutputHealthFromCommunication();

bool probeInverter(uint8_t slaveId, const String& event = "usb_probe") {
    modbus.begin(config.modbusBaud);
    const ModbusResult result = modbus.readRaw(
        slaveId, config.modbusRegister, config.modbusQuantity,
        config.responseTimeoutMs);
    const uint8_t index = indexForSlaveId(slaveId);
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
    for (uint8_t id = FIRST_INVERTER_ID; id <= LAST_INVERTER_ID; ++id) {
        if (!config.inverters[indexForSlaveId(id)].enabled) continue;
        ++count;
        if (probeInverter(id, event)) ++passed;
        responsiveDelay(MODBUS_DEVICE_GAP_MS);
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
    Serial.println("SCAN START (read-only FC03; IDs 2-7)");
    modbus.begin(config.modbusBaud);
    for (uint8_t id = FIRST_INVERTER_ID; id <= LAST_INVERTER_ID; ++id) {
        const uint8_t index = indexForSlaveId(id);
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
        if (id < LAST_INVERTER_ID) responsiveDelay(MODBUS_DEVICE_GAP_MS);
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
        if (!config.inverters[i].ratingVerified) {
            allHealthy = false;
            continue;
        }
        const bool controlOk = config.dryRun || inverterAtTarget[i];
        allHealthy = allHealthy && inverterHealthy[i] && controlOk;
    }
    outputHealthy = anyEnabled && allHealthy;
}

bool enabledInvertersReadyForOta() {
    for (uint8_t i = 0; i < INVERTER_COUNT; ++i) {
        if (!config.inverters[i].enabled) continue;
        if (!config.inverters[i].ratingVerified) return false;
        if (!config.dryRun && (!inverterHealthy[i] || !inverterAtTarget[i]))
            return false;
    }
    return true;
}

bool safeForFirmwareUpdate() {
    const uint8_t physicalMask = readRseMask();
    const bool stable = physicalMask == rawRseMask &&
        physicalMask == stableRseMask &&
        elapsedAtLeast(millis(), rawChangedAt, config.debounceMs + 2000);
    return otaControlStateIsSafe(
        manualTestActive, applyInProgress, decodePercent(physicalMask), stable,
        enabledInvertersReadyForOta());
}

void pollNextEnabledInverter() {
    uint8_t enabledCount = 0;
    for (uint8_t i = 0; i < INVERTER_COUNT; ++i)
        if (controlEnabled(i)) ++enabledCount;
    if (enabledCount == 0) return;
    const uint32_t slotMs = max<uint32_t>(
        PERIODIC_READ_SLOT_MS, config.periodicVerifyMs / enabledCount);
    if (!elapsedAtLeast(millis(), lastPeriodicReadAt, slotMs)) return;
    lastPeriodicReadAt = millis();

    for (uint8_t checked = 0; checked < INVERTER_COUNT; ++checked) {
        const uint8_t index = periodicReadCursor;
        periodicReadCursor = (periodicReadCursor + 1) % INVERTER_COUNT;
        if (!controlEnabled(index)) continue;

        modbus.begin(config.modbusBaud);
        const uint8_t previousFailures = inverterFailureStreak[index];
        const bool wasConfirmedFault =
            previousFailures >= HEALTH_FAILURE_THRESHOLD;
        const ModbusResult result = modbus.readRaw(
            slaveIdForIndex(index), config.modbusRegister, config.modbusQuantity,
            config.responseTimeoutMs);
        updateInverterHealth(index, result);
        if (result.ok && !config.dryRun && activePercent >= 0) {
            const uint32_t expected = wattsForPercent(
                config.inverters[index], static_cast<uint8_t>(activePercent));
            inverterAtTarget[index] = result.value == expected;
        }

        if (result.ok && previousFailures > 0) {
            const String detail = wasConfirmedFault
                ? "communication recovered" : "transient read failure cleared";
            Serial.printf("HEALTH ID=%u STATUS=OK DETAIL=%s\r\n",
                          slaveIdForIndex(index), detail.c_str());
            eventLog.append(timestampNow(), "periodic_recovered", stableRseMask,
                            activePercent, slaveIdForIndex(index),
                            config.inverters[index].maxPvPowerW,
                            lastRequested[index], result.value, detail);
        } else if (!result.ok &&
                   inverterFailureStreak[index] == HEALTH_FAILURE_THRESHOLD) {
            Serial.printf("HEALTH ID=%u STATUS=ERROR DETAIL=%s after %u consecutive failures\r\n",
                          slaveIdForIndex(index), result.detail.c_str(),
                          HEALTH_FAILURE_THRESHOLD);
            eventLog.append(timestampNow(), "periodic_offline", stableRseMask,
                            activePercent, slaveIdForIndex(index),
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
    Serial.println("  probe <2-7>                  FC03-read one inverter ID");
    Serial.println("  id <2-7> <on|off>            Enable/disable an inverter ID");
    Serial.println("  max <2-7> <watts>            Set installed PV power for an ID");
    Serial.println("  limit <2-7> <watts> CONFIRM  Save inverter nameplate ceiling; no Modbus write");
    Serial.println("  time <YYYY-MM-DD HH:MM:SS>    Set the onboard RTC");
    Serial.println("  test <100|60|30|0>            Test level (safe in dry-run)");
    Serial.println("  test <level> CONFIRM          Test level when live");
    Serial.println("  apply                         Reapply current RSE level in dry-run");
    Serial.println("  apply CONFIRM                 Reapply current RSE level when live");
    Serial.println("  baud <rate>                  Set RS485 baud rate");
    Serial.println("  reg <hex|decimal>             Set power-limit register");
    Serial.println("  rse profile <name> CONFIRM    strict, westnetz, ewe, or fnn");
    Serial.println("  wifi show                     Show Wi-Fi and OTA state");
    Serial.println("  wifi sethex <ssid> <password> Save UTF-8 values encoded as hexadecimal");
    Serial.println("  wifi connect                  Start/retry Wi-Fi connection");
    Serial.println("  wifi clear CONFIRM            Erase saved Wi-Fi credentials");
    Serial.println("  ota auto <on|off>             Enable/disable automatic OTA updates");
    Serial.println("  ota time <HH:MM>              Set daily automatic OTA window");
    Serial.println("  ota check                     Check the latest GitHub release");
    Serial.println("  ota update CONFIRM            Verify and install the latest firmware");
#if defined(OTA_FAULT_TEST)
    Serial.println("  rollback test CONFIRM         Test-only: invoke verified rollback path");
#endif
    Serial.println("  Hold A+C for 5 seconds        Boot verified previous firmware (B cancels)");
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

void printManualRollbackDiagnostic(const char* prefix) {
    uint8_t subtype = 0xFF;
    String version;
    const esp_partition_t* partition = nullptr;
    const bool available = loadManualRollbackTarget(subtype, version, partition);
    Serial.printf("%sMANUAL ROLLBACK AVAILABLE=%s VERSION=%s GESTURE=A+C_5S B=CANCEL\r\n",
                  prefix, available ? "yes" : "no",
                  available ? version.c_str() : "--");
}

void printStatus() {
    Serial.println();
    Serial.println("=== RSE BRIDGE STATUS ===");
    Serial.printf("Time: %s\r\n", timestampNow().c_str());
    Serial.printf("RSE DI mask: 0x%02X  level: ", stableRseMask);
    const int16_t physicalPercent = decodePercent(stableRseMask);
    if (physicalPercent >= 0) Serial.printf("%d%%\r\n", physicalPercent);
    else Serial.println(physicalPercent == RSE_HOLD_LAST ? "HOLD" : "INVALID");
    Serial.printf("RSE Profile: %s\r\n", rseProfileText(config.rseProfile));
    Serial.printf("Output: ");
    if (activePercent >= 0) Serial.printf("%d%%  source: %s\r\n",
                                         activePercent, controlModeText());
    else Serial.println("UNCHANGED  source: INVALID RSE");
    Serial.printf("Mode: %s  RS485: %lu baud  register: 0x%04X  quantity: %u\r\n",
                  controlModeText(),
                  config.modbusBaud, config.modbusRegister,
                  config.modbusQuantity);
    Serial.printf("Firmware: %s  WiFi saved: %s connected: %s auto OTA: %s IP: %s RSSI: %ld\r\n",
                  APP_VERSION, otaManager.hasCredentials() ? "yes" : "no",
                  otaManager.connected() ? "yes" : "no",
                  otaManager.automatic() ? "yes" : "no",
                  otaManager.ipAddress().c_str(), static_cast<long>(otaManager.rssi()));
    printOtaDiagnostic("");
    printManualRollbackDiagnostic("");
    Serial.println("ID  Enabled  PV(W)  InvLimit  100%    60%    30%     0%  LastReq  Readback  OK");
    for (uint8_t i = 0; i < INVERTER_COUNT; ++i) {
        const InverterConfig& inverter = config.inverters[i];
        const bool statusOk = inverterHealthy[i] &&
            (config.dryRun || activePercent < 0 || inverterAtTarget[i]);
        Serial.printf("%u   %-7s %8lu %8lu %6lu %6lu %6lu %6u %8lu %9lu  %s\r\n",
                      slaveIdForIndex(i), inverter.enabled ? "yes" : "no",
                      inverter.maxPvPowerW, inverter.inverterLimitW,
                      wattsForPercent(inverter, 100), wattsForPercent(inverter, 60),
                      wattsForPercent(inverter, 30), 0,
                      lastRequested[i], lastReadback[i],
                      statusOk ? "yes" : "no");
        Serial.printf("RATING ID=%u STATUS=%s\r\n", slaveIdForIndex(i),
                      config.inverters[i].ratingVerified ? "VERIFIED" : "PENDING");
    }
    Serial.printf("SD log: %s  Overall output: %s\r\n",
                  eventLog.available() ? "available" : "unavailable",
                  outputHealthy ? "OK" : "CHECK");
    Serial.println("Last result: " + lastResult);
    Serial.printf("HEALTH UPTIME=%llu HEAP=%lu MINHEAP=%lu RESET=%d\r\n",
                  static_cast<unsigned long long>(esp_timer_get_time() / 1000000ULL),
                  static_cast<unsigned long>(ESP.getFreeHeap()),
                  static_cast<unsigned long>(ESP.getMinFreeHeap()),
                  static_cast<int>(esp_reset_reason()));
    Serial.println("=========================\n");
}

void printGuiStatus() {
    Serial.printf("@ HEALTH UPTIME=%llu HEAP=%lu MINHEAP=%lu RESET=%d\r\n",
                  static_cast<unsigned long long>(esp_timer_get_time() / 1000000ULL),
                  static_cast<unsigned long>(ESP.getFreeHeap()),
                  static_cast<unsigned long>(ESP.getMinFreeHeap()),
                  static_cast<int>(esp_reset_reason()));
    Serial.printf("@ RSE DI mask: 0x%02X  level: ", stableRseMask);
    const int16_t physicalPercent = decodePercent(stableRseMask);
    if (physicalPercent >= 0) Serial.printf("%d%%\r\n", physicalPercent);
    else Serial.println(physicalPercent == RSE_HOLD_LAST ? "HOLD" : "INVALID");
    Serial.printf("@ RSE PROFILE=%s\r\n", rseProfileText(config.rseProfile));
    Serial.printf("@ Output: ");
    if (activePercent >= 0) Serial.printf("%d%%  source: %s\r\n",
                                         activePercent, controlModeText());
    else Serial.println("UNCHANGED  source: INVALID RSE");
    Serial.printf("@ Mode: %s  RS485: %lu baud  register: 0x%04X  quantity: %u\r\n",
                  controlModeText(), config.modbusBaud,
                  config.modbusRegister, config.modbusQuantity);
    Serial.printf("@ WIFI VERSION=%s SAVED=%s CONNECTED=%s AUTO=%s SSIDHEX=%s IP=%s RSSI=%ld\r\n",
                  APP_VERSION, otaManager.hasCredentials() ? "yes" : "no",
                  otaManager.connected() ? "yes" : "no",
                  otaManager.automatic() ? "yes" : "no",
                  otaManager.ssidHex().c_str(), otaManager.ipAddress().c_str(),
                  static_cast<long>(otaManager.rssi()));
    printOtaDiagnostic("@ ");
    printManualRollbackDiagnostic("@ ");
    for (uint8_t i = 0; i < INVERTER_COUNT; ++i) {
        const InverterConfig& inverter = config.inverters[i];
        const bool statusOk = inverterHealthy[i] &&
            (config.dryRun || activePercent < 0 || inverterAtTarget[i]);
        Serial.printf("@ %u   %-7s %8lu %8lu %6lu %6lu %6lu %6u %8lu %9lu  %s\r\n",
                      slaveIdForIndex(i), inverter.enabled ? "yes" : "no",
                      inverter.maxPvPowerW, inverter.inverterLimitW,
                      wattsForPercent(inverter, 100), wattsForPercent(inverter, 60),
                      wattsForPercent(inverter, 30), 0, lastRequested[i],
                      lastReadback[i], statusOk ? "yes" : "no");
        Serial.printf("@ RATING ID=%u STATUS=%s\r\n", slaveIdForIndex(i),
                      config.inverters[i].ratingVerified ? "VERIFIED" : "PENDING");
    }
}

bool validId(int id) {
    if (id >= FIRST_INVERTER_ID && id <= LAST_INVERTER_ID) return true;
    Serial.println("ERROR ID must be between 2 and 7");
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

    if (line.startsWith("rse profile ")) {
        String arguments = line.substring(12);
        const int separator = arguments.indexOf(' ');
        const String profileName = separator < 0
            ? arguments : arguments.substring(0, separator);
        String confirmation = separator < 0
            ? "" : arguments.substring(separator + 1);
        confirmation.trim();
        RseProfile requested;
        if (!parseRseProfile(profileName, requested) ||
            !confirmation.equalsIgnoreCase("CONFIRM")) {
            Serial.println("RSE PROFILE STATUS=ERROR DETAIL=use: rse profile <strict|westnetz|ewe|fnn> CONFIRM");
            return;
        }
        const RseProfile previous = config.rseProfile;
        config.rseProfile = requested;
        if (!saveConfig()) {
            config.rseProfile = previous;
            Serial.println("RSE PROFILE STATUS=ERROR DETAIL=configuration save failed; previous profile kept");
            return;
        }
        Serial.printf("RSE PROFILE STATUS=OK VALUE=%s DETAIL=saved; applying physical inputs\r\n",
                      rseProfileText(config.rseProfile));
        returnToPhysicalRse("rse_profile_changed");
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
        uint32_t localDay = 0;
        uint16_t localMinute = 0;
        uint8_t localSecond = 0;
        Serial.printf("OTA SCHEDULE=%02u:%02u WINDOW=60 CLOCK=%s\r\n",
                      otaManager.scheduleHour(), otaManager.scheduleMinute(),
                      readLocalRtc(localDay, localMinute, localSecond)
                          ? "OK" : "INVALID");
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
    int otaHour = -1;
    int otaMinute = -1;
    if (sscanf(line.c_str(), "ota time %d:%d", &otaHour, &otaMinute) == 2) {
        const bool valid = otaHour >= 0 && otaHour <= 23 &&
                           otaMinute >= 0 && otaMinute <= 59;
        const bool saved = valid && otaManager.setSchedule(
            static_cast<uint8_t>(otaHour), static_cast<uint8_t>(otaMinute));
        Serial.printf("OTA SCHEDULE STATUS=%s TIME=%02d:%02d DETAIL=%s\r\n",
                      saved ? "OK" : "ERROR", otaHour, otaMinute,
                      !valid ? "time must be 00:00..23:59" :
                      (saved ? "saved" : "NVS save failed"));
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
        if (!safeForFirmwareUpdate()) {
            Serial.println("OTA STATUS=ERROR DETAIL=requires stable physical 100%, LIVE, idle Modbus, and all enabled inverters ready");
            return;
        }
        String detail;
        const bool ok = otaManager.installUpdate(detail);
        Serial.printf("OTA STATUS=%s DETAIL=%s\r\n", ok ? "OK" : "ERROR", detail.c_str());
        return;
    }
#if defined(OTA_FAULT_TEST)
    if (line.equalsIgnoreCase("rollback test CONFIRM")) {
        Serial.println("MANUAL ROLLBACK TEST=INVOKED SOURCE=USB_TEST_ONLY");
        performManualRollback();
        return;
    }
#endif

    int limitId = 0;
    unsigned long limitWatts = 0;
    char limitConfirm[16] = {};
    if (sscanf(line.c_str(), "limit %d %lu %15s", &limitId, &limitWatts,
               limitConfirm) == 3) {
        if (!validId(limitId)) return;
        if (String(limitConfirm) != "CONFIRM" || limitWatts == 0 ||
            (config.modbusQuantity == 1 && limitWatts > 65535UL)) {
            Serial.println("LIMIT ERROR DETAIL=use: limit <2-7> <watts> CONFIRM");
            return;
        }
        const uint8_t index = indexForSlaveId(static_cast<uint8_t>(limitId));
        const InverterConfig previousConfig = config.inverters[index];
        config.inverters[index].inverterLimitW = limitWatts;
        config.inverters[index].ratingVerified = true;
        if (!saveConfig()) {
            config.inverters[index] = previousConfig;
            Serial.printf("LIMIT ID=%d STATUS=ERROR VALUE=%lu DETAIL=configuration save failed; previous ceiling kept\r\n",
                          limitId, previousConfig.inverterLimitW);
            return;
        }
        Serial.printf("LIMIT ID=%d STATUS=OK VALUE=%lu DETAIL=nameplate ceiling saved; no inverter register written\r\n",
                      limitId, limitWatts);
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
            Serial.println("COMMIT ERROR DETAIL=use: commit <2-7> <on|off> <pv-watts>");
            return;
        }
        const bool enabled = requestedState == "on";
        const uint8_t index = indexForSlaveId(static_cast<uint8_t>(commitId));
        if (!enabled) {
            const InverterConfig previousConfig = config.inverters[index];
            config.inverters[index].enabled = false;
            config.inverters[index].maxPvPowerW = commitWatts;
            if (!saveConfig()) {
                config.inverters[index] = previousConfig;
                Serial.printf("COMMIT ID=%d STATUS=ERROR CONFIG=%lu DETAIL=configuration save failed; previous configuration kept\r\n",
                              commitId, previousConfig.maxPvPowerW);
                return;
            }
            Serial.printf("COMMIT ID=%d STATUS=OK CONFIG=%lu DETAIL=disabled ID saved without inverter check\r\n",
                          commitId, commitWatts);
            return;
        }

        const InverterConfig originalConfig = config.inverters[index];
        // PV module capacity and inverter nameplate power are independent.
        // Changing only the PV capacity must never discard an already known
        // inverter ceiling or attempt to infer it from the live control
        // register (which may legitimately contain 0/30/60%).
        if (originalConfig.ratingVerified && originalConfig.inverterLimitW > 0) {
            config.inverters[index].enabled = true;
            config.inverters[index].maxPvPowerW = commitWatts;
            if (!saveConfig()) {
                config.inverters[index] = originalConfig;
                Serial.printf("COMMIT ID=%d STATUS=ERROR CONFIG=%lu DETAIL=configuration save failed; previous configuration kept\r\n",
                              commitId, originalConfig.maxPvPowerW);
                return;
            }
            Serial.printf("COMMIT ID=%d STATUS=OK CONFIG=%lu DETAIL=saved PV capacity; preserved inverter ceiling %lu W; no inverter write\r\n",
                          commitId, commitWatts, originalConfig.inverterLimitW);
            return;
        }

        const uint8_t physicalMask = readRseMask();
        if (!mayValidateInverterRating(config.dryRun, manualTestActive,
                                       decodePercent(physicalMask))) {
            config.inverters[index].enabled = true;
            config.inverters[index].maxPvPowerW = commitWatts;
            config.inverters[index].inverterLimitW = 0;
            config.inverters[index].ratingVerified = false;
            if (!saveConfig()) {
                config.inverters[index] = originalConfig;
                Serial.printf("COMMIT ID=%d STATUS=ERROR CONFIG=%lu DETAIL=pending configuration could not be saved\r\n",
                              commitId, originalConfig.maxPvPowerW);
                return;
            }
            Serial.printf("COMMIT ID=%d STATUS=PENDING CONFIG=%lu DETAIL=setting saved but excluded from control; rating validation requires LIVE with physical RSE at 100%%\r\n",
                          commitId, commitWatts);
            return;
        }

        // Validate only while physical LIVE is already at 100%. The candidate
        // can therefore never relax a lower 0/30/60% network limit.
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
            config.inverters[index].inverterLimitW = 0;
            config.inverters[index].ratingVerified = false;
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
        const uint32_t validationWatts = config.modbusQuantity == 1
            ? min<uint32_t>(commitWatts, 65535UL) : commitWatts;
        ModbusResult candidate = modbus.writeAndVerify(
            commitId, config.modbusRegister, config.modbusQuantity, validationWatts,
            config.responseTimeoutMs, 1);
        const auto restorePreviousRegister = [&]() {
            const ModbusResult restored = modbus.writeAndVerify(
                commitId, config.modbusRegister, config.modbusQuantity,
                previous.value, config.responseTimeoutMs, 1);
            if (!restored.ok) {
                updateInverterHealth(index, restored);
                Serial.printf("COMMIT ID=%d RESTORE=ERROR DETAIL=%s\r\n",
                              commitId, restored.detail.c_str());
            }
            return restored.ok;
        };
        if (decodePercent(readRseMask()) != 100 || manualTestActive || config.dryRun) {
            const bool restored = restorePreviousRegister();
            Serial.printf("COMMIT ID=%d STATUS=ERROR CONFIG=%lu DETAIL=physical RSE left LIVE 100%% during validation; candidate rejected; previous register %s\r\n",
                          commitId, originalConfig.maxPvPowerW,
                          restored ? "restored" : "restore failed");
            returnToPhysicalRse("rating_validation_rse_changed");
            return;
        }
        const InverterConfig previousConfig = config.inverters[index];
        if (candidate.ok) {
            config.inverters[index].enabled = true;
            config.inverters[index].maxPvPowerW = commitWatts;
            config.inverters[index].inverterLimitW = candidate.value;
            config.inverters[index].ratingVerified = true;
            if (!saveConfig()) {
                config.inverters[index] = previousConfig;
                const bool restored = restorePreviousRegister();
                Serial.printf("COMMIT ID=%d STATUS=ERROR CONFIG=%lu DETAIL=configuration save failed; previous configuration kept; previous register %s\r\n",
                              commitId, previousConfig.maxPvPowerW,
                              restored ? "restored" : "restore failed");
                return;
            }
            lastReadback[index] = candidate.value;
            inverterHasReadback[index] = true;
            inverterAtTarget[index] = true;
            Serial.printf("COMMIT ID=%d STATUS=OK CONFIG=%lu DETAIL=PV capacity saved; verified 100%% feed-in ceiling %lu W\r\n",
                          commitId, commitWatts, candidate.value);
            return;
        }
        if (candidate.communicationOk && candidate.value > 0) {
            // Keep the installed PV capacity unchanged. The clamped readback
            // is a separate verified inverter ceiling; only actual Modbus
            // targets are capped by it.
            config.inverters[index].enabled = true;
            config.inverters[index].maxPvPowerW = commitWatts;
            config.inverters[index].inverterLimitW = candidate.value;
            config.inverters[index].ratingVerified = true;
            if (!saveConfig()) {
                config.inverters[index] = previousConfig;
                const bool restored = restorePreviousRegister();
                Serial.printf("COMMIT ID=%d STATUS=ERROR CONFIG=%lu DETAIL=accepted limit %lu W but configuration save failed; previous register %s\r\n",
                              commitId, previousConfig.maxPvPowerW, candidate.value,
                              restored ? "restored" : "restore failed");
                return;
            }
            lastReadback[index] = candidate.value;
            inverterHasReadback[index] = true;
            inverterAtTarget[index] = true;
            Serial.printf("COMMIT ID=%d STATUS=CLAMPED CONFIG=%lu DETAIL=PV capacity kept at %lu W; verified inverter ceiling %lu W; percentage targets remain PV-based\r\n",
                          commitId, commitWatts, commitWatts, candidate.value);
            return;
        }
        const bool restored = restorePreviousRegister();
        Serial.printf("COMMIT ID=%d STATUS=ERROR CONFIG=%lu DETAIL=validation failed: %s; previous register %s\r\n",
                      commitId, config.inverters[index].maxPvPowerW,
                      candidate.detail.c_str(),
                      restored ? "restored" : "restore failed");
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
            Serial.println("ERROR use: id <2-7> <on|off>");
            return;
        }
        const uint8_t index = indexForSlaveId(static_cast<uint8_t>(id));
        const InverterConfig previous = config.inverters[index];
        config.inverters[index].enabled = requestedState == "on";
        if (!saveConfig()) config.inverters[index] = previous;
        return;
    }

    unsigned long watts = 0;
    if (sscanf(line.c_str(), "max %d %lu", &id, &watts) == 2) {
        if (!validId(id)) return;
        if (watts == 0) {
            Serial.println("ERROR maximum PV power must be greater than zero");
            return;
        }
        const uint8_t index = indexForSlaveId(static_cast<uint8_t>(id));
        const InverterConfig previous = config.inverters[index];
        config.inverters[index].maxPvPowerW = watts;
        if (previous.maxPvPowerW != watts) {
            config.inverters[index].inverterLimitW = 0;
            config.inverters[index].ratingVerified = false;
        }
        if (!saveConfig()) config.inverters[index] = previous;
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
        const uint8_t physicalMask = readRseMask();
        const int16_t physicalPercent = decodePercent(physicalMask);
        const bool physicalStable = physicalMask == rawRseMask &&
            physicalMask == stableRseMask &&
            elapsedAtLeast(millis(), rawChangedAt, config.debounceMs);
        if (!config.dryRun && (!physicalStable ||
            !manualTestRespectsPhysicalRse(static_cast<uint8_t>(percent),
                                           physicalPercent))) {
            Serial.printf("TEST STATUS=REJECTED PHYSICAL=%d REQUESTED=%d DETAIL=test may not relax or bypass the physical RSE command\r\n",
                          physicalPercent, percent);
            return;
        }
        manualTestActive = true;
        manualTestDeadline = millis() + MANUAL_TEST_TIMEOUT_MS;
        applyLevel(percent, "usb_manual_test");
        Serial.println("TEST STATUS=ACTIVE TIMEOUT=300 DETAIL=returns to physical RSE automatically");
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
        const bool previous = config.dryRun;
        config.dryRun = true;
        if (!saveConfig()) config.dryRun = previous;
        return;
    }
    if (line.equalsIgnoreCase("dryrun off CONFIRM")) {
        const bool previous = config.dryRun;
        config.dryRun = false;
        if (!saveConfig()) {
            config.dryRun = previous;
            Serial.println("ERROR LIVE control remains unchanged because configuration was not saved");
            return;
        }
        Serial.println("WARNING LIVE CONTROL ENABLED");
        // A manual test may have left activePercent at its test value.  As
        // soon as LIVE is armed, discard that temporary display/control state
        // and immediately evaluate the physical RSE inputs instead.
        returnToPhysicalRse("live_enabled_actual_rse");
        return;
    }

    unsigned long baud = 0;
    if (sscanf(line.c_str(), "baud %lu", &baud) == 1) {
        if (baud < 1200 || baud > 1000000) {
            Serial.println("ERROR baud must be between 1200 and 1000000");
            return;
        }
        const uint32_t previous = config.modbusBaud;
        config.modbusBaud = baud;
        if (!saveConfig()) config.modbusBaud = previous;
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
        const uint16_t previous = config.modbusRegister;
        config.modbusRegister = address;
        if (!saveConfig()) config.modbusRegister = previous;
        return;
    }

    if (line.equalsIgnoreCase("reset CONFIRM")) {
        Preferences p;
        if (p.begin("rsebridge", false)) {
            const bool cleared = p.clear();
            p.end();
            if (cleared) {
                Serial.println("OK configuration cleared; rebooting");
                delay(250);
                ESP.restart();
            } else {
                Serial.println("ERROR configuration reset failed; current settings kept");
            }
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

void formatCompactKilowatts(char* text, size_t capacity, uint32_t watts) {
    if (watts >= 100000) {
        snprintf(text, capacity, "%lu",
                 static_cast<unsigned long>((watts + 500) / 1000));
        return;
    }
    // Round to the displayed 0.1 kW instead of truncating. This keeps the
    // compact LCD value consistent with the exact watt readback in the GUI.
    const uint32_t roundedTenths = (watts + 50U) / 100U;
    snprintf(text, capacity, "%lu.%lu",
             static_cast<unsigned long>(roundedTenths / 10U),
             static_cast<unsigned long>(roundedTenths % 10U));
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

void drawCentered(M5Canvas& canvas, const char* text, int32_t y,
                  uint8_t size, uint16_t color) {
    canvas.setTextSize(size);
    canvas.setTextColor(color);
    canvas.setTextDatum(top_center);
    canvas.drawString(text, canvas.width() / 2, y);
    canvas.setTextDatum(top_left);
}

void drawCellText(M5Canvas& canvas, const char* text, int32_t x, int32_t y,
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
    const uint32_t animationElapsed = lastAnimationAt == 0
        ? DISPLAY_FRAME_MS : min<uint32_t>(now - lastAnimationAt, 120);
    lastAnimationAt = now;
    // Time-based easing keeps the motion speed constant even when a Modbus
    // transaction makes one frame arrive later than another.
    const float levelEase = min(0.42f, animationElapsed / 220.0f);
    const float layoutEase = min(0.46f, animationElapsed / 190.0f);
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

    if (manualRollbackHoldActive || !manualRollbackNotice.isEmpty()) {
        ui.fillScreen(TFT_BLACK);
        ui.fillRect(0, 0, screenW, 22, COLOR_AMBER);
        ui.setTextDatum(middle_center);
        ui.setTextColor(TFT_BLACK);
        ui.setTextSize(1);
        ui.drawString("LOCAL FIRMWARE ROLLBACK", screenW / 2, 11);
        if (manualRollbackHoldActive) {
            const uint32_t elapsed = millis() - manualRollbackHoldStartedAt;
            const uint32_t remainingMs = elapsed >= MANUAL_ROLLBACK_HOLD_MS
                ? 0 : MANUAL_ROLLBACK_HOLD_MS - elapsed;
            const uint8_t remainingSeconds =
                static_cast<uint8_t>((remainingMs + 999) / 1000);

            // Compact circular countdown designed for the 240x135 display.
            // The filled arc is the authoritative five-second progress while
            // the outer scanner and segmented ticks provide motion without
            // reducing the readability of the central number.
            const int16_t centerX = screenW / 2;
            const int16_t centerY = 70;
            constexpr int16_t ringOuter = 31;
            constexpr int16_t ringInner = 25;
            const float progress = constrain(
                static_cast<float>(elapsed) / MANUAL_ROLLBACK_HOLD_MS,
                0.0f, 1.0f);
            ui.setTextSize(1);
            ui.setTextColor(COLOR_AMBER);
            ui.drawString("HOLD A + C", centerX, 29);
            ui.drawCircle(centerX, centerY, ringOuter + 5, COLOR_NAVY);
            ui.drawCircle(centerX, centerY, ringOuter + 3, COLOR_CYAN);
            ui.fillArc(centerX, centerY, ringOuter, ringInner,
                       0.0f, 359.5f, COLOR_PANEL);
            if (progress > 0.002f)
                ui.fillArc(centerX, centerY, ringOuter, ringInner,
                           0.0f, progress * 359.5f, COLOR_AMBER);

            for (uint8_t tick = 0; tick < 16; ++tick) {
                const float angle = (tick * 22.5f - 90.0f) * DEG_TO_RAD;
                const int16_t x1 = centerX + static_cast<int16_t>(
                    cosf(angle) * (ringOuter + 7));
                const int16_t y1 = centerY + static_cast<int16_t>(
                    sinf(angle) * (ringOuter + 7));
                const int16_t x2 = centerX + static_cast<int16_t>(
                    cosf(angle) * (ringOuter + 9));
                const int16_t y2 = centerY + static_cast<int16_t>(
                    sinf(angle) * (ringOuter + 9));
                ui.drawLine(x1, y1, x2, y2,
                            tick <= static_cast<uint8_t>(progress * 15.0f)
                                ? COLOR_AMBER : COLOR_MUTED);
            }

            const float scannerAngle =
                (static_cast<float>((millis() / 7U) % 360U) - 90.0f) *
                DEG_TO_RAD;
            const int16_t scannerX = centerX + static_cast<int16_t>(
                cosf(scannerAngle) * (ringOuter + 3));
            const int16_t scannerY = centerY + static_cast<int16_t>(
                sinf(scannerAngle) * (ringOuter + 3));
            ui.fillCircle(scannerX, scannerY, 2, COLOR_CYAN);

            ui.setTextColor(TFT_WHITE);
            ui.setTextSize(3);
            ui.drawString(String(remainingSeconds), centerX, centerY - 1);
            ui.setTextSize(1);
            ui.setTextColor(COLOR_MUTED);
            ui.drawString("TARGET " + manualRollbackTargetVersion,
                          centerX, 111);
            ui.setTextColor(COLOR_RED);
            ui.drawString("B CANCELS", centerX, screenH - 7);
        } else {
            ui.setTextColor(COLOR_RED);
            ui.setTextSize(2);
            ui.drawString(manualRollbackNotice, screenW / 2, 66);
            ui.setTextSize(1);
            ui.setTextColor(COLOR_MUTED);
            ui.drawString("CURRENT FIRMWARE KEPT", screenW / 2, 96);
        }
        ui.setTextDatum(top_left);
        ui.pushSprite(0, 0);
        return;
    }

    if (!otaDisplayStage.isEmpty()) {
        ui.fillScreen(TFT_BLACK);
        ui.fillRect(0, 0, screenW, 22, COLOR_NAVY);
        ui.setTextDatum(middle_center);
        ui.setTextSize(1);
        ui.setTextColor(COLOR_BLUE);
        ui.drawString("SMARTPLC OTA  v" APP_VERSION, screenW / 2, 11);
        ui.setTextSize(2);
        ui.setTextColor(otaDisplayStage == "ERROR" ? COLOR_RED : COLOR_BLUE);
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
                                 otaDisplayStage == "ERROR" ? COLOR_RED : COLOR_BLUE);
            ui.setTextSize(1);
            ui.setTextColor(TFT_WHITE);
            ui.drawString(String(otaDisplayPercent) + "%", screenW / 2, 105);
        } else {
            const uint8_t activeDot = (millis() / 180) % 3;
            for (uint8_t dot = 0; dot < 3; ++dot)
                ui.fillCircle(screenW / 2 - 14 + dot * 14, 105, 3,
                              dot == activeDot ? COLOR_BLUE : COLOR_MUTED);
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
    displayedResPercent += resDifference * levelEase;
    if (fabsf(resDifference) < 0.15f) displayedResPercent = resTarget;
    const uint8_t displayedResValue = static_cast<uint8_t>(
        constrain(static_cast<int>(displayedResPercent + 0.5f), 0, 100));
    ui.drawString("14a  RSE", 5, 9);
    ui.setTextColor(animatedLevelColor(displayedResPercent));
    char headerPercent[6];
    if (invalid) snprintf(headerPercent, sizeof(headerPercent), "--");
    else snprintf(headerPercent, sizeof(headerPercent), "%u%%", displayedResValue);
    ui.drawString(headerPercent, 58, 9);
    ui.setTextColor(COLOR_MUTED);
    ui.drawString("kW", 91, 9);
    ui.setTextDatum(middle_right);
    ui.drawString("v" APP_VERSION, screenW - 55, 9);
    const uint16_t modeColor = manualTestActive ? COLOR_AMBER :
                               (config.dryRun ? COLOR_RED : COLOR_GREEN);
    ui.fillRoundRect(screenW - 48, 1, 44, 16, 4, modeColor);
    ui.setTextDatum(middle_center);
    ui.setTextColor(TFT_BLACK);
    ui.drawString(controlModeText(), screenW - 26, 9);

    uint8_t enabledCount = 0;
    uint8_t healthyCount = 0;
    uint8_t pendingCount = 0;
    bool displayHardFault = false;
    uint8_t enabledIds[INVERTER_COUNT] = {};
    for (uint8_t i = 0; i < INVERTER_COUNT; ++i) {
        if (!shouldDisplayInverter(config.inverters[i].enabled)) continue;
        enabledIds[enabledCount] = i;
        ++enabledCount;
        if (!config.inverters[i].ratingVerified) {
            ++pendingCount;
        } else {
            const bool confirmedCommunicationFault =
                inverterFailureStreak[i] >= HEALTH_FAILURE_THRESHOLD;
            const bool confirmedControlFault = !config.dryRun &&
                activePercent >= 0 && inverterHasReadback[i] &&
                !inverterAtTarget[i];
            if (inverterHealthy[i] && inverterFailureStreak[i] == 0 &&
                !confirmedControlFault) ++healthyCount;
            displayHardFault = displayHardFault ||
                confirmedCommunicationFault || confirmedControlFault;
        }
    }

    if (enabledCount == 0) {
        // Keep the layout state coherent when the last tile is removed.
        tileLayoutInitialized = false;
        bool hasPending = false;
        for (const auto& inverter : config.inverters)
            hasPending = hasPending || (inverter.enabled && !inverter.ratingVerified);
        ui.fillRoundRect(4, 21, screenW - 8, screenH - 38, 6, COLOR_PANEL);
        drawCentered(ui, hasPending ? "PENDING SETUP" : "NO INVERTER", 48, 2,
                     COLOR_AMBER);
        drawCentered(ui, hasPending ? "Validate at LIVE 100%" : "Configure via USB",
                     75, 1, COLOR_MUTED);
    } else {
        const uint8_t columns = displayGridColumns(enabledCount);
        const uint8_t rows = displayGridRows(enabledCount);
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
            tile.x += (tile.targetX - tile.x) * layoutEase;
            tile.y += (tile.targetY - tile.y) * layoutEase;
            tile.width += (tile.targetWidth - tile.width) * layoutEase;
            tile.height += (tile.targetHeight - tile.height) * layoutEase;
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
            const bool pending = !config.inverters[inverterIndex].ratingVerified;
            const bool confirmedFault =
                !pending && inverterFailureStreak[inverterIndex] >= HEALTH_FAILURE_THRESHOLD;
            const bool transientFailure = inverterFailureStreak[inverterIndex] > 0 &&
                                          !confirmedFault;
            // A single lost reply is a CHECK/amber condition, not a red
            // fault.  A confirmed readback that differs from the target is
            // still a real control fault and is red immediately.
            const bool controlFault = controlEnabled(inverterIndex) &&
                                      !config.dryRun && activePercent >= 0 &&
                                      !inverterAtTarget[inverterIndex] &&
                                      inverterHasReadback[inverterIndex];
            const bool tileFault = confirmedFault || controlFault;
            const uint32_t inverterCeiling =
                config.inverters[inverterIndex].inverterLimitW;
            const uint32_t displayMaximum = inverterCeiling > 0
                ? inverterCeiling : config.inverters[inverterIndex].maxPvPowerW;
            const float resPercent = invalid ? 0.0f : displayedResPercent;
            const uint16_t resColor = animatedLevelColor(resPercent);
            // Wattage is the single source of truth. Animate it using integer
            // arithmetic, then derive liquid height only for rendering. This
            // avoids all percent -> watt round trips and float accumulation.
            const uint32_t targetWatts = hasReadback && !confirmedFault
                ? lastReadback[inverterIndex] : 0U;
            uint32_t& shownWatts = displayedInvWatts[inverterIndex];
            shownWatts = approachDisplayedWatts(shownWatts, targetWatts);
            const float targetFill = displayMaximum == 0 ? 0.0f :
                min(100.0f, (static_cast<float>(shownWatts) * 100.0f) /
                              static_cast<float>(displayMaximum));
            // The watt counter is already eased, so the liquid must use the
            // same derived value directly. A second easing stage would make
            // the number and water surface visibly disagree.
            displayedInvPercent[inverterIndex] = targetFill;
            // The liquid height represents the inverter readback, but its
            // colour represents the active RSE command. This makes a new
            // 0/30/60/100% command visible immediately, even while the
            // inverter value is still moving toward the target.
            const uint16_t liquidCommandColor = invalid ? COLOR_RED : resColor;

            // Each tile is one gauge: INV is the filled level and RES is the
            // dashed setpoint. Faults deliberately dominate the complete tile.
            const int16_t radius = min<int16_t>(5, min<int16_t>(drawW / 4, drawH / 4));
            if (tileFault) {
                ui.fillRoundRect(x, y, drawW, drawH, radius, COLOR_RED);
            }
            if (hasReadback && !confirmedFault) {
                drawLiquidFill(ui, x, y, drawW, drawH,
                               displayedInvPercent[inverterIndex],
                               controlFault ? TFT_RED : liquidCommandColor,
                               inverterIndex, now);
            }
            ui.drawRoundRect(x, y, drawW, drawH, radius,
                             tileFault ? TFT_RED
                                 : ((pending || transientFailure || !hasReadback)
                                        ? COLOR_AMBER : liquidCommandColor));

            if (!invalid && !pending) {
                const int16_t innerH = drawH - 4;
                const int16_t lineY = y + drawH - 2 -
                    static_cast<int16_t>((static_cast<int32_t>(innerH) *
                                          resPercent) / 100);
                drawDashedLine(ui, x + 3, lineY, drawW - 6,
                               confirmedFault ? TFT_WHITE : resColor);
            }

            if (!hasRoomForText) continue;

            const int16_t centerX = x + drawW / 2;
            const uint32_t animatedInvWatts = displayedInvWatts[inverterIndex];
            char invNumber[10];
            if (confirmedFault) snprintf(invNumber, sizeof(invNumber), "ERR");
            else if (hasReadback)
                formatCompactKilowatts(invNumber, sizeof(invNumber), animatedInvWatts);
            else snprintf(invNumber, sizeof(invNumber), "--");
            const bool largeText = enabledCount <= 2 && drawH >= 70;
            const uint8_t textSize = largeText ? 2 : 1;
            char idText[8];
            snprintf(idText, sizeof(idText), "%sID%u", controlFault ? "!" : "",
                     slaveIdForIndex(inverterIndex));
            drawCellText(ui, idText, centerX,
                         y + (drawH < 60 ? 6 : 10), textSize);
            if (!confirmedFault) {
                const int16_t valueCenter = y + drawH / 2 +
                    (drawH < 60 ? 2 : 4);
                const int16_t valueGap = largeText ? 15 : 7;
                char rseText[10];
                if (pending) snprintf(rseText, sizeof(rseText), "PEND");
                else if (invalid) snprintf(rseText, sizeof(rseText), "R: --");
                else snprintf(rseText, sizeof(rseText), "R: %u%%", displayedResValue);
                drawCellText(ui, rseText, centerX, valueCenter - valueGap, textSize);
                // Keep the unit visible in every layout.  The compact number
                // formatting keeps this readable even in the six-tile view.
                char invText[13];
                snprintf(invText, sizeof(invText), "%skW", invNumber);
                drawCellText(ui, invText, centerX,
                             valueCenter + valueGap, largeText ? 2.0f : 1.25f);
            } else {
                drawCellText(ui, "ERROR", centerX, y + drawH / 2,
                             largeText ? 3 : 1);
            }
        }
    }

    ui.fillRect(0, screenH - 13, screenW, 13, COLOR_PANEL);
    // drawCellText() deliberately changes the text scale for each tile.  The
    // footer is a separate layout region, so never inherit the last tile's
    // scale (for example the 3x "ERROR" label in a single-inverter view).
    // Without this reset both footer labels expand toward the centre and
    // overlap until another frame happens to select a smaller tile font.
    ui.setTextSize(1);
    ui.setTextDatum(middle_left);
    ui.setTextColor(COLOR_MUTED);
    char leftFooter[20];
    if (applyInProgress)
        snprintf(leftFooter, sizeof(leftFooter), "SYNC %u/%u", applyProgress, applyTotal);
    else if (pendingCount > 0)
        snprintf(leftFooter, sizeof(leftFooter), "PEND %u", pendingCount);
    else snprintf(leftFooter, sizeof(leftFooter), "R=RES  I=INV");
    ui.drawString(leftFooter, 5, screenH - 7);
    ui.setTextDatum(middle_right);
    ui.setTextColor(enabledCount > 0 && healthyCount == enabledCount
                        ? COLOR_GREEN : COLOR_AMBER);
    char rightFooter[18];
    snprintf(rightFooter, sizeof(rightFooter), "OK %u/%u%s", healthyCount,
             enabledCount, eventLog.available() ? "  SD" : "");
    ui.drawString(rightFooter, screenW - 5, screenH - 7);
    ui.pushSprite(0, 0);

    // The status LED is an I/O transaction, not part of the animation. Update
    // it only when its state changes instead of repeating it every frame.
    const bool displayWarning = enabledCount == 0 || pendingCount > 0 ||
                                healthyCount < enabledCount;
    const bool displayError = invalid || displayHardFault;
    const uint8_t statusRed = displayError || displayWarning ? 255 : 0;
    const uint8_t statusGreen = displayError ? 0 : 255;
    const uint8_t statusBlue = config.dryRun ? 255 : 0;
    static uint8_t previousRed = 0xFE;
    static uint8_t previousGreen = 0xFE;
    static uint8_t previousBlue = 0xFE;
    if (statusRed != previousRed || statusGreen != previousGreen ||
        statusBlue != previousBlue) {
        M5StamPLC.setStatusLight(statusRed, statusGreen, statusBlue);
        previousRed = statusRed;
        previousGreen = statusGreen;
        previousBlue = statusBlue;
    }
}

void responsiveDelay(uint32_t durationMs) {
    const uint32_t started = millis();
    while (millis() - started < durationMs) {
        serviceTimeCriticalInputs();
        delay(1);
    }
}

void serviceTimeCriticalInputs() {
    if (watchdogSubscribed) esp_task_wdt_reset();
    const uint32_t now = millis();
    // PLC inputs need millisecond responsiveness, but reading all four inputs
    // on every 1 ms UART wait wastes bus time. A 5 ms sampler remains far
    // faster than the 300 ms debounce while leaving CPU time for Modbus RX.
    if (now - lastCriticalInputPollAt >= 5) {
        lastCriticalInputPollAt = now;
        const uint8_t currentMask = readRseMask();
        if (currentMask != rawRseMask) {
            rawRseMask = currentMask;
            rawChangedAt = now;
        }
    }
    updateDisplay();
}
}  // namespace

void setup() {
    Serial.begin(115200);
    delay(300);
    config.load();
    if (!AppConfig::storageSelfTest()) {
        config.dryRun = true;
        for (auto& inverter : config.inverters) inverter.enabled = false;
        Serial.println("FATAL configuration CRC self-test failed; inverter control disabled");
    }

    auto boardConfig = M5StamPLC.config();
    boardConfig.enableModbusSlave = false;
    boardConfig.enableSdCard = true;
    M5StamPLC.config(boardConfig);
    M5StamPLC.begin();
    M5StamPLC.setBacklight(true);
    // Recover automatically from a future unforeseen library deadlock. All
    // normal Modbus/HTTPS waits are bounded and feed this watchdog.
    if (esp_task_wdt_init(60, true) == ESP_OK) {
        const esp_err_t subscribed = esp_task_wdt_add(nullptr);
        watchdogSubscribed = subscribed == ESP_OK || subscribed == ESP_ERR_INVALID_ARG;
    }
    // Modbus verification remains synchronous for safety, but its wait periods
    // no longer freeze the liquid and tile animations.
    modbus.setIdleCallback(serviceTimeCriticalInputs);
    prepareOtaBootValidation();
    otaManager.setLocalClockProvider(readLocalRtc);
    otaManager.setRtcSyncCallback([](const struct tm& localTime) {
        struct tm rtcTime = localTime;
        M5StamPLC.setRtcTime(&rtcTime);
    });
    otaManager.begin();

    eventLog.begin();
    rawRseMask = readRseMask();
    stableRseMask = rawRseMask;
    rawChangedAt = millis();
    otaManager.setSafetyCheck([] {
        return safeForFirmwareUpdate();
    });
    otaManager.setProgressCallback([](const String& stage, int percent) {
        if (watchdogSubscribed) esp_task_wdt_reset();
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
    if (watchdogSubscribed) esp_task_wdt_reset();
    M5StamPLC.update();
    serviceManualRollbackGesture();
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

    if (manualTestActive &&
        deadlineReached(millis(), manualTestDeadline)) {
        Serial.println("TEST STATUS=EXPIRED DETAIL=returning to physical RSE");
        returnToPhysicalRse("usb_test_timeout");
    }

    serviceTimeCriticalInputs();
    if (rawRseMask != stableRseMask &&
        elapsedAtLeast(millis(), rawChangedAt, config.debounceMs)) {
        stableRseMask = rawRseMask;
        controlReapplyRequired = false;
        // A real RSE transition is authoritative and ends a GUI test early.
        manualTestActive = false;
        manualTestDeadline = 0;
        handleStableRseState(stableRseMask, "rse_transition");
    } else if (controlReapplyRequired && !applyInProgress) {
        controlReapplyRequired = false;
        handleStableRseState(stableRseMask, "rse_reapply_after_superseded_batch");
    }
    // The round-robin FC03 poll below is the only periodic inverter access:
    // one enabled ID every two seconds, read-only, and never an auto-write.
    pollNextEnabledInverter();
    otaManager.service(safeForFirmwareUpdate());
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
