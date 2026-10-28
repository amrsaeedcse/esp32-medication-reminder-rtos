#pragma once

#include <stdint.h>

// ============================================================================
// Commands received over BLE and executed by the system-control task only.
// BLE callbacks never touch NVS, timers or the state machine directly.
// ============================================================================
enum class AppCommandType : uint8_t {
    None = 0,
    SetInterval,
    ResetCounters,
    SilenceAlarm,
    Sleep,
};

struct AppCommand {
    AppCommandType type;
    uint32_t       value;   // seconds, only used by SetInterval
};
