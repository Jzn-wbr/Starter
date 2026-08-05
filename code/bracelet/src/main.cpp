#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <esp_system.h>
#include "SparkFun_BMI270_Arduino_Library.h"
#include "../../bracelet_station_protocol.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

#ifndef WIFI_FALLBACK_SSID
#define WIFI_FALLBACK_SSID ""
#endif

#ifndef WIFI_FALLBACK_PASSWORD
#define WIFI_FALLBACK_PASSWORD ""
#endif

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
    constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 10000;
    constexpr uint32_t WIFI_RETRY_MS = 15000;
    constexpr uint8_t MAX_UDP_PACKETS_PER_LOOP = 6;
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
    constexpr uint8_t FLAG_CHARGING = 1 << 0;
    constexpr uint8_t FLAG_SENSOR_READY = 1 << 1;
    constexpr uint8_t FLAG_VIBRATION_ACTIVE = 1 << 2;

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
    WiFiUDP udp;
    MotionReading latestMotion;
    EnergyTracker energyTracker;

    uint8_t deviceId[6] = {};
    uint32_t sequenceNumber = 0;
    uint32_t lastSampleMs = 0;
    uint32_t lastSensorRetryMs = 0;
    uint32_t lastBatterySampleMs = 0;
    uint32_t lastSendMs = 0;
    uint32_t lastLogMs = 0;
    uint32_t lastUdpBatteryDebugMs = 0;
    uint32_t lastStationControlMs = 0;
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
    uint32_t wifiConnectStartedMs = 0;
    uint32_t lastWifiRetryMs = 0;
    uint32_t lastReceivedControlSequence = 0;

    bool sensorReady = false;
    bool udpReady = false;
    bool wifiConnectionInProgress = false;
    bool stationPaired = false;
    bool stationIpKnown = false;
    bool forceStatusSend = false;
    bool vibrationMotorOn = false;
    float batteryVoltage = 0.0F;
    uint8_t batteryPercent = BATTERY_UNKNOWN;
    StationState stationState = StationState::Unknown;
    BraceletState lastSentBraceletState = BraceletState::Fault;
    ProblemCode lastSentProblemCode = ProblemCode::UnknownFault;
    uint8_t lastSentFlags = 0xff;
    bool stationRequestsVibration = false;
    uint8_t wifiNetworkIndex = 0;
    uint8_t pairedStationId[6] = {};
    IPAddress stationIp;

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
        preferences.begin("udp-link", true);
        if (preferences.getBytesLength("stationId") == sizeof(pairedStationId))
        {
            preferences.getBytes("stationId", pairedStationId, sizeof(pairedStationId));
            stationPaired = true;
        }
        preferences.end();

        Serial.println(stationPaired ? "udp_pairing:station_loaded" : "udp_pairing:waiting_for_station");
    }

    void savePairedStation(const uint8_t *stationId)
    {
        memcpy(pairedStationId, stationId, sizeof(pairedStationId));
        preferences.begin("udp-link", false);
        preferences.putBytes("stationId", pairedStationId, sizeof(pairedStationId));
        preferences.end();
        stationPaired = true;
        Serial.printf("udp_pairing:station_saved:%02X:%02X:%02X:%02X:%02X:%02X\n",
                      pairedStationId[0], pairedStationId[1], pairedStationId[2],
                      pairedStationId[3], pairedStationId[4], pairedStationId[5]);
    }

    void stopUdp()
    {
        if (udpReady)
        {
            udp.stop();
            udpReady = false;
        }
        stationIpKnown = false;
    }

    void beginWifiAttempt()
    {
        const char *ssids[] = {WIFI_SSID, WIFI_FALLBACK_SSID};
        const char *passwords[] = {WIFI_PASSWORD, WIFI_FALLBACK_PASSWORD};
        constexpr uint8_t networkCount = sizeof(ssids) / sizeof(ssids[0]);

        for (uint8_t checked = 0; checked < networkCount; ++checked)
        {
            if (strlen(ssids[wifiNetworkIndex]) > 0)
            {
                Serial.printf("wifi_connecting:%s\n", ssids[wifiNetworkIndex]);
                WiFi.setSleep(false);
                WiFi.disconnect();
                const int networkCountFound = WiFi.scanNetworks(false, true);
                bool configuredNetworkFound = false;
                for (int index = 0; index < networkCountFound; ++index)
                {
                    if (WiFi.SSID(index) == ssids[wifiNetworkIndex])
                    {
                        configuredNetworkFound = true;
                        Serial.printf("wifi_scan:target=%s,found=true,channel=%d,rssi=%d,auth=%d\n",
                                      ssids[wifiNetworkIndex],
                                      WiFi.channel(index),
                                      WiFi.RSSI(index),
                                      static_cast<int>(WiFi.encryptionType(index)));
                    }
                }
                if (!configuredNetworkFound)
                {
                    Serial.printf("wifi_scan:target=%s,found=false,visible_networks=%d\n",
                                  ssids[wifiNetworkIndex], networkCountFound);
                }
                WiFi.scanDelete();
                WiFi.begin(ssids[wifiNetworkIndex], passwords[wifiNetworkIndex]);
                wifiConnectStartedMs = millis();
                wifiConnectionInProgress = true;
                return;
            }
            wifiNetworkIndex = (wifiNetworkIndex + 1) % networkCount;
        }

        Serial.println("wifi_status:no_network_configured");
        lastWifiRetryMs = millis();
    }

    bool alarmTransportActive();

    const char *wifiDisconnectReasonName(uint8_t reason)
    {
        switch (reason)
        {
        case 2:
            return "auth_expired";
        case 15:
            return "four_way_handshake_timeout";
        case 23:
            return "8021x_auth_failed";
        case 200:
            return "beacon_timeout";
        case 201:
            return "no_ap_found";
        case 202:
            return "auth_failed";
        case 203:
            return "association_failed";
        case 204:
            return "handshake_timeout";
        case 205:
            return "connection_failed";
        default:
            return "other";
        }
    }

    void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info)
    {
        if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
        {
            const uint8_t reason = info.wifi_sta_disconnected.reason;
            Serial.printf("wifi_disconnected:reason=%u,name=%s,status=%d\n",
                          static_cast<unsigned int>(reason),
                          wifiDisconnectReasonName(reason),
                          static_cast<int>(WiFi.status()));
        }
        else if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED)
        {
            Serial.println("wifi_status:associated");
        }
        else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
        {
            Serial.printf("wifi_status:connected,ip=%s,gateway=%s,rssi=%d\n",
                          WiFi.localIP().toString().c_str(),
                          WiFi.gatewayIP().toString().c_str(),
                          WiFi.RSSI());
        }
    }

    void ensureWifi()
    {
        const uint32_t now = millis();
        if (WiFi.status() == WL_CONNECTED)
        {
            wifiConnectionInProgress = false;
            if (!udpReady)
            {
                udpReady = udp.begin(BraceletStationProtocol::UDP_PORT) == 1;
                Serial.printf("udp_status:%s,ip:%s\n", udpReady ? "ready" : "bind_failed",
                              WiFi.localIP().toString().c_str());
                WiFi.setSleep(!alarmTransportActive());
                forceStatusSend = true;
            }
            return;
        }

        stopUdp();
        if (wifiConnectionInProgress && now - wifiConnectStartedMs < WIFI_CONNECT_TIMEOUT_MS)
        {
            return;
        }

        constexpr uint8_t networkCount = 2;
        if (wifiConnectionInProgress)
        {
            Serial.println("wifi_status:connect_timeout");
            wifiConnectionInProgress = false;
            wifiNetworkIndex = (wifiNetworkIndex + 1) % networkCount;
            if (wifiNetworkIndex == 0)
            {
                lastWifiRetryMs = now;
                return;
            }
        }

        if (lastWifiRetryMs != 0 && now - lastWifiRetryMs < WIFI_RETRY_MS)
        {
            return;
        }
        lastWifiRetryMs = 0;
        beginWifiAttempt();
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
        if (!udpReady || !stationIpKnown)
        {
            return;
        }

        const uint32_t now = millis();
        const BraceletState nextBraceletState = currentBraceletState();
        const ProblemCode nextProblemCode = currentFaultCode();
        const uint8_t nextFlags = currentFlags();
        const bool statusChanged = nextBraceletState != lastSentBraceletState ||
                                   nextProblemCode != lastSentProblemCode ||
                                   nextFlags != lastSentFlags;
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
        packet.activityScore = static_cast<uint8_t>(constrain(energyTracker.lastEnergy / 40, 0UL, 100UL));
        packet.validated = 0;
        packet.batteryVoltageMv = batteryVoltage > 0.1F ? static_cast<uint16_t>(roundf(batteryVoltage * 1000.0F)) : 0;
        packet.wifiRssiDbm = WiFi.status() == WL_CONNECTED
                                 ? static_cast<int8_t>(constrain(WiFi.RSSI(), -127, -1))
                                 : BraceletStationProtocol::WIFI_RSSI_UNKNOWN_DBM;
        packet.faultCode = static_cast<uint8_t>(nextProblemCode);
        packet.flags = nextFlags;
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

        if (!udp.beginPacket(stationIp, BraceletStationProtocol::UDP_PORT) ||
            udp.write(reinterpret_cast<const uint8_t *>(&packet), sizeof(packet)) != sizeof(packet) ||
            !udp.endPacket())
        {
            Serial.println("udp_send_error:bracelet_status");
            return;
        }

        if (now - lastUdpBatteryDebugMs >= TELEMETRY_LOG_PERIOD_MS)
        {
            lastUdpBatteryDebugMs = now;
            Serial.printf("udp_tx_bracelet:seq=%lu,battery_v=%.3f,battery_mv=%u,size=%u,dst=%s\n",
                          static_cast<unsigned long>(packet.header.sequence),
                          batteryVoltage,
                          static_cast<unsigned int>(packet.batteryVoltageMv),
                          static_cast<unsigned int>(sizeof(packet)),
                          stationIp.toString().c_str());
        }

        lastSentBraceletState = nextBraceletState;
        lastSentProblemCode = nextProblemCode;
        lastSentFlags = nextFlags;
    }

    void handleStationControl(const StationControlPacket &packet, const IPAddress &remoteIp)
    {
        const uint32_t now = millis();
        const bool nextVibrationRequest = packet.vibrationRequest != 0;
        const StationState nextStationState = packet.stationState <= static_cast<uint8_t>(StationState::Fault)
                                                  ? static_cast<StationState>(packet.stationState)
                                                  : StationState::Unknown;
        const StationState previousStationState = stationState;
        const bool wasActive = alarmTransportActive();
        stationState = nextStationState;
        stationIp = remoteIp;
        stationIpKnown = true;
        if (wasActive != alarmTransportActive())
        {
            WiFi.setSleep(!alarmTransportActive());
            forceStatusSend = true;
            Serial.printf("udp_cadence:%lu_ms\n", static_cast<unsigned long>(currentSendPeriod()));
        }
        else if (previousStationState != stationState)
        {
            forceStatusSend = true;
        }

        if (lastReceivedControlSequence != 0 &&
            static_cast<int32_t>(packet.header.sequence - lastReceivedControlSequence) > 1)
        {
            Serial.printf("udp_control_gap:%lu\n",
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

    void serviceUdp()
    {
        if (!udpReady)
        {
            return;
        }

        for (uint8_t handled = 0; handled < MAX_UDP_PACKETS_PER_LOOP; ++handled)
        {
            const int packetSize = udp.parsePacket();
            if (packetSize <= 0)
            {
                break;
            }

            const IPAddress remoteIp = udp.remoteIP();
            StationControlPacket packet = {};
            const int bytesRead = udp.read(reinterpret_cast<uint8_t *>(&packet), sizeof(packet));
            if (packetSize != static_cast<int>(sizeof(packet)) || bytesRead != static_cast<int>(sizeof(packet)) ||
                packet.header.magic != BraceletStationProtocol::MAGIC ||
                packet.header.protocolVersion != BraceletStationProtocol::VERSION ||
                packet.header.messageType != BraceletStationProtocol::MESSAGE_STATION_CONTROL ||
                packet.header.senderRole != BraceletStationProtocol::SENDER_STATION)
            {
                Serial.println("udp_packet_rejected:invalid_station_control");
                continue;
            }

            const bool firstContact = !stationPaired || !stationIpKnown;
            if (!stationPaired)
            {
                savePairedStation(packet.header.deviceId);
            }
            else if (!deviceIdMatches(packet.header.deviceId, pairedStationId))
            {
                Serial.println("udp_packet_rejected:foreign_station");
                continue;
            }

            handleStationControl(packet, remoteIp);
            forceStatusSend = forceStatusSend || firstContact;
        }
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

    WiFi.mode(WIFI_STA);
    WiFi.onEvent(onWifiEvent);
    WiFi.macAddress(deviceId);
    WiFi.setSleep(false);
    Serial.print("bracelet_mac:");
    Serial.println(WiFi.macAddress());
    loadPairedStation();
    beginWifiAttempt();
}

void loop()
{
    ensureWifi();
    serviceUdp();
    updateBattery();
    updateVibrationMotor();
    updateMotion();
    sendBraceletStatus();
    logTelemetry();
    delay(1);
}
