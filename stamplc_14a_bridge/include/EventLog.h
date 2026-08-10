#pragma once

#include <Arduino.h>

class EventLog {
public:
    void begin();
    void append(const String& timestamp, const String& event, uint8_t rseMask,
                int16_t percent, uint8_t inverterId, uint32_t maxPowerW,
                uint32_t requested, uint32_t readback, const String& result);
    String tail(size_t maximumRows = 20);
    bool available() const { return available_; }

private:
    bool available_ = false;
    static String csvEscape(const String& value);
};

