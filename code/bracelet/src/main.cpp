#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_now.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include "SparkFun_BMI270_Arduino_Library.h"
#include "../../bracelet_station_protocol.h"

namespace
{
    namespace Pin
    {
        constexpr uint8_t IMU_SDA = 3;
        constexpr uint8_t IMU_SCL = 2;
        constexpr uint8_t BATTERY_ADC = 1;
        constexpr uint8_t VIBRATION_MOTOR = 0;
    }

    constexpr uint32_t SERIAL_BAUD = 115200;
    constexpr uint32_t SAMPLE_PERIOD_MS = 20;
    constexpr uint32_t SENSOR_RETRY_PERIOD_MS = 2000;
    constexpr uint32_t BATTERY_SAMPLE_PERIOD_MS = 1000;
    constexpr uint32_t TELEMETRY_LOG_PERIOD_MS = 5000;
    constexpr uint32_t ACTIVE_SEND_PERIOD_MS = 200;
    constexpr uint32_t INACTIVE_SEND_PERIOD_MS = 10000;
    constexpr uint32_t CHANNEL_HOP_PERIOD_MS = 300;
    constexpr uint32_t STATION_CONTROL_LOCK_MS = 15000;
    constexpr uint32_t ENERGY_WINDOW_MS = 200;
    constexpr uint32_t VIBRATION_PERIOD_MS = 4000;
    constexpr uint32_t VIBRATION_ON_MS = 500;
    constexpr uint32_t VIBRATION_SETTLE_MS = 450;
    constexpr float ACCEL_NOISE_FLOOR_G = 0.003F;
    constexpr float GYRO_NOISE_FLOOR_DPS = 0.6F;
    constexpr float ACCEL_ENERGY_SCALE = 60000.0F;
    constexpr float GYRO_ENERGY_SCALE = 8.0F;
    constexpr float BATTERY_DIVIDER_RATIO = 2.0F;
    constexpr float BATTERY_EMPTY_V = 3.30F;
    constexpr float BATTERY_FULL_V = 4.20F;
    constexpr uint8_t LOW_BATTERY_PERCENT = 30;

    constexpr uint8_t BATTERY_UNKNOWN = 255;
    constexpr uint8_t ESP_NOW_MIN_CHANNEL = 1;
    constexpr uint8_t ESP_NOW_MAX_CHANNEL = 13;
    enum class BraceletState : uint8_t
    {
        Ready = 1,
        LowBattery = 2,
        Fault = 3,
    };

    enum class StationState : uint8_t
    {
        Idle = 0,
        Armed = 1,
        Ringing = 2,
        ValidatingActivity = 3,
        Stopped = 4,
        Fault = 5,
        Unknown = 255,
    };

    enum class ProblemCode : uint8_t
    {
        None = 0,
        BraceletLowBattery = 7,
        SensorFault = 8,
    };

    using BraceletStationProtocol::BraceletStatusPacket;
    using BraceletStationProtocol::PacketHeader;
    using BraceletStationProtocol::StationControlPacket;

    struct MotionReading
    {
        float accelX = 0.0F;
        float accelY = 0.0F;
        float accelZ = 1.0F;
        float gyroX = 0.0F;
        float gyroY = 0.0F;
        float gyroZ = 0.0F;
    };

    struct EnergyTracker
    {
        uint32_t windowStartMs = 0;
        uint32_t lastSampleMs = 0;
        uint32_t energy = 0;
        uint16_t validMs = 0;
        uint32_t lastEnergy = 0;
        uint16_t lastValidMs = 0;
        bool baselineReady = false;
    };

    BMI270 imu;
    Preferences preferences;
    MotionReading latestMotion;
    EnergyTracker energyTracker;

    uint8_t deviceId[6] = {};
    uint32_t sequenceNumber = 0;
    uint32_t lastSampleMs = 0;
    uint32_t lastSensorRetryMs = 0;
    uint32_t lastBatterySampleMs = 0;
    uint32_t lastSendMs = 0;
    uint32_t lastLogMs = 0;
    uint32_t lastEspNowDebugMs = 0;
    uint32_t lastStationControlMs = 0;
    uint32_t lastChannelHopMs = 0;
    uint32_t ignoreMotionUntilMs = 0;
    uint32_t vibrationRequestStartMs = 0;
    uint32_t bootSessionId = 0;
    uint32_t movementEventId = 0;
    uint32_t movementEventUptimeMs = 0;
    uint32_t movementEventEnergy = 0;
    uint16_t movementEventValidMs = 0;
    uint32_t acknowledgedMovementEventId = 0;
    uint32_t activeVibrationRequestId = 0;
    uint32_t vibrationAckId = 0;
    uint32_t lastReceivedControlSequence = 0;

    bool sensorReady = false;
    bool espNowReady = false;
    bool stationPaired = false;
    bool forceStatusSend = false;
    bool vibrationMotorOn = false;
    float batteryVoltage = 0.0F;
    uint8_t batteryPercent = BATTERY_UNKNOWN;
    StationState stationState = StationState::Unknown;
    BraceletState lastSentBraceletState = BraceletState::Fault;
    ProblemCode lastSentProblemCode = ProblemCode::None;
    bool lastSentVibrating = false;
    bool stationRequestsVibration = false;
    uint8_t espNowChannel = ESP_NOW_MIN_CHANNEL;
    uint8_t pairedStationId[6] = {};
    StationControlPacket pendingStationControl = {};
    bool stationControlPending = false;
    portMUX_TYPE espNowMux = portMUX_INITIALIZER_UNLOCKED;

    const uint8_t BROADCAST_PEER[ESP_NOW_ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

    bool beginBmi270()
    {
        const uint8_t addresses[] = {BMI2_I2C_PRIM_ADDR, BMI2_I2C_SEC_ADDR};

        for (uint8_t address : addresses)
        {
            if (imu.beginI2C(address, Wire) == BMI2_OK)
            {
                Serial.print("bmi270_address:0x");
                Serial.println(address, HEX);
                return true;
            }
        }

        Serial.println("bmi270_status:not_found");
        return false;
    }

    float readBatteryVoltage()
    {
        const uint32_t measuredMillivolts = analogReadMilliVolts(Pin::BATTERY_ADC);
        return (static_cast<float>(measuredMillivolts) * BATTERY_DIVIDER_RATIO) / 1000.0F;
    }

    uint8_t estimateBatteryPercent(float voltage)
    {
        if (voltage <= 0.1F)
        {
            return BATTERY_UNKNOWN;
        }

        const float ratio = (voltage - BATTERY_EMPTY_V) / (BATTERY_FULL_V - BATTERY_EMPTY_V);
        const int percent = static_cast<int>(roundf(constrain(ratio, 0.0F, 1.0F) * 100.0F));
        return static_cast<uint8_t>(percent);
    }

    bool isBatteryLow()
    {
        return batteryPercent != BATTERY_UNKNOWN && batteryPercent < LOW_BATTERY_PERCENT;
    }

    void updateBattery()
    {
        const uint32_t now = millis();
        if (now - lastBatterySampleMs < BATTERY_SAMPLE_PERIOD_MS)
        {
            return;
        }

        lastBatterySampleMs = now;
        batteryVoltage = readBatteryVoltage();
        batteryPercent = estimateBatteryPercent(batteryVoltage);
    }

    float noiseFilteredAbs(float value, float noiseFloor)
    {
        const float magnitude = fabsf(value);
        return magnitude <= noiseFloor ? 0.0F : magnitude - noiseFloor;
    }

    void finishEnergyWindows(uint32_t now)
    {
        if (energyTracker.windowStartMs == 0)
        {
            energyTracker.windowStartMs = now;
            return;
        }

        while (now - energyTracker.windowStartMs >= ENERGY_WINDOW_MS)
        {
            energyTracker.lastEnergy = energyTracker.energy;
            energyTracker.lastValidMs = energyTracker.validMs;
            const bool qualifyingMovement =
                energyTracker.lastValidMs >= BraceletStationProtocol::MIN_ENERGY_VALID_MS &&
                energyTracker.lastEnergy >= BraceletStationProtocol::NORMAL_ENERGY_THRESHOLD;
            if (qualifyingMovement)
            {
                movementEventId++;
                if (movementEventId == 0)
                {
                    movementEventId = 1;
                }
                movementEventUptimeMs = energyTracker.windowStartMs + ENERGY_WINDOW_MS;
                movementEventEnergy = energyTracker.lastEnergy;
                movementEventValidMs = energyTracker.lastValidMs;
                acknowledgedMovementEventId = 0;
            }
            energyTracker.energy = 0;
            energyTracker.validMs = 0;
            energyTracker.windowStartMs += ENERGY_WINDOW_MS;
            const bool alarmActive = stationState == StationState::Ringing ||
                                     stationState == StationState::ValidatingActivity;
            forceStatusSend = forceStatusSend || alarmActive;
        }
    }

    void resetEnergyBaseline(uint32_t now)
    {
        energyTracker.baselineReady = false;
        energyTracker.lastSampleMs = now;
    }

    void accumulateEnergy(const MotionReading &reading, uint32_t now)
    {
        finishEnergyWindows(now);

        if (!energyTracker.baselineReady)
        {
            latestMotion = reading;
            energyTracker.lastSampleMs = now;
            energyTracker.baselineReady = true;
            return;
        }

        const uint32_t dt = constrain(now - energyTracker.lastSampleMs, 1UL, 100UL);
        energyTracker.lastSampleMs = now;

        const float accelDx = noiseFilteredAbs(reading.accelX - latestMotion.accelX, ACCEL_NOISE_FLOOR_G);
        const float accelDy = noiseFilteredAbs(reading.accelY - latestMotion.accelY, ACCEL_NOISE_FLOOR_G);
        const float accelDz = noiseFilteredAbs(reading.accelZ - latestMotion.accelZ, ACCEL_NOISE_FLOOR_G);
        const float gyroDx = noiseFilteredAbs(reading.gyroX - latestMotion.gyroX, GYRO_NOISE_FLOOR_DPS);
        const float gyroDy = noiseFilteredAbs(reading.gyroY - latestMotion.gyroY, GYRO_NOISE_FLOOR_DPS);
        const float gyroDz = noiseFilteredAbs(reading.gyroZ - latestMotion.gyroZ, GYRO_NOISE_FLOOR_DPS);

        const float accelEnergy = ((accelDx * accelDx) + (accelDy * accelDy) + (accelDz * accelDz)) * ACCEL_ENERGY_SCALE;
        const float gyroEnergy = ((gyroDx * gyroDx) + (gyroDy * gyroDy) + (gyroDz * gyroDz)) * GYRO_ENERGY_SCALE;
        energyTracker.energy += static_cast<uint32_t>(roundf((accelEnergy + gyroEnergy) * static_cast<float>(dt)));
        energyTracker.validMs = static_cast<uint16_t>(min<uint32_t>(ENERGY_WINDOW_MS, energyTracker.validMs + dt));

        latestMotion = reading;
    }

    void updateMotion()
    {
        const uint32_t now = millis();
        if (now - lastSampleMs < SAMPLE_PERIOD_MS)
        {
            return;
        }

        lastSampleMs = now;
        finishEnergyWindows(now);

        if (now < ignoreMotionUntilMs)
        {
            resetEnergyBaseline(now);
            return;
        }

        if (!sensorReady)
        {
            if (now - lastSensorRetryMs >= SENSOR_RETRY_PERIOD_MS)
            {
                lastSensorRetryMs = now;
                sensorReady = beginBmi270();
            }
            return;
        }

        const int8_t status = imu.getSensorData();
        if (status != BMI2_OK)
        {
            sensorReady = false;
            Serial.print("bmi270_read_error:");
            Serial.println(status);
            return;
        }

        MotionReading reading;
        reading.accelX = imu.data.accelX;
        reading.accelY = imu.data.accelY;
        reading.accelZ = imu.data.accelZ;
        reading.gyroX = imu.data.gyroX;
        reading.gyroY = imu.data.gyroY;
        reading.gyroZ = imu.data.gyroZ;
        accumulateEnergy(reading, now);
    }

    BraceletState currentBraceletState()
    {
        if (!sensorReady)
        {
            return BraceletState::Fault;
        }
        if (isBatteryLow())
        {
            return BraceletState::LowBattery;
        }
        return BraceletState::Ready;
    }

    ProblemCode currentFaultCode()
    {
        if (!sensorReady)
        {
            return ProblemCode::SensorFault;
        }
        if (isBatteryLow())
        {
            return ProblemCode::BraceletLowBattery;
        }
        return ProblemCode::None;
    }

    void fillHeader(PacketHeader &header, uint8_t messageType)
    {
        header.magic = BraceletStationProtocol::MAGIC;
        header.protocolVersion = BraceletStationProtocol::VERSION;
        header.messageType = messageType;
        header.senderRole = BraceletStationProtocol::SENDER_BRACELET;
        memcpy(header.deviceId, deviceId, sizeof(deviceId));
        header.sequence = sequenceNumber++;
        header.uptimeMs = millis();
    }

    bool deviceIdMatches(const uint8_t *left, const uint8_t *right)
    {
        return memcmp(left, right, 6) == 0;
    }

    void loadPairedStation()
    {
        preferences.begin("espnow-link", true);
        if (preferences.getBytesLength("stationId") == sizeof(pairedStationId))
        {
            preferences.getBytes("stationId", pairedStationId, sizeof(pairedStationId));
            stationPaired = true;
        }
        preferences.end();

        Serial.println(stationPaired ? "espnow_pairing:station_loaded" : "espnow_pairing:waiting_for_station");
    }

    void savePairedStation(const uint8_t *stationId)
    {
        memcpy(pairedStationId, stationId, sizeof(pairedStationId));
        preferences.begin("espnow-link", false);
        preferences.putBytes("stationId", pairedStationId, sizeof(pairedStationId));
        preferences.end();
        stationPaired = true;
        Serial.printf("espnow_pairing:station_saved:%02X:%02X:%02X:%02X:%02X:%02X\n",
                      pairedStationId[0], pairedStationId[1], pairedStationId[2],
                      pairedStationId[3], pairedStationId[4], pairedStationId[5]);
    }

    void setEspNowChannel(uint8_t channel)
    {
        if (channel < ESP_NOW_MIN_CHANNEL || channel > ESP_NOW_MAX_CHANNEL || channel == espNowChannel)
        {
            return;
        }

        if (esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) == ESP_OK)
        {
            espNowChannel = channel;
        }
    }

    void updateEspNowChannel()
    {
        if (!espNowReady)
        {
            return;
        }

        const uint32_t now = millis();
        if (lastStationControlMs != 0 && now - lastStationControlMs < STATION_CONTROL_LOCK_MS)
        {
            return;
        }

        if (now - lastChannelHopMs < CHANNEL_HOP_PERIOD_MS)
        {
            return;
        }

        lastChannelHopMs = now;
        const uint8_t nextChannel = espNowChannel >= ESP_NOW_MAX_CHANNEL ? ESP_NOW_MIN_CHANNEL : espNowChannel + 1;
        setEspNowChannel(nextChannel);
        forceStatusSend = true;
    }

    void onEspNowReceive(const uint8_t *, const uint8_t *data, int length)
    {
        if (length != static_cast<int>(sizeof(StationControlPacket)))
        {
            return;
        }

        StationControlPacket packet = {};
        memcpy(&packet, data, sizeof(packet));
        if (packet.header.magic != BraceletStationProtocol::MAGIC ||
            packet.header.protocolVersion != BraceletStationProtocol::VERSION ||
            packet.header.messageType != BraceletStationProtocol::MESSAGE_STATION_CONTROL ||
            packet.header.senderRole != BraceletStationProtocol::SENDER_STATION)
        {
            return;
        }

        portENTER_CRITICAL(&espNowMux);
        pendingStationControl = packet;
        stationControlPending = true;
        portEXIT_CRITICAL(&espNowMux);
    }

    bool beginEspNow()
    {
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        WiFi.setSleep(false);
        WiFi.macAddress(deviceId);
        esp_wifi_set_channel(espNowChannel, WIFI_SECOND_CHAN_NONE);

        Serial.print("bracelet_mac:");
        Serial.println(WiFi.macAddress());

        if (esp_now_init() != ESP_OK)
        {
            Serial.println("espnow_status:init_failed");
            return false;
        }

        esp_now_register_recv_cb(onEspNowReceive);

        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, BROADCAST_PEER, ESP_NOW_ETH_ALEN);
        peer.channel = 0;
        peer.encrypt = false;
        if (esp_now_add_peer(&peer) != ESP_OK)
        {
            Serial.println("espnow_status:add_broadcast_peer_failed");
            return false;
        }

        Serial.println("espnow_status:ready");
        return true;
    }

    bool alarmTransportActive()
    {
        return stationState == StationState::Ringing ||
               stationState == StationState::ValidatingActivity;
    }

    uint32_t currentSendPeriod()
    {
        return alarmTransportActive() ? ACTIVE_SEND_PERIOD_MS : INACTIVE_SEND_PERIOD_MS;
    }

    void sendBraceletStatus(bool force = false)
    {
        if (!espNowReady)
        {
            return;
        }

        const uint32_t now = millis();
        const BraceletState nextBraceletState = currentBraceletState();
        const ProblemCode nextProblemCode = currentFaultCode();
        const bool statusChanged = nextBraceletState != lastSentBraceletState ||
                                   nextProblemCode != lastSentProblemCode ||
                                   vibrationMotorOn != lastSentVibrating;
        const bool shouldForce = force || forceStatusSend || statusChanged;
        if (!shouldForce && now - lastSendMs < currentSendPeriod())
        {
            return;
        }

        lastSendMs = now;
        forceStatusSend = false;

        BraceletStatusPacket packet = {};
        fillHeader(packet.header, BraceletStationProtocol::MESSAGE_BRACELET_STATUS);
        packet.braceletState = static_cast<uint8_t>(nextBraceletState);
        packet.batteryVoltageMv = batteryVoltage > 0.1F ? static_cast<uint16_t>(roundf(batteryVoltage * 1000.0F)) : 0;
        packet.faultCode = static_cast<uint8_t>(nextProblemCode);
        packet.vibrating = vibrationMotorOn ? 1 : 0;
        packet.energy = energyTracker.lastEnergy;
        packet.energyValidMs = energyTracker.lastValidMs;
        packet.bootSessionId = bootSessionId;
        const bool eventIsRecent = movementEventId != 0 &&
                                   now - movementEventUptimeMs <= BraceletStationProtocol::MOVEMENT_HISTORY_MS;
        const bool eventNeedsAcknowledgement = eventIsRecent &&
                                               acknowledgedMovementEventId != movementEventId;
        if (eventNeedsAcknowledgement)
        {
            packet.movementEventId = movementEventId;
            packet.movementEventUptimeMs = movementEventUptimeMs;
            packet.movementEventEnergy = movementEventEnergy;
            packet.movementEventValidMs = movementEventValidMs;
        }
        packet.vibrationAckId = vibrationAckId;

        const esp_err_t result = esp_now_send(BROADCAST_PEER,
                                              reinterpret_cast<const uint8_t *>(&packet),
                                              sizeof(packet));
        if (result != ESP_OK)
        {
            Serial.printf("espnow_send_error:bracelet_status:%d\n", static_cast<int>(result));
            return;
        }

        if (now - lastEspNowDebugMs >= TELEMETRY_LOG_PERIOD_MS)
        {
            lastEspNowDebugMs = now;
            Serial.printf("espnow_tx_bracelet:seq=%lu,battery_v=%.3f,battery_mv=%u,size=%u,channel=%u\n",
                          static_cast<unsigned long>(packet.header.sequence),
                          batteryVoltage,
                          static_cast<unsigned int>(packet.batteryVoltageMv),
                          static_cast<unsigned int>(sizeof(packet)),
                          static_cast<unsigned int>(espNowChannel));
        }

        lastSentBraceletState = nextBraceletState;
        lastSentProblemCode = nextProblemCode;
        lastSentVibrating = vibrationMotorOn;
    }

    void handleStationControl(const StationControlPacket &packet)
    {
        const uint32_t now = millis();
        const bool nextVibrationRequest = packet.vibrationRequest != 0;
        const StationState nextStationState = packet.stationState <= static_cast<uint8_t>(StationState::Fault)
                                                  ? static_cast<StationState>(packet.stationState)
                                                  : StationState::Unknown;
        const StationState previousStationState = stationState;
        stationState = nextStationState;
        if (previousStationState != stationState)
        {
            forceStatusSend = true;
            Serial.printf("espnow_cadence:%lu_ms\n", static_cast<unsigned long>(currentSendPeriod()));
        }

        if (lastReceivedControlSequence != 0 &&
            static_cast<int32_t>(packet.header.sequence - lastReceivedControlSequence) > 1)
        {
            Serial.printf("espnow_control_gap:%lu\n",
                          static_cast<unsigned long>(packet.header.sequence - lastReceivedControlSequence - 1));
        }
        lastReceivedControlSequence = packet.header.sequence;
        if (packet.acknowledgedBootSessionId == bootSessionId &&
            packet.acknowledgedMovementEventId == movementEventId)
        {
            acknowledgedMovementEventId = movementEventId;
        }
        if (nextVibrationRequest &&
            (!stationRequestsVibration || packet.vibrationRequestId != activeVibrationRequestId))
        {
            vibrationRequestStartMs = now;
            activeVibrationRequestId = packet.vibrationRequestId;
        }
        else if (!nextVibrationRequest)
        {
            vibrationRequestStartMs = 0;
            activeVibrationRequestId = 0;
        }
        stationRequestsVibration = nextVibrationRequest;
        lastStationControlMs = now;
    }

    void serviceEspNow()
    {
        StationControlPacket packet = {};
        bool hasPacket = false;
        portENTER_CRITICAL(&espNowMux);
        if (stationControlPending)
        {
            packet = pendingStationControl;
            stationControlPending = false;
            hasPacket = true;
        }
        portEXIT_CRITICAL(&espNowMux);

        if (!hasPacket)
        {
            return;
        }

        const bool firstContact = lastStationControlMs == 0;
        if (!stationPaired)
        {
            savePairedStation(packet.header.deviceId);
        }
        else if (!deviceIdMatches(packet.header.deviceId, pairedStationId))
        {
            Serial.println("espnow_packet_rejected:foreign_station");
            return;
        }

        handleStationControl(packet);
        forceStatusSend = forceStatusSend || firstContact;
    }

    void logTelemetry()
    {
        const uint32_t now = millis();
        if (now - lastLogMs < TELEMETRY_LOG_PERIOD_MS)
        {
            return;
        }

        lastLogMs = now;
        Serial.print("bracelet_state:");
        Serial.print(static_cast<uint8_t>(currentBraceletState()));
        Serial.print(",energy:");
        Serial.print(energyTracker.lastEnergy);
        Serial.print(",energy_valid_ms:");
        Serial.print(energyTracker.lastValidMs);
        Serial.print(",movement_event_id:");
        Serial.print(movementEventId);
        Serial.print(",movement_event_acknowledged:");
        Serial.print(acknowledgedMovementEventId == movementEventId ? "true" : "false");
        Serial.print(",vibration_ack_id:");
        Serial.print(vibrationAckId);
        Serial.print(",vibrating:");
        Serial.print(vibrationMotorOn ? "true" : "false");
        Serial.print(",battery_percent:");
        Serial.print(batteryPercent);
        Serial.print(",battery_v:");
        Serial.println(batteryVoltage, 3);
    }

    void updateVibrationMotor()
    {
        const uint32_t now = millis();
        if (stationRequestsVibration && vibrationRequestStartMs == 0)
        {
            vibrationRequestStartMs = now;
        }

        const uint32_t vibrationElapsedMs = stationRequestsVibration ? now - vibrationRequestStartMs : 0;
        const bool shouldPulse = stationRequestsVibration && (vibrationElapsedMs % VIBRATION_PERIOD_MS) < VIBRATION_ON_MS;

        if (shouldPulse && !vibrationMotorOn)
        {
            ignoreMotionUntilMs = now + VIBRATION_ON_MS + VIBRATION_SETTLE_MS;
            if (activeVibrationRequestId != 0 && vibrationAckId != activeVibrationRequestId)
            {
                vibrationAckId = activeVibrationRequestId;
                forceStatusSend = true;
            }
        }
        else if (!shouldPulse && vibrationMotorOn)
        {
            ignoreMotionUntilMs = max(ignoreMotionUntilMs, now + VIBRATION_SETTLE_MS);
        }

        vibrationMotorOn = shouldPulse;
        digitalWrite(Pin::VIBRATION_MOTOR, shouldPulse ? HIGH : LOW);
    }

}

void setup()
{
    Serial.begin(SERIAL_BAUD);
    delay(500);

    bootSessionId = esp_random();
    if (bootSessionId == 0)
    {
        bootSessionId = 1;
    }

    pinMode(Pin::VIBRATION_MOTOR, OUTPUT);
    digitalWrite(Pin::VIBRATION_MOTOR, LOW);
    analogReadResolution(12);
    analogSetPinAttenuation(Pin::BATTERY_ADC, ADC_11db);

    Wire.begin(Pin::IMU_SDA, Pin::IMU_SCL);
    Wire.setClock(400000);

    sensorReady = beginBmi270();
    batteryVoltage = readBatteryVoltage();
    batteryPercent = estimateBatteryPercent(batteryVoltage);

    loadPairedStation();
    espNowReady = beginEspNow();
    sendBraceletStatus(true);
}

void loop()
{
    serviceEspNow();
    updateBattery();
    updateVibrationMotor();
    updateMotion();
    updateEspNowChannel();
    sendBraceletStatus();
    logTelemetry();
    delay(1);
}
