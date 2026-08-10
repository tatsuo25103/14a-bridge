#pragma once

#include <Arduino.h>

struct InverterDefault {
    bool enabled;
    uint32_t maxPvPowerW;
};

// ---------------------------------------------------------------------------
// USER INVERTER TABLE
// IDs are fixed to 1..6. Set enabled=false for an unused inverter (equivalent
// to commenting it out) and enter the inverter's maximum PV generation power.
// These values seed NVS on first boot. Afterwards they can also be changed by
// USB. Send "reset CONFIRM" if these defaults are edited after the controller
// has already saved configuration.
// ---------------------------------------------------------------------------
constexpr InverterDefault INVERTER_DEFAULTS[6] = {
    // enabled, maximum PV generation power in watts
    {true,  15000},  // Modbus ID 1
    {true,  15000},  // Modbus ID 2
    {true,  10000},  // Modbus ID 3
    {false, 10000},  // Modbus ID 4 (disabled)
    {false, 10000},  // Modbus ID 5 (disabled)
    {false, 10000},  // Modbus ID 6 (disabled)
};
