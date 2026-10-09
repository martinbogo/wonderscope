#pragma once
// Shared definitions for WonderScope firmware.

#include <Arduino.h>
#include <ArduinoJson.h>

#define FW_NAME "WonderScope"
#define FW_VERSION "1.1.1"

namespace pins {
constexpr int RS485_TX = 17;
constexpr int RS485_RX = 18;
constexpr int RS485_DE = 21;  // SP3485 DE+RE, driven by UART RTS in RS485 half-duplex mode
constexpr int CAN_TX = 15;
constexpr int CAN_RX = 16;
constexpr int I2C_SDA = 39;  // on-board RTC bus (not broken out)
constexpr int I2C_SCL = 38;
// Expansion buses
constexpr int QWIIC_SDA = 2;  // SH1.0 connector beside USB-C: GND, 3V3, SDA, SCL
constexpr int QWIIC_SCL = 1;
constexpr int HDR_SDA = 8;    // 2x10 pin header (inside the case)
constexpr int HDR_SCL = 9;
constexpr int SPI_SCK = 12;   // FSPI IO_MUX pins
constexpr int SPI_MOSI = 11;
constexpr int SPI_MISO = 13;
constexpr int SPI_CS = 10;
}  // namespace pins

enum Bus : uint8_t { BUS_RS485 = 0, BUS_CAN = 1, BUS_QWIIC = 2, BUS_I2C = 3, BUS_SPI = 4 };
constexpr uint8_t BUS_COUNT = 5;
enum Proto : uint8_t { PROTO_MODBUS = 0, PROTO_CANOPEN = 1, PROTO_J1939 = 2, PROTO_I2C = 3, PROTO_SPI = 4 };
inline bool bus_is_i2c(uint8_t bus) { return bus == BUS_QWIIC || bus == BUS_I2C; }

const char *bus_name(uint8_t bus);
const char *proto_name(uint8_t proto);
int bus_from_name(const char *s);    // -1 if unknown
int proto_from_name(const char *s);  // -1 if unknown

// ---- Reply routing -------------------------------------------------------
// A request arrives from a WebSocket client or the USB serial console; the
// reply has to go back to the same place, possibly much later (bus jobs).
constexpr uint32_t CLIENT_BROADCAST = 0;
constexpr uint32_t CLIENT_SERIAL = 0xFFFFFFFE;

enum ReplyMode : uint8_t { REPLY_JSON = 0, REPLY_TEXT = 1 };

struct ReplyTo {
  uint32_t client = CLIENT_SERIAL;
  int32_t id = -1;  // <0: no reply wanted
  uint8_t mode = REPLY_JSON;  // REPLY_TEXT: console request, render result as text
  char cmd[24] = "";          // command name, used to pick a text renderer
};

// outbound.cpp - all outgoing data goes through one queue drained by loop().
void out_init();
void out_send(uint32_t client, const JsonDocument &doc);
void out_text(uint32_t client, const String &text, int32_t id = -1);  // console text
void out_pump();  // called from loop()
void reply_ok(const ReplyTo &r, JsonDocument &result);
void reply_ok(const ReplyTo &r);
void reply_err(const ReplyTo &r, const char *fmt, ...);
void event_send(JsonDocument &doc);  // doc must contain "ev"
uint32_t out_dropped();

// ---- Helpers -------------------------------------------------------------
size_t hex_encode(const uint8_t *data, size_t len, char *out, size_t outCap);  // "0A 1B ..." (no spaces: compact)
String hex_string(const uint8_t *data, size_t len);
int hex_decode(const char *s, uint8_t *out, size_t cap);  // tolerant: spaces, commas, 0x; -1 on error
uint64_t uptime_us();
inline uint32_t uptime_ms() { return (uint32_t)(uptime_us() / 1000ULL); }
int64_t epoch_ms();  // 0 if wall clock unknown
bool time_valid();
