#include <Arduino.h>
#include <ArduinoJson.h>
#include <Audio.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <time.h>

#include <driver/i2s.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

static const int PIN_LRCK = 32;
static const int PIN_DIN = 33;
static const int PIN_BCK = 25;
static const int PIN_XSMT = 26;

static const uint8_t PROTOCOL_VERSION = 1;
static const uint32_t CONFIG_REFRESH_MS = 10000;
static const uint32_t STATUS_PUBLISH_MS = 10000;
static const uint32_t CONTROL_SEND_MS = 5000;
static const uint32_t WIFI_RETRY_MS = 15000;
static const uint32_t ARMED_BRACELET_TIMEOUT_MS = 15000;
static const uint32_t RINGING_BRACELET_TIMEOUT_MS = 5000;
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
  bool motionPresent = false;
  bool charging = false;
  int batteryPercent = -1;
  uint32_t lastSeenMs = 0;
  uint32_t sequence = 0;
};

struct __attribute__((packed)) BraceletStatusPacket
{
  uint8_t protocolVersion;
  uint8_t messageType;
  uint8_t senderRole;
  uint8_t deviceId[6];
  uint32_t sequence;
  uint32_t uptimeMs;
  uint8_t braceletState;
  uint8_t activityScore;
  uint8_t validated;
  uint8_t batteryPercent;
  uint8_t faultCode;
  uint8_t flags;
};

struct __attribute__((packed)) StationControlPacket
{
  uint8_t protocolVersion;
  uint8_t messageType;
  uint8_t senderRole;
  uint8_t deviceId[6];
  uint32_t sequence;
  uint32_t uptimeMs;
  uint8_t stationState;
  int32_t alarmRevision;
  uint8_t activityRequired;
  uint8_t thresholdProfile;
};

static Audio audio;
static Preferences preferences;
static AlarmConfig alarmConfig;
static BraceletSnapshot bracelet;
static StationState stationState = StationState::Idle;
static ProblemCode problemCode = ProblemCode::None;
static String problemMessage = "";
static bool remoteAudioActive = false;
static bool fallbackAudioActive = false;
static bool espNowReady = false;
static bool timeReady = false;
static uint32_t lastConfigRefreshMs = 0;
static uint32_t lastStatusPublishMs = 0;
static uint32_t lastControlSendMs = 0;
static uint32_t lastWifiAttemptMs = 0;
static uint32_t controlSequence = 0;
static uint8_t stationMac[6] = {0};

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

static void setStationState(StationState state, ProblemCode code = ProblemCode::None, const String &message = "")
{
  if (stationState != state || problemCode != code || problemMessage != message)
  {
    Serial.printf("Station state: %s, problem: %s, %s\n", stateName(state), problemName(code), message.c_str());
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
    return true;
  }

  const uint32_t now = millis();
  if (now - lastWifiAttemptMs < WIFI_RETRY_MS)
  {
    return false;
  }

  lastWifiAttemptMs = now;
  Serial.printf("Connecting WiFi SSID '%s'\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
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

  HTTPClient http;
  const String url = String(SUPABASE_URL) + path;
  http.begin(url);
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", "return=representation,resolution=merge-duplicates");

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

  const String payload = http.getString();
  http.end();

  if (status < 200 || status >= 300)
  {
    Serial.printf("Supabase %s %s failed: HTTP %d, %s\n", method.c_str(), path.c_str(), status, payload.c_str());
    return false;
  }

  if (jsonOut)
  {
    const DeserializationError err = deserializeJson(*jsonOut, payload);
    if (err)
    {
      Serial.printf("Supabase JSON parse failed: %s\n", err.c_str());
      return false;
    }
  }

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
  preferences.putString("selectedTrackUrl", config.selectedTrackUrl);
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
  cached.selectedTrackUrl = preferences.getString("selectedTrackUrl", "");
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

static void publishStationStatus()
{
  const String activeRevision = alarmConfig.revision >= 1 ? String(alarmConfig.revision) : "null";
  const String body = String("{\"id\":\"main\",\"station_state\":\"") + stateName(stationState) +
                      "\",\"problem_code\":\"" + problemName(problemCode) +
                      "\",\"problem_message\":\"" + escapeJson(problemMessage) +
                      "\",\"active_alarm_revision\":" + activeRevision + "}";
  supabaseRequest("POST", "/rest/v1/station_status", body, nullptr);
}

static void publishBraceletStatus()
{
  String battery = "null";
  if (bracelet.batteryPercent >= 0)
  {
    battery = String(bracelet.batteryPercent);
  }

  String lastSeen = "null";
  if (bracelet.lastSeenMs > 0)
  {
    lastSeen = String(bracelet.lastSeenMs);
  }

  const String body = String("{\"id\":\"main\",\"bracelet_state\":\"") + braceletStateName(bracelet.state) +
                      "\",\"problem_code\":\"" + problemName(bracelet.problem) +
                      "\",\"problem_message\":\"" + escapeJson(problemName(bracelet.problem)) +
                      "\",\"bracelet_battery_percent\":" + battery +
                      ",\"bracelet_last_seen_ms\":" + lastSeen + "}";
  supabaseRequest("POST", "/rest/v1/bracelet_status", body, nullptr);
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
  digitalWrite(PIN_XSMT, HIGH);
  audio.setVolume(map(alarmConfig.volumePercent, 0, 100, 0, 21));

  if (!alarmConfig.useFallback && alarmConfig.selectedTrackUrl.length() > 0)
  {
    Serial.printf("Starting selected Supabase track: %s\n", alarmConfig.selectedTrackUrl.c_str());
    if (audio.connecttohost(alarmConfig.selectedTrackUrl.c_str()))
    {
      remoteAudioActive = true;
      fallbackAudioActive = false;
      return true;
    }

    Serial.println("Selected Supabase track failed; switching to local fallback.");
    setStationState(StationState::Ringing, ProblemCode::MusicStreamFailed, "Supabase music failed, using local fallback");
  }

  return startFallbackAudio();
}

static void stopAlarmAudio()
{
  audio.stopSong();
  remoteAudioActive = false;
  stopFallbackAudio();
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
    setStationState(alarmConfig.enabled ? StationState::Armed : StationState::Idle);
    Serial.printf("Loaded alarm revision %d, enabled=%d\n", alarmConfig.revision, alarmConfig.enabled);
    publishStationStatus();
    return;
  }

  if (loadCachedAlarm(next))
  {
    alarmConfig = next;
    setStationState(alarmConfig.enabled ? StationState::Armed : StationState::Idle,
                    ProblemCode::SupabaseUnavailable,
                    "Using cached alarm because Supabase is unavailable");
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
  packet.protocolVersion = PROTOCOL_VERSION;
  packet.messageType = 2;
  packet.senderRole = 1;
  memcpy(packet.deviceId, stationMac, sizeof(packet.deviceId));
  packet.sequence = ++controlSequence;
  packet.uptimeMs = millis();
  packet.stationState = static_cast<uint8_t>(stationState);
  packet.alarmRevision = alarmConfig.revision;
  packet.activityRequired = (stationState == StationState::Ringing || stationState == StationState::ValidatingActivity) ? 1 : 0;
  packet.thresholdProfile = 0;

  const uint8_t broadcastMac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  esp_now_send(broadcastMac, reinterpret_cast<uint8_t *>(&packet), sizeof(packet));
}

static ProblemCode packetProblemToCode(uint8_t value)
{
  if (value <= static_cast<uint8_t>(ProblemCode::UnknownFault))
  {
    return static_cast<ProblemCode>(value);
  }
  return ProblemCode::UnknownFault;
}

static void onEspNowDataReceived(const uint8_t *mac, const uint8_t *data, int len)
{
  (void)mac;

  if (len != sizeof(BraceletStatusPacket))
  {
    return;
  }

  BraceletStatusPacket packet;
  memcpy(&packet, data, sizeof(packet));
  if (packet.protocolVersion != PROTOCOL_VERSION || packet.messageType != 1 || packet.senderRole != 2)
  {
    return;
  }

  bracelet.state = packet.braceletState <= static_cast<uint8_t>(BraceletState::Fault)
                       ? static_cast<BraceletState>(packet.braceletState)
                       : BraceletState::Unknown;
  bracelet.activityScore = packet.activityScore;
  bracelet.validated = packet.validated != 0;
  bracelet.batteryPercent = packet.batteryPercent == 255 ? -1 : packet.batteryPercent;
  bracelet.problem = packetProblemToCode(packet.faultCode);
  bracelet.charging = packet.flags & 0x01;
  bracelet.sensorReady = packet.flags & 0x02;
  bracelet.motionPresent = packet.flags & 0x04;
  bracelet.sequence = packet.sequence;
  bracelet.lastSeenMs = millis();
}

static void setupEspNow()
{
  if (esp_now_init() != ESP_OK)
  {
    Serial.println("ESP-NOW init failed.");
    return;
  }

  esp_now_register_recv_cb(onEspNowDataReceived);

  const uint8_t broadcastMac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, broadcastMac, sizeof(broadcastMac));
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  espNowReady = true;
  Serial.println("ESP-NOW receiver ready.");
}

static void updateAlarmState()
{
  if (remoteAudioActive)
  {
    audio.loop();
    if (!audio.isRunning() && (stationState == StationState::Ringing || stationState == StationState::ValidatingActivity))
    {
      Serial.println("Selected Supabase track stopped unexpectedly; switching to fallback.");
      setStationState(stationState, ProblemCode::MusicStreamFailed, "Supabase music stopped, using local fallback");
      startFallbackAudio();
    }
  }

  writeFallbackAudio();

  if (stationState == StationState::Ringing || stationState == StationState::ValidatingActivity)
  {
    const uint32_t age = bracelet.lastSeenMs ? millis() - bracelet.lastSeenMs : UINT32_MAX;
    if (REQUIRE_BRACELET_READY && age > RINGING_BRACELET_TIMEOUT_MS)
    {
      stopAlarmAudio();
      setStationState(StationState::Fault, ProblemCode::BraceletMissing, "Bracelet lost during alarm validation");
      return;
    }

    if (bracelet.validated || bracelet.state == BraceletState::Validated)
    {
      stopAlarmAudio();
      setStationState(StationState::Stopped, ProblemCode::None, "Bracelet validated sustained activity");
      publishStationStatus();
      return;
    }

    if (age <= RINGING_BRACELET_TIMEOUT_MS && stationState == StationState::Ringing)
    {
      setStationState(StationState::ValidatingActivity);
    }
    return;
  }

  if (stationState != StationState::Armed || !alarmConfig.valid || !alarmConfig.enabled)
  {
    return;
  }

  if (!timeReady)
  {
    setStationState(StationState::Fault, ProblemCode::TimeUnknown, "Time is not synchronized");
    return;
  }

  if (!braceletIsReadyForAlarm())
  {
    return;
  }

  const time_t now = time(nullptr);
  if (now >= alarmConfig.alarmAt)
  {
    if (startAlarmAudio())
    {
      setStationState(StationState::Ringing);
      publishStationStatus();
    }
  }
}

void setup()
{
  Serial.begin(115200);
  delay(200);

  pinMode(PIN_XSMT, OUTPUT);
  digitalWrite(PIN_XSMT, HIGH);

  WiFi.mode(WIFI_STA);
  WiFi.macAddress(stationMac);
  esp_wifi_set_ps(WIFI_PS_NONE);

  audio.setPinout(PIN_BCK, PIN_LRCK, PIN_DIN);
  audio.setVolume(17);

  ensureWifi();
  setupEspNow();

  if (loadCachedAlarm(alarmConfig))
  {
    setStationState(alarmConfig.enabled ? StationState::Armed : StationState::Idle);
  }
  else
  {
    setStationState(StationState::Fault, ProblemCode::NoValidAlarmConfig, "Waiting for first valid Supabase alarm config");
  }
}

void loop()
{
  ensureWifi();
  syncTime();

  const uint32_t nowMs = millis();
  if (nowMs - lastConfigRefreshMs >= CONFIG_REFRESH_MS || lastConfigRefreshMs == 0)
  {
    lastConfigRefreshMs = nowMs;
    if (WiFi.status() == WL_CONNECTED && timeReady)
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

  if (nowMs - lastControlSendMs >= CONTROL_SEND_MS)
  {
    lastControlSendMs = nowMs;
    sendStationControl();
  }

  if (nowMs - lastStatusPublishMs >= STATUS_PUBLISH_MS || lastStatusPublishMs == 0)
  {
    lastStatusPublishMs = nowMs;
    publishStationStatus();
    publishBraceletStatus();
  }

  delay(2);
}
