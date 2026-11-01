# Home Hub (web_app) – developer manual

ESP32-C6 + Zephyr. Joins the home Wi-Fi, serves a dashboard on port 80,
reads a BME688, keeps a named list of every device on the LAN, collects
reports from client nodes and publishes everything over MQTT.

| Document | Content |
|---|---|
| this README | set up, build, flash, update, test, interfaces |
| [ARCHITECTURE.md](ARCHITECTURE.md) | firmware architecture: memory, threads, modules, timing |
| [BOOTLOADER.md](BOOTLOADER.md) | boot chain, application and bootloader updates, rollback |

## 1. Setup (WSL / Linux, once per machine)

From the workspace root:

1. [Zephyr SDK](https://github.com/zephyrproject-rtos/sdk-ng/releases) 0.17.3
   with the `riscv64-zephyr-elf` toolchain.
2. `make setup HOST_PYTHON=python3.10` – creates `.venv-wsl` (Zephyr needs
   Python ≥ 3.10), fetches Zephyr, esptool, imgtool, smpmgr and the
   Espressif Wi-Fi blobs. The Makefile uses the venv's tools without
   activating it.
3. `cp applications/web_app/wifi.conf.example applications/web_app/wifi.conf`
   and fill in SSID and password (git-ignored).
4. Signing key, once, then **back it up** (git-ignored; without it only USB
   updates work):
   `.venv-wsl/bin/imgtool keygen -k applications/web_app/keys/hub-signing-key.pem -t ecdsa-p256`
5. Board USB into WSL (admin PowerShell; `bind` only once):
   `usbipd bind --busid <id>`, then `wsl -u root -- modprobe vhci_hcd` and
   `usbipd attach --wsl --busid <id>` → `/dev/ttyUSB0`.

## 2. Build, flash, update

```bash
make build                          # MCUboot + signed app (sysbuild)
make flash PORT=/dev/ttyUSB0        # USB: MCUboot at 0x0 + app at 0x20000
make ota                            # Wi-Fi: application
make ota-bootloader                 # Wi-Fi: MCUboot itself
make recover PORT=/dev/ttyUSB0      # USB: application only, MCUboot untouched
```

`HUB_IP=…` overrides the hub address (default `192.168.178.172`).

- **Application:** bump `VERSION`, `make build`, `make ota`. Waits until
  the hub answers (up to 6 min), uploads, resets and returns once the hub
  runs the new image and has confirmed it.
- **Bootloader:** change MCUboot (`sysbuild/mcuboot.conf`, or a new
  Zephyr/MCUboot), `make build`, then **`make ota-bootloader` first** and
  `make ota` afterwards if the app changed too (an app upload overwrites
  the staged bootloader). It returns once the hub has installed and
  verified the new MCUboot and booted through it. Don't cut power during
  the ≤ 0.5 s write, see BOOTLOADER.md §3.
- `/api/status` shows `fw` (version), `fw_confirmed` and `bl` (result of
  the last bootloader update since boot: `none`, `installed <sha>`,
  `rejected`, `failed, retrying`).

## 3. Standalone operation

Once flashed, the hub needs only 5 V (any USB power supply) and Wi-Fi:
every later update of the application and the bootloader goes over Wi-Fi.
Keep a USB cable for the rare cases the hub can't recover by itself: a
power cut during a bootloader write, or a firmware that never reaches the
network twice in a row.

After power-up the hub is online in ~10 s, or under 2 min when the Wi-Fi
driver hangs after the power-on (the hub detects that and reboots itself);
`make ota` waits for that.

## 4. Testing

| Test | How | Expected |
|---|---|---|
| USB install | `make build && make flash PORT=/dev/ttyUSB0` | serial log `Online: http://<ip>/` |
| OTA | bump `PATCHLEVEL`, `make build && make ota` | `ota: done…`; dashboard shows the new version |
| Rollback | `make ota`, then press RESET within ~5 s of the hub restarting (before it is online) | serial `Swap type: revert`; previous version, confirmed |
| Unreachable hub | `make ota HUB_IP=192.168.178.250 OTA_WAIT_S=15` | one-line error after 15 s |
| Bootloader OTA | `make ota-bootloader` | `ota: done, the hub boots through the new bootloader`; `bl` = `installed …` |
| Bootloader verify | `.venv-wsl/bin/esptool --port /dev/ttyUSB0 read-flash 0x0 <size> bl.bin`, `cmp` with `build/mcuboot/zephyr/zephyr.bin` | identical |
| Bad bootloader | stage an image signed with another key | `bl` = `rejected`, 0x0 untouched |
| App-only USB | `make recover PORT=/dev/ttyUSB0` | app boots through the existing MCUboot |
| Power-on | unplug and replug the hub (or open `/dev/ttyUSB0`, which resets it) | online within 2 min, also when the Wi-Fi driver hangs (`Driver stuck connecting, rebooting`) |

Serial log: `make monitor PORT=/dev/ttyUSB0` (Ctrl+] quits). Opening the port
resets the board.

## 5. BME688 wiring (I2C0)

| BME688 breakout | ESP32-C6-DevKitC |
|---|---|
| VCC / VIN | 3V3 |
| GND | GND |
| SDA / SDI | GPIO6 |
| SCL / SCK | GPIO7 |
| SDO | GND → 0x76, 3V3 → 0x77 (both are in the overlay) |
| CS | 3V3 (selects I2C) |

3.3 V only. The sensor is detected at boot: after wiring it, press RESET.
Gas resistance isn't shown: Zephyr's `bosch,bme680` driver uses the
BME680's gas registers and formula, which are wrong for the BME688.

## 6. Interfaces

**Status LED** (onboard WS2812): blue = connecting, green = online,
purple = LAN scan running.

**HTTP (port 80)**

| Method | Path | Returns |
|---|---|---|
| GET | `/` | dashboard |
| GET | `/api/status` | firmware, BME688, chip temperature, CPU load, Wi-Fi, scan state |
| GET | `/api/devices` | LAN devices: name, IP, MAC, online, first/last seen |
| POST | `/api/scan` | starts a scan now (`{"started":false}` if one is running) |
| POST | `/api/node` | report from a client node |
| GET | `/api/nodes` | latest report of every node |

**Client node report** (POST `/api/node` every 10 s, raw UTF-8):

```json
{"id":"imx8mpevk","model":"NXP FRDM-IMX8MPLUS",
 "sensors":[{"name":"CPU temperature","value":46.0,"unit":"°C"}]}
```

Up to 4 nodes × 16 sensors (name ≤ 23 bytes, unit ≤ 7). A new node needs
no change on the hub or the website. Example client:
[NXP-imx8MPlus-applications](https://github.com/bbzorba/NXP-imx8MPlus-applications) (`home_hub_node`).

**MQTT** (`broker.hivemq.com:1883`, both retained; Kconfig `CONFIG_APP_MQTT_*`)

| Topic | Content |
|---|---|
| `bbzorba/home-hub/v1/state` | hub, BME688, all nodes (JSON), every 10 s |
| `bbzorba/home-hub/v1/status` | `online`; last will `offline` |

**mcumgr**: UDP 1337, groups OS and image (no authentication; LAN only).

## 7. Source files

| File | Job |
|---|---|
| `src/main.c` | start-up, 10 s sampler tick, status LED |
| `src/wifi.c` | Wi-Fi station, reconnect, offline watchdog |
| `src/sensors.c`, `src/bme688.c` | chip temperature, CPU load, BME688 |
| `src/lan_scan.c` | ARP sweep + reverse DNS |
| `src/nodes.c` | client node reports |
| `src/web_api.c` | HTTP server, JSON API |
| `src/mqtt_pub.c` | MQTT publisher |
| `src/ota.c` | confirms the running image |
| `src/bl_update.c` | installs a staged bootloader |
| `src/static_web_resources/index.html` | dashboard (gzipped into flash) |
| `sysbuild.conf`, `sysbuild/mcuboot.conf` | MCUboot build settings |
