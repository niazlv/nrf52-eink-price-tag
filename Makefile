# ============================================================
# eink_tag — top-level build Makefile
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
# sysbuild names the application image after the source DIRECTORY, not after
# the CMake project(), so derive it instead of hardcoding — the path stays
# correct whatever the repository is cloned as.
APP_IMAGE    := $(notdir $(APP_DIR))
SIGNED_BIN   := $(BUILD_DIR)/$(APP_IMAGE)/zephyr/zephyr.signed.bin
FW_DIR       := $(ROOT_DIR)/lut_tester_host/web/firmware
FW_MANIFEST  := $(FW_DIR)/manifest.json

# ----------------------------------------------------------
# Build-variant matrix — one source tree, N OTA images.
#   legacy : fielded batch. Current layout, DEFAULT MCUboot key, app_update.bin.
#   v2     : new batch. 12K settings + 4K factory_data, its OWN signing key,
#            its own image (app_update_v2.bin). A wrong-variant image is
#            rejected by MCUboot before swap (different key) — that is the hard
#            cross-flash lock; the host just picks the right file by SYSINFO
#            layout=.
#
#   make            -> legacy
#   make VARIANT=v2 -> v2 (run `make v2-genkey` once first)
#   make release    -> both, merged into one manifest.json
#
# OTA size ceiling (swap-using-move) = slot sectors minus 2 reserved (trailer +
# move). legacy 58->56 sectors = 229376 B; v2 57->55 sectors = 225280 B. Over
# the ceiling MCUboot marks the image "test" but silently never swaps.
# ----------------------------------------------------------
VARIANT ?= legacy
ifeq ($(VARIANT),v2)
  PM_FILE        := $(ROOT_DIR)/pm_static_nrf52dk_nrf52832_v2.yml
  LAYOUT_ID      := 2
  SIGN_KEY       := $(ROOT_DIR)/keys/v2_signing_rsa2048.pem
  OTA_MAX_SIGNED := 225280
  OTA_BIN_NAME   := app_update_v2.bin
  KEY_LABEL      := v2-prod-rsa2048
else
  PM_FILE        := $(ROOT_DIR)/pm_static_nrf52dk_nrf52832.yml
  LAYOUT_ID      := 1
  SIGN_KEY       :=
  OTA_MAX_SIGNED := 229376
  OTA_BIN_NAME   := app_update.bin
  KEY_LABEL      := default-rsa2048
endif
FW_BIN := $(FW_DIR)/$(OTA_BIN_NAME)

# ----------------------------------------------------------
# Deploy target (rsync to web server)
# ----------------------------------------------------------
# The public domain is only a reverse proxy (vitalii's nginx, .60 → .114:7341);
# rsync must target the LAN box that actually serves the files. --chown keeps
# the June-era niazl ownership even though we connect as root.
DEPLOY_HOST  ?= 192.168.99.114
DEPLOY_PATH  ?= /var/www/lut_tester
DEPLOY_USER  ?= root
DEPLOY_CHOWN ?= niazl:niazl
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
# Nothing to inject: the application takes its own build timestamp in
# CMakeLists.txt. Passing it here never worked — every -D after `--` lands in
# the sysbuild cache, not in this image — and the BUILD_* values above are
# still used for the log line and the OTA manifest date.
CMAKE_EXTRA :=

.PHONY: build flash flash-gdb flash-openocd flash-retry clean pristine publish deploy web release v2-genkey test

# ----------------------------------------------------------
# test — host-side unit tests for the portable lib/ modules.
# Needs only a C compiler; no Zephyr, no hardware.
# ----------------------------------------------------------
test:
	$(MAKE) -C tests/host test

build:
	@echo ">>> Build variant: $(VARIANT) (layout $(LAYOUT_ID), key $(KEY_LABEL))"
	@echo ">>> Build timestamp: $(BUILD_YEAR)-$(BUILD_MONTH)-$(BUILD_DAY) $(BUILD_HOUR):$(BUILD_MIN):$(BUILD_SEC)"
	@if [ -n "$(SIGN_KEY)" ] && [ ! -f "$(SIGN_KEY)" ]; then \
		echo "!!! VARIANT=$(VARIANT) needs signing key $(SIGN_KEY)."; \
		echo "    Run 'make v2-genkey' once (and back the key up offline)."; \
		exit 1; \
	fi
	@mkdir -p "$(LOCAL_HOME)/Library/Caches/zephyr/ToolchainCapabilityDatabase"
	"$(WEST)" build -p always -b "$(BOARD)" "$(APP_DIR)" -d "$(BUILD_DIR)" \
		-- $(CMAKE_EXTRA) \
		-DPM_STATIC_YML_FILE="$(PM_FILE)" \
		$(if $(SIGN_KEY),-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=\"$(SIGN_KEY)\")
	@echo ">>> HEX ready: $(BUILD_DIR)/merged.hex"
	@# --- OTA size guard: fail loudly BEFORE publishing an image MCUboot can't swap ---
	@SIGNED_SZ=$$(wc -c < "$(SIGNED_BIN)" | tr -d ' '); \
	if [ "$$SIGNED_SZ" -gt "$(OTA_MAX_SIGNED)" ]; then \
		echo ""; \
		echo "!!! OTA SIZE ERROR: signed image $$SIGNED_SZ B > limit $(OTA_MAX_SIGNED) B (56 sectors)."; \
		echo "    It fits flash and boots when wired-flashed, but over BLE MCUboot will"; \
		echo "    mark it 'test' and then FAIL to swap (verify ok, never applied)."; \
		echo "    Trim flash or switch MCUboot to overwrite-only. NOT publishing."; \
		echo ""; \
		exit 1; \
	fi; \
	echo ">>> OTA size OK: $$SIGNED_SZ / $(OTA_MAX_SIGNED) B ($$(( $(OTA_MAX_SIGNED) - SIGNED_SZ )) B headroom)"
	@# --- Auto-publish firmware for OTA (per-variant) ---
	@mkdir -p "$(FW_DIR)/history"
	@if [ -f "$(FW_BIN)" ]; then \
		PREV_SHA=$$(shasum -a 256 "$(FW_BIN)" | cut -d' ' -f1); \
		NEW_SHA=$$(shasum -a 256 "$(SIGNED_BIN)" | cut -d' ' -f1); \
		if [ "$$PREV_SHA" != "$$NEW_SHA" ]; then \
			cp "$(FW_BIN)" "$(FW_DIR)/history/$(VARIANT)_$$(date +%Y%m%d_%H%M%S).bin"; \
			echo ">>> Archived previous $(VARIANT) image"; \
		fi; \
	fi
	@cp "$(SIGNED_BIN)" "$(FW_BIN)"
	@FW_SIZE=$$(stat -f%z "$(FW_BIN)" 2>/dev/null || stat -c%s "$(FW_BIN)"); \
	FW_SHA=$$(shasum -a 256 "$(FW_BIN)" | cut -d' ' -f1); \
	FW_DATE=$$(date "+%Y-%m-%d"); \
	MF="$(FW_MANIFEST)" LAYOUT="$(LAYOUT_ID)" VNAME="$(VARIANT)" \
	  VFILE="$(OTA_BIN_NAME)" VER="$(FW_VERSION)" VSIZE="$$FW_SIZE" VSHA="$$FW_SHA" \
	  VDATE="$$FW_DATE" VKEY="$(KEY_LABEL)" VOTA="$(OTA_MAX_SIGNED)" \
	  python3 "$(ROOT_DIR)/scripts/update_manifest.py"
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

# ----------------------------------------------------------
# flash-retry — keep re-flashing until it VERIFIES OK. For tags with
# flaky SWD connections. Auto-uses a running OpenOCD telnet (localhost:4444,
# like flash-gdb) or spawns its own openocd per attempt (like flash-openocd).
#   make flash-retry                       # build/merged.hex, retry forever
#   make flash-retry HEX=path/to.hex
#   make flash-retry FLASH_MAX_RETRIES=20  # give up after 20 tries
#   make flash-retry CONTINUOUS=1          # flash tag after tag, forever
# ----------------------------------------------------------
HEX ?= $(BUILD_DIR)/merged.hex
flash-retry:
	@bash "$(ROOT_DIR)/scripts/flash-retry.sh" "$(HEX)"

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
# release — build ALL OTA variants into one manifest.json.
# Run `make v2-genkey` once beforehand (the v2 build needs its signing key).
# ----------------------------------------------------------
release:
	@echo ">>> Building ALL OTA variants (legacy + v2)…"
	$(MAKE) build VARIANT=legacy
	$(MAKE) build VARIANT=v2
	@echo ">>> Release variants in $(FW_DIR):"
	@python3 -c "import json;d=json.load(open('$(FW_MANIFEST)'));[print('   layout',v['layout'],v['name'],v['file'],'v'+v['version'],str(v['size'])+'B','key='+v['key']) for v in d.get('variants',[])]"

# ----------------------------------------------------------
# v2-genkey — generate the v2 batch's MCUboot signing key (RSA-2048), ONE TIME.
# Never overwrite an existing key (would orphan every v2 device already signed
# with it). The key is committed to this (internal) repo so it can't be lost.
# ----------------------------------------------------------
v2-genkey:
	@mkdir -p "$(ROOT_DIR)/keys"
	@if [ -f "$(ROOT_DIR)/keys/v2_signing_rsa2048.pem" ]; then \
		echo "!!! keys/v2_signing_rsa2048.pem already exists — refusing to overwrite."; \
		exit 1; \
	fi
	python3 "$(NCS_DIR)/bootloader/mcuboot/scripts/imgtool.py" keygen \
		-k "$(ROOT_DIR)/keys/v2_signing_rsa2048.pem" -t rsa-2048
	@echo ">>> Generated keys/v2_signing_rsa2048.pem — COMMIT it (internal repo)."
	@echo "    It is the v2 batch MCUboot key; keep it in version control so it"
	@echo "    can't be lost. If the repo ever goes public, rotate it + move it out."

# ----------------------------------------------------------
# deploy — upload entire web app to server via rsync
# ----------------------------------------------------------
deploy: build
	@echo ">>> Deploying to $(DEPLOY_HOST):$(DEPLOY_PATH)…"
	rsync -avz --delete \
		--exclude='.DS_Store' \
		--chown=$(DEPLOY_CHOWN) \
		"$(WEB_DIR)/" \
		"$(DEPLOY_USER)@$(DEPLOY_HOST):$(DEPLOY_PATH)/"
	@echo ">>> Deploy complete: https://pwa.price-tag.sorewa.ru/"
