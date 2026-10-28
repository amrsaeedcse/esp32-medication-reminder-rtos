#pragma once

#include <Arduino.h>
#include "driver/rtc_io.h"

// ============================================================================
// 1. HARDWARE PIN DEFINITIONS
// ============================================================================
// GPIO 33 is an RTC-capable GPIO supporting EXT0 Deep-Sleep Wake-up & Pull-up
#define BUTTON_PIN                  GPIO_NUM_33

// GPIO 25 drives the Buzzer using Hardware LEDC PWM (passive buzzer)
#define BUZZER_PIN                  25

// GPIO 2 drives the Status LED (Onboard DevKit LED)
#define LED_PIN                     2

// Optional battery voltage divider (ADC1 channel, input-only pin).
// Set ENABLE_BATTERY_ADC to 1 only if the board actually has the divider,
// otherwise the telemetry reports -1 (unknown).
#define ENABLE_BATTERY_ADC          0
#define BATTERY_ADC_PIN             34
#define BATTERY_DIVIDER_RATIO       2.0f    // e.g. 100k / 100k divider
#define BATTERY_ADC_INTERVAL_MS     10000   // sample every 10 s while BLE is on

// ============================================================================
// 2. PWM / TIMER CONFIGURATION (Arduino-ESP32 core 3.x API)
// ============================================================================
#define PWM_BUZZER_RESOLUTION       10      // 10-bit duty (ledcWriteTone also uses 10-bit)
#define TONE_FREQ_HIGH_HZ           2000    // First beep tone (2.0 kHz)
#define TONE_FREQ_LOW_HZ            1200    // Second beep tone (1.2 kHz)
#define TONE_FREQ_ACK_HZ            2600    // Short confirmation beep on "Taken"

// Hardware GPTimer runs at 1 MHz -> 1 tick = 1 microsecond
#define DOSE_TIMER_FREQ_HZ          1000000UL
#define US_PER_SECOND               1000000ULL

// ============================================================================
// 3. SYSTEM TIMING CONSTANTS
// ============================================================================
#define DEFAULT_DOSE_INTERVAL_SEC   15      // Default 15 s for quick lab demonstration
#define MIN_DOSE_INTERVAL_SEC       5       // Reject anything shorter than 5 s
#define MAX_DOSE_INTERVAL_SEC       86400   // Reject anything longer than 24 h

#define ALARM_AUTO_TIMEOUT_MS       30000   // 30 s auto-shutoff if patient misses dose
#define BLE_SESSION_TIMEOUT_MS      45000   // 45 s active BLE streaming window
#define BLE_STREAM_PERIOD_MS        1000    // Telemetry notify period while connected
#define BUTTON_DEBOUNCE_MS          200     // 200 ms hardware ISR debounce filter
#define BUTTON_RELEASE_TIMEOUT_MS   5000    // Max wait for button release before sleep
#define MIN_SLEEP_MS                1000    // Never arm a sub-second deep-sleep timer
#define WAKE_EARLY_TOLERANCE_MS     1000    // Re-sleep if the RTC timer woke us this early

// ============================================================================
// 4. BLE GATT STREAMING UUIDs (For Flutter Companion App)
// ============================================================================
#define BLE_DEVICE_NAME             "MedReminder-ESP32"
#define BLE_SERVICE_UUID            "4fafc201-1fb5-459e-8fcc-c5c9c331914b"

// Characteristic 1 (Read + Notify): 20-byte binary telemetry packet.
// Layout (little-endian, packed):
//   uint8  version        (1)
//   uint8  flags          (1)  bit0 = alarm, bit1 = BLE active, bit2 = client connected
//   uint32 takenDoses     (4)
//   uint32 missedDoses    (4)
//   uint32 intervalSec    (4)
//   uint32 secondsToNext  (4)
//   int16  batteryMv      (2)  -1 when battery sensing is disabled
// Total 20 bytes -> always fits in the default BLE MTU (23) payload (20 bytes).
#define BLE_CHAR_STREAM_UUID        "beb5483e-36e1-4688-b7f5-ea07361b26a8"

// Characteristic 2 (Write / Write-Without-Response): text commands from Flutter.
// Supported commands (case-insensitive):
//   "SET:<seconds>" -> validate, persist interval in NVS, reschedule next dose
//   "RESET"         -> reset taken/missed counters in NVS
//   "SILENCE"       -> mark the ringing dose as TAKEN and stop the alarm
//   "SLEEP"         -> close the BLE session and enter deep sleep (ignored while alarm rings)
#define BLE_CHAR_COMMAND_UUID       "82b97631-9357-4c62-bc87-9c9a21f3a6d1"

// Local preferred MTU. The telemetry packet fits in the default 23-byte MTU,
// but a larger MTU keeps the connection efficient for future characteristics.
#define BLE_LOCAL_MTU               247

// ============================================================================
// 5. POWER CONSUMPTION CONSTANTS (From Lecture Slides 5, 6, 8, 9, 36)
// ============================================================================
#define SUPPLY_VOLTAGE_V            3.3f

// GPIO Pin Current for Buzzer/LED (Slide 36: 6mA Best, 12mA Max/Worst)
#define I_PIN_BEST_MA               6.0f
#define I_PIN_WORST_MA              12.0f

// Case 1: Modem-Sleep Alarm Only (Slide 6: 3mA Slow/Best, 20mA Fast/Worst)
#define I_MODEM_CHIP_BEST_MA        3.0f
#define I_MODEM_CHIP_WORST_MA       20.0f

// Case 2 & 3: Active Mode with BLE Radio ON (Slide 5: 80mA Best, 260mA Max/Worst)
#define I_ACTIVE_CHIP_BEST_MA       80.0f
#define I_ACTIVE_CHIP_WORST_MA      260.0f

// Deep Sleep Mode (Slide 8: 10uA Best, 0.15mA = 150uA Worst)
#define I_DEEP_SLEEP_BEST_UA        10.0f
#define I_DEEP_SLEEP_WORST_UA       150.0f
