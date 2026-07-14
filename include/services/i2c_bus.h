#pragma once

// Owner of the ESP32-C3's hardware I²C bus (SDA=GPIO 6, SCL=GPIO 7,
// 100 kHz). services::env_sensor (BME280) uses this bus. The ADXL345
// tap sensor lives on its OWN software I²C bus internal to
// tap_sensor.cpp — see kTapSdaPin / kTapSclPin in config.h — because
// the sub-boards plug onto separate header rows on the C3 and the
// ESP32-C3 has only one hardware I²C controller (Wire1 is a compile-
// time symbol but returns false at runtime — SOC_I2C_NUM=1).
//
// Wire.begin() is idempotent but the pin choice and clock rate are
// latched by the first caller, so keeping the config in one place
// prevents drift if a future sensor also lands on the hardware bus.
//
// Not needed on the native/SDL build — the emulator has no I²C.

namespace services::i2c_bus {

/** Initialize the shared I²C bus with the config.h pins + clock. Safe
 *  to call from every sensor's probe(); only the first invocation
 *  touches Wire. */
void ensureInit();

}  // namespace services::i2c_bus
