#include <Arduino.h>
#include <WiFi.h>
#include "Audio.h"
#include "secrets.h"

static const int PIN_DIN = 22;
static const int PIN_LRCK = 25;
static const int PIN_BCK = 26;
static const int PIN_XSMT = 27;

const double VOLUME = 0.1; // 0.0..1.0

Audio audio;

static void connectWifi()
{
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("WiFi");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

void setup()
{
  Serial.begin(115200);

  pinMode(PIN_XSMT, OUTPUT);
  digitalWrite(PIN_XSMT, HIGH);

  connectWifi();

  audio.setPinout(PIN_BCK, PIN_LRCK, PIN_DIN);
  audio.setConnectionTimeout(1000, 5000);
  audio.forceMono(true);
  audio.setVolume(VOLUME * 21); // 0..21
  audio.connecttohost(STREAM_URL);
}

void loop()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    connectWifi();
    audio.connecttohost(STREAM_URL);
  }

  audio.loop();
}

void audio_info(const char *info)
{
  Serial.print("audio: ");
  Serial.println(info);
}
