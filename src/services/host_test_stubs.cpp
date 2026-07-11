// Stub definitions for symbols that focus_points.cpp and
// metar_config.cpp reference but that unit tests don't exercise. Only
// compiled under UNIT_TEST (`pio test -e native_test`) — see the
// build_src_filter in platformio.ini. Keeps the anon-namespace state
// mutations in the service .cpps from dragging in weather.cpp / HTTP /
// LovyanGFX / etc. at link time.

#ifdef UNIT_TEST

#include <cstdint>

namespace services::weather { void invalidate() {} }

namespace services::location {
namespace {
double s_override_lat = 0.0;
double s_override_lon = 0.0;
bool s_override_active = false;
}
void setOverride(double lat, double lon) {
  s_override_lat = lat;
  s_override_lon = lon;
  s_override_active = true;
}
void clearOverride() { s_override_active = false; }
bool isOverrideActive() { return s_override_active; }
}  // namespace services::location

namespace ui::radar {
namespace { uint8_t s_range_idx = 0; }
void rangeSetIndex(uint8_t idx) { s_range_idx = idx; }
uint8_t rangeIndexForTest() { return s_range_idx; }
}

#endif  // UNIT_TEST
