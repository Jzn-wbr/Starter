#include <Arduino.h>
#include <ArduinoJson.h>
#include <Audio.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <time.h>

#include <driver/i2s.h>
#include "../../bracelet_station_protocol.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

// Existing local secrets.h files predate the fallback network fields. Keep
// them compatible until the optional values are added there.
#ifndef WIFI_FALLBACK_SSID
#define WIFI_FALLBACK_SSID ""
#endif

#ifndef WIFI_FALLBACK_PASSWORD
#define WIFI_FALLBACK_PASSWORD ""
#endif

static const int PIN_LRCK = 32;
static const int PIN_DIN = 33;
static const int PIN_BCK = 25;
static const int PIN_XSMT = 26;
static const int PIN_BATTERY_ADC = 34;

static const uint32_t CONFIG_REFRESH_MS = 10000;
static const uint32_t STATUS_PUBLISH_MS = 10000;
static const uint32_t ACTIVE_CONTROL_SEND_MS = 700;
static const uint32_t INACTIVE_CONTROL_SEND_MS = 5000;
static const uint32_t WIFI_RETRY_MS = 15000;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 10000;
static const uint32_t SUPABASE_FAILURE_BACKOFF_MS = 30000;
// The ESP32 without PSRAM must keep enough contiguous internal heap for TLS.
// This remains comfortably above the audio library's MP3/AAC minimum buffer.
static const int AUDIO_BUFFER_RAM_BYTES = 24 * 1024;
static const int AUDIO_BUFFER_PSRAM_BYTES = 192 * 1024;
static const uint32_t ARMED_BRACELET_TIMEOUT_MS = 25000;
static const uint32_t RINGING_BRACELET_TIMEOUT_MS = 3000;
static const uint32_t ENERGY_PRE_UNMUTE_WARNING_MS = 3000;
static const uint32_t ENERGY_UNMUTE_GRACE_MS = 500;
static const uint32_t REMOTE_AUDIO_PRIME_MS = 700;
static const uint32_t REMOTE_AUDIO_LOOP_BUDGET_MS = 20;
static const uint32_t REMOTE_AUDIO_AFTER_CONTROL_MS = 4;
static const time_t INITIAL_VIBRATION_DELAY_SECONDS = 15;
static const time_t ALARM_WINDOW_SECONDS = 10 * 60;
static const float BATTERY_DIVIDER_RATIO = 2.0F;
static const float BATTERY_EMPTY_V = 3.30F;
static const float BATTERY_FULL_V = 4.20F;
static const uint8_t BRACELET_BLOCKING_BATTERY_PERCENT = 20;
static const uint8_t BRACELET_LOW_BATTERY_PERCENT = 30;
static const i2s_port_t FALLBACK_I2S_PORT = I2S_NUM_0;
static const uint32_t FALLBACK_SAMPLE_RATE = 22050;
static const uint16_t FALLBACK_FRAMES = 256;

enum class StationState : uint8_t
{
  Idle = 0,
  Armed = 1,
  Ringing = 2,
  ValidatingActivity = 3,
  Stopped = 4,
  Fault = 5,
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

enum class BraceletState : uint8_t
{
  Unknown = 0,
  Charging = 1,
  Ready = 2,
  Active = 3,
  Validated = 4,
  LowBattery = 5,
  Fault = 6,
};

struct AlarmConfig
{
  bool valid = false;
  bool enabled = false;
  bool useFallback = true;
  int revision = 0;
  int volumePercent = 80;
  time_t alarmAt = 0;
  String timezone = "Europe/Zurich";
  String selectedTrackUrl;
};

struct BraceletSnapshot
{
  BraceletState state = BraceletState::Unknown;
  ProblemCode problem = ProblemCode::None;
  uint8_t activityScore = 0;
  bool validated = false;
  bool sensorReady = false;
  bool vibrating = false;
  bool charging = false;
  uint32_t energy = 0;
  uint16_t energyValidMs = 0;
  float batteryVoltage = -1.0F;
  int batteryPercent = -1;
  int wifiRssiDbm = BraceletStationProtocol::WIFI_RSSI_UNKNOWN_DBM;
  uint32_t lastSeenMs = 0;
  uint32_t sequence = 0;
  uint32_t bootSessionId = 0;
  uint32_t movementEventId = 0;
  uint32_t vibrationAckId = 0;
};

using BraceletStationProtocol::BraceletStatusPacket;
using BraceletStationProtocol::StationControlPacket;

static Audio audio;
static Preferences preferences;
static AlarmConfig alarmConfig;
static BraceletSnapshot bracelet;
static StationState stationState = StationState::Idle;
static ProblemCode problemCode = ProblemCode::None;
static String problemMessage = "";
static bool remoteAudioActive = false;
static bool fallbackAudioActive = false;
static bool alarmAudioMuted = false;
static bool espNowReady = false;
static bool braceletPaired = false;
static bool timeReady = false;
static float stationBatteryVoltage = -1.0F;
static uint32_t lastConfigRefreshMs = 0;
static uint32_t lastStatusPublishMs = 0;
static uint32_t lastControlSendMs = 0;
static uint32_t lastBraceletEspNowDebugMs = 0;
static uint32_t lastWifiCycleFailureMs = 0;
static uint32_t wifiConnectStartedMs = 0;
static uint8_t wifiNetworkIndex = 0;
static bool wifiConnectionInProgress = false;
static uint32_t lastSupabaseFailureMs = 0;
static uint32_t energyMuteUntilMs = 0;
static uint32_t unmuteWarningUntilMs = 0;
static uint32_t vibrationAckDeadlineMs = 0;
static uint32_t vibrationRequestId = 0;
static bool vibrationRequestActive = false;
static uint32_t acknowledgedBootSessionId = 0;
static uint32_t acknowledgedMovementEventId = 0;
static uint32_t processedMovementBootSessionId = 0;
static uint32_t processedMovementEventId = 0;
static uint32_t alarmAudioStartedMs = 0;
static bool stationControlDirty = false;
static uint32_t controlSequence = 0;
static uint8_t stationMac[6] = {0};
static uint8_t pairedBraceletId[6] = {0};
static BraceletStatusPacket pendingBraceletStatus = {};
static bool braceletStatusPending = false;
static portMUX_TYPE espNowMux = portMUX_INITIALIZER_UNLOCKED;
static int completedAlarmRevision = 0;
static bool pendingStationStatusPublish = false;
static bool pendingBraceletStatusPublish = false;

static const uint8_t BROADCAST_PEER[ESP_NOW_ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static const char *stateName(StationState state)
{
  switch (state)
  {
  case StationState::Idle: return "idle";
  case StationState::Armed: return "armed";
  case StationState::Ringing: return "ringing";
  case StationState::ValidatingActivity: return "validating_activity";
  case StationState::Stopped: return "stopped";
  case StationState::Fault: return "fault";
  }
  return "fault";
}

static const char *braceletStateName(BraceletState state)
{
  switch (state)
  {
  case BraceletState::Unknown: return "unknown";
  case BraceletState::Charging: return "charging";
  case BraceletState::Ready: return "ready";
  case BraceletState::Active: return "active";
  case BraceletState::Validated: return "validated";
  case BraceletState::LowBattery: return "low_battery";
  case BraceletState::Fault: return "fault";
  }
  return "unknown";
}

static const char *problemName(ProblemCode code)
{
  switch (code)
  {
  case ProblemCode::None: return "none";
  case ProblemCode::WifiUnavailable: return "wifi_unavailable";
  case ProblemCode::SupabaseUnavailable: return "supabase_unavailable";
  case ProblemCode::TimeUnknown: return "time_unknown";
  case ProblemCode::NoValidAlarmConfig: return "no_valid_alarm_config";
  case ProblemCode::MusicStreamFailed: return "music_stream_failed";
  case ProblemCode::FallbackAudioFailed: return "fallback_audio_failed";
  case ProblemCode::BraceletMissing: return "bracelet_missing";
  case ProblemCode::BraceletLowBattery: return "bracelet_low_battery";
  case ProblemCode::BraceletFault: return "bracelet_fault";
  case ProblemCode::SensorFault: return "sensor_fault";
  case ProblemCode::AudioFault: return "audio_fault";
  case ProblemCode::UnknownFault: return "unknown_fault";
  }
  return "unknown_fault";
}

static bool alarmIsActive()
{
  return stationState == StationState::Ringing || stationState == StationState::ValidatingActivity;
}

static bool alarmRevisionWasCompleted(const AlarmConfig &config)
{
  return config.revision >= 1 && config.revision == completedAlarmRevision;
}

static bool alarmIsStale(const AlarmConfig &config)
{
  if (!timeReady || !config.enabled || !config.alarmAt)
  {
    return false;
  }

  return time(nullptr) > config.alarmAt + ALARM_WINDOW_SECONDS;
}

static bool alarmShouldBeArmed(const AlarmConfig &config)
{
  return config.valid && config.enabled && !alarmRevisionWasCompleted(config) && !alarmIsStale(config);
}

static bool alarmCanStartNow(const AlarmConfig &config)
{
  if (!alarmShouldBeArmed(config) || !timeReady)
  {
    return false;
  }

  const time_t now = time(nullptr);
  return now >= config.alarmAt && now <= config.alarmAt + ALARM_WINDOW_SECONDS;
}

static bool alarmWindowIsOpen(const AlarmConfig &config)
{
  return alarmCanStartNow(config);
}

static bool alarmWindowHasEnded(const AlarmConfig &config)
{
  return config.valid && config.enabled && timeReady && time(nullptr) > config.alarmAt + ALARM_WINDOW_SECONDS;
}

static bool initialVibrationDelayIsActive()
{
  if (!timeReady || !alarmConfig.valid || !alarmConfig.enabled || !alarmConfig.alarmAt)
  {
    return false;
  }

  const time_t now = time(nullptr);
  return now >= alarmConfig.alarmAt &&
         now < alarmConfig.alarmAt + INITIAL_VIBRATION_DELAY_SECONDS;
}

static bool deadlineIsPending(uint32_t now, uint32_t deadline)
{
  return deadline != 0 && static_cast<int32_t>(deadline - now) > 0;
}

static bool eventIdIsNewer(uint32_t candidate, uint32_t previous)
{
  return candidate != 0 && (previous == 0 || static_cast<int32_t>(candidate - previous) > 0);
}

static void setVibrationRequest(bool active)
{
  if (active && initialVibrationDelayIsActive())
  {
    active = false;
  }

  if (active == vibrationRequestActive)
  {
    return;
  }

  vibrationRequestActive = active;
  if (active)
  {
    vibrationRequestId++;
    if (vibrationRequestId == 0)
    {
      vibrationRequestId = 1;
    }
  }
  stationControlDirty = true;
}

static bool energyKeepsAudioMuted()
{
  if (energyMuteUntilMs == 0)
  {
    return false;
  }

  const uint32_t now = millis();
  if (deadlineIsPending(now, energyMuteUntilMs))
  {
    unmuteWarningUntilMs = 0;
    vibrationAckDeadlineMs = 0;
    setVibrationRequest(false);
    return true;
  }

  if (vibrationAckDeadlineMs == 0 && unmuteWarningUntilMs == 0)
  {
    if (initialVibrationDelayIsActive())
    {
      setVibrationRequest(false);
      unmuteWarningUntilMs = now + ENERGY_PRE_UNMUTE_WARNING_MS;
      Serial.println("Initial vibration delay active; continuing the pre-unmute wait without vibration.");
      return true;
    }

    setVibrationRequest(true);
    vibrationAckDeadlineMs = now + ENERGY_PRE_UNMUTE_WARNING_MS;
    Serial.printf("Waiting up to %lu ms for vibration acknowledgement %lu.\n",
                  static_cast<unsigned long>(ENERGY_PRE_UNMUTE_WARNING_MS),
                  static_cast<unsigned long>(vibrationRequestId));
    return true;
  }

  if (vibrationAckDeadlineMs != 0)
  {
    if (bracelet.vibrationAckId == vibrationRequestId)
    {
      vibrationAckDeadlineMs = 0;
      unmuteWarningUntilMs = now + ENERGY_PRE_UNMUTE_WARNING_MS;
      Serial.printf("Vibration acknowledgement %lu received; starting pre-unmute warning.\n",
                    static_cast<unsigned long>(vibrationRequestId));
      return true;
    }
    if (deadlineIsPending(now, vibrationAckDeadlineMs))
    {
      return true;
    }

    vibrationAckDeadlineMs = 0;
    energyMuteUntilMs = 0;
    Serial.println("Vibration acknowledgement timed out; resuming alarm audio.");
    return false;
  }

  if (deadlineIsPending(now, unmuteWarningUntilMs) ||
      now - unmuteWarningUntilMs < ENERGY_UNMUTE_GRACE_MS)
  {
    setVibrationRequest(true);
    return true;
  }

  energyMuteUntilMs = 0;
  unmuteWarningUntilMs = 0;
  return false;
}

static uint32_t energyMuteRemainingMs()
{
  const uint32_t now = millis();
  if (energyMuteUntilMs == 0)
  {
    return 0;
  }

  if (deadlineIsPending(now, energyMuteUntilMs))
  {
    return energyMuteUntilMs - now;
  }
  if (deadlineIsPending(now, vibrationAckDeadlineMs))
  {
    return vibrationAckDeadlineMs - now;
  }
  if (deadlineIsPending(now, unmuteWarningUntilMs))
  {
    return unmuteWarningUntilMs - now + ENERGY_UNMUTE_GRACE_MS;
  }
  return 0;
}

static void saveCompletedAlarmRevision(int revision)
{
  if (revision < 1 || completedAlarmRevision == revision)
  {
    return;
  }

  completedAlarmRevision = revision;
  preferences.begin("station", false);
  preferences.putInt("completedRev", completedAlarmRevision);
  preferences.end();
}

static void loadCompletedAlarmRevision()
{
  preferences.begin("station", true);
  completedAlarmRevision = preferences.getInt("completedRev", 0);
  preferences.end();
}

static StationState inactiveStateForAlarm(const AlarmConfig &config)
{
  return alarmShouldBeArmed(config) ? StationState::Armed : StationState::Idle;
}

static void setStationState(StationState state, ProblemCode code = ProblemCode::None, const String &message = "")
{
  const bool changed = stationState != state || problemCode != code || problemMessage != message;
  if (changed)
  {
    Serial.printf("Station state: %s, problem: %s, %s\n", stateName(state), problemName(code), message.c_str());
    stationControlDirty = true;
  }

  stationState = state;
  problemCode = code;
  problemMessage = message;
}

static void applyTimezone(const String &timezone)
{
  if (timezone != "Europe/Zurich")
  {
    Serial.printf("Unsupported timezone '%s'; using Europe/Zurich rules for v1.\n", timezone.c_str());
  }

  setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
  tzset();
}

static bool hasRealSupabaseConfig()
{
  return String(SUPABASE_URL).startsWith("https://") && String(SUPABASE_ANON_KEY).length() > 20;
}

static bool ensureWifi()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    wifiConnectionInProgress = false;
    return true;
  }

  const uint32_t now = millis();
  const char *ssids[] = {WIFI_SSID, WIFI_FALLBACK_SSID};
  const char *passwords[] = {WIFI_PASSWORD, WIFI_FALLBACK_PASSWORD};
  const uint8_t networkCount = sizeof(ssids) / sizeof(ssids[0]);

  if (wifiConnectionInProgress && now - wifiConnectStartedMs < WIFI_CONNECT_TIMEOUT_MS)
  {
    return false;
  }

  const bool fallbackConfigured = strlen(WIFI_FALLBACK_SSID) > 0;
  if (wifiConnectionInProgress)
  {
    Serial.printf("WiFi connection to '%s' timed out\n", ssids[wifiNetworkIndex]);
    WiFi.disconnect();
    wifiNetworkIndex = (wifiNetworkIndex + 1) % networkCount;
    wifiConnectionInProgress = false;

    // Wait only after both configured networks have been tried. This makes
    // the fallback attempt start immediately after the primary times out.
    if (!fallbackConfigured || wifiNetworkIndex == 0)
    {
      lastWifiCycleFailureMs = now;
      return false;
    }
  }

  if (lastWifiCycleFailureMs != 0 && now - lastWifiCycleFailureMs < WIFI_RETRY_MS)
  {
    return false;
  }

  // An empty fallback entry is intentionally skipped, so the example
  // configuration keeps working until a second network is configured.
  for (uint8_t checked = 0; checked < networkCount; ++checked)
  {
    if (strlen(ssids[wifiNetworkIndex]) > 0)
    {
      break;
    }
    wifiNetworkIndex = (wifiNetworkIndex + 1) % networkCount;
  }

  if (strlen(ssids[wifiNetworkIndex]) == 0)
  {
    Serial.println("No WiFi SSID configured");
    return false;
  }

  wifiConnectStartedMs = now;
  wifiConnectionInProgress = true;
  Serial.printf("Connecting WiFi SSID '%s'\n", ssids[wifiNetworkIndex]);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssids[wifiNetworkIndex], passwords[wifiNetworkIndex]);
  return false;
}

static bool syncTime()
{
  if (timeReady)
  {
    return true;
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    return false;
  }

  applyTimezone(alarmConfig.timezone);
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  struct tm timeInfo;
  for (uint8_t i = 0; i < 20; ++i)
  {
    if (getLocalTime(&timeInfo, 500))
    {
      timeReady = true;
      Serial.printf("NTP time synchronized: %04d-%02d-%02d %02d:%02d:%02d\n",
                    timeInfo.tm_year + 1900,
                    timeInfo.tm_mon + 1,
                    timeInfo.tm_mday,
                    timeInfo.tm_hour,
                    timeInfo.tm_min,
                    timeInfo.tm_sec);
      return true;
    }
  }

  return false;
}

static String escapeJson(const String &value)
{
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i)
  {
    const char c = value[i];
    if (c == '\\' || c == '"')
    {
      escaped += '\\';
    }
    if (c == '\n' || c == '\r')
    {
      escaped += ' ';
    }
    else
    {
      escaped += c;
    }
  }
  return escaped;
}

static bool supabaseRequest(const String &method, const String &path, const String &body, JsonDocument *jsonOut)
{
  if (!hasRealSupabaseConfig() || WiFi.status() != WL_CONNECTED)
  {
    return false;
  }

  const uint32_t now = millis();
  if (lastSupabaseFailureMs != 0 && now - lastSupabaseFailureMs < SUPABASE_FAILURE_BACKOFF_MS)
  {
    return false;
  }

  HTTPClient http;
  const String url = String(SUPABASE_URL) + path;
  Serial.printf("Supabase %s %s (free heap: %u, largest block: %u)\n",
                method.c_str(),
                path.c_str(),
                ESP.getFreeHeap(),
                ESP.getMaxAllocHeap());
  http.begin(url);
  http.setReuse(false);
  http.setTimeout(10000);
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Content-Type", "application/json");
  // Status responses are discarded. Asking Supabase for no representation
  // avoids allocating response Strings after every 10-second publication.
  http.addHeader("Prefer", method == "POST"
                               ? "return=minimal,resolution=merge-duplicates"
                               : "return=representation");

  int status = 0;
  if (method == "GET")
  {
    status = http.GET();
  }
  else if (method == "POST")
  {
    status = http.POST(body);
  }
  else
  {
    http.end();
    return false;
  }

  String payload;
  if (status < 200 || status >= 300 || jsonOut)
  {
    payload = http.getString();
  }
  http.end();

  if (status < 200 || status >= 300)
  {
    Serial.printf("Supabase %s %s failed: HTTP %d, %s (free heap: %u, largest block: %u)\n",
                  method.c_str(),
                  path.c_str(),
                  status,
                  payload.c_str(),
                  ESP.getFreeHeap(),
                  ESP.getMaxAllocHeap());
    lastSupabaseFailureMs = millis();
    return false;
  }

  if (jsonOut)
  {
    const DeserializationError err = deserializeJson(*jsonOut, payload);
    if (err)
    {
      Serial.printf("Supabase JSON parse failed: %s\n", err.c_str());
      lastSupabaseFailureMs = millis();
      return false;
    }
  }

  lastSupabaseFailureMs = 0;
  return true;
}

static time_t parseLocalAlarmTime(const char *dateText, const char *timeText, const String &timezone)
{
  if (!dateText || !timeText || strlen(dateText) < 10 || strlen(timeText) < 5)
  {
    return 0;
  }

  applyTimezone(timezone);

  struct tm alarmTm = {};
  alarmTm.tm_year = String(dateText).substring(0, 4).toInt() - 1900;
  alarmTm.tm_mon = String(dateText).substring(5, 7).toInt() - 1;
  alarmTm.tm_mday = String(dateText).substring(8, 10).toInt();
  alarmTm.tm_hour = String(timeText).substring(0, 2).toInt();
  alarmTm.tm_min = String(timeText).substring(3, 5).toInt();
  alarmTm.tm_sec = 0;
  alarmTm.tm_isdst = -1;
  return mktime(&alarmTm);
}

static bool fetchAlarmConfig(AlarmConfig &out)
{
  JsonDocument planDoc;
  JsonDocument selectionDoc;

  if (!supabaseRequest("GET", "/rest/v1/alarm_plan?id=eq.main&select=*", "", &planDoc))
  {
    return false;
  }
  if (!supabaseRequest("GET", "/rest/v1/alarm_audio_selection?id=eq.main&select=*", "", &selectionDoc))
  {
    return false;
  }

  JsonObject plan = planDoc[0];
  JsonObject selection = selectionDoc[0];
  if (plan.isNull() || selection.isNull())
  {
    Serial.println("Supabase config rows are missing.");
    return false;
  }

  AlarmConfig next;
  next.enabled = plan["enabled"] | false;
  next.revision = plan["revision"] | 0;
  next.timezone = String(plan["timezone"] | "Europe/Zurich");
  next.volumePercent = constrain(selection["volume_percent"] | 80, 0, 100);
  next.useFallback = String(selection["audio_source"] | "fallback") == "fallback";

  if (next.revision < 1)
  {
    Serial.println("Alarm plan has invalid revision.");
    return false;
  }

  if (next.enabled)
  {
    next.alarmAt = parseLocalAlarmTime(plan["alarm_date"] | "", plan["alarm_time"] | "", next.timezone);
    if (!next.alarmAt)
    {
      Serial.println("Enabled alarm plan has invalid date or time.");
      return false;
    }
  }

  if (next.enabled && !next.useFallback)
  {
    const char *trackId = selection["selected_track_id"] | "";
    if (!strlen(trackId))
    {
      Serial.println("Audio selection is track without selected_track_id.");
      return false;
    }

    JsonDocument trackDoc;
    const String path = String("/rest/v1/music_tracks?id=eq.") + trackId + "&is_available=eq.true&select=public_url";
    if (!supabaseRequest("GET", path, "", &trackDoc))
    {
      return false;
    }

    JsonObject track = trackDoc[0];
    next.selectedTrackUrl = String(track["public_url"] | "");
    if (next.selectedTrackUrl.length() == 0)
    {
      Serial.println("Selected track has no playable public_url.");
      return false;
    }
  }

  next.valid = true;
  out = next;
  return true;
}

static void saveCachedAlarm(const AlarmConfig &config)
{
  preferences.begin("station", false);
  preferences.putBool("valid", config.valid);
  preferences.putBool("enabled", config.enabled);
  preferences.putBool("fallback", config.useFallback);
  preferences.putInt("revision", config.revision);
  preferences.putInt("volume", config.volumePercent);
  preferences.putLong64("alarmAt", static_cast<int64_t>(config.alarmAt));
  preferences.putString("timezone", config.timezone);
  preferences.putString("trackUrl", config.selectedTrackUrl);
  preferences.end();
}

static bool loadCachedAlarm(AlarmConfig &out)
{
  preferences.begin("station", true);
  AlarmConfig cached;
  cached.valid = preferences.getBool("valid", false);
  cached.enabled = preferences.getBool("enabled", false);
  cached.useFallback = preferences.getBool("fallback", true);
  cached.revision = preferences.getInt("revision", 0);
  cached.volumePercent = preferences.getInt("volume", 80);
  cached.alarmAt = static_cast<time_t>(preferences.getLong64("alarmAt", 0));
  cached.timezone = preferences.getString("timezone", "Europe/Zurich");
  cached.selectedTrackUrl = preferences.getString("trackUrl", "");
  preferences.end();

  if (!cached.valid || !cached.alarmAt)
  {
    return false;
  }

  if (timeReady && cached.alarmAt < time(nullptr) - 3600)
  {
    Serial.println("Cached alarm is stale.");
    return false;
  }

  out = cached;
  return true;
}

static int estimateBatteryPercent(float voltage)
{
  if (voltage <= 0.1F)
  {
    return -1;
  }

  const float ratio = (voltage - BATTERY_EMPTY_V) / (BATTERY_FULL_V - BATTERY_EMPTY_V);
  return static_cast<int>(roundf(constrain(ratio, 0.0F, 1.0F) * 100.0F));
}

static float readStationBatteryVoltage()
{
  const uint32_t measuredMillivolts = analogReadMilliVolts(PIN_BATTERY_ADC);
  return (static_cast<float>(measuredMillivolts) * BATTERY_DIVIDER_RATIO) / 1000.0F;
}

static void publishStationStatus()
{
  const String activeRevision = alarmConfig.revision >= 1 ? String(alarmConfig.revision) : "null";
  const String batteryVoltage = stationBatteryVoltage > 0.1F ? String(stationBatteryVoltage, 3) : "null";
  const String body = String("{\"id\":\"main\",\"station_state\":\"") + stateName(stationState) +
                      "\",\"problem_code\":\"" + problemName(problemCode) +
                      "\",\"problem_message\":\"" + escapeJson(problemMessage) +
                      "\",\"active_alarm_revision\":" + activeRevision +
                      ",\"station_battery_voltage\":" + batteryVoltage + "}";
  supabaseRequest("POST", "/rest/v1/station_status", body, nullptr);
}

static void publishBraceletStatus()
{
  String batteryVoltage = "null";
  if (bracelet.batteryVoltage > 0.1F)
  {
    batteryVoltage = String(bracelet.batteryVoltage, 3);
  }

  String lastSeen = "null";
  if (bracelet.lastSeenMs > 0)
  {
    lastSeen = String(bracelet.lastSeenMs);
  }

  String wifiRssiDbm = "null";
  const bool braceletRecentlySeen = bracelet.lastSeenMs > 0 &&
                                     millis() - bracelet.lastSeenMs <= ARMED_BRACELET_TIMEOUT_MS;
  if (braceletRecentlySeen && bracelet.wifiRssiDbm != BraceletStationProtocol::WIFI_RSSI_UNKNOWN_DBM)
  {
    wifiRssiDbm = String(bracelet.wifiRssiDbm);
  }

  const String body = String("{\"id\":\"main\",\"bracelet_state\":\"") + braceletStateName(bracelet.state) +
                      "\",\"problem_code\":\"" + problemName(bracelet.problem) +
                      "\",\"problem_message\":\"" + escapeJson(problemName(bracelet.problem)) +
                      "\",\"bracelet_battery_voltage\":" + batteryVoltage +
                      ",\"bracelet_wifi_rssi_dbm\":" + wifiRssiDbm +
                      ",\"bracelet_last_seen_ms\":" + lastSeen +
                      ",\"bracelet_energy\":" + String(bracelet.energy) +
                      ",\"bracelet_energy_valid_ms\":" + String(bracelet.energyValidMs) +
                      ",\"bracelet_energy_threshold\":" + String(BraceletStationProtocol::NORMAL_ENERGY_THRESHOLD) +
                      ",\"energy_mute_remaining_ms\":" + String(energyMuteRemainingMs()) +
                      ",\"bracelet_vibrating\":" + String(bracelet.vibrating ? "true" : "false") + "}";
  supabaseRequest("POST", "/rest/v1/bracelet_status", body, nullptr);
}

static void requestStationStatusPublish()
{
  if (remoteAudioActive)
  {
    pendingStationStatusPublish = true;
    return;
  }

  publishStationStatus();
  lastStatusPublishMs = millis();
}

static void requestBraceletStatusPublish()
{
  if (remoteAudioActive)
  {
    pendingBraceletStatusPublish = true;
    return;
  }

  publishBraceletStatus();
  lastStatusPublishMs = millis();
}

static void flushDeferredStatusPublishes()
{
  if (remoteAudioActive || (!pendingStationStatusPublish && !pendingBraceletStatusPublish))
  {
    return;
  }

  if (pendingStationStatusPublish)
  {
    pendingStationStatusPublish = false;
    publishStationStatus();
  }

  if (pendingBraceletStatusPublish)
  {
    pendingBraceletStatusPublish = false;
    publishBraceletStatus();
  }

  lastStatusPublishMs = millis();
}

static void stopFallbackAudio()
{
  if (!fallbackAudioActive)
  {
    return;
  }

  i2s_zero_dma_buffer(FALLBACK_I2S_PORT);
  i2s_driver_uninstall(FALLBACK_I2S_PORT);
  fallbackAudioActive = false;
}

static bool startFallbackAudio()
{
  audio.stopSong();
  remoteAudioActive = false;
  stopFallbackAudio();

  const i2s_config_t config = {
      .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX),
      .sample_rate = FALLBACK_SAMPLE_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = FALLBACK_FRAMES,
      .use_apll = false,
      .tx_desc_auto_clear = true,
      .fixed_mclk = 0,
      .mclk_multiple = I2S_MCLK_MULTIPLE_DEFAULT,
      .bits_per_chan = I2S_BITS_PER_CHAN_DEFAULT,
  };

  const i2s_pin_config_t pins = {
      .mck_io_num = I2S_PIN_NO_CHANGE,
      .bck_io_num = PIN_BCK,
      .ws_io_num = PIN_LRCK,
      .data_out_num = PIN_DIN,
      .data_in_num = I2S_PIN_NO_CHANGE,
  };

  esp_err_t err = i2s_driver_install(FALLBACK_I2S_PORT, &config, 0, nullptr);
  if (err != ESP_OK)
  {
    Serial.printf("Fallback I2S install failed: %d\n", err);
    setStationState(StationState::Fault, ProblemCode::FallbackAudioFailed, "Generated fallback audio could not start");
    return false;
  }

  err = i2s_set_pin(FALLBACK_I2S_PORT, &pins);
  if (err != ESP_OK)
  {
    Serial.printf("Fallback I2S pin setup failed: %d\n", err);
    setStationState(StationState::Fault, ProblemCode::FallbackAudioFailed, "Generated fallback audio pin setup failed");
    return false;
  }

  fallbackAudioActive = true;
  Serial.println("Generated local fallback alarm started.");
  return true;
}

static void writeFallbackAudio()
{
  if (!fallbackAudioActive)
  {
    return;
  }

  static uint32_t phase = 0;
  static int16_t frames[FALLBACK_FRAMES * 2];
  const uint32_t toneHz = ((millis() / 700) % 2 == 0) ? 880 : 1320;
  const uint32_t step = (toneHz * 65536UL) / FALLBACK_SAMPLE_RATE;

  for (uint16_t i = 0; i < FALLBACK_FRAMES; ++i)
  {
    phase += step;
    const int16_t sample = (phase & 0x8000) ? 12000 : -12000;
    frames[i * 2] = sample;
    frames[i * 2 + 1] = sample;
  }

  size_t bytesWritten = 0;
  i2s_write(FALLBACK_I2S_PORT, frames, sizeof(frames), &bytesWritten, 0);
}

static bool startAlarmAudio()
{
  energyMuteUntilMs = 0;
  unmuteWarningUntilMs = 0;
  vibrationAckDeadlineMs = 0;
  alarmAudioStartedMs = millis();
  if (initialVibrationDelayIsActive())
  {
    Serial.printf("Bracelet vibration delayed for the first %lld seconds after the configured alarm time.\n",
                  static_cast<long long>(INITIAL_VIBRATION_DELAY_SECONDS));
  }
  setVibrationRequest(true);
  digitalWrite(PIN_XSMT, HIGH);
  alarmAudioMuted = false;
  audio.setVolume(map(alarmConfig.volumePercent, 0, 100, 0, 21));

  if (!alarmConfig.useFallback && alarmConfig.selectedTrackUrl.length() > 0)
  {
    Serial.printf("Starting selected Supabase track: %s\n", alarmConfig.selectedTrackUrl.c_str());
    if (audio.connecttohost(alarmConfig.selectedTrackUrl.c_str()))
    {
      remoteAudioActive = true;
      fallbackAudioActive = false;
      const uint32_t primeStartMs = millis();
      while (millis() - primeStartMs < REMOTE_AUDIO_PRIME_MS && audio.isRunning())
      {
        audio.loop();
        delay(1);
      }
      return true;
    }

    Serial.println("Selected Supabase track failed; switching to local fallback.");
    setStationState(StationState::Ringing, ProblemCode::MusicStreamFailed, "Supabase music failed, using local fallback");
  }

  const bool started = startFallbackAudio();
  if (!started)
  {
    alarmAudioStartedMs = 0;
    setVibrationRequest(false);
  }
  return started;
}

static void stopAlarmAudio()
{
  audio.stopSong();
  remoteAudioActive = false;
  stopFallbackAudio();
  energyMuteUntilMs = 0;
  unmuteWarningUntilMs = 0;
  vibrationAckDeadlineMs = 0;
  alarmAudioStartedMs = 0;
  setVibrationRequest(false);
  digitalWrite(PIN_XSMT, LOW);
  alarmAudioMuted = true;
}

static void serviceRemoteAudio(uint32_t budgetMs)
{
  if (!remoteAudioActive)
  {
    return;
  }

  const uint32_t startedMs = millis();
  do
  {
    audio.loop();
    delay(0);
  } while (remoteAudioActive && audio.isRunning() && millis() - startedMs < budgetMs);

  if (!audio.isRunning() && (stationState == StationState::Ringing || stationState == StationState::ValidatingActivity))
  {
    Serial.println("Selected Supabase track stopped unexpectedly; switching to fallback.");
    setStationState(stationState, ProblemCode::MusicStreamFailed, "Supabase music stopped, using local fallback");
    startFallbackAudio();
  }
}

static bool setAlarmAudioMuted(bool muted)
{
  if (alarmAudioMuted == muted)
  {
    return false;
  }

  digitalWrite(PIN_XSMT, muted ? LOW : HIGH);
  alarmAudioMuted = muted;
  return true;
}

static bool braceletIsReadyForAlarm()
{
  if (!REQUIRE_BRACELET_READY)
  {
    return true;
  }

  const uint32_t age = bracelet.lastSeenMs ? millis() - bracelet.lastSeenMs : UINT32_MAX;
  if (age > ARMED_BRACELET_TIMEOUT_MS)
  {
    setStationState(StationState::Fault, ProblemCode::BraceletMissing, "Bracelet packet timeout before alarm");
    return false;
  }

  if (bracelet.state == BraceletState::Fault || bracelet.problem == ProblemCode::BraceletFault || bracelet.problem == ProblemCode::SensorFault)
  {
    setStationState(StationState::Fault, ProblemCode::BraceletFault, "Bracelet reports a blocking fault");
    return false;
  }

  if (bracelet.batteryPercent >= 0 && bracelet.batteryPercent < BRACELET_BLOCKING_BATTERY_PERCENT)
  {
    setStationState(StationState::Fault, ProblemCode::BraceletLowBattery, "Bracelet battery is below blocking threshold");
    return false;
  }

  return true;
}

static void refreshConfig()
{
  AlarmConfig next;
  if (fetchAlarmConfig(next))
  {
    alarmConfig = next;
    saveCachedAlarm(alarmConfig);
    if (!alarmIsActive())
    {
      if (alarmIsStale(alarmConfig))
      {
        saveCompletedAlarmRevision(alarmConfig.revision);
        Serial.println("Loaded alarm is stale; it will not start until the app saves a new revision.");
      }
      setStationState(inactiveStateForAlarm(alarmConfig));
    }
    Serial.printf("Loaded alarm revision %d, enabled=%d\n", alarmConfig.revision, alarmConfig.enabled);
    publishStationStatus();
    return;
  }

  if (loadCachedAlarm(next))
  {
    alarmConfig = next;
    if (!alarmIsActive())
    {
      if (alarmIsStale(alarmConfig))
      {
        saveCompletedAlarmRevision(alarmConfig.revision);
        Serial.println("Cached alarm is stale; it will not start until the app saves a new revision.");
      }
      setStationState(inactiveStateForAlarm(alarmConfig),
                      ProblemCode::SupabaseUnavailable,
                      "Using cached alarm because Supabase is unavailable");
    }
    publishStationStatus();
    return;
  }

  if (alarmIsActive())
  {
    setStationState(stationState, ProblemCode::SupabaseUnavailable, "Config refresh failed during active alarm; continuing alarm");
    publishStationStatus();
    return;
  }

  setStationState(StationState::Fault, ProblemCode::NoValidAlarmConfig, "No valid Supabase or cached alarm config");
  publishStationStatus();
}

static void sendStationControl()
{
  if (!espNowReady)
  {
    return;
  }

  StationControlPacket packet = {};
  packet.header.magic = BraceletStationProtocol::MAGIC;
  packet.header.protocolVersion = BraceletStationProtocol::VERSION;
  packet.header.messageType = BraceletStationProtocol::MESSAGE_STATION_CONTROL;
  packet.header.senderRole = BraceletStationProtocol::SENDER_STATION;
  memcpy(packet.header.deviceId, stationMac, sizeof(packet.header.deviceId));
  packet.header.sequence = ++controlSequence;
  packet.header.uptimeMs = millis();
  packet.stationState = static_cast<uint8_t>(stationState);
  packet.alarmRevision = alarmConfig.revision;
  packet.vibrationRequest = vibrationRequestActive ? 1 : 0;
  packet.thresholdProfile = BraceletStationProtocol::THRESHOLD_PROFILE_NORMAL;
  packet.acknowledgedBootSessionId = acknowledgedBootSessionId;
  packet.acknowledgedMovementEventId = acknowledgedMovementEventId;
  packet.vibrationRequestId = vibrationRequestActive ? vibrationRequestId : 0;

  const esp_err_t result = esp_now_send(BROADCAST_PEER,
                                        reinterpret_cast<const uint8_t *>(&packet),
                                        sizeof(packet));
  if (result != ESP_OK)
  {
    Serial.printf("espnow_send_error:station_control:%d\n", static_cast<int>(result));
  }

  stationControlDirty = false;
  serviceRemoteAudio(REMOTE_AUDIO_AFTER_CONTROL_MS);
}

static ProblemCode packetProblemToCode(uint8_t value)
{
  if (value <= static_cast<uint8_t>(ProblemCode::UnknownFault))
  {
    return static_cast<ProblemCode>(value);
  }
  return ProblemCode::UnknownFault;
}

static bool deviceIdMatches(const uint8_t *left, const uint8_t *right)
{
  return memcmp(left, right, 6) == 0;
}

static void loadPairedBracelet()
{
  // Keep the existing namespace so installations paired over UDP retain the
  // same dedicated bracelet after the ESP-NOW migration.
  preferences.begin("udp-link", true);
  if (preferences.getBytesLength("braceletId") == sizeof(pairedBraceletId))
  {
    preferences.getBytes("braceletId", pairedBraceletId, sizeof(pairedBraceletId));
    braceletPaired = true;
  }
  preferences.end();
  Serial.println(braceletPaired ? "espnow_pairing:bracelet_loaded" : "espnow_pairing:waiting_for_bracelet");
}

static void savePairedBracelet(const uint8_t *braceletId)
{
  memcpy(pairedBraceletId, braceletId, sizeof(pairedBraceletId));
  preferences.begin("udp-link", false);
  preferences.putBytes("braceletId", pairedBraceletId, sizeof(pairedBraceletId));
  preferences.end();
  braceletPaired = true;
  Serial.printf("espnow_pairing:bracelet_saved:%02X:%02X:%02X:%02X:%02X:%02X\n",
                pairedBraceletId[0], pairedBraceletId[1], pairedBraceletId[2],
                pairedBraceletId[3], pairedBraceletId[4], pairedBraceletId[5]);
}

static void handleBraceletStatus(const BraceletStatusPacket &packet)
{
  const uint32_t previousBootSessionId = bracelet.bootSessionId;
  const uint32_t previousSequence = bracelet.sequence;
  bracelet.state = packet.braceletState <= static_cast<uint8_t>(BraceletState::Fault)
                       ? static_cast<BraceletState>(packet.braceletState)
                       : BraceletState::Unknown;
  bracelet.activityScore = packet.activityScore;
  bracelet.validated = packet.validated != 0;
  bracelet.batteryVoltage = packet.batteryVoltageMv == 0 ? -1.0F : static_cast<float>(packet.batteryVoltageMv) / 1000.0F;
  bracelet.batteryPercent = estimateBatteryPercent(bracelet.batteryVoltage);
  bracelet.wifiRssiDbm = packet.wifiRssiDbm;
  bracelet.problem = packetProblemToCode(packet.faultCode);
  bracelet.charging = packet.flags & 0x01;
  bracelet.sensorReady = packet.flags & 0x02;
  bracelet.vibrating = packet.flags & 0x04;
  bracelet.energy = packet.energy;
  bracelet.energyValidMs = packet.energyValidMs;
  bracelet.sequence = packet.header.sequence;
  bracelet.bootSessionId = packet.bootSessionId;
  bracelet.movementEventId = packet.movementEventId;
  bracelet.vibrationAckId = packet.vibrationAckId;
  const uint32_t now = millis();
  bracelet.lastSeenMs = now;

  if (now - lastBraceletEspNowDebugMs >= 5000)
  {
    lastBraceletEspNowDebugMs = now;
    Serial.printf("espnow_rx_bracelet:seq=%lu,battery_mv=%u,battery_v=%.3f,size=%u,channel=%d\n",
                  static_cast<unsigned long>(packet.header.sequence),
                  static_cast<unsigned int>(packet.batteryVoltageMv),
                  bracelet.batteryVoltage,
                  static_cast<unsigned int>(sizeof(packet)),
                  WiFi.channel());
  }

  if (previousBootSessionId == packet.bootSessionId && previousSequence != 0 &&
      static_cast<int32_t>(packet.header.sequence - previousSequence) > 1)
  {
    Serial.printf("espnow_bracelet_gap:%lu\n",
                  static_cast<unsigned long>(packet.header.sequence - previousSequence - 1));
  }

  if (packet.bootSessionId != processedMovementBootSessionId)
  {
    processedMovementBootSessionId = packet.bootSessionId;
    processedMovementEventId = 0;
    acknowledgedBootSessionId = packet.bootSessionId;
    acknowledgedMovementEventId = 0;
  }

  if (eventIdIsNewer(packet.movementEventId, processedMovementEventId))
  {
    processedMovementEventId = packet.movementEventId;
    acknowledgedBootSessionId = packet.bootSessionId;
    acknowledgedMovementEventId = packet.movementEventId;
    stationControlDirty = true;

    const uint32_t eventAgeMs = packet.header.uptimeMs - packet.movementEventUptimeMs;
    const bool eventIsValid = packet.movementEventValidMs >= BraceletStationProtocol::MIN_ENERGY_VALID_MS &&
                              packet.movementEventEnergy >= BraceletStationProtocol::NORMAL_ENERGY_THRESHOLD;
    const bool eventOccurredAfterAudioStarted = alarmAudioStartedMs != 0 &&
                                                now - alarmAudioStartedMs >= eventAgeMs;
    if (alarmIsActive() && eventIsValid &&
        eventAgeMs < BraceletStationProtocol::MOVEMENT_HISTORY_MS &&
        eventOccurredAfterAudioStarted)
    {
      const uint32_t remainingMuteMs = BraceletStationProtocol::MOVEMENT_HISTORY_MS - eventAgeMs;
      const uint32_t candidateMuteUntilMs = now + remainingMuteMs;
      if (energyMuteUntilMs == 0 ||
          static_cast<int32_t>(candidateMuteUntilMs - energyMuteUntilMs) > 0)
      {
        energyMuteUntilMs = candidateMuteUntilMs;
      }
      unmuteWarningUntilMs = 0;
      vibrationAckDeadlineMs = 0;
      setVibrationRequest(false);
      Serial.printf("Movement event %lu recovered at age %lu ms; muting for %lu ms.\n",
                    static_cast<unsigned long>(packet.movementEventId),
                    static_cast<unsigned long>(eventAgeMs),
                    static_cast<unsigned long>(remainingMuteMs));
    }
  }
}

static void onEspNowDataReceived(const uint8_t *, const uint8_t *data, int length)
{
  if (length != static_cast<int>(sizeof(BraceletStatusPacket)))
  {
    return;
  }

  BraceletStatusPacket packet = {};
  memcpy(&packet, data, sizeof(packet));
  if (packet.header.magic != BraceletStationProtocol::MAGIC ||
      packet.header.protocolVersion != BraceletStationProtocol::VERSION ||
      packet.header.messageType != BraceletStationProtocol::MESSAGE_BRACELET_STATUS ||
      packet.header.senderRole != BraceletStationProtocol::SENDER_BRACELET)
  {
    return;
  }

  portENTER_CRITICAL(&espNowMux);
  pendingBraceletStatus = packet;
  braceletStatusPending = true;
  portEXIT_CRITICAL(&espNowMux);
}

static void setupEspNow()
{
  if (esp_now_init() != ESP_OK)
  {
    Serial.println("espnow_status:init_failed");
    return;
  }

  esp_now_register_recv_cb(onEspNowDataReceived);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_PEER, ESP_NOW_ETH_ALEN);
  peer.channel = 0;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK)
  {
    Serial.println("espnow_status:add_broadcast_peer_failed");
    return;
  }

  espNowReady = true;
  stationControlDirty = true;
  Serial.printf("espnow_status:ready,channel=%d\n", WiFi.channel());
}

static void serviceBraceletEspNow()
{
  BraceletStatusPacket packet = {};
  bool hasPacket = false;
  portENTER_CRITICAL(&espNowMux);
  if (braceletStatusPending)
  {
    packet = pendingBraceletStatus;
    braceletStatusPending = false;
    hasPacket = true;
  }
  portEXIT_CRITICAL(&espNowMux);

  if (!hasPacket)
  {
    return;
  }

  if (!braceletPaired)
  {
    savePairedBracelet(packet.header.deviceId);
  }
  else if (!deviceIdMatches(packet.header.deviceId, pairedBraceletId))
  {
    Serial.println("espnow_packet_rejected:foreign_bracelet");
    return;
  }

  handleBraceletStatus(packet);
}

static void updateAlarmState()
{
  serviceRemoteAudio(REMOTE_AUDIO_LOOP_BUDGET_MS);

  writeFallbackAudio();

  if (stationState == StationState::Ringing || stationState == StationState::ValidatingActivity)
  {
    const uint32_t age = bracelet.lastSeenMs ? millis() - bracelet.lastSeenMs : UINT32_MAX;

    if (alarmWindowHasEnded(alarmConfig))
    {
      stopAlarmAudio();
      saveCompletedAlarmRevision(alarmConfig.revision);
      setStationState(StationState::Stopped, ProblemCode::None, "10 minute alarm activity window ended");
      requestStationStatusPublish();
      return;
    }

    const bool braceletMissing = REQUIRE_BRACELET_READY && age > RINGING_BRACELET_TIMEOUT_MS;
    const bool keepAudioMuted = energyKeepsAudioMuted();
    if (keepAudioMuted)
    {
      const bool muteChanged = setAlarmAudioMuted(true);
      const ProblemCode nextProblem = braceletMissing ? ProblemCode::BraceletMissing : ProblemCode::None;
      const String nextMessage = braceletMissing
                                     ? "Bracelet missing; honoring the remaining movement mute"
                                     : "Alarm audio muted by confirmed bracelet movement";
      const bool stateChanged = stationState != StationState::ValidatingActivity || problemCode != nextProblem;
      setStationState(StationState::ValidatingActivity, nextProblem, nextMessage);
      if (stateChanged || muteChanged || stationControlDirty)
      {
        sendStationControl();
        lastControlSendMs = millis();
        requestStationStatusPublish();
      }
      return;
    }

    setVibrationRequest(true);
    if (braceletMissing)
    {
      setStationState(StationState::Ringing,
                      ProblemCode::BraceletMissing,
                      "Bracelet missing; alarm audio remains audible");
    }
    else if (problemCode == ProblemCode::BraceletMissing || stationState != StationState::Ringing)
    {
      setStationState(StationState::Ringing, ProblemCode::None, "Bracelet communication available");
    }

    if (alarmWindowIsOpen(alarmConfig))
    {
      if (!remoteAudioActive && !fallbackAudioActive && startAlarmAudio())
      {
        setAlarmAudioMuted(false);
        setStationState(StationState::Ringing, problemCode, problemMessage);
        sendStationControl();
        lastControlSendMs = millis();
        requestStationStatusPublish();
      }
      else if (stationState != StationState::Ringing)
      {
        setAlarmAudioMuted(false);
        setStationState(StationState::Ringing, braceletMissing ? ProblemCode::BraceletMissing : ProblemCode::None,
                        braceletMissing ? "Bracelet missing; alarm audio remains audible" : "");
        sendStationControl();
        lastControlSendMs = millis();
        requestStationStatusPublish();
      }
      else
      {
        setAlarmAudioMuted(false);
        if (stationControlDirty)
        {
          sendStationControl();
          lastControlSendMs = millis();
        }
      }
    }
    return;
  }

  if (stationState != StationState::Armed || !alarmConfig.valid || !alarmConfig.enabled)
  {
    return;
  }

  if (alarmRevisionWasCompleted(alarmConfig))
  {
    setStationState(StationState::Idle);
    return;
  }

  if (!timeReady)
  {
    setStationState(StationState::Fault, ProblemCode::TimeUnknown, "Time is not synchronized");
    return;
  }

  if (alarmWindowHasEnded(alarmConfig))
  {
    saveCompletedAlarmRevision(alarmConfig.revision);
    setStationState(StationState::Idle, ProblemCode::None, "10 minute alarm activity window already ended");
    requestStationStatusPublish();
    return;
  }

  if (!braceletIsReadyForAlarm())
  {
    return;
  }

  if (alarmWindowIsOpen(alarmConfig))
  {
    if (startAlarmAudio())
    {
      if (energyKeepsAudioMuted())
      {
        setAlarmAudioMuted(true);
        setStationState(StationState::ValidatingActivity, ProblemCode::None, "Alarm audio muted by bracelet energy threshold");
      }
      else
      {
        setAlarmAudioMuted(false);
        setStationState(StationState::Ringing);
      }
      sendStationControl();
      lastControlSendMs = millis();
      requestStationStatusPublish();
    }
  }
}

void setup()
{
  Serial.begin(115200);
  delay(200);

  vibrationRequestId = esp_random();

  pinMode(PIN_XSMT, OUTPUT);
  digitalWrite(PIN_XSMT, HIGH);
  pinMode(PIN_BATTERY_ADC, INPUT);
  stationBatteryVoltage = readStationBatteryVoltage();

  WiFi.mode(WIFI_STA);
  WiFi.macAddress(stationMac);
  esp_wifi_set_ps(WIFI_PS_NONE);
  loadPairedBracelet();

  audio.setBufsize(AUDIO_BUFFER_RAM_BYTES, AUDIO_BUFFER_PSRAM_BYTES);
  audio.setPinout(PIN_BCK, PIN_LRCK, PIN_DIN);
  audio.setConnectionTimeout(500, 2700);
  audio.setVolume(17);

  ensureWifi();
  setupEspNow();
  loadCompletedAlarmRevision();

  if (loadCachedAlarm(alarmConfig))
  {
    setStationState(inactiveStateForAlarm(alarmConfig));
  }
  else
  {
    setStationState(StationState::Fault, ProblemCode::NoValidAlarmConfig, "Waiting for first valid Supabase alarm config");
  }
}

void loop()
{
  if (!remoteAudioActive)
  {
    ensureWifi();
    syncTime();
  }

  const uint32_t nowMs = millis();
  serviceBraceletEspNow();
  if (!remoteAudioActive)
  {
    stationBatteryVoltage = readStationBatteryVoltage();
  }

  if (nowMs - lastConfigRefreshMs >= CONFIG_REFRESH_MS || lastConfigRefreshMs == 0)
  {
    lastConfigRefreshMs = nowMs;
    if (alarmIsActive())
    {
      // Keep audio.loop() fed during the alarm; HTTPS refreshes can block long enough
      // to underrun streamed audio on the ESP32.
    }
    else if (WiFi.status() == WL_CONNECTED && timeReady)
    {
      refreshConfig();
    }
    else if (!timeReady)
    {
      setStationState(StationState::Fault, ProblemCode::TimeUnknown, "Waiting for NTP time");
    }
  }

  if (bracelet.batteryPercent >= 0 && bracelet.batteryPercent < BRACELET_LOW_BATTERY_PERCENT)
  {
    bracelet.problem = ProblemCode::BraceletLowBattery;
  }

  updateAlarmState();
  flushDeferredStatusPublishes();

  const uint32_t controlSendPeriod = alarmIsActive() ? ACTIVE_CONTROL_SEND_MS : INACTIVE_CONTROL_SEND_MS;
  if (stationControlDirty || nowMs - lastControlSendMs >= controlSendPeriod)
  {
    lastControlSendMs = nowMs;
    sendStationControl();
  }

  if (!remoteAudioActive && (nowMs - lastStatusPublishMs >= STATUS_PUBLISH_MS || lastStatusPublishMs == 0))
  {
    lastStatusPublishMs = nowMs;
    publishStationStatus();
    publishBraceletStatus();
  }

  if (remoteAudioActive)
  {
    serviceRemoteAudio(REMOTE_AUDIO_LOOP_BUDGET_MS);
    delay(0);
  }
  else
  {
    delay(2);
  }
}
