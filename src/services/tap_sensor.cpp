#include "services/tap_sensor.h"

#ifdef USE_NATIVE

namespace services::tap_sensor {

// Native/SDL build: no I²C, no accelerometer. The SDL emulator drives
// gestures from the SPACE key through the BOOT-button discriminator, so
// this module is a no-op there.
void init() {}
void poll() {}
bool consumeSingleTap() { return false; }
bool consumeDoubleTap() { return false; }

}  // namespace services::tap_sensor

#else

#include <Arduino.h>
#include <Wire.h>

#include "config.h"
#include "services/i2c_bus.h"

namespace services::tap_sensor {
namespace {

// ADXL345 register map (from the datasheet, rev D).
constexpr uint8_t kRegDeviceId    = 0x00;
constexpr uint8_t kRegThreshTap   = 0x1D;   // 62.5 mg / LSB
constexpr uint8_t kRegDur         = 0x21;   // 625 µs / LSB
constexpr uint8_t kRegLatent      = 0x22;   // 1.25 ms / LSB (double-tap gap)
constexpr uint8_t kRegWindow      = 0x23;   // 1.25 ms / LSB (double-tap window)
constexpr uint8_t kRegTapAxes     = 0x2A;
constexpr uint8_t kRegIntEnable   = 0x2E;
constexpr uint8_t kRegIntSource   = 0x30;
constexpr uint8_t kRegDataFormat  = 0x31;
constexpr uint8_t kRegPowerCtl    = 0x2D;

constexpr uint8_t kExpectedDeviceId = 0xE5;

// INT_SOURCE / INT_ENABLE bit masks.
constexpr uint8_t kIntSingleTap = 0x40;
constexpr uint8_t kIntDoubleTap = 0x20;

// Tap-detection tuning. Chosen for a small knock on a rigid 3D-printed
// enclosure — sensitive enough to fire on a fingertip tap without
// triggering on background desk vibration. The user can tune these
// later if it feels wrong in their case:
//   threshold  ~3 g (48 * 62.5 mg = 3.0 g)
//   duration   ~10 ms (16 * 625 µs)
//   latent     ~40 ms gap before double-tap window opens
//   window     ~250 ms window to catch the second tap
constexpr uint8_t kThreshTap = 48;
constexpr uint8_t kDur       = 16;
constexpr uint8_t kLatent    = 32;
constexpr uint8_t kWindow    = 200;

// All three axes participate in tap detection so it doesn't matter how
// the chip is glued inside the case.
constexpr uint8_t kTapAxesAll = 0x07;

bool s_present = false;
bool s_single_pending = false;
bool s_double_pending = false;

bool writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(config::kTapSensorI2cAddress);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

uint8_t readReg(uint8_t reg) {
  Wire.beginTransmission(config::kTapSensorI2cAddress);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  Wire.requestFrom(config::kTapSensorI2cAddress, static_cast<uint8_t>(1));
  return Wire.available() ? Wire.read() : 0;
}

bool probe() {
  services::i2c_bus::ensureInit();
  return readReg(kRegDeviceId) == kExpectedDeviceId;
}

}  // namespace

void init() {
  s_present = false;
  s_single_pending = false;
  s_double_pending = false;

  if (!probe()) {
    Serial.println("tap_sensor: no ADXL345 at 0x53 (case-tap disabled)");
    return;
  }

  // Full-resolution measure mode. Data format: ±2 g by default, which is
  // plenty since a tap easily saturates the axis anyway.
  writeReg(kRegDataFormat, 0x08);           // full-resolution
  writeReg(kRegThreshTap,  kThreshTap);
  writeReg(kRegDur,        kDur);
  writeReg(kRegLatent,     kLatent);
  writeReg(kRegWindow,     kWindow);
  writeReg(kRegTapAxes,    kTapAxesAll);
  writeReg(kRegIntEnable,  kIntSingleTap | kIntDoubleTap);
  writeReg(kRegPowerCtl,   0x08);           // MEASURE bit

  s_present = true;
  Serial.println("tap_sensor: ADXL345 found at 0x53 (case-tap enabled)");
}

void poll() {
  if (!s_present) return;
  const uint8_t src = readReg(kRegIntSource);   // Reading clears the flags.
  // ADXL345 fires SINGLE_TAP on the first tap AND on the confirmed second
  // tap of a double-tap sequence. To avoid double-counting, prefer
  // DOUBLE_TAP when both bits are set in the same poll.
  if (src & kIntDoubleTap) {
    s_double_pending = true;
  } else if (src & kIntSingleTap) {
    s_single_pending = true;
  }
}

bool consumeSingleTap() {
  const bool p = s_single_pending;
  s_single_pending = false;
  return p;
}

bool consumeDoubleTap() {
  const bool p = s_double_pending;
  s_double_pending = false;
  return p;
}

}  // namespace services::tap_sensor

#endif
