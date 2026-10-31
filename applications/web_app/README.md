# Home Hub (web_app)

ESP32-C6 + Zephyr. Joins the home Wi-Fi, serves a dashboard on port 80,
reads a BME688 and keeps a named list of every device on the LAN.

## Setup

1. Copy `wifi.conf.example` to `wifi.conf` and fill in SSID and password
   (`wifi.conf` is git-ignored). The build fails with a clear message if the
   SSID is missing.
2. Build and flash (from the workspace root):

   ```
   .venv\Scripts\python.exe tools\west_compat.py build -b esp32c6_devkitc/esp32c6/hpcore applications/web_app -d applications/web_app/build --pristine=auto
   .venv\Scripts\python.exe tools\west_compat.py flash -d applications/web_app/build --esp-device COM8
   ```

   or `make build-flash` (the Makefile points at this app).
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

**Gas resistance is not shown.** Zephyr's `bosch,bme680` driver reads
temperature, humidity and pressure correctly on the BME688, but its gas
value is wrong there: it uses the BME680's run_gas bit, gas registers
(0x2A/0x2B) and formula, while the BME688 needs 0x20, 0x2C/0x2D and a
different formula.

## Timing

One timer in `main.c` wakes the CPU every 10 s. On each tick the chip
temperature and the BME688 are read together. Every 60th tick (10 min),
the same tick also starts a LAN scan. "Scan now" runs an extra scan without
moving that schedule.

## Status LED (onboard RGB)

| Colour | Meaning |
|---|---|
| blue | connecting to Wi-Fi |
| green | online |
| purple | LAN scan running |

If Wi-Fi stays down for 5 minutes the hub reboots (see `src/wifi.c`).

## HTTP API

| Method | Path | Returns |
|---|---|---|
| GET | `/` | dashboard |
| GET | `/api/status` | BME688 (`env`), chip temperature, CPU load, Wi-Fi (SSID, RSSI, IP, MAC), scan state |
| GET | `/api/devices` | LAN devices: name, IP, MAC, online, first/last seen (hub uptime seconds) |
| POST | `/api/scan` | starts a scan now, `{"started":false}` if one is running |
| POST | `/api/node` | sensor report from a client node (below) |
| GET | `/api/nodes` | latest report of every node, with `online` |

## Client nodes

Other boards report to the hub by POSTing to `/api/node` every 10 s:

```json
{"id":"imx8mpevk","model":"NXP FRDM-IMX8MPLUS",
 "sensors":[{"name":"CPU temperature","value":46.0,"unit":"°C"}]}
```

Up to 4 nodes and 16 sensors per node (name ≤ 23 bytes, unit ≤ 7 bytes).
Send raw UTF-8: Zephyr's JSON parser keeps `\uXXXX` escapes literally. A
node shows as offline after 30 s without a report. Each node gets its own
card on the dashboard. The i.MX 8M Plus client lives in
[NXP-imx8MPlus-applications](https://github.com/bbzorba/NXP-imx8MPlus-applications)
(`home_hub_node`).

## Live page on bbzorba.github.io (MQTT)

On every 10 s tick the hub also publishes to an MQTT broker
(`src/mqtt_pub.c`); https://bbzorba.github.io/live subscribes over
WebSocket from the visitor's browser.

| Topic | Content |
|---|---|
| `bbzorba/home-hub/v1/state` | JSON: hub (chip temperature, CPU load, uptime, RSSI), BME688, all client nodes |
| `bbzorba/home-hub/v1/status` | `online`; the broker sets `offline` (last will) ~90 s after the hub drops off |

Both are retained, so a new visitor sees the latest values at once. Broker,
port and topic prefix are Kconfig options (`CONFIG_APP_MQTT_*`).

Only sensor values are published; the LAN device list, addresses and Wi-Fi
name stay on the local page. The default broker (`broker.hivemq.com`) is
public: anyone who knows the topic can read it and could publish fake
values. For that, move to a private broker (e.g. HiveMQ Cloud free tier)
with a write password for the hub and a read-only user for the page.

## How the LAN scan works

Every host of the hub's /24 gets one 1-byte UDP datagram to port 9. That
forces an ARP request, and every device answers ARP even if it ignores ping.
After each batch of 16 hosts the ARP table is read back. A full /24 takes
about 35 s. Devices that stop answering stay in the list as offline.

Names come from a reverse-DNS (PTR) query to the router, which knows the
hostname each device sent when it asked for its address. Devices that send
no hostname show as "unknown".

Phones in deep sleep can miss a scan and show offline for one round.

## Files

- `src/main.c` - start-up, 10 s sampler tick, status LED
- `src/wifi.c` - Wi-Fi station (wifi_credentials + conn_mgr), recovery
- `src/lan_scan.c` - ARP sweep, reverse-DNS names, device list
- `src/sensors.c` - caches all readings; chip temperature
- `src/bme688.c` - BME688 temperature, humidity, pressure (I2C0)
- `src/nodes.c` - client node reports
- `src/mqtt_pub.c` - publishes readings for the live web page
- `src/web_api.c` - HTTP server, JSON API (Zephyr JSON library)
- `src/static_web_resources/index.html` - dashboard (gzipped into flash at build time)
