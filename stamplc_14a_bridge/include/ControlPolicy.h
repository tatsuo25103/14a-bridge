#pragma once

#include <stdint.h>

// Pure control rules kept free of Arduino and hardware dependencies. They are
// used by production firmware and exercised by compile-time regression tests.
enum class RseProfile : uint8_t {
    StrictOneHot4 = 0,
    Westnetz4 = 1,
    EweHold4 = 2,
    FnnEza3 = 3,
};

constexpr int16_t RSE_INVALID = -1;
constexpr int16_t RSE_HOLD_LAST = -2;

constexpr bool isOneHot4(uint8_t mask) {
    return mask == 0x01 || mask == 0x02 || mask == 0x04 || mask == 0x08;
}

constexpr uint8_t activeContactCount(uint8_t mask) {
    return static_cast<uint8_t>((mask & 1U) + ((mask >> 1U) & 1U) +
                                ((mask >> 2U) & 1U) + ((mask >> 3U) & 1U));
}

// Returns 0/30/60/100, RSE_INVALID, or RSE_HOLD_LAST. The profile is an
// installation setting because German grid operators use different relay
// truth tables; guessing a table at runtime would be unsafe.
constexpr int16_t decodeRsePercent(uint8_t mask, RseProfile profile) {
    return profile == RseProfile::StrictOneHot4
        ? (mask == 0x01 ? 100 : mask == 0x02 ? 60 :
           mask == 0x04 ? 30 : mask == 0x08 ? 0 : RSE_INVALID)
        : profile == RseProfile::Westnetz4
        ? ((mask & 0x01U) ? 100 : (mask & 0x08U) ? 0 :
           (mask & 0x04U) ? 30 : (mask & 0x02U) ? 60 : 100)
        : profile == RseProfile::EweHold4
        ? (mask == 0x01 ? 100 : mask == 0x02 ? 60 :
           mask == 0x04 ? 30 : mask == 0x08 ? 0 : RSE_HOLD_LAST)
        : profile == RseProfile::FnnEza3
        ? ((mask & 0x01U) ? RSE_INVALID : (mask & 0x08U) ? 0 :
           (mask & 0x04U) ? 30 : (mask & 0x02U) ? 60 : 100)
        : RSE_INVALID;
}

constexpr bool rseMaskNeedsWarning(uint8_t mask, RseProfile profile) {
    return profile == RseProfile::Westnetz4
        ? activeContactCount(mask) > 1
        : profile == RseProfile::FnnEza3
        ? activeContactCount(static_cast<uint8_t>(mask & 0x0EU)) > 1
        : false;
}

constexpr bool manualTestRespectsPhysicalRse(uint8_t requestedPercent,
                                              int16_t physicalPercent) {
    return physicalPercent >= 0 && requestedPercent <= physicalPercent;
}

constexpr bool otaControlStateIsSafe(bool manualTest, bool applyInProgress,
                                     int16_t physicalPercent,
                                     bool inputsStable,
                                     bool enabledInvertersReady) {
    return !manualTest && !applyInProgress && physicalPercent == 100 &&
           inputsStable && enabledInvertersReady;
}

constexpr uint32_t calculatePowerLimit(uint32_t maximum, uint8_t percent) {
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(maximum) * percent + 50ULL) / 100ULL);
}

// EEG feed-in control uses installed PV generator capacity as the percentage
// basis, while the inverter itself remains the hard physical feed-in ceiling.
// Example: 18 kWp PV + 15 kW inverter => 100%=15 kW, 60%=10.8 kW.
constexpr uint32_t calculateFeedInLimit(uint32_t installedPvPower,
                                        uint32_t inverterCeiling,
                                        uint8_t percent) {
    return calculatePowerLimit(installedPvPower, percent) < inverterCeiling
        ? calculatePowerLimit(installedPvPower, percent) : inverterCeiling;
}

// Integer-only display animation. Wattage remains exact end-to-end; the LCD
// renderer may derive a percentage from this value, but never reconstructs
// watts from that percentage.
constexpr uint32_t displayedWattDistance(uint32_t current, uint32_t target) {
    return current < target ? target - current : current - target;
}

constexpr uint32_t displayedWattStep(uint32_t current, uint32_t target) {
    return displayedWattDistance(current, target) <= 25U
        ? displayedWattDistance(current, target)
        : (displayedWattDistance(current, target) / 7U > 0U
            ? displayedWattDistance(current, target) / 7U : 1U);
}

constexpr uint32_t approachDisplayedWatts(uint32_t current, uint32_t target) {
    return current < target ? current + displayedWattStep(current, target)
         : current > target ? current - displayedWattStep(current, target)
                            : target;
}

constexpr bool elapsedAtLeast(uint32_t now, uint32_t since,
                              uint32_t interval) {
    return static_cast<uint32_t>(now - since) >= interval;
}

// Valid for deadlines less than 2^31 ms in the future, which covers every
// firmware timer (maximum OTA backoff is 24 hours).
constexpr bool deadlineReached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

constexpr bool mayValidateInverterRating(bool dryRun, bool manualTest,
                                         int16_t physicalRsePercent) {
    return !dryRun && !manualTest && physicalRsePercent == 100;
}

constexpr bool shouldDisplayInverter(bool configuredEnabled) {
    return configuredEnabled;
}

constexpr bool mayControlInverter(bool configuredEnabled,
                                  bool ratingVerified) {
    return configuredEnabled && ratingVerified;
}

constexpr uint8_t displayGridColumns(uint8_t inverterCount) {
    return inverterCount <= 3 ? inverterCount :
           inverterCount == 4 ? 2 : 3;
}

constexpr uint8_t displayGridRows(uint8_t inverterCount) {
    return inverterCount <= 3 ? 1 : 2;
}
