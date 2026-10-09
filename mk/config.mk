# WonderScope build configuration.
# Override on the command line (make flash PORT=COM7) or in local.mk.

# PlatformIO environment (platformio.ini)
ENV ?= wonderscope

# Serial port of the board. Empty = auto-detect.
PORT ?=

# Serial console baud rate
MONITOR_BAUD ?= 115200

# Flashing baud rate for esptool operations (backup / restore / erase)
FLASH_BAUD ?= 921600

# Network address of the board, used by OTA update and 'make open'
HOST ?= wonderscope.local

# Dashboard login for OTA when enabled on the board (user:password)
AUTH ?=

# Directory for flash backups
BACKUP_DIR ?= backup

# Port for the dashboard mock server ('make mock')
MOCK_PORT ?= 8765
