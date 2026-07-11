#pragma once

// Shared owner of the I²C bus (SDA=GPIO 6, SCL=GPIO 7, 100 kHz).
// Both services::env_sensor (BME280) and services::tap_sensor
// (ADXL345) live on this bus at different addresses. Wire.begin() is
// idempotent but the pin choice and clock rate are latched by the
// first caller, so keeping the config in one place prevents drift if
// a future sensor lands with different assumptions.
//
// Not needed on the native/SDL build — the emulator has no I²C.

namespace services::i2c_bus {

/** Initialize the shared I²C bus with the config.h pins + clock. Safe
 *  to call from every sensor's probe(); only the first invocation
 *  touches Wire. */
void ensureInit();

}  // namespace services::i2c_bus
