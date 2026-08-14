#include "EventLog.h"

#include <SD.h>

namespace {
constexpr const char* LOG_PATH = "/rse_events.csv";
constexpr const char* OLD_LOG_PATH = "/rse_events.previous.csv";
constexpr size_t MAX_LOG_BYTES = 4UL * 1024UL * 1024UL;

bool createLogFile() {
    File f = SD.open(LOG_PATH, FILE_WRITE);
    if (!f) return false;
    f.println("timestamp,event,rse_mask,percent,inverter_id,max_power_w,requested_w,readback_w,result");
    f.flush();
    f.close();
    return true;
}
}

void EventLog::begin() {
    available_ = SD.cardType() != CARD_NONE;
    if (!available_) return;
    if (!SD.exists(LOG_PATH)) {
        available_ = createLogFile();
    }
}

String EventLog::csvEscape(const String& value) {
    String escaped = value;
    escaped.replace("\"", "\"\"");
    return "\"" + escaped + "\"";
}

void EventLog::append(const String& timestamp, const String& event,
                      uint8_t rseMask, int16_t percent, uint8_t inverterId,
                      uint32_t maxPowerW, uint32_t requested,
                      uint32_t readback, const String& result) {
    char mask[8];
    snprintf(mask, sizeof(mask), "0x%02X", rseMask);
    const String line = csvEscape(timestamp) + "," + csvEscape(event) + "," +
        mask + "," + String(percent) + "," + String(inverterId) + "," +
        String(maxPowerW) + "," + String(requested) + "," +
        String(readback) + "," + csvEscape(result);
    Serial.println(line);
    if (!available_) return;
    File f = SD.open(LOG_PATH, FILE_APPEND);
    if (!f) {
        available_ = false;
        return;
    }
    if (f.size() >= MAX_LOG_BYTES) {
        f.close();
        if (SD.exists(OLD_LOG_PATH)) SD.remove(OLD_LOG_PATH);
        // If rotation fails, keep using the current file. Logging must never
        // interfere with inverter control merely because the SD card is worn,
        // full, or write-protected.
        if (SD.rename(LOG_PATH, OLD_LOG_PATH)) {
            if (!createLogFile()) {
                available_ = false;
                return;
            }
        }
        f = SD.open(LOG_PATH, FILE_APPEND);
        if (!f) {
            available_ = false;
            return;
        }
    }
    f.println(line);
    f.flush();
    f.close();
}

String EventLog::tail(size_t maximumRows) {
    if (!available_) return "SD card unavailable";
    File f = SD.open(LOG_PATH, FILE_READ);
    if (!f) return "Log unavailable";
    String rows[20];
    const size_t capacity = min(maximumRows, static_cast<size_t>(20));
    size_t count = 0;
    while (f.available()) {
        const String line = f.readStringUntil('\n');
        if (!line.isEmpty()) rows[count++ % capacity] = line;
    }
    f.close();
    String output;
    const size_t stored = min(count, capacity);
    const size_t start = count > capacity ? count % capacity : 0;
    for (size_t i = 0; i < stored; ++i) output += rows[(start + i) % capacity] + "\n";
    return output;
}
