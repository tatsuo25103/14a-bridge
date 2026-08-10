#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "DeviceDefaults.h"

constexpr uint8_t MAX_WRITE_VERIFY_RETRIES = 3;

struct InverterConfig {
    bool enabled = false;
    uint32_t maxPvPowerW = 0;
};

struct AppConfig {
    // RSE K1..K4 are connected to StampPLC DI1..DI4 and decoded one-hot as
    // 100%, 60%, 30%, and 0% respectively.
    bool inputActiveHigh = true;
    uint32_t debounceMs = 1000;
    InverterConfig inverters[6];

    uint32_t modbusBaud = 19200;
    uint16_t modbusRegister = 0x04E5;
    uint8_t modbusQuantity = 2;
    uint32_t responseTimeoutMs = 1200;
    uint8_t verifyRetries = 3;

    // Safe commissioning default: observe and log, but do not control outputs.
    bool dryRun = true;
    uint32_t periodicVerifyMs = 60000;

    AppConfig() {
        for (uint8_t i = 0; i < 6; ++i) {
            inverters[i].enabled = INVERTER_DEFAULTS[i].enabled;
            inverters[i].maxPvPowerW = INVERTER_DEFAULTS[i].maxPvPowerW;
        }
    }

    void load() {
        Preferences p;
        if (!p.begin("rsebridge", true)) return;
        inputActiveHigh = p.getBool("in_hi", inputActiveHigh);
        debounceMs = p.getULong("debounce", debounceMs);
        modbusBaud = p.getULong("baud", modbusBaud);
        modbusRegister = p.getUShort("reg", modbusRegister);
        modbusQuantity = p.getUChar("qty", modbusQuantity);
        responseTimeoutMs = p.getULong("timeout", responseTimeoutMs);
        verifyRetries = p.getUChar("retries", verifyRetries);
        dryRun = p.getBool("dryrun", dryRun);
        periodicVerifyMs = p.getULong("verify_ms", periodicVerifyMs);
        for (uint8_t i = 0; i < 6; ++i) {
            const String enabledKey = "en" + String(i + 1);
            const String powerKey = "max" + String(i + 1);
            inverters[i].enabled = p.getBool(enabledKey.c_str(), inverters[i].enabled);
            inverters[i].maxPvPowerW = p.getULong(powerKey.c_str(), inverters[i].maxPvPowerW);
        }
        p.end();
        sanitize();
    }

    bool save() {
        sanitize();
        Preferences p;
        if (!p.begin("rsebridge", false)) return false;
        p.putBool("in_hi", inputActiveHigh);
        p.putULong("debounce", debounceMs);
        p.putULong("baud", modbusBaud);
        p.putUShort("reg", modbusRegister);
        p.putUChar("qty", modbusQuantity);
        p.putULong("timeout", responseTimeoutMs);
        p.putUChar("retries", verifyRetries);
        p.putBool("dryrun", dryRun);
        p.putULong("verify_ms", periodicVerifyMs);
        for (uint8_t i = 0; i < 6; ++i) {
            const String enabledKey = "en" + String(i + 1);
            const String powerKey = "max" + String(i + 1);
            p.putBool(enabledKey.c_str(), inverters[i].enabled);
            p.putULong(powerKey.c_str(), inverters[i].maxPvPowerW);
        }
        p.end();
        return true;
    }

    void sanitize() {
        debounceMs = constrain(debounceMs, 50UL, 10000UL);
        if (modbusQuantity != 1 && modbusQuantity != 2) modbusQuantity = 2;
        responseTimeoutMs = constrain(responseTimeoutMs, 100UL, 10000UL);
        verifyRetries = constrain(verifyRetries, static_cast<uint8_t>(1),
                                  MAX_WRITE_VERIFY_RETRIES);
        periodicVerifyMs = constrain(periodicVerifyMs, 5000UL, 3600000UL);
        for (auto& inverter : inverters) {
            if (modbusQuantity == 1 && inverter.maxPvPowerW > 65535UL) {
                inverter.enabled = false;
            }
        }
    }
};
