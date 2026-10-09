# WonderScope

RS485 / CAN bus monitor and configurator for the Waveshare ESP32-S3-RS485-CAN(-U).
Web dashboard served by the board, plus a text console on USB serial and in the dashboard.

## Features

- **RS485 / Modbus RTU** — passive sniffing, address scan with baud/parity sweep, device identification (FC2B/0E, FC11), register and coil read/write, raw frames.
- **CAN** — listen-only by default, bitrate detection, identifier map, CANopen (SDO, NMT, heartbeat, EMCY, identity), J1939 (address claim, PGN requests, BAM reassembly), raw frames.
- **Devices** — topology map and address grid per bus, labels, notes, per-device link settings, polled watch lists. Stored on the board.
- **Traffic** — live decoded trace for both buses, filter, CSV export.
- **System** — Wi-Fi AP + station, mDNS, optional login, OTA update, RTC time.

## Access

| Method | Address |
|---|---|
| Station (LAN) | http://wonderscope.local/ |
| Access point | SSID `WonderScope-XXXX`, password `wonderscope`, http://192.168.4.1/ |
| USB serial | 115200 baud, `help` for commands |

## Build and flash

Requires [PlatformIO Core](https://platformio.org/install/cli) and GNU Make. On Windows, recipes use Git for Windows' shell.

| Command | Action |
|---|---|
| `make flash` | Build and install over USB |
| `make ota` | Build and install over Wi-Fi |
| `make monitor` | Serial console |
| `make backup` | Save the complete flash to `backup/` |
| `make restore IMAGE=file.bin` | Write a full flash image |
| `make` | List all targets |

Defaults are in `mk/config.mk`: serial port auto-detected, host `wonderscope.local`. Override per command (`make flash PORT=COM13`) or in `local.mk` (see `local.mk.example`).

Release binaries: `firmware.bin` for OTA (Settings → Firmware, or `make ota`), `firmware.factory.bin` for a complete USB install at offset 0.

## Hardware notes

| Function | Pins |
|---|---|
| RS485 (UART1, SP3485) | TX 17, RX 18, DE/RE 21 |
| CAN (TWAI, TJA1051) | TX 15, RX 16 |
| RTC (PCF85063, I²C) | SDA 39, SCL 38 |

Termination: jumper H1 (CAN), H2 (RS485), 120 Ω. Fit only at a line end.

## License

Copyright © 2026 Martin Bogomolni (martinbogo@gmail.com).
[CC BY-NC 4.0](https://creativecommons.org/licenses/by-nc/4.0/) — attribution required, non-commercial use only. Third-party components keep their own licenses; see [LICENSE](LICENSE).
