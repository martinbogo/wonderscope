#pragma once
// Expansion buses: I2C on the Qwiic connector (IO2/IO1) and the pin header
// (IO8/IO9), and SPI on the pin header (IO10-13). One task owns them.
#include "common.h"
#include "jobs.h"

void xbus_begin();
bool xbus_submit(Job *job);  // takes ownership
void xbus_cancel();
void xbus_status_json(uint8_t bus, JsonObject o);

// I2C transport for drivers (call only from the xbus task).
struct I2cDev {
  uint8_t bus, addr;
};
bool xi2c_write(const I2cDev &d, const uint8_t *data, size_t n);
bool xi2c_read(const I2cDev &d, uint8_t *data, size_t n);
bool xi2c_write_read(const I2cDev &d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn);
bool xi2c_reg_read(const I2cDev &d, uint8_t reg, uint8_t *r, size_t rn);
bool xi2c_reg_write(const I2cDev &d, uint8_t reg, uint8_t value);
