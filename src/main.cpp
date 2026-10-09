// WonderScope - RS485 / CAN bus monitor and configurator for the
// Waveshare ESP32-S3-RS485-CAN(-U).
//
// Tasks:
//   rs485  (core 1)  owns UART1: sniffing, Modbus master, scans, polling
//   can    (core 1)  owns TWAI: receive, CANopen/J1939, scans, polling
//   xbus   (core 1)  owns Wire1 + FSPI: Qwiic/header I2C, SPI, sensor drivers
//   async_tcp        web server / WebSocket requests -> rpc_dispatch()
//   loop   (core 1)  serial console, outbound queue, periodic pushes

#include "can.h"
#include "cli.h"
#include "common.h"
#include "devices.h"
#include "rpc.h"
#include "rs485.h"
#include "rtc.h"
#include "settings.h"
#include "trace.h"
#include "web.h"
#include "wifi_mgr.h"
#include "xbus.h"

void setup() {
  Serial.setTxBufferSize(8192);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // never block when no USB host is listening

  out_init();
  trace_init();
  settings_load();
  bool rtcTime = rtc_begin();
  dev_init();
  rs485_begin();
  can_begin();
  xbus_begin();
  wifi_begin();
  web_begin();
  Serial.printf("[boot] %s %s, RTC %s\n", FW_NAME, FW_VERSION,
                rtc_present() ? (rtcTime ? "time loaded" : "present, time not set") : "not found");
}

void loop() {
  cli_serial_loop();
  out_pump();
  push_loop();
  wifi_loop();
  web_loop();
  dev_loop();
  rpc_loop();
  delay(2);
}
