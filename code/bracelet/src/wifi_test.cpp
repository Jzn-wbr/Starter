#include <Arduino.h>
#include <WiFi.h>

namespace
{
constexpr uint32_t SCAN_PERIOD_MS = 5000;
constexpr uint32_t SERIAL_BAUD = 115200;

uint32_t scanNumber = 0;

const char *authName(wifi_auth_mode_t mode)
{
    switch (mode)
    {
    case WIFI_AUTH_OPEN:
        return "open";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3";
    default:
        return "other";
    }
}

void scanWifi()
{
    ++scanNumber;
    Serial.printf("\n=== wifi_scan:%lu ===\n", static_cast<unsigned long>(scanNumber));

    const int networkCount = WiFi.scanNetworks(false, true);
    if (networkCount < 0)
    {
        Serial.printf("scan_error:%d\n", networkCount);
        return;
    }

    Serial.printf("visible_networks:%d\n", networkCount);

    for (int index = 0; index < networkCount; ++index)
    {
        const String ssid = WiFi.SSID(index);
        const int32_t rssi = WiFi.RSSI(index);
        const int32_t channel = WiFi.channel(index);

        Serial.printf("%02d | rssi=%4ld dBm | channel=%2ld | auth=%-9s | %s\n",
                      index + 1,
                      static_cast<long>(rssi),
                      static_cast<long>(channel),
                      authName(WiFi.encryptionType(index)),
                      ssid.c_str());
    }

    WiFi.scanDelete();
}
}

void setup()
{
    Serial.begin(SERIAL_BAUD);
    delay(1000);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    Serial.println("bracelet_wifi_antenna_test");
    Serial.println("scan_only:no_wifi_connection");
    Serial.println("Keep the bracelet, router, USB cable and surrounding objects in the same position for every antenna test.");
}

void loop()
{
    scanWifi();
    delay(SCAN_PERIOD_MS);
}
