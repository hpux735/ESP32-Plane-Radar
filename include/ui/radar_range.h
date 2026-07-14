#pragma once

#include <cstddef>
#include <cstdint>

namespace ui::radar {

/**
 * Range presets — the OUTER ring's distance is the range. Aviation-standard:
 * "10 nm range" means aircraft within 10 nm are on the display, with the
 * outermost ring at 10 nm.
 *
 *    5 nm  — pattern / airfield vicinity
 *   10 nm  — default; local traffic
 *   15 nm  — wider local
 *   25 nm  — metro / regional picture
 *
 * Physical distance stored in km because projection math uses km/deg. Label
 * value stored as an int nm so formatting is exact (no round-trip).
 */
struct RangePreset {
  int nm;          // label value (outer-ring range in nautical miles)
  float outer_km;  // physical distance to outer ring
};

constexpr float kKmPerNm = 1.852f;

constexpr RangePreset kRangePresets[] = {
    { 5,  5.0f * kKmPerNm},
    {10, 10.0f * kKmPerNm},
    {15, 15.0f * kKmPerNm},
    {25, 25.0f * kKmPerNm},
};

constexpr size_t kRangePresetCount =
    sizeof(kRangePresets) / sizeof(kRangePresets[0]);

/** Load saved range and distance units from flash. Call once after boot. */
void rangeInit();
/** Cycle preset and save to flash. */
void rangeNext();
/** Set preset by index; safe to call with any value (clamps). Persists. */
void rangeSetIndex(uint8_t idx);
const RangePreset& rangeCurrent();
uint8_t rangeIndex();
/** ADSB fetch radius (km): scaled to screen edge so beyond-ring dots have data. */
float fetchRadiusKm();

bool showRunways();
void saveRunwaysFromPortal(const char* checkbox_value);
/** Interpret a WiFiManager checkbox value: empty = unchecked, otherwise
 *  checked. Shared with wifi_setup.cpp for the layer-visibility checkboxes. */
bool portalCheckboxChecked(const char* value);
/** Format "5nm" / "10nm" / … for the given nautical-mile value. */
void formatRangeLabel(char* buf, size_t len, int nm);
void formatCurrentRangeLabel(char* buf, size_t len);
/** Reset persistent display settings (called on credential wipe). */
void unitsReset();

}  // namespace ui::radar
