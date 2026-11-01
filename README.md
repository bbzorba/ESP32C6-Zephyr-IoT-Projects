# ESP32C6-Zephyr-IoT-Projects

Zephyr RTOS applications for the ESP32-C6-DevKitC, built with west from
WSL/Linux (`make build`, `make flash PORT=/dev/ttyUSB0`, see the
`Makefile`). The Linux venv lives in `.venv-wsl`; set it up with
`make setup HOST_PYTHON=python3.10`.

| Application | What it is |
|---|---|
| [web_app](applications/web_app) | **Home Hub**: always-on Wi-Fi sensor hub with a local dashboard, LAN device discovery, BME688 sensing, client nodes and MQTT publishing to the [Live Lab page](https://bbzorba.github.io/live). Start with its [architecture overview](applications/web_app/ARCHITECTURE.md), then the [README](applications/web_app/README.md). |
| [blink_LED](applications/blink_LED) | Onboard WS2812 RGB LED blink |
| [threaded_blink_LEDs](applications/threaded_blink_LEDs) | Several LEDs blinking from separate threads |
