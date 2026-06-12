# ============================================================
# peripheral_uart — top-level build Makefile
# ============================================================
# Usage:
#   make              — build + package OTA + sync versions
#   make flash        — flash via west
#   make clean        — remove build/
#   make pristine     — clean + build
#   make deploy       — build + rsync to pwa.price-tag.sorewa.ru
#
# Version: two independent version tracks:
#   ./VERSION              → firmware (Zephyr app_version.h, manifest.json, SYSINFO)
#   ./lut_tester_host/web/WEB_VERSION → web app (sw.js cache, version.json)
#
#   DFU banner shows ONLY when firmware version on server > firmware on device.
#   Web app updates via service worker silently (no DFU needed).
#
#   Override board or toolchain at the command line:
#     make BOARD=nrf52840dk/nrf52840
#     make NCS_DIR=/opt/nordic/ncs/v2.6.0
# ============================================================

APP_DIR      := $(CURDIR)
ROOT_DIR     := $(CURDIR)
BUILD_DIR    := $(ROOT_DIR)/build

BOARD         ?= nrf52dk/nrf52832
NCS_DIR       ?= /opt/nordic/ncs/v3.0.1
TOOLCHAIN_DIR ?= /opt/nordic/ncs/toolchains/ef4fc6722e

LOCAL_HOME    := $(ROOT_DIR)/.home
WEST          := $(TOOLCHAIN_DIR)/bin/west

# ----------------------------------------------------------
# Firmware version — parsed from VERSION file
# ----------------------------------------------------------
FW_VERSION_MAJOR := $(shell grep 'VERSION_MAJOR' VERSION | awk '{print $$3}')
FW_VERSION_MINOR := $(shell grep 'VERSION_MINOR' VERSION | awk '{print $$3}')
FW_PATCHLEVEL    := $(shell grep 'PATCHLEVEL' VERSION | awk '{print $$3}')
FW_VERSION       := $(FW_VERSION_MAJOR).$(FW_VERSION_MINOR).$(FW_PATCHLEVEL)

# Web app version — independent from firmware
WEB_VERSION      := $(shell cat lut_tester_host/web/WEB_VERSION 2>/dev/null | tr -d '[:space:]')

# ----------------------------------------------------------
# OTA firmware paths
# ----------------------------------------------------------
SIGNED_BIN   := $(BUILD_DIR)/peripheral_uart/zephyr/zephyr.signed.bin
FW_DIR       := $(ROOT_DIR)/lut_tester_host/web/firmware
FW_BIN       := $(FW_DIR)/app_update.bin
FW_MANIFEST  := $(FW_DIR)/manifest.json

# ----------------------------------------------------------
# Deploy target (rsync to web server)
# ----------------------------------------------------------
DEPLOY_HOST  ?= pwa.price-tag.sorewa.ru
DEPLOY_PATH  ?= /var/www/pwa.price-tag.sorewa.ru
DEPLOY_USER  ?= $(USER)
WEB_DIR      := $(ROOT_DIR)/lut_tester_host/web

# ----------------------------------------------------------
# Build timestamp — captured once when make is invoked.
# expr strips leading zeros so the values are plain decimals
# (avoids octal literal issues in C: 08 / 09 are invalid).
# ----------------------------------------------------------
BUILD_YEAR  := $(shell date "+%Y")
BUILD_MONTH := $(shell expr `date "+%m"` + 0)
BUILD_DAY   := $(shell expr `date "+%d"` + 0)
BUILD_HOUR  := $(shell expr `date "+%H"` + 0)
BUILD_MIN   := $(shell expr `date "+%M"` + 0)
BUILD_SEC   := $(shell expr `date "+%S"` + 0)

export HOME                     := $(LOCAL_HOME)
export ZEPHYR_BASE              := $(NCS_DIR)/zephyr
export ZEPHYR_TOOLCHAIN_VARIANT := zephyr
export ZEPHYR_SDK_INSTALL_DIR   := $(TOOLCHAIN_DIR)/opt/zephyr-sdk
export PATH := $(TOOLCHAIN_DIR)/bin:$(TOOLCHAIN_DIR)/usr/bin:$(TOOLCHAIN_DIR)/usr/local/bin:$(TOOLCHAIN_DIR)/opt/bin:$(TOOLCHAIN_DIR)/opt/nanopb/generator-bin:$(TOOLCHAIN_DIR)/opt/zephyr-sdk/arm-zephyr-eabi/bin:$(PATH)

# CMake extra args — time injected into every build unit via zephyr_compile_definitions.
CMAKE_EXTRA := \
	-DAPP_BUILD_YEAR=$(BUILD_YEAR)   \
	-DAPP_BUILD_MONTH=$(BUILD_MONTH) \
	-DAPP_BUILD_DAY=$(BUILD_DAY)     \
	-DAPP_BUILD_HOUR=$(BUILD_HOUR)   \
	-DAPP_BUILD_MIN=$(BUILD_MIN)     \
	-DAPP_BUILD_SEC=$(BUILD_SEC)

.PHONY: build flash clean pristine publish deploy web

build:
	@echo ">>> Build timestamp: $(BUILD_YEAR)-$(BUILD_MONTH)-$(BUILD_DAY) $(BUILD_HOUR):$(BUILD_MIN):$(BUILD_SEC)"
	@mkdir -p "$(LOCAL_HOME)/Library/Caches/zephyr/ToolchainCapabilityDatabase"
	"$(WEST)" build -p always -b "$(BOARD)" "$(APP_DIR)" -d "$(BUILD_DIR)" \
		-- $(CMAKE_EXTRA)
	@echo ">>> HEX ready: $(BUILD_DIR)/merged.hex"
	@# --- Auto-publish firmware for OTA ---
	@mkdir -p "$(FW_DIR)/history"
	@if [ -f "$(FW_BIN)" ]; then \
		PREV_SHA=$$(shasum -a 256 "$(FW_BIN)" | cut -d' ' -f1); \
		NEW_SHA=$$(shasum -a 256 "$(SIGNED_BIN)" | cut -d' ' -f1); \
		if [ "$$PREV_SHA" != "$$NEW_SHA" ] && [ -f "$(FW_MANIFEST)" ]; then \
			PREV_VER=$$(python3 -c "import json;print(json.load(open('$(FW_MANIFEST)'))['version'])" 2>/dev/null || echo "unknown"); \
			PREV_DATE=$$(python3 -c "import json;print(json.load(open('$(FW_MANIFEST)'))['date'])" 2>/dev/null || echo "unknown"); \
			cp "$(FW_BIN)" "$(FW_DIR)/history/app_update_v$${PREV_VER}_$${PREV_DATE}.bin"; \
			echo ">>> Archived previous: v$$PREV_VER ($$PREV_DATE)"; \
		fi; \
	fi
	@cp "$(SIGNED_BIN)" "$(FW_BIN)"
	@FW_SIZE=$$(stat -f%z "$(FW_BIN)" 2>/dev/null || stat -c%s "$(FW_BIN)"); \
	FW_SHA=$$(shasum -a 256 "$(FW_BIN)" | cut -d' ' -f1); \
	FW_DATE=$$(date "+%Y-%m-%d"); \
	printf '{\n  "version": "$(FW_VERSION)",\n  "file": "app_update.bin",\n  "size": %s,\n  "sha256": "%s",\n  "notes": "",\n  "date": "%s",\n  "min_version": "1.0.0"\n}\n' \
		"$$FW_SIZE" "$$FW_SHA" "$$FW_DATE" > "$(FW_MANIFEST)"
	@# Keep only last 10 history files
	@cd "$(FW_DIR)/history" && ls -t *.bin 2>/dev/null | tail -n +11 | xargs rm -f 2>/dev/null; true
	@echo ">>> OTA firmware: v$(FW_VERSION) → $(FW_BIN) ($$(du -h "$(FW_BIN)" | cut -f1))"
	@# --- Sync version to web assets (sw.js cache name, version.json) ---
	@sed -i '' "s/const CACHE = 'eink-v[^']*'/const CACHE = 'eink-v$(WEB_VERSION)'/" "$(WEB_DIR)/sw.js"
	@printf '{"fw":"%s","web":"%s","build":"%s-%s-%s %s:%s:%s"}\n' \
		"$(FW_VERSION)" "$(WEB_VERSION)" \
		"$(BUILD_YEAR)" "$(BUILD_MONTH)" "$(BUILD_DAY)" \
		"$(BUILD_HOUR)" "$(BUILD_MIN)" "$(BUILD_SEC)" > "$(WEB_DIR)/version.json"
	@echo ">>> Version synced: fw=v$(FW_VERSION), web=v$(WEB_VERSION)"

flash:
	"$(WEST)" flash -d "$(BUILD_DIR)"

# ----------------------------------------------------------
# flash-gdb — flash merged.hex via OpenOCD/GDB (telnet localhost 4444)
# Use when nrfutil is not available / board connected via J-Link OpenOCD.
# ----------------------------------------------------------
flash-gdb:
	@echo ">>> Flashing merged.hex via OpenOCD (localhost:4444)…"
	@echo "reset halt\nprogram $(BUILD_DIR)/merged.hex verify\nreset run\nexit" | nc localhost 4444
	@echo ">>> Flash complete"

flash-openocd:
	@echo ">>> Flashing merged.hex via openocd CLI…"
	openocd -f interface/jlink.cfg -f target/nrf52.cfg \
		-c "program $(BUILD_DIR)/merged.hex verify reset exit"

clean:
	rm -rf "$(BUILD_DIR)"

pristine: clean build

# ----------------------------------------------------------
# web — update only web assets (no firmware rebuild)
# Use when you change only HTML/JS/CSS, not firmware.
# Bumps WEB_VERSION automatically (patch +1).
# ----------------------------------------------------------
web:
	@WV=$$(cat "$(WEB_DIR)/WEB_VERSION" | tr -d '[:space:]'); \
	MAJOR=$$(echo $$WV | cut -d. -f1); \
	MINOR=$$(echo $$WV | cut -d. -f2); \
	PATCH=$$(echo $$WV | cut -d. -f3); \
	NEW_PATCH=$$((PATCH + 1)); \
	NEW_VER="$$MAJOR.$$MINOR.$$NEW_PATCH"; \
	echo "$$NEW_VER" > "$(WEB_DIR)/WEB_VERSION"; \
	sed -i '' "s/const CACHE = 'eink-v[^']*'/const CACHE = 'eink-v$$NEW_VER'/" "$(WEB_DIR)/sw.js"; \
	printf '{"fw":"$(FW_VERSION)","web":"%s","build":"%s"}\n' "$$NEW_VER" "$$(date '+%Y-%m-%d %H:%M:%S')" > "$(WEB_DIR)/version.json"; \
	echo ">>> Web version bumped: v$$WV → v$$NEW_VER (sw.js, version.json)"

# ----------------------------------------------------------
# publish — alias for build (build already packages OTA)
# ----------------------------------------------------------
publish: build

# ----------------------------------------------------------
# deploy — upload entire web app to server via rsync
# ----------------------------------------------------------
deploy: build
	@echo ">>> Deploying to $(DEPLOY_HOST):$(DEPLOY_PATH)…"
	rsync -avz --delete \
		--exclude='.DS_Store' \
		"$(WEB_DIR)/" \
		"$(DEPLOY_USER)@$(DEPLOY_HOST):$(DEPLOY_PATH)/"
	@echo ">>> Deploy complete: https://$(DEPLOY_HOST)/"
