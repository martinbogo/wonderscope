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

Run from PowerShell (Espressif's tool installer rejects Git Bash / MSYS):

```powershell
pio run -e wonderscope -t upload --upload-port COM13
```

The dashboard (`web/`) is gzipped into the firmware at build time by `tools/embed_web.py`.
Subsequent updates can be uploaded from Settings → Firmware (`.pio/build/wonderscope/firmware.bin`).

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
