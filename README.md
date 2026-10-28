# Smart IoT Medication Reminder (ESP32 + FreeRTOS + Deep Sleep + BLE)

[![PlatformIO](https://img.shields.io/badge/PlatformIO-Compatible-orange.svg)](https://platformio.org/)
[![ESP32](https://img.shields.io/badge/ESP--IDF%20%2F%20Arduino-ESP32-blue.svg)](https://www.espressif.com/)
[![FreeRTOS](https://img.shields.io/badge/RTOS-FreeRTOS-green.svg)](https://www.freertos.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

An ultra-low-power, event-driven smart medical reminder node built on the **ESP32** utilizing **FreeRTOS**, persistent **RTC Slow Memory**, dual wake-up sources (`Timer` & `EXT0`), **Flash ROM (NVS)** storage, and real-time **Bluetooth Low Energy (BLE)** streaming for companion mobile applications.

---

## 🌟 Key Engineering Features

1. **Dual Power-Domain Architecture:**
   - **Deep Sleep State:** Shuts off power to the Dual-Core CPUs, main 520 KB SRAM, and radios. Conserves battery by keeping only the 8 KB RTC Slow RAM and RTC power island active (`10 µA` silicon baseline).
   - **Modem-Sleep Alarm State:** During medication alerts, the radio remains powered OFF and the CPU clock scales down to **80 MHz** to provide steady current consumption (~45 mA) without RF spikes.
   - **Active BLE Streaming State:** When awakened by the patient via `EXT0`, the system clocks up to **240 MHz** to service a 45-second BLE GATT streaming window.

2. **Conflict-Free State Machine:**
   - If the medication timer expires while a BLE session is active, the system **does not disconnect BLE**; it initiates the buzzer alarm concurrently while streaming live `"ALARM_RINGING"` status packets.
   - Button press prioritization: First press silences the alarm and records a `TAKEN` dose; subsequent press closes the BLE session early and transitions to sleep.

3. **Multi-Tiered Memory Hierarchy:**
   - **Flash ROM (NVS):** Stores permanent configurations (e.g., custom dose intervals from Flutter) so parameters survive complete battery depletion.
   - **RTC Slow Memory (`RTC_DATA_ATTR`):** Caches counters across sleep reboots without causing flash memory wear-out.
   - **Main SRAM:** Fast operational buffer used during active states.

---

## 🔌 Hardware Wiring & Pin Mapping

| Peripheral | ESP32 Pin | Domain | Notes |
| :--- | :--- | :--- | :--- |
| **Push Button** | `GPIO 33` | RTC IO (`RTC_GPIO`) | Internal RTC Pull-up enabled; level-triggered `EXT0` wake-up |
| **Piezo Buzzer** | `GPIO 25` | Main Domain | Driven by Hardware **LEDC PWM** (2.0 kHz / 1.2 kHz medical tones) |
| **Status LED** | `GPIO 2` | Main Domain | Built-in DevKit LED / External visual indicator |
| **Power Supply** | `3.3V / GND` | Power Rails | Regulated 3.3V or LiPo battery via onboard LDO |

---

## 📊 Power Consumption Analysis (Datasheet & Lab Specifications)

All calculations assume standard operating supply voltage: **$V = 3.3\text{ V}$**.

$$P_{\text{active}} = V \times I_{\text{active}}$$
$$P_{\text{sleep}} = V \times I_{\text{sleep}}$$

| Operating State | Mode / Scenario | Current ($I$) | Power ($P = V \times I$) |
| :--- | :--- | :--- | :--- |
| **1. Alarm Mode (Radio OFF)** | **Best Case** (Slow Clock + Typical Pin) | $3\text{ mA} + 6\text{ mA} = \mathbf{9.0\text{ mA}}$ | **`29.70 mW`** |
| | **Worst Case** (Fast Clock + Max Pin) | $20\text{ mA} + 12\text{ mA} = \mathbf{32.0\text{ mA}}$ | **`105.60 mW`** |
| **2. BLE Streaming (Radio ON)** | **Best Case** (BLE RX / Idle Radio) | $80\text{ mA} + 0\text{ mA} = \mathbf{80.0\text{ mA}}$ | **`264.00 mW`** |
| | **Worst Case** (Max RF TX Burst) | $260\text{ mA} + 0\text{ mA} = \mathbf{260.0\text{ mA}}$ | **`858.00 mW`** |
| **3. Concurrent State (BLE + Alarm)** | **Absolute Worst-Case Peak** | $260\text{ mA} + 12\text{ mA} = \mathbf{272.0\text{ mA}}$ | **`897.60 mW`** |
| **4. Deep Sleep State (Chip Only)** | **Best Case** (RTC Timer + Memory) | $\mathbf{10.0\text{ µA}}$ | **`0.033 mW` (`33 µW`)** |
| | **Worst Case** (RTC + Active ULP) | $\mathbf{0.15\text{ mA}}$ ($150\text{ µA}$) | **`0.495 mW` (`495 µW`)** |

> *Note for Physical Multimeter Testing: A standard commercial NodeMCU-32S / DevKit V1 board will draw an additional ~6.5 mA in Deep Sleep due to the onboard linear regulator (AMS1117) and power indicator LED.*

---

## 🏗️ Software Architecture

```text
esp32-medication-reminder-rtos/
├── hardware/
│   ├── platformio.ini         # PlatformIO project configuration (esp32dev)
│   ├── include/
│   │   ├── config.h           # Hardware pinout, timing & power constants
│   │   ├── app_commands.h     # BLE command definitions (SetInterval, Reset, Sleep)
│   │   ├── hal_manager.h      # Hardware Abstraction Layer interface
│   │   └── ble_service.h      # BLE GATT server & telemetry definitions
│   └── src/
│       ├── hal_manager.cpp    # GPIO, LEDC PWM, GPTimer, NVS & RTC sleep control
│       ├── ble_service.cpp    # BLE advertising, GATT characteristics, command queue
│       └── main.cpp           # FreeRTOS tasks (vSystemControlTask, vAlarmOutputTask)
├── software/                  # Flutter mobile companion application (In Progress)
├── project.md                 # Complete engineering documentation & theoretical guide
└── README.md
```

---

## 🚀 Getting Started

### Prerequisites
* [VS Code](https://code.visualstudio.com/) with the [PlatformIO IDE](https://platformio.org/install/ide?install=vscode) extension installed.
* An **ESP32 DevKit** board with a push button on `GPIO 33` and buzzer on `GPIO 25`.

### Build & Flash
1. Open the project in VS Code:
   ```bash
   cd hardware
   ```
2. Build the firmware:
   ```bash
   pio run
   ```
3. Upload to your connected ESP32:
   ```bash
   pio run --target upload
   ```
4. Open the serial monitor (115200 baud):
   ```bash
   pio device monitor
   ```

---

## 👥 Team & Roles
* **Low-Power & Silicon Architecture:** RTC Power domains, Sleep Wake-up routing, NVS Flash & Power calculations.
* **RTOS & Firmware Engineering:** FreeRTOS dual-task management, Hardware ISR debounce, binary semaphore synchronization.
* **BLE GATT & Telemetry Protocol:** Binary telemetry packet serialization, lock-free timer updates via queue.
* **Mobile & Systems Integration:** Flutter BLE interface design, video demonstration & report documentation.

---

## 📄 License
This project is open-source and licensed under the [MIT License](LICENSE).
