# WonderScope - Waveshare ESP32-S3-RS485-CAN bus monitor
#
#   make            show targets
#   make flash      build and install over USB
#   make ota        build and install over Wi-Fi
#
# Settings: mk/config.mk. Per-machine overrides: local.mk (not tracked).

-include local.mk
include mk/config.mk
include mk/tools.mk

.DEFAULT_GOAL := help

.PHONY: help build flash flash-monitor monitor console ota open size clean distclean \
        erase backup restore web deps compiledb mock check-tools

## Build -------------------------------------------------------------------

build: check-tools ## Compile firmware and embed the dashboard
	$(PIO) run -e $(ENV)

size: check-tools ## Show firmware memory usage
	$(PIO) run -e $(ENV) -t size

web: ## Regenerate embedded dashboard assets only
	$(PYTHON) tools/embed_web.py

deps: check-tools ## Install platform, toolchain and libraries
	$(PIO) pkg install -e $(ENV)

compiledb: check-tools ## Generate compile_commands.json for editors
	$(PIO) run -e $(ENV) -t compiledb

## Install -----------------------------------------------------------------

flash: check-tools ## Build and install over USB (PORT= to select a port)
	$(PIO) run -e $(ENV) -t upload $(UPLOAD_PORT_ARG)

flash-monitor: flash ## Install over USB, then open the serial console
	$(PIO) device monitor -e $(ENV) -b $(MONITOR_BAUD) $(MONITOR_PORT_ARG)

ota: build ## Build and install over Wi-Fi (HOST=, AUTH=user:pass)
	@echo "Uploading $(FIRMWARE) to http://$(HOST)/"
	curl -fsS --max-time 120 $(AUTH_ARG) -F "firmware=@$(FIRMWARE)" "http://$(HOST)/api/update"
	@echo

## Device ------------------------------------------------------------------

monitor: check-tools ## Open the serial console
	$(PIO) device monitor -e $(ENV) -b $(MONITOR_BAUD) $(MONITOR_PORT_ARG)

console: monitor ## Alias for monitor

open: ## Open the dashboard in a browser
ifeq ($(OS),Windows_NT)
	cmd //c start "" "http://$(HOST)/"
else
	@(command -v xdg-open >/dev/null && xdg-open "http://$(HOST)/") || open "http://$(HOST)/"
endif

backup: ## Save the complete 16 MB flash to BACKUP_DIR (default backup/)
	@mkdir -p $(BACKUP_DIR)
	$(ESPTOOL) $(PORT_ARG) --baud $(FLASH_BAUD) read-flash 0 0x1000000 \
	  $(BACKUP_DIR)/flash-$$(date +%Y%m%d-%H%M%S).bin

restore: ## Write a full flash image: make restore IMAGE=file.bin
	@test -n "$(IMAGE)" || { echo "usage: make restore IMAGE=<file.bin>"; exit 1; }
	@test -f "$(IMAGE)" || { echo "not found: $(IMAGE)"; exit 1; }
	$(ESPTOOL) $(PORT_ARG) --baud $(FLASH_BAUD) write-flash 0 "$(IMAGE)"

erase: ## Erase the entire flash, including settings (CONFIRM=yes)
	@test "$(CONFIRM)" = "yes" || { echo "Erases firmware, settings and device list. Run: make erase CONFIRM=yes"; exit 1; }
	$(ESPTOOL) $(PORT_ARG) erase-flash

## Maintenance -------------------------------------------------------------

clean: check-tools ## Remove build output
	$(PIO) run -e $(ENV) -t clean
	rm -rf src/generated

distclean: ## Remove all build output and caches
	rm -rf .pio src/generated

mock: ## Serve the dashboard with a simulated backend
	@echo "http://localhost:$(MOCK_PORT)/tools/mock/"
	$(PYTHON) -m http.server $(MOCK_PORT) --bind 127.0.0.1

check-tools:
	@"$(PIO)" --version >/dev/null 2>&1 || { echo "PlatformIO not found ($(PIO)). Install PlatformIO Core or set PIO=<path>."; exit 1; }

help: ## Show this help
	@echo "WonderScope - make targets"
	@echo
	@awk 'BEGIN {FS = ":.*## "} \
	  /^## / { sub(/^## /, ""); sub(/ -+$$/, ""); printf "\n%s\n", $$0; next } \
	  /^[a-zA-Z_-]+:.*## / { printf "  %-15s %s\n", $$1, $$2 }' $(MAKEFILE_LIST)
	@echo
	@echo "Variables: PORT=$(if $(PORT),$(PORT),<auto>)  HOST=$(HOST)  ENV=$(ENV)"
