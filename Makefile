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
COMPILE_DIR ?= applications/blink_LED

BOARD     ?= esp32c6_devkitc/esp32c6/hpcore
BUILD_DIR ?= $(COMPILE_DIR)/build
BAUD      ?= 115200
PORT      ?=
ZEPHYR_REQS := external/zephyr/scripts/requirements.txt

# Python from the workspace virtual environment (platform-detected)
ifeq ($(OS),Windows_NT)
    PYTHON := .venv/Scripts/python.exe
	PYTHON_CMD := .venv\Scripts\python.exe
    VENV_MARKER := .venv/Scripts/python.exe
else
    PYTHON := .venv/bin/python
	PYTHON_CMD := $(PYTHON)
    VENV_MARKER := .venv/bin/python
endif
DEPS_MARKER := .venv/.deps-ready

# Use a west launcher with git revision compatibility fallbacks
WEST := $(PYTHON_CMD) tools/west_compat.py

# ============================================================
.DEFAULT_GOAL := build
.PHONY: help setup build flash clean build-flash update debug monitor flashmonitor-auto _gen-debug-context

help:
	@echo Usage: make [setup, build, flash, clean, build-flash, update, debug, monitor, flashmonitor-auto] [COMPILE_DIR=...] [BOARD=...]
	@echo   setup       - Create virtual environment and install dependencies
	@echo   build       - Build the selected application
	@echo   flash       - Flash to ESP32C6 DevKitC board
	@echo   clean       - Remove build directory
	@echo   build-flash - Build then flash
	@echo   update      - Update Zephyr and dependencies
	@echo   debug       - Build then open VS Code debug session (press F5)
	@echo   monitor     - Open serial monitor
	@echo   flashmonitor-auto - Build, flash, then open serial monitor

	@echo COMPILE_DIR=$(COMPILE_DIR)
	@echo BOARD=$(BOARD)

# Bootstrap: create venv, install west, init workspace, fetch zephyr, install deps
$(VENV_MARKER):
	python -m venv .venv
	$(PYTHON_CMD) -m pip install --upgrade pip

$(DEPS_MARKER): $(VENV_MARKER)
	$(PYTHON_CMD) -m pip install --upgrade pip
	$(PYTHON_CMD) -m pip install --upgrade west
	$(PYTHON_CMD) -m pip install -r $(ZEPHYR_REQS)
	$(PYTHON_CMD) -c "from pathlib import Path; Path('$(DEPS_MARKER)').touch()"

setup: $(DEPS_MARKER)
	-$(WEST) init -l manifest-local
	$(WEST) update --fetch always
	$(PYTHON_CMD) -m pip install -r $(ZEPHYR_REQS)
	$(PYTHON_CMD) -c "from pathlib import Path; Path('$(DEPS_MARKER)').touch()"

build: $(DEPS_MARKER)
	$(WEST) build -b $(BOARD) $(COMPILE_DIR) -d $(BUILD_DIR) --pristine=auto

flash:
	$(WEST) flash -d $(BUILD_DIR)

clean:
ifeq ($(OS),Windows_NT)
	powershell -NoProfile -ExecutionPolicy Bypass -Command "if (Test-Path '$(subst /,\,$(BUILD_DIR))') { Remove-Item -Recurse -Force '$(subst /,\,$(BUILD_DIR))' }"
else
	rm -rf "$(BUILD_DIR)"
endif
	@echo Cleaned: $(BUILD_DIR)

update:
	$(PYTHON_CMD) -m pip install --upgrade pip
	$(PYTHON_CMD) -m pip install --upgrade west
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