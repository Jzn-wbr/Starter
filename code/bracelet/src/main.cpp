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

    constexpr uint32_t SEND_PERIOD_READY_MS = 5000;
    constexpr uint32_t SEND_PERIOD_ARMED_MS = 2000;
    constexpr uint32_t SEND_PERIOD_ACTIVE_MS = 200;
    constexpr uint32_t SEND_PERIOD_FAULT_MS = 2000;

    constexpr uint32_t MOVEMENT_CONFIRM_MS = 700;
    constexpr uint32_t PAUSE_TOLERANCE_MS = 5000;
    constexpr float ACTIVE_ACCEL_DELTA_G = 0.18F;
    constexpr float ACTIVE_GYRO_DPS = 80.0F;
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
    constexpr uint8_t FLAG_MOTION_PRESENT = 1 << 2;

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
    };

    struct __attribute__((packed)) StationControlPacket
    {
        PacketHeader header;
        uint8_t stationState;
        uint32_t alarmRevision;
        uint8_t activityRequired;
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

    struct ActivityTracker
    {
        uint32_t activeMs = 0;
        uint32_t candidateActiveMs = 0;
        uint32_t pauseMs = 0;
        uint32_t lastUpdateMs = 0;
        float scoreEma = 0.0F;
        bool motionPresent = false;
    };

    BMI270 imu;
    MotionReading latestMotion;
    ActivityTracker activity;

    uint8_t deviceId[6] = {};
    uint32_t sequenceNumber = 0;
    uint32_t lastSampleMs = 0;
    uint32_t lastSensorRetryMs = 0;
    uint32_t lastBatterySampleMs = 0;
    uint32_t lastSendMs = 0;
    uint32_t lastLogMs = 0;
    uint32_t lastChannelHopMs = 0;
    uint32_t lastStationControlMs = 0;

    bool sensorReady = false;
    bool espNowReady = false;
    bool forceStatusSend = false;
    float batteryVoltage = 0.0F;
    uint8_t batteryPercent = BATTERY_UNKNOWN;
    StationState stationState = StationState::Unknown;
    bool stationRequiresActivity = false;
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

    float magnitude3(float x, float y, float z)
    {
        return sqrtf((x * x) + (y * y) + (z * z));
    }

    uint8_t calculateInstantScore(const MotionReading &reading)
    {
        const float accelDelta = fabsf(magnitude3(reading.accelX, reading.accelY, reading.accelZ) - 1.0F);
        const float gyroMagnitude = magnitude3(reading.gyroX, reading.gyroY, reading.gyroZ);
        const float accelScore = constrain((accelDelta / 0.65F) * 100.0F, 0.0F, 100.0F);
        const float gyroScore = constrain((gyroMagnitude / 240.0F) * 100.0F, 0.0F, 100.0F);
        return static_cast<uint8_t>(roundf(max(accelScore, gyroScore)));
    }

    void updateActivity(const MotionReading &reading)
    {
        const uint32_t now = millis();
        if (activity.lastUpdateMs == 0)
        {
            activity.lastUpdateMs = now;
            return;
        }

        const uint32_t dt = now - activity.lastUpdateMs;
        activity.lastUpdateMs = now;

        const float accelDelta = fabsf(magnitude3(reading.accelX, reading.accelY, reading.accelZ) - 1.0F);
        const float gyroMagnitude = magnitude3(reading.gyroX, reading.gyroY, reading.gyroZ);
        const bool moving = accelDelta >= ACTIVE_ACCEL_DELTA_G || gyroMagnitude >= ACTIVE_GYRO_DPS;
        const uint8_t instantScore = calculateInstantScore(reading);

        activity.scoreEma = (activity.scoreEma * 0.85F) + (static_cast<float>(instantScore) * 0.15F);

        if (moving)
        {
            activity.candidateActiveMs += dt;
            activity.pauseMs = 0;
            if (activity.candidateActiveMs >= MOVEMENT_CONFIRM_MS)
            {
                activity.activeMs += dt;
                activity.motionPresent = true;
            }
        }
        else if (activity.motionPresent && activity.pauseMs + dt <= PAUSE_TOLERANCE_MS)
        {
            activity.pauseMs += dt;
            activity.motionPresent = true;
        }
        else
        {
            activity.activeMs = 0;
            activity.candidateActiveMs = 0;
            activity.pauseMs = 0;
            activity.motionPresent = false;
        }
    }

    void updateMotion()
    {
        const uint32_t now = millis();
        if (now - lastSampleMs < SAMPLE_PERIOD_MS)
        {
            return;
        }

        lastSampleMs = now;

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

        latestMotion.accelX = imu.data.accelX;
        latestMotion.accelY = imu.data.accelY;
        latestMotion.accelZ = imu.data.accelZ;
        latestMotion.gyroX = imu.data.gyroX;
        latestMotion.gyroY = imu.data.gyroY;
        latestMotion.gyroZ = imu.data.gyroZ;
        updateActivity(latestMotion);
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
        if (activity.motionPresent)
        {
            return BraceletState::Active;
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
        if (activity.motionPresent)
        {
            flags |= FLAG_MOTION_PRESENT;
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
        const BraceletState state = currentBraceletState();
        if (state == BraceletState::Fault || state == BraceletState::LowBattery)
        {
            return SEND_PERIOD_FAULT_MS;
        }
        if (stationState == StationState::Ringing || stationState == StationState::ValidatingActivity || stationRequiresActivity)
        {
            return SEND_PERIOD_ACTIVE_MS;
        }
        if (stationState == StationState::Armed)
        {
            return SEND_PERIOD_ARMED_MS;
        }
        return SEND_PERIOD_READY_MS;
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
        packet.activityScore = static_cast<uint8_t>(roundf(constrain(activity.scoreEma, 0.0F, 100.0F)));
        packet.validated = activity.motionPresent ? 1 : 0;
        packet.batteryVoltageMv = batteryVoltage > 0.1F ? static_cast<uint16_t>(roundf(batteryVoltage * 1000.0F)) : 0;
        packet.faultCode = static_cast<uint8_t>(currentFaultCode());
        packet.flags = currentFlags();

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

        stationState = static_cast<StationState>(packet.stationState);
        stationRequiresActivity = packet.activityRequired != 0;
        lastStationControlMs = millis();
        sendBraceletStatus(true);
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
        Serial.print(",activity_score:");
        Serial.print(static_cast<uint8_t>(roundf(constrain(activity.scoreEma, 0.0F, 100.0F))));
        Serial.print(",active_ms:");
        Serial.print(activity.activeMs);
        Serial.print(",motion_present:");
        Serial.print(activity.motionPresent ? "true" : "false");
        Serial.print(",battery_percent:");
        Serial.print(batteryPercent);
        Serial.print(",battery_v:");
        Serial.println(batteryVoltage, 3);
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
    updateMotion();
    updateEspNowChannel();
    sendBraceletStatus();
    logTelemetry();
}
