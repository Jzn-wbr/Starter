#pragma once

#include <stdint.h>

namespace BraceletStationProtocol
{
constexpr uint32_t MAGIC = 0x54525453UL; // "STRT" on ESP32 little-endian targets.
constexpr uint8_t VERSION = 6;

constexpr uint8_t MESSAGE_BRACELET_STATUS = 1;
constexpr uint8_t MESSAGE_STATION_CONTROL = 2;
constexpr uint8_t SENDER_STATION = 1;
constexpr uint8_t SENDER_BRACELET = 2;

constexpr uint32_t NORMAL_ENERGY_THRESHOLD = 1000000;
constexpr uint16_t MIN_ENERGY_VALID_MS = 80;
constexpr uint32_t MOVEMENT_HISTORY_MS = 10000;

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
  uint16_t batteryVoltageMv;
  uint8_t faultCode;
  uint8_t vibrating;
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
  uint8_t vibrationRequest;
  uint32_t acknowledgedBootSessionId;
  uint32_t acknowledgedMovementEventId;
  uint32_t vibrationRequestId;
};

static_assert(sizeof(PacketHeader) == 21, "Unexpected ESP-NOW protocol header size");
static_assert(sizeof(BraceletStatusPacket) == 54, "Unexpected bracelet_status size");
static_assert(sizeof(StationControlPacket) == 35, "Unexpected station_control size");
}
