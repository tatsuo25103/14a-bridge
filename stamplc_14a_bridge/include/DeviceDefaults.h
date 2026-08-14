#pragma once

#include <Arduino.h>

struct InverterDefault {
    bool enabled;
    uint32_t maxPvPowerW;
};

// ---------------------------------------------------------------------------
// USER INVERTER TABLE
// IDs are fixed to 2..7. Set enabled=false for an unused inverter (equivalent
// to commenting it out) and enter the installed PV module capacity. Runtime
// validation stores the inverter feed-in ceiling separately.
// These values seed NVS on first boot. Afterwards they can also be changed by
// USB. Send "reset CONFIRM" if these defaults are edited after the controller
// has already saved configuration.
// ---------------------------------------------------------------------------
constexpr InverterDefault INVERTER_DEFAULTS[6] = {
    // enabled, installed PV module power in watts
    {true,  15000},  // Modbus ID 2
    {true,  15000},  // Modbus ID 3
    {true,  10000},  // Modbus ID 4
    {false, 10000},  // Modbus ID 5 (disabled)
    {false, 10000},  // Modbus ID 6 (disabled)
    {false, 10000},  // Modbus ID 7 (disabled)
};
