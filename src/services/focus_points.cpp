#include "services/focus_points.h"

#include <Arduino.h>
#include <Preferences.h>

#include "services/radar_location.h"
#include "ui/radar_range.h"

namespace services::focus {
namespace {

constexpr char kPrefsNamespace[] = "focus";
constexpr char kPrefsIndexKey[] = "idx";
constexpr unsigned long kOverlayMs = 1500;

// Bay Area default focus ring. Airport coordinates from OurAirports data;
// default ranges chosen for each field's typical traffic pattern (Class B
// = 10 nm, GA = 5 nm).
const FocusPoint kFocusPoints[] = {
    {"Home", 0.0,       0.0,        1 /*10 nm*/, true},   // Bryant St, SF
    {"SFO",  37.6188,  -122.3750,   1 /*10 nm*/, false},
    {"OAK",  37.7213,  -122.2214,   1 /*10 nm*/, false},
    {"SQL",  37.5119,  -122.2495,   0 /* 5 nm*/, false},  // San Carlos
    {"HAF",  37.5136,  -122.5006,   0 /* 5 nm*/, false},  // Half Moon Bay
    {"PAO",  37.4611,  -122.1150,   0 /* 5 nm*/, false},  // Palo Alto
};
constexpr size_t kFocusCount = sizeof(kFocusPoints) / sizeof(kFocusPoints[0]);

uint8_t s_index = 0;
unsigned long s_overlay_shown_at_ms = 0;

void applyCurrent() {
  const FocusPoint& fp = kFocusPoints[s_index];
  if (fp.is_home) {
    services::location::clearOverride();
  } else {
    services::location::setOverride(fp.lat, fp.lon);
  }
  ui::radar::rangeSetIndex(fp.default_range_idx);
}

}  // namespace

void init() {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, true)) {
    const uint8_t saved = prefs.getUChar(kPrefsIndexKey, 0);
    prefs.end();
    if (saved < kFocusCount) s_index = saved;
  }
  applyCurrent();
}

void cycle() {
  s_index = static_cast<uint8_t>((s_index + 1) % kFocusCount);
  applyCurrent();
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putUChar(kPrefsIndexKey, s_index);
    prefs.end();
  }
  s_overlay_shown_at_ms = millis();
  Serial.printf("focus: %s (%d)\n", kFocusPoints[s_index].name, s_index);
}

const FocusPoint& current() { return kFocusPoints[s_index]; }

size_t count() { return kFocusCount; }

unsigned long overlayRemainingMs() {
  if (s_overlay_shown_at_ms == 0) return 0;
  const unsigned long elapsed = millis() - s_overlay_shown_at_ms;
  return (elapsed >= kOverlayMs) ? 0 : (kOverlayMs - elapsed);
}

}  // namespace services::focus
