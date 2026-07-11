#include "services/i2c_bus.h"

#ifdef USE_NATIVE

namespace services::i2c_bus {

// No I²C on the desktop emulator.
void ensureInit() {}

}  // namespace services::i2c_bus

#else

#include <Arduino.h>
#include <Wire.h>

#include "config.h"

namespace services::i2c_bus {
namespace {

bool s_inited = false;

}  // namespace

void ensureInit() {
  if (s_inited) return;
  Wire.begin(config::kBmeSdaPin, config::kBmeSclPin);
  Wire.setClock(100000);
  s_inited = true;
}

}  // namespace services::i2c_bus

#endif
