# Smart Timer-Based Medication Reminder (ESP32 + FreeRTOS + BLE + Flutter)

## 1. Project Overview
This project implements an ultra-low-power, real-time **Smart Medication Reminder** built on the **ESP32** using **FreeRTOS**, hardware interrupts, power-domain management, and a companion **Flutter Mobile Application** over **Bluetooth Low Energy (BLE)**.

### Why BLE Direct Streaming Instead of Wi-Fi / Cloud IoT?
1. **Personal Proximity & Security:** A medication reminder is a personal medical device. Configuration and dose acknowledgement should only occur when the user is physically near the device.
2. **Power Budget:** Wi-Fi requires high current peaks (`180mA–260mA`) and several seconds for router handshakes, rapidly draining a small battery. BLE enables lightweight, low-power **real-time streaming** directly between the ESP32 and the Flutter app without relying on an external router or internet connection.

---

## 2. Hardware Components & Pin Mapping
| Component | ESP32 Pin | Hardware Domain & Purpose |
| :--- | :--- | :--- |
| **Push Button** | `GPIO 33` | **RTC GPIO** (Supports `EXT0` Deep-Sleep Wake-up + Internal Pull-up + Active Hardware ISR) |
| **Buzzer** | `GPIO 25` | Driven via Hardware **LEDC PWM** for medical double-beep tone generation |
| **Status LED** | `GPIO 2` | Visual indicator for Alarm State and BLE Streaming State |
| **Battery** | `3.3V / VIN` | Small rechargeable LiPo (`3.7V`) or `3.3V` supply |

---

## 3. ESP32 Hardware Power Modes & Clock Architecture

The ESP32 is architecturally divided into two main hardware power/clock domains:
1. **Main Digital Domain:** Houses the Dual-Core Xtensa CPUs (`Core 0` & `Core 1`), `520 KB SRAM`, `FreeRTOS SysTick`, high-speed peripherals (`PWM`, `SPI`, `I2C`, `UART`), and standard digital GPIOs. Driven by the **High-Speed System Clock (`80MHz–240MHz`)**, where flip-flops continuously sample edge interrupts.
2. **RTC Power Domain:** Houses the **RTC Controller / PMU**, **RTC Slow Oscillator (`150 kHz`)**, **RTC Timer & Watchdog (WDT)**, **8 KB RTC RAM**, **ULP Co-processor + ADC**, **Touch/Glitch Filters**, and **RTC GPIOs**.

### How Sleep & Wake-up Work in Hardware
* When the **System Clock** stops during sleep, the Dual-Core CPUs halt completely. No software runs while the CPU is asleep.
* **External Pin Wake-up (`EXT0`):** Because the System Clock is off, synchronous edge-triggered flip-flops are inactive. Instead, the RTC pin uses **asynchronous level-triggered logic** (with a low-frequency RTC glitch filter to reject noise). When triggered, the signal goes directly to the **Hardware PMU**, which turns on the power switches, restarts the **System Clock**, and wakes up the **CPU** back into Active Mode so the CPU can execute code.
* **Small RTC Oscillator (`150 kHz`) Role:** Too slow for heavy peripherals, so it is strictly used as a low-power fallback during sleep for the **RTC Timer**, **RTC Watchdog Timer**, **Pin Noise/Glitch Filtering**, **Touchpads**, and **ULP/ADC sampling**.
* **Software-Controlled Sleep Entry:** Hardware wakes the CPU up into Active Mode, and our software explicitly commands the chip back to sleep once tasks finish.

| Power Mode | CPU & System Clock | Main RAM (520KB) | Radio & Peripherals | Datasheet Current (Slides 5–9) | Project Usage |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **1. Active Mode** | ON (`240MHz` / `80MHz`) | ON (Retained) | All ON + BLE/Wi-Fi Radio ON | `80 mA` (Best) – `260 mA` (Worst/Max) | **BLE Sync & Streaming Mode** (with Flutter App) |
| **2. Modem-Sleep** | ON (Configurable Clock) | ON (Retained) | **Radio OFF** (Peripherals ON) | `3 mA` (Slow/Best) – `20 mA` (Fast/Worst) | **Normal Alarm Mode** (Buzzer + LED + Button ISR) |
| **3. Light-Sleep** | Clock-Gated (Paused) | ON (Retained) | Paused (Resumes instantly) | `~0.8 mA` (`800 µA`) | Not used for multi-hour intervals |
| **4. Deep-Sleep** | Powered OFF (Reboots) | OFF (`8KB RTC RAM` ON) | OFF (Only RTC Timer + `EXT0`) | `10 µA` (Best) – `0.15 mA` (Worst w/ ULP) | **Primary Sleep Mode** between medication doses |
| **5. Hibernation** | Powered OFF (Reboots) | OFF (Even `RTC RAM` OFF) | OFF (Only RTC Timer + `EXT0`) | `~2.5 µA` | Supported when persisting config in **Flash (NVS/ROM)** |

---

## 4. System State Machine & RTOS Execution Flow

### State 1: Sleep Mode (`Deep Sleep` + `Flash NVS` Persistence)
* All configuration parameters (such as `sleepIntervalSec`, `takenDoses`, and `missedDoses`) are persisted in **Flash ROM (`NVS Preferences`)** and mirrored in **RTC Memory**.
* Two wake-up triggers are armed before sleeping:
  1. **Timer Wake-up (`esp_sleep_enable_timer_wakeup`)**: Wakes the board when it is time for the next medication dose.
  2. **Button Wake-up (`esp_sleep_enable_ext0_wakeup` on `GPIO 33`)**: Wakes the board when the user presses the button while asleep to open **BLE Streaming Mode**.

### State 2: Normal Alarm Mode (`Modem-Sleep` — Woken by Timer, BLE is OFF)
* When woken up solely by the medication timer, **BLE and Wi-Fi remain completely OFF** (`Modem-Sleep`) and the CPU clock is reduced to `80MHz` to save battery and provide a steady current reading.
* **FreeRTOS Tasks Launched:**
  * `vAlarmTask`: Generates a medical double-beep tone using hardware PWM (`LEDC`) and blinks the LED.
  * `vButtonHandlerTask`: Waits on a `Binary Semaphore` given by the hardware `IRAM_ATTR` Button ISR (with tick-based debouncing).
* **Button Action (when BLE is OFF):** Pressing the button stops `vAlarmTask`, increments `takenDoses`, saves to Flash/RTC, waits for physical button release, and immediately returns to **Sleep Mode**.
* **Auto-Timeout Battery Guard (`30s`):** If nobody presses the button within 30 seconds, the alarm stops automatically, increments `missedDoses`, and returns to **Sleep Mode**.

### State 3: BLE Streaming & Configuration Mode (`Active Mode` — Woken by Button during Sleep)
* Pressing the button while the ESP32 is asleep wakes the CPU via `EXT0` and opens the **BLE GATT Server** for `45 seconds` to stream live data with the **Flutter App**.
* **Dynamic Timer Reconfiguration (Lock-Free / No Unnecessary Mutex):**
  * Only the BLE Write Callback updates the timer schedule (`uint32_t`, which is hardware-atomic on the 32-bit Xtensa CPU).
  * When Flutter sends a new interval, the callback directly **stops the existing timer**, updates the interval in **Flash ROM (NVS)**, and **starts a fresh timer** with the new schedule—eliminating race conditions without needing a Mutex.

### State 4: Conflict-Free Edge Case (Alarm Triggers *While* BLE Mode is Active!)
What happens if the user is actively connected to **BLE Mode** (watching live telemetry on Flutter) and the medication timer expires at that exact moment?
1. **Do NOT Turn Off BLE:** The timer checks `isBleActive`. Because BLE is currently open, it keeps the BLE connection alive instead of dropping into Modem-Sleep, starts `vAlarmTask` in parallel, and streams an `"ALARM_RINGING"` status to the Flutter app.
2. **Smart Button Priority Check:**
   * If the user presses the physical button **while `isAlarmRinging == true` AND `isBleActive == true`**: The button press **only silences the alarm** and increments `takenDoses` (updating Flutter live), **keeping the BLE session open**!
   * Once the alarm is silent (`isAlarmRinging == false`), pressing the button again (or waiting for the 45-second BLE window to finish) cleanly closes BLE and puts the ESP32 back into **Sleep Mode**.

```text
                        +-----------------------------------+
                        |     SLEEP MODE (Deep Sleep)       |
                        |  Armed: RTC Timer + Button (EXT0) |
                        +-----------------+-----------------+
                                          |
                 +------------------------+------------------------+
                 | (Woken by Timer)                                | (Woken by Button EXT0)
                 v                                                 v
+----------------------------------+             +----------------------------------+
|    MODEM-SLEEP ALARM STATE       |             |     ACTIVE BLE STREAMING MODE    |
|  - Radio OFF (CPU @ 80MHz)       |             |  - BLE Server ON (45s Window)    |
|  - isBleActive = false           |             |  - isBleActive = true            |
|  - isAlarmRinging = true         |             |  - Live Stream with Flutter App  |
|  - Button Press -> Silence Alarm |             |  - Config Changes -> Stop Old    |
|    & Go Directly to Sleep        |             |    Timer, Save Flash, Start New  |
+----------------------------------+             +-----------------+----------------+
                                                                   |
                                            (If Dose Timer Fires While BLE is Open)
                                                                   v
                                                 +----------------------------------+
                                                 |   BLE + ALARM CONCURRENT STATE   |
                                                 |  - Keep BLE ON! Start Alarm Task |
                                                 |  - 1st Button Press: Silence     |
                                                 |    Alarm ONLY (Keep BLE Open!)   |
                                                 |  - BLE Timeout / 2nd Press:      |
                                                 |    Close BLE -> Go to Sleep      |
                                                 +----------------------------------+
```

---

## 5. Power Consumption Analysis (Best Case vs. Worst Case — Lecture & Datasheet Reference)

Required formulas for the project video demonstration (at standard operating voltage $V = 3.3\text{ V}$):
$$P_{\text{active}} = V \times I_{\text{active}}$$
$$P_{\text{sleep}} = V \times I_{\text{sleep}}$$

### A. Component Current Breakdown (From Lecture Slides 5, 6, 8, 9 & 36)
* **GPIO Pin Current (Buzzer / LED Output — Slide 36):**
  * **Best Case (Typical):** $I_{\text{pin(best)}} = 6\text{ mA}$
  * **Worst Case (Maximum):** $I_{\text{pin(worst)}} = 12\text{ mA}$
* **ESP32 Chip Current in Alarm Mode (`Modem-Sleep`, Radio OFF — Slide 6):**
  * **Best Case (Slow Clock):** $I_{\text{modem(best)}} = 3\text{ mA}$
  * **Worst Case (High Speed Clock):** $I_{\text{modem(worst)}} = 20\text{ mA}$
* **ESP32 Chip Current in Full `Active Mode` (Radio ON — Slide 5):**
  * **Best Case (RX / Listening):** $I_{\text{active(best)}} = 80\text{ mA}$ (up to $120\text{ mA}$ in `0dBm` BLE TX)
  * **Worst Case (Max RF TX):** $I_{\text{active(worst)}} = 260\text{ mA}$
* **ESP32 Chip Current in `Deep-Sleep Mode` (Slide 8):**
  * **Best Case (RTC Timer + RTC Memory only):** $I_{\text{sleep(best)}} = 10\text{ }\mu\text{A} = 0.01\text{ mA}$
  * **Worst Case (With ULP Co-processor active):** $I_{\text{sleep(worst)}} = 0.15\text{ mA} = 150\text{ }\mu\text{A}$
  * *(Note: In `Hibernation Mode` — Slide 9, current drops further to $2.5\text{ }\mu\text{A}$).*

---

### B. Summary Power Table: Best Case vs. Worst Case ($V = 3.3\text{ V}$)

| Operating State | Case Type | Current Calculation ($I$) | Total Current ($I$) | Power Consumption ($P = V \times I$) | Reference Source |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **1. Alarm Active (`Modem-Sleep`, Radio OFF)** | **Best Case** | $3\text{ mA (Chip)} + 6\text{ mA (Pin)}$ | **`9.0 mA`** | **`29.70 mW`** | Slides 6 & 36 |
| **1. Alarm Active (`Modem-Sleep`, Radio OFF)** | **Worst Case** | $20\text{ mA (Chip)} + 12\text{ mA (Pin)}$ | **`32.0 mA`** | **`105.60 mW`** | Slides 6 & 36 |
| **2. Alarm + BLE Active (`Full Active Mode`)** | **Best Case** | $80\text{ mA (Chip)} + 6\text{ mA (Pin)}$ | **`86.0 mA`** | **`283.80 mW`** | Slides 5 & 36 |
| **2. Alarm + BLE Active (`Full Active Mode`)** | **Worst Case** | $260\text{ mA (Chip)} + 12\text{ mA (Pin)}$ | **`272.0 mA`** | **`897.60 mW`** | Slides 5 & 36 |
| **3. Deep Sleep Mode (ESP32 Chip)** | **Best Case** | $10\text{ }\mu\text{A (RTC Timer + RAM)}$ | **`10.0 µA`** | **`0.033 mW` (`33 µW`)** | Slide 8 |
| **3. Deep Sleep Mode (ESP32 Chip)** | **Worst Case** | $0.15\text{ mA (RTC + ULP ON)}$ | **`0.15 mA` (`150 µA`)** | **`0.495 mW` (`495 µW`)** | Slide 8 |
| **4. Hibernation Mode (ESP32 Chip)** | **Best Case** | $2.5\text{ }\mu\text{A (RTC Timer Only)}$ | **`2.5 µA`** | **`0.00825 mW` (`8.25 µW`)** | Slide 9 |

> **Practical Hardware Note (DevKit Board vs. Bare Chip):**
> While the bare ESP32 silicon consumes **`10 µA – 150 µA`** in Deep Sleep (Datasheet / Slide 8), measuring a complete **ESP32 DevKit V1** board with a multimeter in series shows an additional **`~6.5 mA` (`~21.45 mW`)** quiescent current due to the onboard `AMS1117` voltage regulator, constant power indicator LED, and USB-to-UART bridge chip. Mentioning both the **Datasheet Best/Worst Case** and the **DevKit Board overhead** demonstrates complete theoretical and practical mastery.

---

### C. Operational Duration: Best Case vs. Worst Case Patient Response
* **Best Case Response Time:** The patient presses the button immediately (**`~2 seconds`** of active alarm time per dose), returning the ESP32 to Deep Sleep right away.
* **Worst Case Response Time (Auto-Timeout Guard):** The patient is away, so the alarm runs until our software auto-timeout cuts it off at **`30 seconds`**, preventing infinite battery drain and logging a missed dose.

---

## 6. Team Roles (2 to 4 Members)
1. **Member 1 — Power Architecture & Non-Volatile Storage (`Deep Sleep` & `Flash NVS`):**
   Configures `RTC Timer` and `EXT0` wake-up sources, manages `NVS Flash` / `RTC` persistence, and performs power consumption calculations ($P = V \times I$).
2. **Member 2 — FreeRTOS Tasks, Hardware Timers & Interrupt Engine:**
   Implements `vAlarmTask` (LEDC PWM), `vButtonHandlerTask`, the `IRAM_ATTR` Button ISR with debounce, `Binary Semaphore` signaling, and state-aware button priority logic.
3. **Member 3 — BLE GATT Streaming Protocol & Hardware Integration:**
   Builds the BLE GATT Server on the ESP32 for live telemetry streaming and lock-free timer reconfiguration upon receiving commands from the mobile app.
4. **Member 4 — Flutter Companion App & Video Demonstration:**
   Develops the Flutter BLE mobile interface (live dose counters, dynamic schedule configuration, and power/battery analyzer) and records the final demonstration.
