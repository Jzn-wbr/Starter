# Bracelet / station WiFi UDP setup

The station and bracelet must use protocol v3 firmware together and must join
the same non-guest WiFi network. The router must allow broadcast UDP and direct
traffic between clients.

## Configure and flash

1. Keep the station credentials in `boite hp/include/secrets.h`.
2. Copy `bracelet/include/secrets.example.h` to
   `bracelet/include/secrets.h` and enter the same primary and optional fallback
   networks, in the same order. Both real secret files are ignored by Git.
3. Build and upload both firmwares.
4. Boot the station and bracelet on the same LAN. The station broadcasts a
   protocol v3 control on UDP port `42100`; the first valid exchange stores the
   two peer MAC identifiers in NVS.

Expected logs include `udp_status:ready`, `udp_pairing:station_saved` on the
bracelet, and `udp_pairing:bracelet_saved` on the station. Later boots should
log the corresponding `*_loaded` messages.

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
- The armed missing-bracelet timeout is 25 seconds. The active alarm timeout
  remains 3 seconds and keeps the alarm behavior conservative.
- There is no direct AP or ESP-NOW fallback. If the router is unavailable, the
  normal fault and audible-alarm policies apply.
