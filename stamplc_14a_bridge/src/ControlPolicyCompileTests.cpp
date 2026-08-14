#include "ControlPolicy.h"

// Backward-compatible strict profile: exactly the four one-hot inputs work.
static_assert(decodeRsePercent(0x00, RseProfile::StrictOneHot4) == RSE_INVALID, "strict none");
static_assert(decodeRsePercent(0x01, RseProfile::StrictOneHot4) == 100, "strict K1");
static_assert(decodeRsePercent(0x02, RseProfile::StrictOneHot4) == 60, "strict K2");
static_assert(decodeRsePercent(0x04, RseProfile::StrictOneHot4) == 30, "strict K3");
static_assert(decodeRsePercent(0x08, RseProfile::StrictOneHot4) == 0, "strict K4");
static_assert(decodeRsePercent(0x03, RseProfile::StrictOneHot4) == RSE_INVALID, "strict multi");
static_assert(decodeRsePercent(0x0F, RseProfile::StrictOneHot4) == RSE_INVALID, "strict all");

// Westnetz: K1 release dominates; otherwise the most restrictive active
// contact wins, and no contact means 100%.
static_assert(decodeRsePercent(0x00, RseProfile::Westnetz4) == 100, "Westnetz none");
static_assert(decodeRsePercent(0x03, RseProfile::Westnetz4) == 100, "Westnetz K1 priority");
static_assert(decodeRsePercent(0x06, RseProfile::Westnetz4) == 30, "Westnetz restrictive");
static_assert(decodeRsePercent(0x0A, RseProfile::Westnetz4) == 0, "Westnetz K4");
static_assert(decodeRsePercent(0x0F, RseProfile::Westnetz4) == 100, "Westnetz K1 all");
static_assert(rseMaskNeedsWarning(0x06, RseProfile::Westnetz4), "Westnetz multi warning");

// EWE: exactly one contact is valid; none or multiple keeps the last valid
// command and must never invent a new output level.
static_assert(decodeRsePercent(0x00, RseProfile::EweHold4) == RSE_HOLD_LAST, "EWE none hold");
static_assert(decodeRsePercent(0x04, RseProfile::EweHold4) == 30, "EWE K3");
static_assert(decodeRsePercent(0x05, RseProfile::EweHold4) == RSE_HOLD_LAST, "EWE multi hold");
static_assert(decodeRsePercent(0x0F, RseProfile::EweHold4) == RSE_HOLD_LAST, "EWE all hold");

// VDE FNN / Netze BW EZA: DI2/DI3/DI4 are 60/30/0; no contact releases to
// 100%. Multiple reduction contacts resolve to the most restrictive level.
static_assert(decodeRsePercent(0x00, RseProfile::FnnEza3) == 100, "FNN none");
static_assert(decodeRsePercent(0x02, RseProfile::FnnEza3) == 60, "FNN 60");
static_assert(decodeRsePercent(0x04, RseProfile::FnnEza3) == 30, "FNN 30");
static_assert(decodeRsePercent(0x08, RseProfile::FnnEza3) == 0, "FNN 0");
static_assert(decodeRsePercent(0x06, RseProfile::FnnEza3) == 30, "FNN restrictive multi");
static_assert(decodeRsePercent(0x0E, RseProfile::FnnEza3) == 0, "FNN all reductions");
static_assert(decodeRsePercent(0x01, RseProfile::FnnEza3) == RSE_INVALID, "FNN DI1 wiring error");
static_assert(rseMaskNeedsWarning(0x0E, RseProfile::FnnEza3), "FNN multi warning");

// Exhaustive 16-mask regression tables. Keeping the entire table in one
// assertion per profile makes any future change to an untested combination a
// compile failure, not a field surprise.
static_assert(
    decodeRsePercent(0x0, RseProfile::StrictOneHot4) == RSE_INVALID &&
    decodeRsePercent(0x1, RseProfile::StrictOneHot4) == 100 &&
    decodeRsePercent(0x2, RseProfile::StrictOneHot4) == 60 &&
    decodeRsePercent(0x3, RseProfile::StrictOneHot4) == RSE_INVALID &&
    decodeRsePercent(0x4, RseProfile::StrictOneHot4) == 30 &&
    decodeRsePercent(0x5, RseProfile::StrictOneHot4) == RSE_INVALID &&
    decodeRsePercent(0x6, RseProfile::StrictOneHot4) == RSE_INVALID &&
    decodeRsePercent(0x7, RseProfile::StrictOneHot4) == RSE_INVALID &&
    decodeRsePercent(0x8, RseProfile::StrictOneHot4) == 0 &&
    decodeRsePercent(0x9, RseProfile::StrictOneHot4) == RSE_INVALID &&
    decodeRsePercent(0xA, RseProfile::StrictOneHot4) == RSE_INVALID &&
    decodeRsePercent(0xB, RseProfile::StrictOneHot4) == RSE_INVALID &&
    decodeRsePercent(0xC, RseProfile::StrictOneHot4) == RSE_INVALID &&
    decodeRsePercent(0xD, RseProfile::StrictOneHot4) == RSE_INVALID &&
    decodeRsePercent(0xE, RseProfile::StrictOneHot4) == RSE_INVALID &&
    decodeRsePercent(0xF, RseProfile::StrictOneHot4) == RSE_INVALID,
    "strict exhaustive truth table");
static_assert(
    decodeRsePercent(0x0, RseProfile::Westnetz4) == 100 &&
    decodeRsePercent(0x1, RseProfile::Westnetz4) == 100 &&
    decodeRsePercent(0x2, RseProfile::Westnetz4) == 60 &&
    decodeRsePercent(0x3, RseProfile::Westnetz4) == 100 &&
    decodeRsePercent(0x4, RseProfile::Westnetz4) == 30 &&
    decodeRsePercent(0x5, RseProfile::Westnetz4) == 100 &&
    decodeRsePercent(0x6, RseProfile::Westnetz4) == 30 &&
    decodeRsePercent(0x7, RseProfile::Westnetz4) == 100 &&
    decodeRsePercent(0x8, RseProfile::Westnetz4) == 0 &&
    decodeRsePercent(0x9, RseProfile::Westnetz4) == 100 &&
    decodeRsePercent(0xA, RseProfile::Westnetz4) == 0 &&
    decodeRsePercent(0xB, RseProfile::Westnetz4) == 100 &&
    decodeRsePercent(0xC, RseProfile::Westnetz4) == 0 &&
    decodeRsePercent(0xD, RseProfile::Westnetz4) == 100 &&
    decodeRsePercent(0xE, RseProfile::Westnetz4) == 0 &&
    decodeRsePercent(0xF, RseProfile::Westnetz4) == 100,
    "Westnetz exhaustive truth table");
static_assert(
    decodeRsePercent(0x0, RseProfile::EweHold4) == RSE_HOLD_LAST &&
    decodeRsePercent(0x1, RseProfile::EweHold4) == 100 &&
    decodeRsePercent(0x2, RseProfile::EweHold4) == 60 &&
    decodeRsePercent(0x3, RseProfile::EweHold4) == RSE_HOLD_LAST &&
    decodeRsePercent(0x4, RseProfile::EweHold4) == 30 &&
    decodeRsePercent(0x5, RseProfile::EweHold4) == RSE_HOLD_LAST &&
    decodeRsePercent(0x6, RseProfile::EweHold4) == RSE_HOLD_LAST &&
    decodeRsePercent(0x7, RseProfile::EweHold4) == RSE_HOLD_LAST &&
    decodeRsePercent(0x8, RseProfile::EweHold4) == 0 &&
    decodeRsePercent(0x9, RseProfile::EweHold4) == RSE_HOLD_LAST &&
    decodeRsePercent(0xA, RseProfile::EweHold4) == RSE_HOLD_LAST &&
    decodeRsePercent(0xB, RseProfile::EweHold4) == RSE_HOLD_LAST &&
    decodeRsePercent(0xC, RseProfile::EweHold4) == RSE_HOLD_LAST &&
    decodeRsePercent(0xD, RseProfile::EweHold4) == RSE_HOLD_LAST &&
    decodeRsePercent(0xE, RseProfile::EweHold4) == RSE_HOLD_LAST &&
    decodeRsePercent(0xF, RseProfile::EweHold4) == RSE_HOLD_LAST,
    "EWE exhaustive truth table");
static_assert(
    decodeRsePercent(0x0, RseProfile::FnnEza3) == 100 &&
    decodeRsePercent(0x1, RseProfile::FnnEza3) == RSE_INVALID &&
    decodeRsePercent(0x2, RseProfile::FnnEza3) == 60 &&
    decodeRsePercent(0x3, RseProfile::FnnEza3) == RSE_INVALID &&
    decodeRsePercent(0x4, RseProfile::FnnEza3) == 30 &&
    decodeRsePercent(0x5, RseProfile::FnnEza3) == RSE_INVALID &&
    decodeRsePercent(0x6, RseProfile::FnnEza3) == 30 &&
    decodeRsePercent(0x7, RseProfile::FnnEza3) == RSE_INVALID &&
    decodeRsePercent(0x8, RseProfile::FnnEza3) == 0 &&
    decodeRsePercent(0x9, RseProfile::FnnEza3) == RSE_INVALID &&
    decodeRsePercent(0xA, RseProfile::FnnEza3) == 0 &&
    decodeRsePercent(0xB, RseProfile::FnnEza3) == RSE_INVALID &&
    decodeRsePercent(0xC, RseProfile::FnnEza3) == 0 &&
    decodeRsePercent(0xD, RseProfile::FnnEza3) == RSE_INVALID &&
    decodeRsePercent(0xE, RseProfile::FnnEza3) == 0 &&
    decodeRsePercent(0xF, RseProfile::FnnEza3) == RSE_INVALID,
    "FNN exhaustive truth table");

static_assert(manualTestRespectsPhysicalRse(30, 60), "stricter test allowed");
static_assert(!manualTestRespectsPhysicalRse(100, 60), "test cannot release physical reduction");
static_assert(!manualTestRespectsPhysicalRse(0, RSE_INVALID), "unknown physical state blocks test");
static_assert(otaControlStateIsSafe(false, false, 100, true, true), "safe OTA state");
static_assert(!otaControlStateIsSafe(false, false, 60, true, true), "OTA blocked during reduction");
static_assert(!otaControlStateIsSafe(true, false, 100, true, true), "OTA blocked in test");

static_assert(calculatePowerLimit(10000, 100) == 10000, "10 kW 100%");
static_assert(calculatePowerLimit(10000, 60) == 6000, "10 kW 60%");
static_assert(calculatePowerLimit(10000, 30) == 3000, "10 kW 30%");
static_assert(calculatePowerLimit(10000, 0) == 0, "10 kW 0%");
static_assert(calculatePowerLimit(15000, 100) == 15000, "15 kW 100%");
static_assert(calculatePowerLimit(15000, 60) == 9000, "15 kW 60%");
static_assert(calculatePowerLimit(15000, 30) == 4500, "15 kW 30%");
static_assert(calculatePowerLimit(15000, 0) == 0, "15 kW 0%");
static_assert(calculatePowerLimit(12345, 30) == 3704, "rounded integer power");

static_assert(calculateFeedInLimit(18000, 15000, 100) == 15000,
              "18 kWp PV is capped by a 15 kW inverter at 100%");
static_assert(calculateFeedInLimit(18000, 15000, 60) == 10800,
              "18 kWp PV uses the PV basis at 60%");
static_assert(calculateFeedInLimit(18000, 15000, 30) == 5400,
              "18 kWp PV uses the PV basis at 30%");
static_assert(calculateFeedInLimit(18000, 15000, 0) == 0,
              "zero remains zero");
static_assert(calculateFeedInLimit(10000, 15000, 100) == 10000,
              "PV capacity below inverter rating is not increased");
static_assert(calculateFeedInLimit(30000, 15000, 60) == 15000,
              "percentage result can never exceed inverter ceiling");

static_assert(!elapsedAtLeast(1049, 1000, 50), "debounce not early");
static_assert(elapsedAtLeast(1050, 1000, 50), "debounce exact boundary");
static_assert(elapsedAtLeast(0x00000010U, 0xFFFFFFF0U, 32),
              "millis wrap elapsed calculation");
static_assert(!elapsedAtLeast(0x0000000FU, 0xFFFFFFF0U, 32),
              "millis wrap before boundary");
static_assert(deadlineReached(0x00000010U, 0x00000010U), "deadline boundary");
static_assert(deadlineReached(0x00000010U, 0xFFFFFFF0U),
              "deadline across millis wrap");
static_assert(!deadlineReached(0xFFFFFFF0U, 0x00000010U),
              "future deadline across millis wrap");

static_assert(mayValidateInverterRating(false, false, 100),
              "LIVE physical 100% permits validation");
static_assert(!mayValidateInverterRating(true, false, 100),
              "SAFE cannot validate");
static_assert(!mayValidateInverterRating(false, true, 100),
              "TEST cannot validate");
static_assert(!mayValidateInverterRating(false, false, 60),
              "60% cannot validate");
static_assert(!mayValidateInverterRating(false, false, 30),
              "30% cannot validate");
static_assert(!mayValidateInverterRating(false, false, 0),
              "0% cannot validate");
static_assert(!mayValidateInverterRating(false, false, -1),
              "invalid RSE cannot validate");

static_assert(shouldDisplayInverter(true),
              "every configured inverter must have a display tile");
static_assert(!shouldDisplayInverter(false),
              "disabled inverter must not consume a display tile");
static_assert(!mayControlInverter(true, false),
              "pending inverter is visible but excluded from control");
static_assert(mayControlInverter(true, true),
              "verified configured inverter is controllable");
static_assert(displayGridColumns(1) == 1 && displayGridRows(1) == 1,
              "one-inverter layout");
static_assert(displayGridColumns(2) == 2 && displayGridRows(2) == 1,
              "two-inverter layout");
static_assert(displayGridColumns(3) == 3 && displayGridRows(3) == 1,
              "three-inverter layout");
static_assert(displayGridColumns(4) == 2 && displayGridRows(4) == 2,
              "four-inverter layout");
static_assert(displayGridColumns(5) == 3 && displayGridRows(5) == 2,
              "five-inverter layout");
static_assert(displayGridColumns(6) == 3 && displayGridRows(6) == 2,
              "six-inverter layout");
static_assert(approachDisplayedWatts(14000, 14000) == 14000,
              "settled display must retain exact readback");
static_assert(approachDisplayedWatts(13980, 14000) == 14000,
              "near target must snap to exact readback");
static_assert(approachDisplayedWatts(14020, 14000) == 14000,
              "descending near target must snap exactly");
