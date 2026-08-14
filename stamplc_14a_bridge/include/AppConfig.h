#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#include "ControlPolicy.h"
#include "DeviceDefaults.h"

constexpr uint8_t MAX_WRITE_VERIFY_RETRIES = 3;

struct InverterConfig {
    bool enabled = false;
    // Installed generator capacity (kWp converted to W). RSE feed-in
    // percentages are always calculated from this value.
    uint32_t maxPvPowerW = 0;
    // Verified effective 100% feed-in ceiling. This is normally the inverter
    // rating, or maxPvPowerW when the PV array is smaller than the inverter.
    uint32_t inverterLimitW = 0;
    bool ratingVerified = false;
};

struct AppConfig {
private:
    static constexpr uint32_t CONFIG_MAGIC = 0x41313442UL;  // "A14B"
    static constexpr uint16_t CONFIG_SCHEMA = 3;

    struct PersistentInverterV1 {
        uint32_t maxPvPowerW;
        uint8_t enabled;
        uint8_t ratingVerified;
        uint16_t reserved;
    };

    struct PersistentRecordV1 {
        uint32_t magic;
        uint32_t generation;
        uint16_t schema;
        uint16_t modbusRegister;
        uint32_t debounceMs;
        uint32_t modbusBaud;
        uint32_t responseTimeoutMs;
        uint32_t periodicVerifyMs;
        uint8_t inputActiveHigh;
        uint8_t modbusQuantity;
        uint8_t verifyRetries;
        uint8_t dryRun;
        PersistentInverterV1 inverters[6];
        uint32_t crc;
    };

    struct PersistentInverter {
        uint32_t maxPvPowerW;
        uint32_t inverterLimitW;
        uint8_t enabled;
        uint8_t ratingVerified;
        uint16_t reserved;
    };

    struct PersistentRecordV2 {
        uint32_t magic;
        uint32_t generation;
        uint16_t schema;
        uint16_t modbusRegister;
        uint32_t debounceMs;
        uint32_t modbusBaud;
        uint32_t responseTimeoutMs;
        uint32_t periodicVerifyMs;
        uint8_t inputActiveHigh;
        uint8_t modbusQuantity;
        uint8_t verifyRetries;
        uint8_t dryRun;
        PersistentInverter inverters[6];
        uint32_t crc;
    };

    struct PersistentRecord {
        uint32_t magic;
        uint32_t generation;
        uint16_t schema;
        uint16_t modbusRegister;
        uint32_t debounceMs;
        uint32_t modbusBaud;
        uint32_t responseTimeoutMs;
        uint32_t periodicVerifyMs;
        uint8_t inputActiveHigh;
        uint8_t modbusQuantity;
        uint8_t verifyRetries;
        uint8_t dryRun;
        uint8_t rseProfile;
        uint8_t reserved[3];
        PersistentInverter inverters[6];
        uint32_t crc;
    };
    static_assert(sizeof(PersistentInverterV1) == 8,
                  "legacy persistent inverter layout changed");
    static_assert(sizeof(PersistentRecordV1) == 84,
                  "legacy persistent configuration layout changed");
    static_assert(sizeof(PersistentInverter) == 12,
                  "persistent inverter layout changed");
    static_assert(sizeof(PersistentRecordV2) == 108,
                  "V2 persistent configuration layout changed");
    static_assert(sizeof(PersistentRecord) == 112,
                  "persistent configuration layout changed");

    int8_t activeConfigSlot_ = -1;
    uint32_t configGeneration_ = 0;

    static uint32_t crc32(const uint8_t* data, size_t length) {
        uint32_t crc = 0xFFFFFFFFUL;
        for (size_t i = 0; i < length; ++i) {
            crc ^= data[i];
            for (uint8_t bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320UL : 0);
        }
        return ~crc;
    }

    static bool validRecord(const PersistentRecord& record) {
        return record.magic == CONFIG_MAGIC &&
               record.schema == CONFIG_SCHEMA &&
               record.crc == crc32(reinterpret_cast<const uint8_t*>(&record),
                                   offsetof(PersistentRecord, crc));
    }

    static bool validRecord(const PersistentRecordV1& record) {
        return record.magic == CONFIG_MAGIC && record.schema == 1 &&
               record.crc == crc32(reinterpret_cast<const uint8_t*>(&record),
                                   offsetof(PersistentRecordV1, crc));
    }

    static bool validRecord(const PersistentRecordV2& record) {
        return record.magic == CONFIG_MAGIC && record.schema == 2 &&
               record.crc == crc32(reinterpret_cast<const uint8_t*>(&record),
                                   offsetof(PersistentRecordV2, crc));
    }

    static bool readRecord(Preferences& p, const char* key,
                           PersistentRecord& record) {
        if (p.getBytesLength(key) != sizeof(record)) return false;
        return p.getBytes(key, &record, sizeof(record)) == sizeof(record) &&
               validRecord(record);
    }

    static bool readRecord(Preferences& p, const char* key,
                           PersistentRecordV1& record) {
        if (p.getBytesLength(key) != sizeof(record)) return false;
        return p.getBytes(key, &record, sizeof(record)) == sizeof(record) &&
               validRecord(record);
    }

    static bool readRecord(Preferences& p, const char* key,
                           PersistentRecordV2& record) {
        if (p.getBytesLength(key) != sizeof(record)) return false;
        return p.getBytes(key, &record, sizeof(record)) == sizeof(record) &&
               validRecord(record);
    }

    static bool generationIsNewer(uint32_t left, uint32_t right) {
        return static_cast<int32_t>(left - right) > 0;
    }

    PersistentRecord makeRecord(uint32_t generation) const {
        PersistentRecord record {};
        record.magic = CONFIG_MAGIC;
        record.generation = generation;
        record.schema = CONFIG_SCHEMA;
        record.modbusRegister = modbusRegister;
        record.debounceMs = debounceMs;
        record.modbusBaud = modbusBaud;
        record.responseTimeoutMs = responseTimeoutMs;
        record.periodicVerifyMs = periodicVerifyMs;
        record.inputActiveHigh = inputActiveHigh ? 1 : 0;
        record.modbusQuantity = modbusQuantity;
        record.verifyRetries = verifyRetries;
        record.dryRun = dryRun ? 1 : 0;
        record.rseProfile = static_cast<uint8_t>(rseProfile);
        for (uint8_t i = 0; i < 6; ++i) {
            record.inverters[i].maxPvPowerW = inverters[i].maxPvPowerW;
            record.inverters[i].inverterLimitW = inverters[i].inverterLimitW;
            record.inverters[i].enabled = inverters[i].enabled ? 1 : 0;
            record.inverters[i].ratingVerified =
                inverters[i].ratingVerified ? 1 : 0;
        }
        record.crc = crc32(reinterpret_cast<const uint8_t*>(&record),
                           offsetof(PersistentRecord, crc));
        return record;
    }

    void applyRecord(const PersistentRecord& record) {
        inputActiveHigh = record.inputActiveHigh != 0;
        debounceMs = record.debounceMs;
        modbusBaud = record.modbusBaud;
        modbusRegister = record.modbusRegister;
        modbusQuantity = record.modbusQuantity;
        responseTimeoutMs = record.responseTimeoutMs;
        verifyRetries = record.verifyRetries;
        dryRun = record.dryRun != 0;
        periodicVerifyMs = record.periodicVerifyMs;
        rseProfile = static_cast<RseProfile>(record.rseProfile);
        for (uint8_t i = 0; i < 6; ++i) {
            inverters[i].enabled = record.inverters[i].enabled != 0;
            inverters[i].maxPvPowerW = record.inverters[i].maxPvPowerW;
            inverters[i].inverterLimitW = record.inverters[i].inverterLimitW;
            inverters[i].ratingVerified =
                record.inverters[i].ratingVerified != 0;
        }
    }


    void applyRecord(const PersistentRecordV2& record) {
        inputActiveHigh = record.inputActiveHigh != 0;
        debounceMs = record.debounceMs;
        modbusBaud = record.modbusBaud;
        modbusRegister = record.modbusRegister;
        modbusQuantity = record.modbusQuantity;
        responseTimeoutMs = record.responseTimeoutMs;
        verifyRetries = record.verifyRetries;
        dryRun = record.dryRun != 0;
        periodicVerifyMs = record.periodicVerifyMs;
        rseProfile = RseProfile::StrictOneHot4;
        for (uint8_t i = 0; i < 6; ++i) {
            inverters[i].enabled = record.inverters[i].enabled != 0;
            inverters[i].maxPvPowerW = record.inverters[i].maxPvPowerW;
            inverters[i].inverterLimitW = record.inverters[i].inverterLimitW;
            inverters[i].ratingVerified =
                record.inverters[i].ratingVerified != 0;
        }
    }


    void applyRecord(const PersistentRecordV1& record) {
        inputActiveHigh = record.inputActiveHigh != 0;
        debounceMs = record.debounceMs;
        modbusBaud = record.modbusBaud;
        modbusRegister = record.modbusRegister;
        modbusQuantity = record.modbusQuantity;
        responseTimeoutMs = record.responseTimeoutMs;
        verifyRetries = record.verifyRetries;
        dryRun = record.dryRun != 0;
        periodicVerifyMs = record.periodicVerifyMs;
        rseProfile = RseProfile::StrictOneHot4;
        // Old positions represented IDs 1..6; new positions represent 2..7.
        // Preserve settings by actual Modbus ID: old ID2 -> new ID2, etc.
        // Old ID1 is intentionally dropped and new ID7 keeps safe defaults.
        for (uint8_t i = 0; i < 5; ++i) {
            const uint8_t oldIndex = i + 1;
            inverters[i].enabled = record.inverters[oldIndex].enabled != 0;
            inverters[i].maxPvPowerW = record.inverters[oldIndex].maxPvPowerW;
            // V1 stored one value for both the PV basis and verified 100%
            // limit. Preserve the exact old output behaviour during upgrade.
            inverters[i].inverterLimitW = record.inverters[oldIndex].maxPvPowerW;
            inverters[i].ratingVerified =
                record.inverters[oldIndex].ratingVerified != 0;
        }
        inverters[5].ratingVerified = false;
    }

public:
    // Existing installations migrate to the exact legacy one-hot behaviour.
    // A different grid-operator truth table must be selected deliberately.
    RseProfile rseProfile = RseProfile::StrictOneHot4;
    bool inputActiveHigh = true;
    // RSE relay contacts settle quickly. 300 ms still rejects contact bounce
    // and transitional multi-contact states without making an operator wait a
    // full second before the command is visible.
    uint32_t debounceMs = 300;
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
            inverters[i].inverterLimitW = INVERTER_DEFAULTS[i].maxPvPowerW;
        }
    }

    static bool storageSelfTest() {
        AppConfig sample;
        PersistentRecord record = sample.makeRecord(7);
        if (!validRecord(record)) return false;
        record.inverters[2].maxPvPowerW ^= 1U;
        if (validRecord(record)) return false;
        PersistentRecordV1 legacy {};
        legacy.inverters[0].maxPvPowerW = 1111;  // old ID1, must be dropped
        legacy.inverters[1].maxPvPowerW = 2222;  // old ID2
        legacy.inverters[1].enabled = 1;
        legacy.inverters[1].ratingVerified = 1;
        legacy.inverters[5].maxPvPowerW = 6666;  // old ID6
        AppConfig migrated;
        migrated.applyRecord(legacy);
        if (migrated.inverters[0].maxPvPowerW != 2222 ||
            migrated.inverters[0].inverterLimitW != 2222 ||
            !migrated.inverters[0].enabled ||
            !migrated.inverters[0].ratingVerified ||
            migrated.inverters[4].maxPvPowerW != 6666 ||
            migrated.inverters[5].ratingVerified ||
            migrated.rseProfile != RseProfile::StrictOneHot4) return false;
        PersistentRecordV2 previous {};
        previous.inverters[0].maxPvPowerW = 12345;
        previous.inverters[0].inverterLimitW = 10000;
        previous.inverters[0].enabled = 1;
        AppConfig migratedV2;
        migratedV2.applyRecord(previous);
        if (migratedV2.inverters[0].maxPvPowerW != 12345 ||
            migratedV2.inverters[0].inverterLimitW != 10000 ||
            migratedV2.rseProfile != RseProfile::StrictOneHot4) return false;
        return generationIsNewer(8, 7) &&
               generationIsNewer(0, 0xFFFFFFFFUL) &&
               !generationIsNewer(7, 7);
    }

    void load() {
        Preferences p;
        if (!p.begin("rsebridge", true)) return;
        PersistentRecord slot0 {};
        PersistentRecord slot1 {};
        const bool valid0 = readRecord(p, "cfg0", slot0);
        const bool valid1 = readRecord(p, "cfg1", slot1);
        if (valid0 || valid1) {
            if (valid1 && (!valid0 || generationIsNewer(slot1.generation,
                                                        slot0.generation))) {
                applyRecord(slot1);
                activeConfigSlot_ = 1;
                configGeneration_ = slot1.generation;
            } else {
                applyRecord(slot0);
                activeConfigSlot_ = 0;
                configGeneration_ = slot0.generation;
            }
            p.end();
            sanitize();
            return;
        }

        // V1.0.5 used schema 2. Preserve every inverter value and select the
        // legacy strict truth table explicitly during the schema-3 migration.
        PersistentRecordV2 v2Slot0 {};
        PersistentRecordV2 v2Slot1 {};
        const bool v2Valid0 = readRecord(p, "cfg0", v2Slot0);
        const bool v2Valid1 = readRecord(p, "cfg1", v2Slot1);
        if (v2Valid0 || v2Valid1) {
            const PersistentRecordV2& selected =
                v2Valid1 && (!v2Valid0 || generationIsNewer(
                    v2Slot1.generation, v2Slot0.generation))
                    ? v2Slot1 : v2Slot0;
            applyRecord(selected);
            activeConfigSlot_ = (&selected == &v2Slot1) ? 1 : 0;
            configGeneration_ = selected.generation;
            p.end();
            sanitize();
            save();
            return;
        }

        // Atomic migration from the V1 single-power/ID1-6 model. Keep settings
        // attached to the same physical IDs 2-6 and use the former power as
        // both installed PV capacity and effective inverter ceiling.
        PersistentRecordV1 oldSlot0 {};
        PersistentRecordV1 oldSlot1 {};
        const bool oldValid0 = readRecord(p, "cfg0", oldSlot0);
        const bool oldValid1 = readRecord(p, "cfg1", oldSlot1);
        if (oldValid0 || oldValid1) {
            const PersistentRecordV1& selected =
                oldValid1 && (!oldValid0 || generationIsNewer(
                    oldSlot1.generation, oldSlot0.generation))
                    ? oldSlot1 : oldSlot0;
            applyRecord(selected);
            activeConfigSlot_ = (&selected == &oldSlot1) ? 1 : 0;
            configGeneration_ = selected.generation;
            p.end();
            sanitize();
            save();
            return;
        }

        // One-time migration from V1.0.4 and earlier individual NVS keys.
        inputActiveHigh = p.getBool("in_hi", inputActiveHigh);
        debounceMs = p.getULong("debounce", debounceMs);
        // V1.0.4 and earlier always stored the old fixed 1000 ms default and
        // exposed no GUI control for it. Migrate only that exact legacy value;
        // preserve any deliberately configured custom debounce interval.
        if (debounceMs == 1000) debounceMs = 300;
        modbusBaud = p.getULong("baud", modbusBaud);
        modbusRegister = p.getUShort("reg", modbusRegister);
        modbusQuantity = p.getUChar("qty", modbusQuantity);
        responseTimeoutMs = p.getULong("timeout", responseTimeoutMs);
        verifyRetries = p.getUChar("retries", verifyRetries);
        dryRun = p.getBool("dryrun", dryRun);
        periodicVerifyMs = p.getULong("verify_ms", periodicVerifyMs);
        rseProfile = RseProfile::StrictOneHot4;
        for (uint8_t i = 0; i < 6; ++i) {
            const String idSuffix = String(i + 2);
            const String enabledKey = "en" + idSuffix;
            const String powerKey = "max" + idSuffix;
            const String verifiedKey = "val" + idSuffix;
            inverters[i].enabled = p.getBool(enabledKey.c_str(), inverters[i].enabled);
            inverters[i].maxPvPowerW = p.getULong(powerKey.c_str(), inverters[i].maxPvPowerW);
            inverters[i].inverterLimitW = inverters[i].maxPvPowerW;
            // Preserve already commissioned installations during migration.
            // New offline settings explicitly store false and remain pending.
            inverters[i].ratingVerified = p.isKey(verifiedKey.c_str())
                ? p.getBool(verifiedKey.c_str(), false)
                : inverters[i].enabled;
        }
        p.end();
        sanitize();
        if (save() && p.begin("rsebridge", false)) {
            const char* fixedKeys[] = {
                "in_hi", "debounce", "baud", "reg", "qty", "timeout",
                "retries", "dryrun", "verify_ms"
            };
            for (const char* key : fixedKeys) p.remove(key);
            for (uint8_t id = 1; id <= 7; ++id) {
                const String suffix = String(id);
                p.remove(("en" + suffix).c_str());
                p.remove(("max" + suffix).c_str());
                p.remove(("val" + suffix).c_str());
            }
            p.end();
        }
    }

    bool save() {
        sanitize();
        Preferences p;
        if (!p.begin("rsebridge", false)) return false;

        // Avoid any NVS write if the complete payload is already current.
        if (activeConfigSlot_ >= 0) {
            PersistentRecord stored {};
            const char* activeKey = activeConfigSlot_ == 0 ? "cfg0" : "cfg1";
            if (readRecord(p, activeKey, stored)) {
                const PersistentRecord current = makeRecord(stored.generation);
                if (memcmp(&stored, &current, sizeof(stored)) == 0) {
                    p.end();
                    return true;
                }
            }
        }

        const uint32_t nextGeneration = configGeneration_ + 1U;
        const PersistentRecord candidate = makeRecord(nextGeneration);
        const int8_t targetSlot = activeConfigSlot_ == 0 ? 1 : 0;
        const char* targetKey = targetSlot == 0 ? "cfg0" : "cfg1";
        const bool written = p.putBytes(targetKey, &candidate,
                                        sizeof(candidate)) == sizeof(candidate);
        PersistentRecord verified {};
        const bool ok = written && readRecord(p, targetKey, verified) &&
                        memcmp(&candidate, &verified, sizeof(candidate)) == 0;
        p.end();
        if (ok) {
            activeConfigSlot_ = targetSlot;
            configGeneration_ = nextGeneration;
        }
        return ok;
    }

    void sanitize() {
        if (static_cast<uint8_t>(rseProfile) >
            static_cast<uint8_t>(RseProfile::FnnEza3)) {
            rseProfile = RseProfile::StrictOneHot4;
        }
        debounceMs = constrain(debounceMs, 50UL, 10000UL);
        modbusBaud = constrain(modbusBaud, 1200UL, 1000000UL);
        if (modbusQuantity != 1 && modbusQuantity != 2) modbusQuantity = 2;
        responseTimeoutMs = constrain(responseTimeoutMs, 100UL, 10000UL);
        verifyRetries = constrain(verifyRetries, static_cast<uint8_t>(1),
                                  MAX_WRITE_VERIFY_RETRIES);
        periodicVerifyMs = constrain(periodicVerifyMs, 5000UL, 3600000UL);
        for (auto& inverter : inverters) {
            // Never turn a corrupted/missing zero power rating into an
            // unintended 0 W control command. Leave that ID disabled until a
            // user supplies a valid rating again.
            if (inverter.maxPvPowerW == 0) {
                inverter.enabled = false;
                inverter.ratingVerified = false;
            }
            if (inverter.ratingVerified && inverter.inverterLimitW == 0) {
                inverter.enabled = false;
                inverter.ratingVerified = false;
            }
            // Installed PV capacity is only a calculation basis and may be
            // larger than one Modbus register. Only the value that can
            // actually be written to a one-register inverter must fit.
            if (modbusQuantity == 1 && inverter.inverterLimitW > 65535UL) {
                inverter.enabled = false;
                inverter.ratingVerified = false;
            }
        }
    }
};
