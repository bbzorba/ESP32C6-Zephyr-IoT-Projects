# ============================================================
# STM32F4 Zephyr Workspace - Makefile
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
COMPILE_DIR ?= applications/BME688

BOARD     ?= esp32c6_devkitc/esp32c6/hpcore
BUILD_DIR ?= $(COMPILE_DIR)/build
BAUD      ?= 115200
PORT      ?=

# Python from the workspace virtual environment (platform-detected)
ifeq ($(OS),Windows_NT)
    PYTHON := .venv/Scripts/python.exe
    VENV_MARKER := .venv/Scripts/python.exe
else
    PYTHON := .venv/bin/python
    VENV_MARKER := .venv/bin/python
endif

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
	$(PYTHON) -m pip install --upgrade pip
	$(PYTHON) -m pip install west
	-$(PYTHON) -m west init -l manifest-local
	$(PYTHON) -m west update
	$(PYTHON) -m pip install -r requirements.txt

setup: $(VENV_MARKER)

build: $(VENV_MARKER)
	$(PYTHON) -m west build -b $(BOARD) $(COMPILE_DIR) -d $(BUILD_DIR) --pristine=auto

flash:
	$(PYTHON) -m west flash -d $(BUILD_DIR)

clean:
ifeq ($(OS),Windows_NT)
	-@rmdir /s /q "$(subst /,\,$(BUILD_DIR))" 2>nul
else
	rm -rf "$(BUILD_DIR)"
endif
	@echo Cleaned: $(BUILD_DIR)

update:
	$(PYTHON) -m pip install --upgrade pip
	$(PYTHON) -m pip install --upgrade west
	$(PYTHON) -m west update

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
	$(PYTHON) .vscode/gen_debug_context.py