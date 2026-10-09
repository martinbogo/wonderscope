# Tool discovery and platform handling.

ifeq ($(OS),Windows_NT)
  # Use Git for Windows' sh for recipes so the same Makefile works from
  # PowerShell, cmd and Git Bash.
  GIT_SH := $(firstword $(wildcard C:/PROGRA~1/Git/usr/bin/sh.exe C:/PROGRA~1/Git/bin/sh.exe))
  ifneq ($(GIT_SH),)
    SHELL := $(GIT_SH)
    # Unix tools used by recipes (echo, awk, date, rm, ...)
    export PATH := C:/PROGRA~1/Git/usr/bin;$(PATH)
  endif
  PIO_HOME ?= $(subst \,/,$(if $(PLATFORMIO_CORE_DIR),$(PLATFORMIO_CORE_DIR),$(USERPROFILE)/.platformio))
  PENV_BIN := $(PIO_HOME)/penv/Scripts
  EXE := .exe
else
  PIO_HOME ?= $(if $(PLATFORMIO_CORE_DIR),$(PLATFORMIO_CORE_DIR),$(HOME)/.platformio)
  PENV_BIN := $(PIO_HOME)/penv/bin
  EXE :=
endif

# Espressif's tool installer refuses to run when MSYSTEM is set (Git Bash).
unexport MSYSTEM

PIO ?= $(firstword $(wildcard $(PENV_BIN)/pio$(EXE)) pio)
PYTHON ?= $(firstword $(wildcard $(PENV_BIN)/python$(EXE)) python3)
ESPTOOL := $(PYTHON) -m esptool --chip esp32s3

PORT_ARG := $(if $(PORT),--port $(PORT))
UPLOAD_PORT_ARG := $(if $(PORT),--upload-port $(PORT))
MONITOR_PORT_ARG := $(if $(PORT),--port $(PORT))
AUTH_ARG := $(if $(AUTH),-u $(AUTH))

BUILD_DIR := .pio/build/$(ENV)
FIRMWARE := $(BUILD_DIR)/firmware.bin
MERGED := $(BUILD_DIR)/firmware.factory.bin
