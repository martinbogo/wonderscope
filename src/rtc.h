#pragma once
// PCF85063 real-time clock (I2C 0x51). Keeps wall-clock time across reboots
// when the optional RTC battery is fitted.
#include "common.h"

bool rtc_begin();     // probe + load time into the system clock if valid
void rtc_write_now(); // store the current system time
bool rtc_present();
