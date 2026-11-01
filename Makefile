# ============================================================
# ESP32C6 Zephyr Workspace - Makefile
# Platform-independent: Windows, Linux, macOS
#
# Select the application to build below, then:
#   make              -> build
#   make build
#   make flash
#   make build-flash
#   make clean
# ============================================================

# --- Select application to build (uncomment one) ---
#COMPILE_DIR ?= applications/BME688
#COMPILE_DIR ?= applications/LED_Blink
#COMPILE_DIR ?= applications/MQTT_Publisher
#COMPILE_DIR ?= applications/HTTP_Server
#COMPILE_DIR ?= applications/HTTPS_Server
#COMPILE_DIR ?= applications/HTTP_Client
#COMPILE_DIR ?= applications/blink_LED
#COMPILE_DIR ?= applications/threaded_blink_LEDs
COMPILE_DIR ?= applications/web_app

BOARD     ?= esp32c6_devkitc/esp32c6/hpcore
#BOARD     ?= esp32_devkitc/esp32/procpu
BUILD_DIR ?= $(COMPILE_DIR)/build
BAUD      ?= 115200
PORT      ?=
ZEPHYR_REQS := external/zephyr/scripts/requirements.txt

# Python from the workspace virtual environment (platform-detected)
ifeq ($(OS),Windows_NT)
    PYTHON := .venv/Scripts/python.exe
	PYTHON_CMD := .venv\Scripts\python.exe
    VENV_MARKER := .venv/Scripts/python.exe
    VENV := .venv
else
    # Linux / WSL: separate venv so it can live next to the Windows one
    VENV := .venv-wsl
    PYTHON := $(VENV)/bin/python
	PYTHON_CMD := $(PYTHON)
    VENV_MARKER := $(VENV)/bin/python
    # Tools that west and the recipes call by name (esptool, imgtool,
    # smpmgr) must come from the venv, not from the system: Ubuntu's
    # /usr/bin/esptool is 2.x and doesn't know the ESP32-C6
    export PATH := $(CURDIR)/$(VENV)/bin:$(PATH)
endif
DEPS_MARKER := $(VENV)/.deps-ready

# Apps with a sysbuild.conf (e.g. web_app) are built with sysbuild, which
# adds MCUboot and signs the application image.
SYSBUILD := $(if $(wildcard $(COMPILE_DIR)/sysbuild.conf),--sysbuild,--no-sysbuild)

# OTA / recovery (applications with MCUboot, see web_app/BOOTLOADER.md)
HUB_IP    ?= 192.168.178.172
SMPMGR    := $(dir $(PYTHON))smpmgr
APP_NAME  := $(notdir $(COMPILE_DIR))
SIGNED    := $(BUILD_DIR)/$(APP_NAME)/zephyr/zephyr.signed.bin

# Use a west launcher with git revision compatibility fallbacks
WEST := $(PYTHON_CMD) tools/west_compat.py

# ============================================================
.DEFAULT_GOAL := build
.PHONY: help setup build flash ota recover clean build-flash update debug monitor flashmonitor-auto _gen-debug-context

help:
	@echo Usage: make [setup, build, flash, clean, build-flash, update, debug, monitor, flashmonitor-auto] [COMPILE_DIR=...] [BOARD=...]
	@echo   setup       - Create virtual environment and install dependencies
	@echo   build       - Build the selected application
	@echo   flash       - Flash to ESP32C6 DevKitC board (USB, esptool)
	@echo   ota         - Update a running MCUboot app over Wi-Fi [HUB_IP=...]
	@echo   recover     - Rewrite only the app over USB (ROM bootloader) [PORT=...]
	@echo   clean       - Remove build directory
	@echo   build-flash - Build then flash
	@echo   update      - Update Zephyr and dependencies
	@echo   debug       - Build then open VS Code debug session (press F5)
	@echo   monitor     - Open serial monitor
	@echo   flashmonitor-auto - Build, flash, then open serial monitor

	@echo COMPILE_DIR=$(COMPILE_DIR)
	@echo BOARD=$(BOARD)

# Bootstrap: create venv, install west, init workspace, fetch zephyr, install deps
# Interpreter used to create the venv; Zephyr needs Python >= 3.10
# (on Ubuntu 22.04 WSL: make setup HOST_PYTHON=python3.10)
HOST_PYTHON ?= python

$(VENV_MARKER):
	$(HOST_PYTHON) -m venv $(VENV)
	$(PYTHON_CMD) -m pip install --upgrade pip

$(DEPS_MARKER): $(VENV_MARKER)
	$(PYTHON_CMD) -m pip install --upgrade pip
	$(PYTHON_CMD) -m pip install --upgrade west smpmgr
	$(PYTHON_CMD) -m pip install -r $(ZEPHYR_REQS)
	$(PYTHON_CMD) -c "from pathlib import Path; Path('$(DEPS_MARKER)').touch()"

setup: $(DEPS_MARKER)
	-$(WEST) init -l manifest-local
	$(WEST) update --fetch always
	$(PYTHON_CMD) -m pip install -r $(ZEPHYR_REQS)
	# Python packages the modules need (e.g. esptool for Espressif), plus imgtool
	$(WEST) packages pip --install
	$(PYTHON_CMD) -m pip install imgtool
	$(WEST) blobs fetch hal_espressif
	$(PYTHON_CMD) -c "from pathlib import Path; Path('$(DEPS_MARKER)').touch()"

build: $(DEPS_MARKER)
	$(WEST) build -b $(BOARD) $(COMPILE_DIR) -d $(BUILD_DIR) --pristine=auto $(SYSBUILD)

flash:
	$(WEST) flash -d $(BUILD_DIR) $(if $(PORT),--esp-device $(PORT),)

# Over the air: wait for the hub, upload the signed image to slot 1, mark it
# for a trial boot (by its hash) and reset. MCUboot swaps it in; the new
# firmware confirms itself once online, otherwise MCUboot rolls back.
# tools/ota.sh waits until the hub runs and has confirmed the new image.
ota:
	sh tools/ota.sh $(SMPMGR) $(dir $(PYTHON))imgtool $(HUB_IP) $(SIGNED)

# Over USB: write only the signed application into slot 0 (0x20000) through
# the ESP32-C6 ROM bootloader; MCUboot and its settings stay untouched.
SLOT0_ADDR ?= 0x20000
recover:
	$(dir $(PYTHON))esptool --port $(PORT) write-flash $(SLOT0_ADDR) $(SIGNED)

clean:
ifeq ($(OS),Windows_NT)
	powershell -NoProfile -ExecutionPolicy Bypass -Command "if (Test-Path '$(subst /,\,$(BUILD_DIR))') { Remove-Item -Recurse -Force '$(subst /,\,$(BUILD_DIR))' }"
else
	rm -rf "$(BUILD_DIR)"
endif
	@echo Cleaned: $(BUILD_DIR)

update:
	$(PYTHON_CMD) -m pip install --upgrade pip
	$(PYTHON_CMD) -m pip install --upgrade west smpmgr
	$(WEST) update --fetch always
	$(PYTHON_CMD) -m pip install -r $(ZEPHYR_REQS)
	$(PYTHON_CMD) -c "from pathlib import Path; Path('$(DEPS_MARKER)').touch()"

debug: clean build
	@echo ""
	@echo ">>> Build ready. Press F5 in VS Code to start the debug session."

monitor:
ifeq ($(OS),Windows_NT)
	powershell -NoProfile -ExecutionPolicy Bypass -File "tools/monitor.ps1" $(if $(PORT),-ComPort $(PORT),) -Baud $(BAUD) $(if $(MONITOR_SECONDS),-DurationSec $(MONITOR_SECONDS),)
else
	@echo ">>> Opening serial monitor..."
	$(PYTHON) -m minicom -D /dev/ttyUSB0 -b 115200
endif

build-flash: build flash

flashmonitor-auto: build flash monitor

_gen-debug-context:
	$(PYTHON_CMD) .vscode/gen_debug_context.py
