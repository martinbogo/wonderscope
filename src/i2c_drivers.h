#pragma once
// Decoding drivers for common I2C sensors, and identification by address and
// chip-ID registers.
#include "devices.h"
#include "xbus.h"

struct DriverInfo {
  const char *id;
  const char *name;
  const char *addrs;  // typical addresses, for display
};
extern const DriverInfo DRIVERS[];
extern const size_t DRIVER_COUNT;
bool driver_known(const char *id);

struct I2cIdent {
  char driver[12];  // empty if no decoding driver
  char product[40];
  char vendor[24];
  bool confirmed;   // identified by a chip ID / checksum rather than by address alone
};
void i2c_identify(const I2cDev &d, I2cIdent &out);

// Read decoded values. Returns count (>0), 0 when no new data is ready, <0 on error.
int driver_read(const char *driver, const I2cDev &d, DevValue *out, int max);
void driver_reset(const I2cDev &d);  // forget cached calibration / init state
