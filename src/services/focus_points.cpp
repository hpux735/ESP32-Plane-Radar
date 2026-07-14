#include "services/focus_points.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include <cmath>
#include <cstring>

#include "services/radar_location.h"
#include "ui/radar_range.h"

namespace services::focus {
namespace {

constexpr char kPrefsNamespace[] = "focus";
constexpr char kPrefsIndexKey[] = "idx";
constexpr char kPrefsJsonKey[] = "ring";
constexpr unsigned long kOverlayMs = 1500;
constexpr size_t kMaxRingSize = 16;

// Runtime-mutable ring. Slot 0 is always synthetic Home (reads its lat/lon
// from services::location at draw time, so home moves when the user
// reconfigures). Slots 1..N-1 come from the user's saved JSON, or from the
// baked default list below if no JSON is stored or the JSON fails to parse.
FocusPoint s_ring[kMaxRingSize];
size_t s_ring_count = 0;
uint8_t s_index = 0;
unsigned long s_overlay_shown_at_ms = 0;

// Default focus ring used when the "focus.ring" preference is empty or
// malformed. Two airports plus the synthetic Home slot (added elsewhere) —
// the whole app now navigates a 5-screen ring (3 radars + weather +
// cockpit) via single/double tap, so the number of focus slots directly
// caps how many radar screens are in the ring.
struct BakedAirport {
  const char* name;
  double lat;
  double lon;
  uint8_t default_range_idx;
};
// ICAO 4-letter codes for consistency with anything picked from the LAN
// portal's airport search (which always inserts ICAO). Chip UI auto-migrates
// any 3-letter IATA it finds in a persisted ring to the matching ICAO on
// load, so users upgrading from an older firmware don't end up with a mix.
constexpr BakedAirport kFallbackAirports[] = {
    {"KSFO", 37.6188, -122.3750, 2},  // Class B, 15 nm view
    {"KOAK", 37.7213, -122.2214, 2},  // Class C, 15 nm view
};
constexpr size_t kFallbackCount =
    sizeof(kFallbackAirports) / sizeof(kFallbackAirports[0]);

void setName(FocusPoint& fp, const char* src) {
  std::strncpy(fp.name, src, sizeof(fp.name) - 1);
  fp.name[sizeof(fp.name) - 1] = '\0';
}

void seedHomeSlot() {
  setName(s_ring[0], "Home");
  s_ring[0].lat = 0.0;
  s_ring[0].lon = 0.0;
  s_ring[0].default_range_idx = 1;
  s_ring[0].is_home = true;
}

void appendBakedFallback() {
  for (size_t i = 0; i < kFallbackCount && s_ring_count < kMaxRingSize; ++i) {
    FocusPoint& fp = s_ring[s_ring_count++];
    setName(fp, kFallbackAirports[i].name);
    fp.lat = kFallbackAirports[i].lat;
    fp.lon = kFallbackAirports[i].lon;
    fp.default_range_idx = kFallbackAirports[i].default_range_idx;
    fp.is_home = false;
  }
}

// Returns true if `json` parsed into at least one airport entry (which was
// appended to s_ring). Leaves s_ring untouched on failure.
bool tryAppendFromJson(const String& json) {
  if (json.length() == 0) return false;
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.printf("focus: JSON parse error: %s\n", err.c_str());
    return false;
  }
  if (!doc.is<JsonArray>()) {
    Serial.println("focus: JSON root is not an array");
    return false;
  }
  const size_t before = s_ring_count;
  for (JsonObject entry : doc.as<JsonArray>()) {
    if (s_ring_count >= kMaxRingSize) break;
    const char* name = entry["name"] | "?";
    const float lat = entry["lat"] | NAN;
    const float lon = entry["lon"] | NAN;
    const int range_idx = entry["range_idx"] | 1;
    if (!(lat >= -90.0f && lat <= 90.0f) ||
        !(lon >= -180.0f && lon <= 180.0f)) {
      continue;
    }
    FocusPoint& fp = s_ring[s_ring_count++];
    setName(fp, name);
    fp.lat = lat;
    fp.lon = lon;
    fp.default_range_idx =
        (range_idx >= 0 && range_idx <= 255) ? static_cast<uint8_t>(range_idx) : 1;
    fp.is_home = false;
  }
  return s_ring_count > before;
}

void loadRing() {
  s_ring_count = 0;
  seedHomeSlot();
  s_ring_count = 1;

  Preferences prefs;
  String stored;
  if (prefs.begin(kPrefsNamespace, true)) {
    stored = prefs.getString(kPrefsJsonKey, "");
    prefs.end();
  }
  if (!tryAppendFromJson(stored)) {
    appendBakedFallback();
  }
}

void applyCurrent() {
  const FocusPoint& fp = s_ring[s_index];
  if (fp.is_home) {
    services::location::clearOverride();
  } else {
    services::location::setOverride(fp.lat, fp.lon);
  }
  ui::radar::rangeSetIndex(fp.default_range_idx);
}

}  // namespace

void init() {
  loadRing();
  // Always land on Home (slot 0) at boot. The setIndex() write path still
  // records the last-visited focus in NVS so the LAN portal / debugging
  // can see it; we just don't restore it, because coming back to whatever
  // focus was last on-screen before power-off is more surprising than
  // starting fresh at the pilot's home planning point.
  s_index = 0;
  applyCurrent();
}

void setIndex(size_t idx) {
  if (s_ring_count == 0 || idx >= s_ring_count) return;
  s_index = static_cast<uint8_t>(idx);
  applyCurrent();
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putUChar(kPrefsIndexKey, s_index);
    prefs.end();
  }
  s_overlay_shown_at_ms = millis();
  Serial.printf("focus: %s (%d)\n", s_ring[s_index].name, s_index);
}

const FocusPoint& current() { return s_ring[s_index]; }

size_t currentIndex() { return s_index; }

size_t count() { return s_ring_count; }

unsigned long overlayRemainingMs() {
  if (s_overlay_shown_at_ms == 0) return 0;
  const unsigned long elapsed = millis() - s_overlay_shown_at_ms;
  return (elapsed >= kOverlayMs) ? 0 : (kOverlayMs - elapsed);
}

void saveRingJson(const char* json) {
  if (json == nullptr) return;
  // Validate parse without touching the live ring; if it doesn't parse,
  // don't persist junk that init() would reject on next boot anyway.
  JsonDocument doc;
  if (deserializeJson(doc, json) || !doc.is<JsonArray>()) {
    Serial.println("focus: refusing to save invalid ring JSON");
    return;
  }
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) return;
  prefs.putString(kPrefsJsonKey, json);
  prefs.end();
  Serial.println("focus: ring JSON saved (takes effect on next reboot)");
}

String currentRingJson() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  // Home is synthetic — never included in the JSON. Only serialize the
  // user-editable airports (indexes 1..count-1).
  for (size_t i = 1; i < s_ring_count; ++i) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = s_ring[i].name;
    o["lat"] = s_ring[i].lat;
    o["lon"] = s_ring[i].lon;
    o["range_idx"] = s_ring[i].default_range_idx;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

}  // namespace services::focus
