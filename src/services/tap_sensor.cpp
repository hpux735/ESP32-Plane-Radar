#include "services/tap_sensor.h"

// Pure classifier — compiled in every build so the native unit test
// can exercise it without any Wire / ADXL345 mock. Lives outside the
// USE_NATIVE / !USE_NATIVE split for that reason.
namespace services::tap_sensor {

TapEvents classifyIntSource(uint8_t src) {
  TapEvents out{false, false};
  // The ADXL345 sets SINGLE_TAP on the first tap of a pair AND on the
  // confirmed second tap. When DOUBLE_TAP is asserted the SINGLE_TAP
  // bit becomes redundant — drop it so one physical pair of knocks
  // doesn't produce two gestures.
  if (src & kIntDoubleTap) {
    out.double_tap = true;
  } else if (src & kIntSingleTap) {
    out.single = true;
  }
  return out;
}

}  // namespace services::tap_sensor

#ifdef USE_NATIVE

namespace services::tap_sensor {

// Native/SDL build: no I²C, no accelerometer. The SDL emulator drives
// gestures from the SPACE key through the BOOT-button discriminator, so
// the hardware-facing side of this module is a no-op there.
void init() {}
void poll() {}
bool consumeSingleTap() { return false; }
bool consumeDoubleTap() { return false; }

}  // namespace services::tap_sensor

#else

#include <Arduino.h>
#include <AsyncDelay.h>
#include <SoftWire.h>

#include "config.h"

namespace services::tap_sensor {
namespace {

// Dedicated software I²C bus for the ADXL345 (see kTapSdaPin/kTapSclPin
// in config.h). The BME280 owns the hardware Wire; ESP32-C3 has no
// second hardware I²C controller so a software bus is the only way to
// give each sub-board its own SDA/SCL. Bit-bang cost is ~50 µs/byte at
// 100 kHz — irrelevant for the tap sensor, which reads 1-2 bytes per
// interrupt. SoftWire needs caller-owned TX/RX buffers; the ADXL345
// exchanges are all 1-byte reg-addr + 1-byte value so 4-byte buffers
// are ample. Kept static to survive across probe() re-entries.
SoftWire s_tap_wire(config::kTapSdaPin, config::kTapSclPin);
uint8_t s_tap_tx_buf[4];
uint8_t s_tap_rx_buf[4];
bool s_wire_inited = false;

void ensureTapWireInit() {
  if (s_wire_inited) return;
  s_tap_wire.setTxBuffer(s_tap_tx_buf, sizeof(s_tap_tx_buf));
  s_tap_wire.setRxBuffer(s_tap_rx_buf, sizeof(s_tap_rx_buf));
  s_tap_wire.setTimeout_ms(10);
  s_tap_wire.setDelay_us(5);   // ~100 kHz
  s_tap_wire.begin();
  s_wire_inited = true;
}

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

// INT_SOURCE / INT_ENABLE bit masks — kIntSingleTap / kIntDoubleTap
// are declared in the header alongside the pure classifier, so tests
// and firmware share the same constants.

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
  s_tap_wire.beginTransmission(config::kTapSensorI2cAddress);
  s_tap_wire.write(reg);
  s_tap_wire.write(val);
  return s_tap_wire.endTransmission() == 0;
}

uint8_t readReg(uint8_t reg) {
  s_tap_wire.beginTransmission(config::kTapSensorI2cAddress);
  s_tap_wire.write(reg);
  if (s_tap_wire.endTransmission(false) != 0) return 0;
  s_tap_wire.requestFrom(config::kTapSensorI2cAddress,
                          static_cast<uint8_t>(1));
  return s_tap_wire.available() ? s_tap_wire.read() : 0;
}

bool probe() {
  ensureTapWireInit();
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
  const TapEvents ev = classifyIntSource(src);
  if (ev.double_tap) s_double_pending = true;
  if (ev.single)     s_single_pending = true;
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
