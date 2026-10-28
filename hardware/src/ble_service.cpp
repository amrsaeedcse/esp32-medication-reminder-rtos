#include "ble_service.h"
#include "hal_manager.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ============================================================================
// 1. PRIVATE BLE MODULE STATE
// ============================================================================
static BLEServer*         s_pServer        = NULL;
static BLECharacteristic* s_pStreamChar    = NULL;
static BLECharacteristic* s_pCommandChar   = NULL;
static QueueHandle_t      s_commandQueue   = NULL;

static volatile bool s_clientConnected    = false;
static volatile bool s_restartAdvertising = false;
static volatile bool s_justConnected      = false;

// ============================================================================
// 2. BLE SERVER CONNECTION CALLBACKS (run in the Bluedroid task)
// ============================================================================
class ServerConnectionCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) override {
        s_clientConnected = true;
        s_justConnected   = true;
        Serial.println("[BLE] Flutter app connected.");
    }

    void onDisconnect(BLEServer* pServer) override {
        s_clientConnected = false;
        Serial.println("[BLE] Flutter app disconnected.");
        if (g_sysState.isBleActive) {
            s_restartAdvertising = true;   // restarted from the system task
        }
    }
};

// ============================================================================
// 3. COMMAND WRITE CALLBACK (never executes the command itself)
// ============================================================================
class CommandWriteCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) override {
        // getValue() returns String on core 3.x and std::string on core 2.x;
        // c_str() keeps this source compatible with both.
        String raw(pCharacteristic->getValue().c_str());
        raw.trim();
        if (raw.isEmpty()) {
            return;
        }
        raw.toUpperCase();

        AppCommand cmd = {AppCommandType::None, 0};

        if (raw.startsWith("SET:")) {
            long seconds = raw.substring(4).toInt();
            if (seconds < MIN_DOSE_INTERVAL_SEC || seconds > MAX_DOSE_INTERVAL_SEC) {
                Serial.printf("[BLE RX] Rejected interval: %s\n", raw.c_str());
                return;
            }
            cmd.type  = AppCommandType::SetInterval;
            cmd.value = (uint32_t)seconds;
        } else if (raw == "RESET") {
            cmd.type = AppCommandType::ResetCounters;
        } else if (raw == "SILENCE") {
            cmd.type = AppCommandType::SilenceAlarm;
        } else if (raw == "SLEEP") {
            cmd.type = AppCommandType::Sleep;
        } else {
            Serial.printf("[BLE RX] Unknown command: %s\n", raw.c_str());
            return;
        }

        Serial.printf("[BLE RX] Queued command: %s\n", raw.c_str());

        if (s_commandQueue == NULL) {
            Serial.println("[BLE RX] ERROR: command queue not available.");
            return;
        }
        if (xQueueSend(s_commandQueue, &cmd, 0) != pdTRUE) {
            Serial.println("[BLE RX] ERROR: command queue full.");
        }
    }
};

// ============================================================================
// 4. PUBLIC BLE SERVICE IMPLEMENTATION
// ============================================================================
void BLE_StartService(QueueHandle_t commandQueue) {
    if (g_sysState.isBleActive) {
        return;
    }

    s_commandQueue = commandQueue;
    s_clientConnected    = false;
    s_restartAdvertising = false;
    s_justConnected      = false;

    HAL_SetFullActiveMode();   // 240 MHz for the BLE stack

    if (!BLEDevice::init(BLE_DEVICE_NAME)) {
        Serial.println("[BLE] ERROR: BLEDevice::init() failed.");
        return;
    }
    BLEDevice::setMTU(BLE_LOCAL_MTU);

    s_pServer = BLEDevice::createServer();
    if (s_pServer == NULL) {
        Serial.println("[BLE] ERROR: createServer() failed.");
        BLEDevice::deinit(true);
        return;
    }
    s_pServer->setCallbacks(new ServerConnectionCallbacks());
    s_pServer->advertiseOnDisconnect(false);   // BLE_Poll() handles the restart

    BLEService* pService = s_pServer->createService(BLE_SERVICE_UUID);
    if (pService == NULL) {
        Serial.println("[BLE] ERROR: createService() failed.");
        BLEDevice::deinit(true);
        s_pServer = NULL;
        return;
    }

    // Characteristic 1: 20-byte binary telemetry (Read + Notify)
    s_pStreamChar = pService->createCharacteristic(
        BLE_CHAR_STREAM_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    if (s_pStreamChar == NULL) {
        Serial.println("[BLE] ERROR: telemetry characteristic creation failed.");
        BLEDevice::deinit(true);
        s_pServer = NULL;
        return;
    }
    s_pStreamChar->addDescriptor(new BLE2902());

    // Characteristic 2: text commands from Flutter (Write + Write-NR)
    s_pCommandChar = pService->createCharacteristic(
        BLE_CHAR_COMMAND_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
    );
    if (s_pCommandChar == NULL) {
        Serial.println("[BLE] ERROR: command characteristic creation failed.");
        BLEDevice::deinit(true);
        s_pServer = NULL;
        s_pStreamChar = NULL;
        return;
    }
    s_pCommandChar->setCallbacks(new CommandWriteCallbacks());

    if (!pService->start()) {
        Serial.println("[BLE] ERROR: service start failed.");
        BLEDevice::deinit(true);
        s_pServer = NULL;
        s_pStreamChar = NULL;
        s_pCommandChar = NULL;
        return;
    }

    g_sysState.isBleActive = true;

    BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(BLE_SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);   // 7.5 ms connection interval
    BLEDevice::startAdvertising();

    Serial.println("[BLE] Advertising as '" BLE_DEVICE_NAME "' (45 s window).");
}

void BLE_StopService() {
    if (!g_sysState.isBleActive && s_pServer == NULL) {
        return;
    }

    g_sysState.isBleActive = false;
    s_clientConnected      = false;
    s_restartAdvertising   = false;
    s_justConnected        = false;

    if (BLEDevice::getInitialized()) {
        BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
        if (pAdvertising != NULL) {
            pAdvertising->stop();
        }
        // Give the controller a moment to terminate the link before deinit.
        vTaskDelay(pdMS_TO_TICKS(50));
        BLEDevice::deinit(true);
    }

    // Avoid dangling pointers after the stack memory is released.
    s_pServer      = NULL;
    s_pStreamChar  = NULL;
    s_pCommandChar = NULL;

    Serial.println("[BLE] Radio powered OFF.");
}

void BLE_Poll() {
    if (!g_sysState.isBleActive || s_pServer == NULL) {
        return;
    }

    if (s_restartAdvertising) {
        s_restartAdvertising = false;
        BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
        if (pAdvertising != NULL) {
            pAdvertising->start();
        }
    }

    if (s_justConnected) {
        s_justConnected = false;
        BLE_StreamTelemetry();
    }
}

void BLE_StreamTelemetry() {
    if (!g_sysState.isBleActive || s_pStreamChar == NULL) {
        return;
    }

    TelemetryPacket packet;
    packet.version = TELEMETRY_VERSION;

    uint8_t flags = 0;
    if (g_sysState.isAlarmRinging) {
        flags |= TELEM_FLAG_ALARM;
    }
    if (g_sysState.isBleActive) {
        flags |= TELEM_FLAG_BLE;
    }
    if (s_clientConnected) {
        flags |= TELEM_FLAG_CLIENT;
    }
    packet.flags = flags;

    packet.takenDoses     = g_sysState.takenDoses;
    packet.missedDoses    = g_sysState.missedDoses;
    packet.intervalSec    = g_sysState.doseIntervalSec;
    packet.secondsToNext  = g_sysState.secondsToNextDose;

    int32_t mv = g_sysState.batteryMillivolts;
    if (mv < -1) {
        mv = -1;
    } else if (mv > 32767) {
        mv = 32767;
    }
    packet.batteryMillivolts = (int16_t)mv;

    s_pStreamChar->setValue((uint8_t*)&packet, sizeof(packet));

    if (s_clientConnected) {
        s_pStreamChar->notify();
    }
}

bool BLE_IsClientConnected() {
    return s_clientConnected;
}
