#pragma once

#include <stdint.h>

namespace BraceletStationProtocol
{
constexpr uint32_t MAGIC = 0x54525453UL; // "STRT" on ESP32 little-endian targets.
constexpr uint8_t VERSION = 4;
constexpr uint16_t UDP_PORT = 42100;

constexpr uint8_t MESSAGE_BRACELET_STATUS = 1;
constexpr uint8_t MESSAGE_STATION_CONTROL = 2;
constexpr uint8_t SENDER_STATION = 1;
constexpr uint8_t SENDER_BRACELET = 2;

constexpr uint8_t THRESHOLD_PROFILE_NORMAL = 0;
constexpr uint32_t NORMAL_ENERGY_THRESHOLD = 1000000;
constexpr uint16_t MIN_ENERGY_VALID_MS = 80;
constexpr uint32_t MOVEMENT_HISTORY_MS = 10000;
constexpr int8_t WIFI_RSSI_UNKNOWN_DBM = 0;

struct __attribute__((packed)) PacketHeader
{
  uint32_t magic;
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
  int8_t wifiRssiDbm;
  uint8_t faultCode;
  uint8_t flags;
  uint32_t energy;
  uint16_t energyValidMs;
  uint32_t bootSessionId;
  uint32_t movementEventId;
  uint32_t movementEventUptimeMs;
  uint32_t movementEventEnergy;
  uint16_t movementEventValidMs;
  uint32_t vibrationAckId;
};

struct __attribute__((packed)) StationControlPacket
{
  PacketHeader header;
  uint8_t stationState;
  int32_t alarmRevision;
  uint8_t vibrationRequest;
  uint8_t thresholdProfile;
  uint32_t acknowledgedBootSessionId;
  uint32_t acknowledgedMovementEventId;
  uint32_t vibrationRequestId;
};

static_assert(sizeof(PacketHeader) == 21, "Unexpected UDP protocol header size");
static_assert(sizeof(BraceletStatusPacket) == 57, "Unexpected bracelet_status size");
static_assert(sizeof(StationControlPacket) == 40, "Unexpected station_control size");
}
