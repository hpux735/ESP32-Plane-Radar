#include "services/env_sensor.h"

#ifdef USE_NATIVE

namespace services::env_sensor {

// Native/SDL build: no I²C, no sensor. Always returns valid=false so the
// cockpit screen's optional CABIN/RH lines stay hidden in the emulator.
void init() {}
Reading read() { return {0.0f, 0.0f, false}; }

}  // namespace services::env_sensor

#else

#include <Adafruit_BME280.h>
#include <Adafruit_Sensor.h>
#include <Arduino.h>
#include <Wire.h>

#include "services/i2c_bus.h"

namespace services::env_sensor {
namespace {

Adafruit_BME280 s_bme;
uint8_t s_address = 0;  // 0 = not present

bool probe() {
  services::i2c_bus::ensureInit();
  if (s_bme.begin(0x77, &Wire)) { s_address = 0x77; return true; }
  if (s_bme.begin(0x76, &Wire)) { s_address = 0x76; return true; }
  return false;
}

}  // namespace

void init() {
  s_address = 0;
  if (probe()) {
    Serial.printf("env_sensor: BME280 found at 0x%02X\n", s_address);
  } else {
    Serial.println("env_sensor: no BME280 (CABIN/RH lines will be hidden)");
  }
}

Reading read() {
  if (s_address == 0) {
    // Re-probe occasionally so a sensor plugged in after boot still works.
    if (!probe()) return {0.0f, 0.0f, false};
  }
  const float temp_c = s_bme.readTemperature();
  const float rh = s_bme.readHumidity();
  if (std::isnan(temp_c) || std::isnan(rh)) {
    // Chip stopped responding — force a re-probe on next call.
    s_address = 0;
    return {0.0f, 0.0f, false};
  }
  return {temp_c * 9.0f / 5.0f + 32.0f, rh, true};
}

}  // namespace services::env_sensor

#endif
