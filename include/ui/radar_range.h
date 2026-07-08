#pragma once

#include <cstddef>
#include <cstdint>

namespace ui::radar {

/**
 * Range presets (label on ring 3 = ¾ of outer radius).
 *
 * Aviation-standard nautical miles. Physical distances stored in km (kept
 * that way for projection math which uses km/deg); nm is derived at display
 * time. Presets sized to give useful ADS-B coverage:
 *    5 nm  — pattern / airfield vicinity
 *   10 nm  — default; local traffic
 *   15 nm  — wider local
 *   25 nm  — metro / regional picture
 *
 * Outer radius (for aircraft math) is ring-3 distance ÷ 0.75.
 */
struct RangePreset {
  float ring3_km;
  float outer_km;
};

constexpr float kRing3ToOuterKm = 4.0f / 3.0f;
constexpr float kKmPerNm = 1.852f;

constexpr RangePreset kRangePresets[] = {
    { 5.0f * kKmPerNm,  5.0f * kKmPerNm * kRing3ToOuterKm},  //  5 nm
    {10.0f * kKmPerNm, 10.0f * kKmPerNm * kRing3ToOuterKm},  // 10 nm
    {15.0f * kKmPerNm, 15.0f * kKmPerNm * kRing3ToOuterKm},  // 15 nm
    {25.0f * kKmPerNm, 25.0f * kKmPerNm * kRing3ToOuterKm},  // 25 nm
};

constexpr size_t kRangePresetCount =
    sizeof(kRangePresets) / sizeof(kRangePresets[0]);

/** Load saved range and distance units from flash. Call once after boot. */
void rangeInit();
/** Cycle preset and save to flash. */
void rangeNext();
const RangePreset& rangeCurrent();
uint8_t rangeIndex();
/** ADSB fetch radius (km): scaled to screen edge so beyond-ring dots have data. */
float fetchRadiusKm();

bool showRunways();
void saveRunwaysFromPortal(const char* checkbox_value);
/** Formats "5nm" / "10nm" / … from an internal km distance. */
void formatRing3Label(char* buf, size_t len, float ring3_km);
void formatCurrentRing3Label(char* buf, size_t len);
/** Reset persistent display settings (called on credential wipe). */
void unitsReset();

}  // namespace ui::radar
