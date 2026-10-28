/**
 * ============================================================================
 * SMART MEDICATION REMINDER - MAIN APPLICATION & FREERTOS STATE MACHINE
 * ============================================================================
 * 3-module architecture:
 *   1. hal_manager (.h/.cpp) : Hardware I/O, ISR, GPTimer, NVS & Deep Sleep
 *   2. ble_service (.h/.cpp) : BLE GATT server, binary telemetry & command RX
 *   3. main.cpp              : FreeRTOS tasks, scheduling policy & state machine
 *
 * Deep-sleep model:
 *   - The chip is a full reset on every wake; all schedule state lives in
 *     RTC_DATA_ATTR memory + NVS.
 *   - The next dose is an absolute RTC timestamp (s_nextDoseAtMs). Opening a
 *     BLE session never postpones it; the active timer is armed for the
 *     remaining time, not for a full interval.
 *   - The alarm timer is re-armed only after the dose is resolved (Taken or
 *     Missed), so a 15 s demo interval cannot restart the 30 s alarm timeout.
 * ============================================================================
 */

#include <Arduino.h>
#include "config.h"
#include "hal_manager.h"
#include "ble_service.h"
#include "app_commands.h"

// ============================================================================
// RTC-RETAINED SCHEDULE STATE (survives deep sleep)
// ============================================================================
RTC_DATA_ATTR static uint64_t s_nextDoseAtMs   = 0;
RTC_DATA_ATTR static uint32_t s_scheduleMagic  = 0;
static const uint32_t SCHEDULE_MAGIC           = 0x4D454452UL;   // "MEDR"

// ============================================================================
// MODULE-LOCAL STATE
// ============================================================================
static QueueHandle_t s_cmdQueue      = NULL;
static volatile bool s_outputTaskIdle = true;

static uint64_t s_alarmStartMs  = 0;
static uint64_t s_bleStartMs    = 0;
static uint64_t s_lastStreamMs  = 0;
static uint64_t s_lastBatteryMs = 0;

// Forward declarations
static uint64_t intervalMs();
static void     refreshDerivedState(uint64_t now);
static void     startAlarm(uint64_t now);
static void     finishDose(bool taken, uint64_t now);
static void     handleCommand(const AppCommand& cmd, uint64_t now);
static void     enterDeepSleep(uint64_t now);

// ============================================================================
// FREERTOS TASK 1: BUZZER & LED OUTPUT (sole owner of the buzzer/LED)
// ============================================================================
void vAlarmOutputTask(void* pvParameters) {
    while (true) {
        if (g_sysState.outputsMuted) {
            HAL_BuzzerTone(0);
            HAL_LedWrite(false);
            s_outputTaskIdle = true;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        s_outputTaskIdle = false;

        if (g_sysState.requestAckChirp) {
            g_sysState.requestAckChirp = false;
            HAL_PlayAckChirp();
        }

        if (g_sysState.isAlarmRinging) {
            // Double-beep medical alarm pattern + LED flash
            HAL_LedWrite(true);
            HAL_BuzzerTone(TONE_FREQ_HIGH_HZ);
            vTaskDelay(pdMS_TO_TICKS(150));

            HAL_BuzzerTone(0);
            vTaskDelay(pdMS_TO_TICKS(100));

            if (!g_sysState.isAlarmRinging) {
                continue;
            }

            HAL_BuzzerTone(TONE_FREQ_LOW_HZ);
            vTaskDelay(pdMS_TO_TICKS(150));

            HAL_LedWrite(false);
            HAL_BuzzerTone(0);
            vTaskDelay(pdMS_TO_TICKS(600));
        } else if (g_sysState.isBleActive) {
            // Soft status blink while the BLE window is open (buzzer OFF)
            HAL_BuzzerTone(0);
            HAL_LedWrite((HAL_NowMs() / 300ULL) % 2ULL);
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            HAL_BuzzerTone(0);
            HAL_LedWrite(false);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// ============================================================================
// SCHEDULING HELPERS
// ============================================================================
static uint64_t intervalMs() {
    return (uint64_t)g_sysState.doseIntervalSec * 1000ULL;
}

static void refreshDerivedState(uint64_t now) {
    if (g_sysState.isAlarmRinging || s_nextDoseAtMs == 0) {
        g_sysState.secondsToNextDose = 0;
        return;
    }

    if (s_nextDoseAtMs > now) {
        uint64_t remainingMs  = s_nextDoseAtMs - now;
        uint64_t remainingSec = (remainingMs + 999ULL) / 1000ULL;
        g_sysState.secondsToNextDose =
            (remainingSec > 0xFFFFFFFFULL) ? 0xFFFFFFFFUL : (uint32_t)remainingSec;
    } else {
        g_sysState.secondsToNextDose = 0;
    }
}

static void startAlarm(uint64_t now) {
    if (g_sysState.isAlarmRinging) {
        return;
    }

    // Do NOT re-arm the dose timer here: the next interval starts only after
    // this dose is resolved. Re-arming now would reset the 30 s alarm timeout
    // every time the demo interval (15 s) elapses.
    HAL_DisarmDoseTimer();

    g_sysState.isAlarmRinging = true;
    s_alarmStartMs            = now;
    g_sysState.secondsToNextDose = 0;

    Serial.println("==================================================");
    Serial.println("[ALARM] Medication dose is due - buzzer ON.");
    Serial.println("==================================================");

    BLE_StreamTelemetry();
}

static void finishDose(bool taken, uint64_t now) {
    if (!g_sysState.isAlarmRinging) {
        return;
    }

    g_sysState.isAlarmRinging = false;
    HAL_DisarmDoseTimer();

    if (taken) {
        g_sysState.takenDoses = g_sysState.takenDoses + 1;
        g_sysState.requestAckChirp = true;
    } else {
        g_sysState.missedDoses = g_sysState.missedDoses + 1;
    }
    HAL_SaveStatsToFlash();

    // The next dose starts from the moment this one was resolved.
    s_nextDoseAtMs = now + intervalMs();

    Serial.println("==================================================");
    Serial.printf("[DOSE] Marked as %s. Taken=%lu | Missed=%lu | Next in %lu s\n",
                  taken ? "TAKEN" : "MISSED",
                  (unsigned long)g_sysState.takenDoses,
                  (unsigned long)g_sysState.missedDoses,
                  (unsigned long)g_sysState.doseIntervalSec);
    Serial.println("==================================================");

    // While the BLE window is still open, keep counting down to the next dose.
    // Only an acknowledged dose refreshes the 45 s window; a missed dose must
    // not keep the radio alive forever on short demo intervals.
    if (g_sysState.isBleActive) {
        if (taken) {
            s_bleStartMs = now;
        }
        HAL_ArmDoseTimer((uint32_t)intervalMs());
    }

    refreshDerivedState(now);
    BLE_StreamTelemetry();
}

static void handleCommand(const AppCommand& cmd, uint64_t now) {
    switch (cmd.type) {
        case AppCommandType::SetInterval: {
            HAL_SaveIntervalToFlash(cmd.value);

            // Use the value that was actually stored (HAL validates the range).
            uint32_t appliedSec = g_sysState.doseIntervalSec;

            if (!g_sysState.isAlarmRinging) {
                s_nextDoseAtMs = now + (uint64_t)appliedSec * 1000ULL;
                if (g_sysState.isBleActive) {
                    HAL_ArmDoseTimer(appliedSec * 1000UL);
                }
                Serial.printf("[CONFIG] Interval set to %lu s; next dose in %lu s.\n",
                              (unsigned long)appliedSec, (unsigned long)appliedSec);
            } else {
                Serial.printf("[CONFIG] Interval %lu s saved; applies after the current alarm.\n",
                              (unsigned long)appliedSec);
            }

            refreshDerivedState(now);
            BLE_StreamTelemetry();
            break;
        }

        case AppCommandType::ResetCounters:
            HAL_ResetCountersInFlash();
            Serial.println("[CONFIG] Dose counters reset.");
            BLE_StreamTelemetry();
            break;

        case AppCommandType::SilenceAlarm:
            if (g_sysState.isAlarmRinging) {
                finishDose(true, now);   // also refreshes the BLE session window
                if (!g_sysState.isBleActive) {
                    enterDeepSleep(now);
                }
            } else {
                Serial.println("[CONFIG] SILENCE ignored: no alarm is ringing.");
            }
            break;

        case AppCommandType::Sleep:
            if (!g_sysState.isAlarmRinging) {
                Serial.println("[CONFIG] SLEEP requested by the app.");
                enterDeepSleep(now);
            } else {
                Serial.println("[CONFIG] SLEEP ignored: resolve the alarm first.");
            }
            break;

        default:
            break;
    }
}

// ============================================================================
// SAFE DEEP-SLEEP PATH
// ============================================================================
static void enterDeepSleep(uint64_t now) {
    if (g_sysState.isAlarmRinging) {
        return;
    }

    // Give a pending "Taken" chirp a moment to play, then mute the output task
    // and wait until it is idle. The output task may be inside a ~750 ms alarm
    // beep cycle when the button is pressed, so allow up to 1.2 s for it to
    // reach the top of its loop and service requestAckChirp.
    if (g_sysState.requestAckChirp) {
        uint32_t chirpStart = millis();
        while (g_sysState.requestAckChirp && (uint32_t)(millis() - chirpStart) < 1200) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    g_sysState.outputsMuted = true;

    uint32_t waitStart = millis();
    while (!s_outputTaskIdle && (uint32_t)(millis() - waitStart) < 1000) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    HAL_BuzzerTone(0);
    HAL_LedWrite(false);
    BLE_StopService();

    // Recompute after BLE teardown (deinit can take a few tens of ms).
    now = HAL_NowMs();

    uint64_t remaining = (s_nextDoseAtMs > now)
                             ? (s_nextDoseAtMs - now)
                             : (uint64_t)MIN_SLEEP_MS;
    uint64_t maxSleep  = (uint64_t)MAX_DOSE_INTERVAL_SEC * 1000ULL;
    if (remaining > maxSleep) {
        remaining = maxSleep;
    }

    Serial.printf("[SLEEP] Next dose in %llu ms.\n", (unsigned long long)remaining);
    HAL_EnterDeepSleep((uint32_t)remaining);   // never returns
}

// ============================================================================
// FREERTOS TASK 2: SYSTEM STATE MACHINE (only writer of schedule state/NVS)
// ============================================================================
void vSystemControlTask(void* pvParameters) {
    uint64_t now = HAL_NowMs();
    refreshDerivedState(now);

    while (true) {
        // --------------------------------------------------------------------
        // 1. DRAIN BLE COMMANDS (queued by the BLE callback, executed here)
        // --------------------------------------------------------------------
        if (s_cmdQueue != NULL) {
            AppCommand cmd;
            while (xQueueReceive(s_cmdQueue, &cmd, 0) == pdTRUE) {
                now = HAL_NowMs();
                handleCommand(cmd, now);
            }
        }

        // --------------------------------------------------------------------
        // 2. BLE HOUSEKEEPING (advertising restart / immediate first notify)
        // --------------------------------------------------------------------
        if (g_sysState.isBleActive) {
            BLE_Poll();
        }

        // --------------------------------------------------------------------
        // 3. WAIT FOR BUTTON ISR OR 50 ms TIMEOUT
        // --------------------------------------------------------------------
        BaseType_t buttonPressed = pdFALSE;
        if (g_buttonSemaphore != NULL) {
            buttonPressed = xSemaphoreTake(g_buttonSemaphore, pdMS_TO_TICKS(50));
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        now = HAL_NowMs();

        // --------------------------------------------------------------------
        // 4. DOSE TIMER FIRED WHILE AWAKE (BLE mode or alarm mode)
        // --------------------------------------------------------------------
        if (g_sysState.activeDoseTimerExpired) {
            g_sysState.activeDoseTimerExpired = false;
            if (!g_sysState.isAlarmRinging) {
                startAlarm(now);
            }
        }

        // --------------------------------------------------------------------
        // 5. BUTTON PRESS
        // --------------------------------------------------------------------
        if (buttonPressed == pdTRUE) {
            if (g_sysState.isAlarmRinging) {
                // First press only silences the alarm and marks the dose TAKEN.
                finishDose(true, now);

                if (!g_sysState.isBleActive) {
                    enterDeepSleep(now);
                }
            } else if (g_sysState.isBleActive) {
                // Button in BLE mode with no alarm -> close the session early.
                Serial.println("[BUTTON] Closing BLE session early.");
                enterDeepSleep(now);
            }
        }

        // --------------------------------------------------------------------
        // 6. ALARM AUTO-TIMEOUT (30 s battery guard if the patient is away)
        // --------------------------------------------------------------------
        if (g_sysState.isAlarmRinging &&
            (now - s_alarmStartMs >= (uint64_t)ALARM_AUTO_TIMEOUT_MS)) {
            Serial.println("[ALARM] 30 s elapsed without a button press.");
            finishDose(false, now);

            if (!g_sysState.isBleActive) {
                enterDeepSleep(now);
            }
        }

        // --------------------------------------------------------------------
        // 7. BLE STREAMING & SESSION TIMEOUT (45 s window)
        // --------------------------------------------------------------------
        if (g_sysState.isBleActive) {
            if (now - s_lastStreamMs >= (uint64_t)BLE_STREAM_PERIOD_MS) {
                s_lastStreamMs = now;

                if (now - s_lastBatteryMs >= (uint64_t)BATTERY_ADC_INTERVAL_MS) {
                    s_lastBatteryMs = now;
                    g_sysState.batteryMillivolts = HAL_ReadBatteryMillivolts();
                }

                refreshDerivedState(now);
                BLE_StreamTelemetry();
            }

            if (!g_sysState.isAlarmRinging &&
                (now - s_bleStartMs >= (uint64_t)BLE_SESSION_TIMEOUT_MS)) {
                Serial.println("[BLE] Session window expired.");
                enterDeepSleep(now);
            }
        }

        refreshDerivedState(now);
    }
}

// ============================================================================
// MAIN SETUP: ENTRY POINT ON POWER-ON, RESET OR DEEP-SLEEP WAKE-UP
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(100);

    // Create the command queue before BLE can receive any write.
    s_cmdQueue = xQueueCreate(8, sizeof(AppCommand));
    if (s_cmdQueue == NULL) {
        Serial.println("[FATAL] Could not create the BLE command queue.");
    }

    esp_sleep_wakeup_cause_t wakeReason = HAL_Init();
    uint64_t now = HAL_NowMs();

    // ------------------------------------------------------------------------
    // Initialise the persistent schedule (cold boot / power loss only)
    // ------------------------------------------------------------------------
    if (s_scheduleMagic != SCHEDULE_MAGIC || s_nextDoseAtMs == 0) {
        s_scheduleMagic = SCHEDULE_MAGIC;
        s_nextDoseAtMs  = now + intervalMs();
        Serial.println("[SCHEDULE] Initialised a new dose schedule.");
    }

    // Guard against corrupted RTC state (e.g. after an unexpected reset).
    uint64_t maxAhead = (uint64_t)MAX_DOSE_INTERVAL_SEC * 1000ULL + 60000ULL;
    if (s_nextDoseAtMs > now + maxAhead) {
        s_nextDoseAtMs = now + intervalMs();
        Serial.println("[SCHEDULE] Invalid future deadline - schedule reset.");
    }

    g_sysState.batteryMillivolts = HAL_ReadBatteryMillivolts();
    refreshDerivedState(now);

    Serial.println("\n==================================================================");
    Serial.printf(" SMART MEDICATION REMINDER | Boot #%lu\n",
                  (unsigned long)g_sysState.bootCount);
    Serial.printf(" Stored -> Taken: %lu | Missed: %lu | Interval: %lu s\n",
                  (unsigned long)g_sysState.takenDoses,
                  (unsigned long)g_sysState.missedDoses,
                  (unsigned long)g_sysState.doseIntervalSec);
    Serial.printf(" Next dose at RTC %llu ms (now %llu ms)\n",
                  (unsigned long long)s_nextDoseAtMs,
                  (unsigned long long)now);
    Serial.printf(" Wake reason: %d\n", (int)wakeReason);
    Serial.println("==================================================================");

    // ------------------------------------------------------------------------
    // Route the initial state based on the deep-sleep wake cause
    // ------------------------------------------------------------------------
    if (wakeReason == ESP_SLEEP_WAKEUP_EXT0) {
        // Button wake -> BLE streaming window (never an immediate alarm)
        Serial.println("[WAKE] Button (EXT0) -> BLE mode.");

        if (!HAL_WaitForButtonRelease(BUTTON_RELEASE_TIMEOUT_MS)) {
            Serial.println("[WAKE] Warning: button still held; starting BLE anyway.");
        }
        if (g_buttonSemaphore != NULL) {
            xSemaphoreTake(g_buttonSemaphore, 0);   // drop bounce/residual event
        }

        BLE_StartService(s_cmdQueue);
        now = HAL_NowMs();

        if (!g_sysState.isBleActive) {
            Serial.println("[WAKE] BLE init failed - sleeping until the next dose.");
            enterDeepSleep(now);
        }

        s_bleStartMs    = now;
        s_lastStreamMs  = now;
        s_lastBatteryMs = now;

        if (now >= s_nextDoseAtMs) {
            startAlarm(now);
        } else {
            HAL_ArmDoseTimer((uint32_t)(s_nextDoseAtMs - now));
        }
    } else if (wakeReason == ESP_SLEEP_WAKEUP_TIMER) {
        // RTC timer wake -> alarm-only mode (radio OFF, 80 MHz)
        Serial.println("[WAKE] RTC timer -> alarm mode.");

        HAL_SetModemSleepMode();
        now = HAL_NowMs();

        // The RTC timer may fire slightly early on a long interval: re-sleep
        // for the remainder instead of ringing the alarm early.
        if (now + (uint64_t)WAKE_EARLY_TOLERANCE_MS < s_nextDoseAtMs) {
            uint64_t remaining = s_nextDoseAtMs - now;
            Serial.printf("[WAKE] Early timer wake - re-sleeping for %llu ms.\n",
                          (unsigned long long)remaining);
            HAL_EnterDeepSleep((uint32_t)remaining);
        }

        startAlarm(now);
    } else {
        // Cold boot, power-on reset or software reset -> BLE provisioning mode
        Serial.println("[WAKE] Power-on/reset -> BLE provisioning mode.");

        BLE_StartService(s_cmdQueue);
        now = HAL_NowMs();

        if (!g_sysState.isBleActive) {
            Serial.println("[WAKE] BLE init failed - sleeping until the next dose.");
            enterDeepSleep(now);
        }

        s_bleStartMs    = now;
        s_lastStreamMs  = now;
        s_lastBatteryMs = now;

        if (now >= s_nextDoseAtMs) {
            startAlarm(now);
        } else {
            HAL_ArmDoseTimer((uint32_t)(s_nextDoseAtMs - now));
        }
    }

    // ------------------------------------------------------------------------
    // Launch FreeRTOS tasks on core 1 (after all initial state is settled)
    // ------------------------------------------------------------------------
    BaseType_t alarmTaskOk = xTaskCreatePinnedToCore(
        vAlarmOutputTask, "AlarmOutTask", 2048, NULL, 2, NULL, 1);
    BaseType_t sysTaskOk = xTaskCreatePinnedToCore(
        vSystemControlTask, "SysCtrlTask", 6144, NULL, 3, NULL, 1);

    if (alarmTaskOk != pdPASS || sysTaskOk != pdPASS) {
        Serial.println("[FATAL] FreeRTOS task creation failed.");
    }

    Serial.println("[BOOT] System ready.");
}

void loop() {
    // Execution is driven entirely by FreeRTOS tasks and hardware interrupts.
    vTaskDelay(pdMS_TO_TICKS(1000));
}
