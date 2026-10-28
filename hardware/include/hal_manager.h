#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_sleep.h"
#include "config.h"

// ============================================================================
// SYSTEM STATE (single writer = system-control task, plus ISR flags)
// ============================================================================
struct SystemState_t {
    uint32_t          bootCount;
    volatile uint32_t takenDoses;
    volatile uint32_t missedDoses;
    volatile uint32_t doseIntervalSec;

    // Written by the system-control task, read by the output task / BLE module.
    volatile bool     isBleActive;
    volatile bool     isAlarmRinging;
    volatile bool     requestAckChirp;      // "Taken" confirmation requested
    volatile bool     outputsMuted;         // Set before deep sleep, output task goes idle

    // Written from the hardware-timer ISR, consumed by the system-control task.
    volatile bool     activeDoseTimerExpired;

    // Derived telemetry values, refreshed by the system-control task.
    volatile uint32_t secondsToNextDose;
    volatile int32_t  batteryMillivolts;
};

// Global system state accessible across modules
extern SystemState_t g_sysState;

// FreeRTOS Binary Semaphore signalled by the button hardware ISR
extern SemaphoreHandle_t g_buttonSemaphore;

// ============================================================================
// PUBLIC HARDWARE & STORAGE INTERFACE (HAL)
// ============================================================================

// Initialises pins, PWM, NVS, button ISR and the dose GPTimer.
// Returns the reason why the chip left deep sleep.
esp_sleep_wakeup_cause_t HAL_Init();

// Monotonic millisecond clock that keeps counting through deep sleep.
// Backed by the RTC counter (esp_rtc_get_time_us), not by millis().
uint64_t HAL_NowMs();

// Flash ROM (NVS) persistence. Only the system-control task may call the
// write functions (NVS is not re-entrant).
void HAL_LoadFromFlash();
void HAL_SaveStatsToFlash();
void HAL_SaveIntervalToFlash(uint32_t newIntervalSec);
void HAL_ResetCountersInFlash();

// Power mode switching (Radio OFF @ 80 MHz / Radio ON @ 240 MHz)
void HAL_SetModemSleepMode();
void HAL_SetFullActiveMode();
void HAL_PrintPowerReport();

// Buzzer (LEDC PWM) & LED. Only the alarm-output task calls these while awake.
void HAL_BuzzerTone(uint32_t freqHz);
void HAL_LedWrite(bool on);
void HAL_PlayAckChirp();

// Hardware GPTimer (dose countdown while the chip is awake in BLE mode).
// The timer is created once in HAL_Init(); arming only reconfigures it.
void HAL_InitDoseTimer();
void HAL_ArmDoseTimer(uint32_t delayMs);
void HAL_DisarmDoseTimer();

// Waits until the button is physically released.
// Returns false if it is still held after timeoutMs.
bool HAL_WaitForButtonRelease(uint32_t timeoutMs);

// Battery divider in millivolts, or -1 when battery sensing is disabled.
int HAL_ReadBatteryMillivolts();

// Stops the dose timer and enters deep sleep. If the button is stuck, EXT0 is
// not armed (prevents an immediate wake-up loop) and only the RTC timer wakes
// the chip. Never returns.
void HAL_EnterDeepSleep(uint32_t sleepMs) __attribute__((noreturn));
