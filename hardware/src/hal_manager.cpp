#include "hal_manager.h"
#include <Preferences.h>
#include "esp_wifi.h"
#include "esp_bt.h"
#include "esp_rtc_time.h"
#include "driver/rtc_io.h"

// ============================================================================
// 1. PRIVATE & RTC-RETAINED VARIABLES
// ============================================================================
// Stored in 8 KB RTC slow memory (survives deep sleep and software resets)
RTC_DATA_ATTR static uint32_t s_rtcBootCount       = 0;
RTC_DATA_ATTR static uint32_t s_rtcTakenDoses      = 0;
RTC_DATA_ATTR static uint32_t s_rtcMissedDoses     = 0;
RTC_DATA_ATTR static uint32_t s_rtcDoseIntervalSec = DEFAULT_DOSE_INTERVAL_SEC;
RTC_DATA_ATTR static bool     s_rtcInitialized     = false;

// Flash ROM (NVS) instance for permanent non-volatile storage
static Preferences s_preferences;

// Hardware GPTimer instance (created once per boot)
static hw_timer_t* s_doseHwTimer = NULL;

// Hardware ISR debounce timestamp
static volatile uint32_t s_lastButtonIsrTick = 0;

// Global state & semaphore instances
SystemState_t g_sysState = {
    0, 0, 0, DEFAULT_DOSE_INTERVAL_SEC,
    false, false, false, false, false,
    0, -1
};
SemaphoreHandle_t g_buttonSemaphore = NULL;

// ============================================================================
// 2. HARDWARE INTERRUPT SERVICE ROUTINES (IRAM_ATTR)
// ============================================================================

// External pin ISR: fired when the push button (GPIO 33) is pressed in active mode
static void IRAM_ATTR onButtonPressISR() {
    if (g_buttonSemaphore == NULL) {
        return;
    }
    uint32_t nowTick = xTaskGetTickCountFromISR();
    if ((uint32_t)(nowTick - s_lastButtonIsrTick) > pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS)) {
        s_lastButtonIsrTick = nowTick;
        BaseType_t higherPriorityTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR(g_buttonSemaphore, &higherPriorityTaskWoken);
        if (higherPriorityTaskWoken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

// Hardware timer ISR: fired when the medication deadline arrives while awake
static void IRAM_ATTR onDoseTimerISR() {
    g_sysState.activeDoseTimerExpired = true;
}

// ============================================================================
// 3. MONOTONIC TIME BASE (keeps counting through deep sleep)
// ============================================================================
uint64_t HAL_NowMs() {
    // esp_rtc_get_time_us() accumulates RTC ticks in RTC-retained memory, so the
    // returned value is monotonic across deep sleep and software resets.
    return esp_rtc_get_time_us() / 1000ULL;
}

// ============================================================================
// 4. INITIALISATION & FLASH ROM (NVS) PERSISTENCE
// ============================================================================
esp_sleep_wakeup_cause_t HAL_Init() {
    // Release GPIO 33 from the RTC domain so the digital GPIO matrix can use it
    rtc_gpio_deinit(BUTTON_PIN);

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    // Configure LEDC hardware PWM for the buzzer (Arduino-ESP32 core 3.x API)
    if (!ledcAttach(BUZZER_PIN, TONE_FREQ_HIGH_HZ, PWM_BUZZER_RESOLUTION)) {
        Serial.println("[HAL] ERROR: ledcAttach() failed - buzzer disabled.");
    }
    ledcWriteTone(BUZZER_PIN, 0);

#if ENABLE_BATTERY_ADC
    analogReadResolution(12);
    analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
#endif

    // Create the binary semaphore and attach the button edge interrupt
    g_buttonSemaphore = xSemaphoreCreateBinary();
    if (g_buttonSemaphore == NULL) {
        Serial.println("[HAL] ERROR: could not create button semaphore.");
    } else {
        s_lastButtonIsrTick = xTaskGetTickCount();
        attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), onButtonPressISR, FALLING);
    }

    HAL_InitDoseTimer();

    // Increment the boot counter and load persistent configuration only on the
    // first boot after power-on (RTC memory keeps it across deep sleep).
    s_rtcBootCount++;
    if (!s_rtcInitialized) {
        HAL_LoadFromFlash();
        s_rtcInitialized = true;
    }

    g_sysState.bootCount              = s_rtcBootCount;
    g_sysState.takenDoses             = s_rtcTakenDoses;
    g_sysState.missedDoses            = s_rtcMissedDoses;
    g_sysState.doseIntervalSec        = s_rtcDoseIntervalSec;
    g_sysState.isBleActive            = false;
    g_sysState.isAlarmRinging         = false;
    g_sysState.requestAckChirp        = false;
    g_sysState.outputsMuted           = false;
    g_sysState.activeDoseTimerExpired = false;
    g_sysState.secondsToNextDose      = 0;
    g_sysState.batteryMillivolts      = -1;

    return esp_sleep_get_wakeup_cause();
}

void HAL_LoadFromFlash() {
    if (!s_preferences.begin("med_rom", true)) {
        Serial.println("[NVS] No saved configuration - using defaults.");
        return;
    }

    s_rtcTakenDoses  = s_preferences.getUInt("taken", 0);
    s_rtcMissedDoses = s_preferences.getUInt("missed", 0);

    uint32_t interval = s_preferences.getUInt("interval", DEFAULT_DOSE_INTERVAL_SEC);
    if (interval < MIN_DOSE_INTERVAL_SEC || interval > MAX_DOSE_INTERVAL_SEC) {
        interval = DEFAULT_DOSE_INTERVAL_SEC;
    }
    s_rtcDoseIntervalSec = interval;

    s_preferences.end();
}

void HAL_SaveStatsToFlash() {
    s_rtcTakenDoses  = g_sysState.takenDoses;
    s_rtcMissedDoses = g_sysState.missedDoses;

    if (!s_preferences.begin("med_rom", false)) {
        Serial.println("[NVS] ERROR: could not open namespace for writing.");
        return;
    }
    s_preferences.putUInt("taken", s_rtcTakenDoses);
    s_preferences.putUInt("missed", s_rtcMissedDoses);
    s_preferences.end();
}

void HAL_SaveIntervalToFlash(uint32_t newIntervalSec) {
    if (newIntervalSec < MIN_DOSE_INTERVAL_SEC || newIntervalSec > MAX_DOSE_INTERVAL_SEC) {
        Serial.printf("[NVS] Rejected out-of-range interval: %lu s\n",
                      (unsigned long)newIntervalSec);
        return;
    }

    g_sysState.doseIntervalSec = newIntervalSec;
    s_rtcDoseIntervalSec       = newIntervalSec;

    if (!s_preferences.begin("med_rom", false)) {
        Serial.println("[NVS] ERROR: could not open namespace for writing.");
        return;
    }
    s_preferences.putUInt("interval", newIntervalSec);
    s_preferences.end();
}

void HAL_ResetCountersInFlash() {
    g_sysState.takenDoses  = 0;
    g_sysState.missedDoses = 0;
    s_rtcTakenDoses        = 0;
    s_rtcMissedDoses       = 0;

    if (!s_preferences.begin("med_rom", false)) {
        Serial.println("[NVS] ERROR: could not open namespace for writing.");
        return;
    }
    s_preferences.putUInt("taken", 0);
    s_preferences.putUInt("missed", 0);
    s_preferences.end();
}

// ============================================================================
// 5. POWER MODES & CONSUMPTION REPORT
// ============================================================================
void HAL_SetModemSleepMode() {
    // Radio OFF, CPU @ 80 MHz (alarm-only mode)
    if (btStarted()) {
        btStop();
    }
    esp_wifi_stop();          // safe no-op if Wi-Fi was never initialised
    setCpuFrequencyMhz(80);
}

void HAL_SetFullActiveMode() {
    // Full speed clock for the BLE radio stack
    setCpuFrequencyMhz(240);
}

void HAL_PrintPowerReport() {
    float pModemBest  = SUPPLY_VOLTAGE_V * (I_MODEM_CHIP_BEST_MA + I_PIN_BEST_MA);
    float pModemWorst = SUPPLY_VOLTAGE_V * (I_MODEM_CHIP_WORST_MA + I_PIN_WORST_MA);
    float pBleBest    = SUPPLY_VOLTAGE_V * I_ACTIVE_CHIP_BEST_MA;
    float pBleWorst   = SUPPLY_VOLTAGE_V * I_ACTIVE_CHIP_WORST_MA;
    float pPeakBest   = SUPPLY_VOLTAGE_V * (I_ACTIVE_CHIP_BEST_MA + I_PIN_BEST_MA);
    float pPeakWorst  = SUPPLY_VOLTAGE_V * (I_ACTIVE_CHIP_WORST_MA + I_PIN_WORST_MA);
    float pSleepBest  = SUPPLY_VOLTAGE_V * (I_DEEP_SLEEP_BEST_UA / 1000.0f);
    float pSleepWorst = SUPPLY_VOLTAGE_V * (I_DEEP_SLEEP_WORST_UA / 1000.0f);

    Serial.println("------------------------------------------------------------------");
    Serial.println(" POWER CONSUMPTION ANALYSIS (V = 3.3V | Best vs. Worst Case):");
    Serial.printf(" 1. Alarm Only (Modem-Sleep) : Best = %.2f mW (9mA)  | Worst = %.2f mW (32mA)\n",
                  pModemBest, pModemWorst);
    Serial.printf(" 2. BLE Only (Active Mode)   : Best = %.2f mW (80mA) | Worst = %.2f mW (260mA)\n",
                  pBleBest, pBleWorst);
    Serial.printf(" 3. BLE + Alarm (Peak State) : Best = %.2f mW (86mA) | Worst = %.2f mW (272mA)\n",
                  pPeakBest, pPeakWorst);
    Serial.printf(" 4. Deep Sleep Mode          : Best = %.3f mW (10uA) | Worst = %.3f mW (150uA)\n",
                  pSleepBest, pSleepWorst);
    Serial.println("------------------------------------------------------------------");
}

// ============================================================================
// 6. BUZZER & LED DRIVER FUNCTIONS
// ============================================================================
void HAL_BuzzerTone(uint32_t freqHz) {
    ledcWriteTone(BUZZER_PIN, freqHz);   // freqHz == 0 silences the buzzer
}

void HAL_LedWrite(bool on) {
    digitalWrite(LED_PIN, on ? HIGH : LOW);
}

void HAL_PlayAckChirp() {
    HAL_LedWrite(true);
    HAL_BuzzerTone(TONE_FREQ_ACK_HZ);
    vTaskDelay(pdMS_TO_TICKS(120));
    HAL_BuzzerTone(0);
    HAL_LedWrite(false);
}

// ============================================================================
// 7. HARDWARE GPTIMER ENGINE (Arduino-ESP32 core 3.x API)
// ============================================================================
void HAL_InitDoseTimer() {
    if (s_doseHwTimer != NULL) {
        return;
    }

    // 1 MHz time base -> timerAlarm() values are in microseconds
    s_doseHwTimer = timerBegin(DOSE_TIMER_FREQ_HZ);
    if (s_doseHwTimer == NULL) {
        Serial.println("[HW TIMER] ERROR: no free GPTimer available.");
        return;
    }

    timerAttachInterrupt(s_doseHwTimer, &onDoseTimerISR);
    timerStop(s_doseHwTimer);
    timerWrite(s_doseHwTimer, 0);
}

void HAL_ArmDoseTimer(uint32_t delayMs) {
    if (s_doseHwTimer == NULL) {
        HAL_InitDoseTimer();
    }
    if (s_doseHwTimer == NULL) {
        return;
    }

    if (delayMs == 0) {
        delayMs = 1;
    }

    // Stop first, clear the previous event, reload and start. timerAlarm() alone
    // does not start the timer on core 3.x - timerStart() is required.
    timerStop(s_doseHwTimer);
    g_sysState.activeDoseTimerExpired = false;
    timerWrite(s_doseHwTimer, 0);

    uint64_t ticks = (uint64_t)delayMs * (DOSE_TIMER_FREQ_HZ / 1000ULL);
    timerAlarm(s_doseHwTimer, ticks, false, 0);
    timerStart(s_doseHwTimer);

    Serial.printf("[HW TIMER] Armed for %lu ms.\n", (unsigned long)delayMs);
}

void HAL_DisarmDoseTimer() {
    if (s_doseHwTimer != NULL) {
        timerStop(s_doseHwTimer);
        timerWrite(s_doseHwTimer, 0);
    }
    // The timer ISR runs on the same core as the system task, so once the timer
    // is stopped this clear cannot race with a new event.
    g_sysState.activeDoseTimerExpired = false;
}

// ============================================================================
// 8. BUTTON RELEASE & SAFE DEEP-SLEEP TRANSITION
// ============================================================================
bool HAL_WaitForButtonRelease(uint32_t timeoutMs) {
    if (digitalRead(BUTTON_PIN) != LOW) {
        return true;
    }

    uint32_t start = millis();
    while (digitalRead(BUTTON_PIN) == LOW) {
        if ((uint32_t)(millis() - start) >= timeoutMs) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    vTaskDelay(pdMS_TO_TICKS(30));   // contact-bounce settling
    return digitalRead(BUTTON_PIN) != LOW;
}

int HAL_ReadBatteryMillivolts() {
#if ENABLE_BATTERY_ADC
    uint32_t pinMv = analogReadMilliVolts(BATTERY_ADC_PIN);
    return (int)(pinMv * BATTERY_DIVIDER_RATIO);
#else
    return -1;
#endif
}

void HAL_EnterDeepSleep(uint32_t sleepMs) {
    HAL_DisarmDoseTimer();
    HAL_BuzzerTone(0);
    HAL_LedWrite(false);

    // Detach the active-mode edge interrupt before reconfiguring for EXT0
    detachInterrupt(digitalPinToInterrupt(BUTTON_PIN));

    // EXT0 is level-triggered: sleeping while the button is still held would
    // wake the chip immediately and create a wake-up loop.
    bool released = HAL_WaitForButtonRelease(BUTTON_RELEASE_TIMEOUT_MS);

    // RTC-domain pull-up keeps the button line defined while the digital domain
    // is powered down. EXT0 keeps RTC_PERIPH powered automatically.
    rtc_gpio_init(BUTTON_PIN);
    rtc_gpio_set_direction(BUTTON_PIN, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(BUTTON_PIN);
    rtc_gpio_pulldown_dis(BUTTON_PIN);

    if (sleepMs < MIN_SLEEP_MS) {
        sleepMs = MIN_SLEEP_MS;
    }
    esp_sleep_enable_timer_wakeup((uint64_t)sleepMs * 1000ULL);

    if (released) {
        esp_sleep_enable_ext0_wakeup(BUTTON_PIN, 0);
        Serial.printf("[SLEEP] Deep sleep for %lu ms (timer + EXT0 armed).\n",
                      (unsigned long)sleepMs);
    } else {
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT0);
        Serial.printf("[SLEEP] WARNING: button still held. Sleeping %lu ms with timer wake only.\n",
                      (unsigned long)sleepMs);
    }

    HAL_PrintPowerReport();
    Serial.flush();

    esp_deep_sleep_start();
    while (true) {
        // esp_deep_sleep_start() never returns
    }
}
