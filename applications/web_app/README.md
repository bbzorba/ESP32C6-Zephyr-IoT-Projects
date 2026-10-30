# Home Hub (web_app)

ESP32-C6 + Zephyr. Joins the home Wi-Fi, serves a dashboard on port 80 and
keeps a list of every device on the LAN.

## Setup

1. Copy `wifi.conf.example` to `wifi.conf` and fill in SSID and password
   (`wifi.conf` is git-ignored).
2. Build and flash (from the workspace root):

   ```
   .venv\Scripts\python.exe tools\west_compat.py build -b esp32c6_devkitc/esp32c6/hpcore applications/web_app -d applications/web_app/build --pristine=auto
   .venv\Scripts\python.exe tools\west_compat.py flash -d applications/web_app/build --esp-device COM8
   ```

   or `make build-flash` (the Makefile now points at this app).
3. The serial log prints `Online: http://<ip>/`. Open that address.

Wi-Fi needs Espressif's binary blobs once per workspace:
`west blobs fetch hal_espressif`.

## BME688 wiring (I2C0)

| BME688 breakout | ESP32-C6-DevKitC |
|---|---|
| VCC / VIN | 3V3 |
| GND | GND |
| SDA / SDI | GPIO6 |
| SCL / SCK | GPIO7 |
| SDO | GND for address 0x76, 3V3 for 0x77 (or leave as the breakout sets it) |
| CS | 3V3 (selects I2C; most breakouts already pull it up) |

Use 3.3 V, never 5 V on a bare BME688. Both addresses are in the overlay, so
either works. The sensor is detected at boot: after wiring it, press RESET.
Readings refresh every 10 s. The gas resistance needs about 30 minutes of
running before it is stable.

## Status LED (onboard RGB)

| Colour | Meaning |
|---|---|
| red | `wifi.conf` missing |
| blue | connecting to Wi-Fi |
| green | online |
| purple | LAN scan running |

## HTTP API

| Method | Path | Returns |
|---|---|---|
| GET | `/` | dashboard |
| GET | `/api/status` | BME688 (`env`), chip temperature, uptime, Wi-Fi (SSID, RSSI, IP, MAC), scan state |
| GET | `/api/devices` | LAN devices: IP, MAC, online, first/last seen (hub uptime seconds) |
| POST | `/api/scan` | starts a scan now, `{"started":false}` if one is running |

## How the LAN scan works

Every host of the hub's /24 gets one 1-byte UDP datagram to port 9. That
forces an ARP request, and every device answers ARP even if it ignores ping.
After each batch of 16 hosts the ARP table is read back. A full /24 takes
about 35 s and runs every `CONFIG_APP_SCAN_INTERVAL_MIN` (10) minutes.
Devices that stop answering stay in the list as offline.

Phones in deep sleep can miss a scan and show offline for one round.

## Files

- `src/main.c` - start-up, status LED
- `src/wifi.c` - Wi-Fi station, DHCP, reconnect
- `src/lan_scan.c` - ARP sweep and device list
- `src/sensors.c` - on-die temperature
- `src/bme688.c` - BME688 temperature, humidity, pressure, gas (I2C0)
- `src/web_api.c` - HTTP server, JSON API
- `src/static_web_resources/index.html` - dashboard (gzipped into flash at build time)
