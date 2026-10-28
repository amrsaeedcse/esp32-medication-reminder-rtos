#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "config.h"
#include "app_commands.h"

// ============================================================================
// BINARY TELEMETRY PACKET (little-endian, 20 bytes)
// ============================================================================
#define TELEMETRY_VERSION   1

enum : uint8_t {
    TELEM_FLAG_ALARM  = 1u << 0,
    TELEM_FLAG_BLE    = 1u << 1,
    TELEM_FLAG_CLIENT = 1u << 2,
};

struct __attribute__((packed)) TelemetryPacket {
    uint8_t  version;
    uint8_t  flags;
    uint32_t takenDoses;
    uint32_t missedDoses;
    uint32_t intervalSec;
    uint32_t secondsToNext;
    int16_t  batteryMillivolts;   // -1 when battery sensing is disabled
};

static_assert(sizeof(TelemetryPacket) == 20,
              "TelemetryPacket must fit in the default BLE MTU payload (20 bytes)");

// ============================================================================
// PUBLIC BLE STREAMING SERVICE INTERFACE
// ============================================================================

// Starts the BLE GATT server and advertising. Commands written by the Flutter
// app are queued into commandQueue; they are never executed in the BLE callback.
void BLE_StartService(QueueHandle_t commandQueue);

// Stops advertising, tears down the GATT server and powers down the radio.
void BLE_StopService();

// Processes connection / advertising housekeeping. Must be called from the
// system-control task only (keeps BLE calls single-threaded).
void BLE_Poll();

// Pushes a 20-byte telemetry packet via Notify (and updates the Read value).
// Called from the system-control task only.
void BLE_StreamTelemetry();

// Returns true if a Flutter client is currently connected over BLE.
bool BLE_IsClientConnected();
