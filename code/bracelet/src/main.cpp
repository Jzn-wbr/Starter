#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "SparkFun_BMI270_Arduino_Library.h"

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
    constexpr uint32_t CHANNEL_HOP_PERIOD_MS = 300;
    constexpr uint32_t STATION_CONTROL_LOCK_MS = 15000;

    constexpr uint32_t SEND_PERIOD_MS = 200;
    constexpr uint32_t CONTROL_REPLY_DELAY_MS = 100;
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

    constexpr uint8_t PROTOCOL_VERSION = 1;
    constexpr uint8_t MESSAGE_BRACELET_STATUS = 1;
    constexpr uint8_t MESSAGE_STATION_CONTROL = 2;
    constexpr uint8_t SENDER_STATION = 1;
    constexpr uint8_t SENDER_BRACELET = 2;
    constexpr uint8_t BATTERY_UNKNOWN = 255;
    constexpr uint8_t ESP_NOW_MIN_CHANNEL = 1;
    constexpr uint8_t ESP_NOW_MAX_CHANNEL = 13;

    constexpr uint8_t FLAG_CHARGING = 1 << 0;
    constexpr uint8_t FLAG_SENSOR_READY = 1 << 1;
    constexpr uint8_t FLAG_VIBRATION_ACTIVE = 1 << 2;

    const uint8_t BROADCAST_PEER[ESP_NOW_ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

    enum class BraceletState : uint8_t
    {
        Charging = 1,
        Ready = 2,
        Active = 3,
        Validated = 4,
        LowBattery = 5,
        Fault = 6,
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
        WifiUnavailable = 1,
        SupabaseUnavailable = 2,
        TimeUnknown = 3,
        NoValidAlarmConfig = 4,
        MusicStreamFailed = 5,
        FallbackAudioFailed = 6,
        BraceletMissing = 7,
        BraceletLowBattery = 8,
        BraceletFault = 9,
        SensorFault = 10,
        AudioFault = 11,
        UnknownFault = 12,
    };

    struct __attribute__((packed)) PacketHeader
    {
        uint8_t protocolVersion;
        uint8_t messageType;
        uint8_t senderRole;
        uint8_t deviceId[6];
        uint32_t sequence;
        uint32_t uptimeMs;
    };

    struct __attribute__((packed)) BraceletStatusPacket
    {
        PacketHeader header;
        uint8_t braceletState;
        uint8_t activityScore;
        uint8_t validated;
        uint16_t batteryVoltageMv;
        uint8_t faultCode;
        uint8_t flags;
        uint32_t energy;
        uint16_t energyValidMs;
    };

    struct __attribute__((packed)) StationControlPacket
    {
        PacketHeader header;
        uint8_t stationState;
        uint32_t alarmRevision;
        uint8_t vibrationRequest;
        uint8_t thresholdProfile;
    };

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
    MotionReading latestMotion;
    EnergyTracker energyTracker;

    uint8_t deviceId[6] = {};
    uint32_t sequenceNumber = 0;
    uint32_t lastSampleMs = 0;
    uint32_t lastSensorRetryMs = 0;
    uint32_t lastBatterySampleMs = 0;
    uint32_t lastSendMs = 0;
    uint32_t lastLogMs = 0;
    uint32_t lastChannelHopMs = 0;
    uint32_t lastStationControlMs = 0;
    uint32_t delayedControlReplyMs = 0;
    uint32_t ignoreMotionUntilMs = 0;
    uint32_t vibrationRequestStartMs = 0;

    bool sensorReady = false;
    bool espNowReady = false;
    bool forceStatusSend = false;
    bool vibrationMotorOn = false;
    float batteryVoltage = 0.0F;
    uint8_t batteryPercent = BATTERY_UNKNOWN;
    StationState stationState = StationState::Unknown;
    bool stationRequestsVibration = false;
    uint8_t espNowChannel = ESP_NOW_MIN_CHANNEL;

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
            energyTracker.energy = 0;
            energyTracker.validMs = 0;
            energyTracker.windowStartMs += ENERGY_WINDOW_MS;
            forceStatusSend = true;
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

    uint8_t currentFlags()
    {
        uint8_t flags = 0;
        if (sensorReady)
        {
            flags |= FLAG_SENSOR_READY;
        }
        if (vibrationMotorOn)
        {
            flags |= FLAG_VIBRATION_ACTIVE;
        }
        return flags;
    }

    void fillHeader(PacketHeader &header, uint8_t messageType)
    {
        header.protocolVersion = PROTOCOL_VERSION;
        header.messageType = messageType;
        header.senderRole = SENDER_BRACELET;
        memcpy(header.deviceId, deviceId, sizeof(deviceId));
        header.sequence = sequenceNumber++;
        header.uptimeMs = millis();
    }

    void setEspNowChannel(uint8_t channel)
    {
        if (channel < ESP_NOW_MIN_CHANNEL || channel > ESP_NOW_MAX_CHANNEL || channel == espNowChannel)
        {
            return;
        }

        espNowChannel = channel;
        esp_wifi_set_channel(espNowChannel, WIFI_SECOND_CHAN_NONE);
    }

    void updateEspNowChannel()
    {
        if (!espNowReady)
        {
            return;
        }

        const uint32_t now = millis();
        if (lastStationControlMs > 0 && now - lastStationControlMs < STATION_CONTROL_LOCK_MS)
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

    uint32_t currentSendPeriod()
    {
        return SEND_PERIOD_MS;
    }

    void sendBraceletStatus(bool force = false)
    {
        if (!espNowReady)
        {
            return;
        }

        const uint32_t now = millis();
        const bool shouldForce = force || forceStatusSend;
        if (!shouldForce && now - lastSendMs < currentSendPeriod())
        {
            return;
        }

        lastSendMs = now;
        forceStatusSend = false;

        BraceletStatusPacket packet = {};
        fillHeader(packet.header, MESSAGE_BRACELET_STATUS);
        packet.braceletState = static_cast<uint8_t>(currentBraceletState());
        packet.activityScore = static_cast<uint8_t>(constrain(energyTracker.lastEnergy / 40, 0UL, 100UL));
        packet.validated = 0;
        packet.batteryVoltageMv = batteryVoltage > 0.1F ? static_cast<uint16_t>(roundf(batteryVoltage * 1000.0F)) : 0;
        packet.faultCode = static_cast<uint8_t>(currentFaultCode());
        packet.flags = currentFlags();
        packet.energy = energyTracker.lastEnergy;
        packet.energyValidMs = energyTracker.lastValidMs;

        const esp_err_t result = esp_now_send(BROADCAST_PEER, reinterpret_cast<const uint8_t *>(&packet), sizeof(packet));
        if (result != ESP_OK)
        {
            Serial.print("espnow_send_error:");
            Serial.println(result);
        }
    }

    void handleStationControl(const uint8_t *data, int length)
    {
        if (length != static_cast<int>(sizeof(StationControlPacket)))
        {
            return;
        }

        StationControlPacket packet = {};
        memcpy(&packet, data, sizeof(packet));
        if (packet.header.protocolVersion != PROTOCOL_VERSION ||
            packet.header.messageType != MESSAGE_STATION_CONTROL ||
            packet.header.senderRole != SENDER_STATION)
        {
            return;
        }

        const uint32_t now = millis();
        const bool nextVibrationRequest = packet.vibrationRequest != 0;
        stationState = static_cast<StationState>(packet.stationState);
        if (nextVibrationRequest && !stationRequestsVibration)
        {
            vibrationRequestStartMs = now;
        }
        else if (!nextVibrationRequest)
        {
            vibrationRequestStartMs = 0;
        }
        stationRequestsVibration = nextVibrationRequest;
        lastStationControlMs = now;
        delayedControlReplyMs = now + CONTROL_REPLY_DELAY_MS;
    }

    void onEspNowReceive(const uint8_t *, const uint8_t *data, int length)
    {
        if (length < static_cast<int>(sizeof(PacketHeader)))
        {
            return;
        }

        const PacketHeader *header = reinterpret_cast<const PacketHeader *>(data);
        if (header->protocolVersion == PROTOCOL_VERSION && header->messageType == MESSAGE_STATION_CONTROL)
        {
            handleStationControl(data, length);
        }
    }

    void onEspNowSent(const uint8_t *, esp_now_send_status_t status)
    {
        if (status != ESP_NOW_SEND_SUCCESS)
        {
            Serial.println("espnow_send_status:failed");
        }
    }

    bool beginEspNow()
    {
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
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
        esp_now_register_send_cb(onEspNowSent);

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
        }
        else if (!shouldPulse && vibrationMotorOn)
        {
            ignoreMotionUntilMs = max(ignoreMotionUntilMs, now + VIBRATION_SETTLE_MS);
        }

        vibrationMotorOn = shouldPulse;
        digitalWrite(Pin::VIBRATION_MOTOR, shouldPulse ? HIGH : LOW);
    }

    void sendDelayedControlReply()
    {
        if (delayedControlReplyMs == 0)
        {
            return;
        }

        const uint32_t now = millis();
        if (now - delayedControlReplyMs < UINT32_MAX / 2)
        {
            delayedControlReplyMs = 0;
            sendBraceletStatus(true);
        }
    }
}

void setup()
{
    Serial.begin(SERIAL_BAUD);
    delay(500);

    pinMode(Pin::VIBRATION_MOTOR, OUTPUT);
    digitalWrite(Pin::VIBRATION_MOTOR, LOW);
    analogReadResolution(12);
    analogSetPinAttenuation(Pin::BATTERY_ADC, ADC_11db);

    Wire.begin(Pin::IMU_SDA, Pin::IMU_SCL);
    Wire.setClock(400000);

    sensorReady = beginBmi270();
    batteryVoltage = readBatteryVoltage();
    batteryPercent = estimateBatteryPercent(batteryVoltage);
    espNowReady = beginEspNow();
    sendBraceletStatus(true);
}

void loop()
{
    updateBattery();
    updateVibrationMotor();
    updateMotion();
    updateEspNowChannel();
    sendDelayedControlReply();
    sendBraceletStatus();
    logTelemetry();
}
