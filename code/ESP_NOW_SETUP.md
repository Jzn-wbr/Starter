# Bracelet / station ESP-NOW setup

The station and bracelet must use protocol v5 firmware together. The station
still joins the configured home WiFi for NTP, Supabase, and music streaming;
the bracelet communicates only through ESP-NOW and needs no router credentials.

## Configure and flash

1. Keep the station WiFi and Supabase credentials in
   `boite hp/include/secrets.h`.
2. Build and upload both firmwares.
3. Boot the station first so it can start joining the home WiFi.
4. Boot the bracelet. It scans channels 1 through 13 until it receives a valid
   protocol v5 station control, then locks to that channel.
5. The first valid exchange stores the two peer MAC identifiers in NVS.

Expected logs include `espnow_status:ready`,
`espnow_pairing:station_saved` on the bracelet, and
`espnow_pairing:bracelet_saved` on the station. Later boots should log the
corresponding `*_loaded` messages.

## Replace a paired device

Pairing intentionally has no automatic takeover. Erase NVS/flash on both
devices before pairing a replacement, then upload both current firmwares:

```powershell
pio run -t erase
pio run -t upload
```

Run each command from the appropriate `bracelet` or `boite hp` directory.
Erasing the station also removes its cached alarm and completed-revision state;
it reloads the current configuration from Supabase after reconnecting.

## Runtime expectations

- Bracelet status cadence is 10 seconds outside an alarm and 200 ms during an
  active alarm. Movement during the alarm, vibration acknowledgement, first
  contact, faults, and state changes send immediately.
- Station control cadence is 5 seconds outside an alarm and 700 ms during an
  active alarm.
- If no valid control arrives for 15 seconds, the bracelet resumes channel
  scanning. This also recovers automatically if the router moves the station
  to a different 2.4 GHz channel.
- The armed missing-bracelet timeout is 25 seconds. The active alarm timeout
  remains 3 seconds and keeps the alarm behavior conservative.
- The bracelet RSSI compatibility field is reported as unknown because the
  bracelet is not associated with the home WiFi.
