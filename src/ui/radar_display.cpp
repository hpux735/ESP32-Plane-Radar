#include "ui/radar_display.h"

#include <lgfx/v1/lgfx_fonts.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "services/focus_points.h"
#include "ui/coastline_overlay.h"
#include "ui/water_overlay.h"
#include "ui/label_layout.h"
#include "ui/land_overlay.h"
#include "ui/layer_style.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"
#include "ui/runway_overlay.h"

namespace fonts = lgfx::v1::fonts;

namespace ui {
namespace radar {

uint16_t kColorBackground = 0x0000;
uint16_t kColorGrid = 0x0320;
uint16_t kColorLabel = 0xFFFF;
uint16_t kColorAircraft = 0x001F;
uint16_t kColorTrackVector = 0xFFFF;
uint16_t kColorTagType = 0x5DFF;
uint16_t kColorTagAltitude = 0xFFE0;
uint16_t kColorRunway = 0x4D5F;
uint16_t kColorRunwayLabel = 0x7DFF;
uint16_t kColorLand = 0x0824;  // ~RGB(12, 20, 36) after color565 init
uint16_t kColorEmergency = 0xF800;  // pure red

}  // namespace radar

namespace {

bool s_label_metrics_ready = false;
bool s_cardinal_use_vlw = false;
bool s_scale_use_vlw = false;
float s_cardinal_vlw_size = 0.56f;
float s_scale_vlw_size = 0.50f;
float s_tag_vlw_size = 0.56f;
const lgfx::GFXfont* s_cardinal_gfx = &fonts::FreeSansBold12pt7b;
const lgfx::GFXfont* s_scale_gfx = &fonts::FreeSansBold9pt7b;
const lgfx::GFXfont* s_tag_gfx = &fonts::FreeSansBold12pt7b;

bool s_tag_label_metrics_ready = false;
bool s_tag_use_vlw = false;

int s_scale_label_max_w = 0;
int s_scale_label_h = 0;

lgfx::LovyanGFX* s_draw = &tft;
LGFX_Sprite s_frame(&tft);
bool s_frame_ready = false;

class DrawScope {
 public:
  explicit DrawScope(lgfx::LovyanGFX& gfx) : prev_(s_draw) { s_draw = &gfx; }
  ~DrawScope() { s_draw = prev_; }

 private:
  lgfx::LovyanGFX* prev_;
};

int absDiff(int a, int b) { return std::abs(a - b); }

int measureGfxHeight(const lgfx::GFXfont& font) {
  tft.setFont(&font);
  tft.setTextSize(1);
  return tft.fontHeight();
}

int measureVlwHeight(float size) {
  tft.setTextSize(size);
  return tft.fontHeight();
}

float findVlwSizeForHeight(int target_px) {
  float lo = 0.25f;
  float hi = 1.2f;
  for (int i = 0; i < 16; ++i) {
    const float mid = (lo + hi) * 0.5f;
    if (measureVlwHeight(mid) < target_px) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return hi;
}

void applyScaleStyle();

const lgfx::GFXfont* pickGfxFontClosest(
    int target_px, const lgfx::GFXfont* const* candidates, size_t count) {
  const lgfx::GFXfont* best = candidates[0];
  int best_diff = absDiff(measureGfxHeight(*best), target_px);

  for (size_t i = 1; i < count; ++i) {
    const int diff = absDiff(measureGfxHeight(*candidates[i]), target_px);
    if (diff < best_diff) {
      best_diff = diff;
      best = candidates[i];
    }
  }
  return best;
}

void initLabelMetrics() {
  if (s_label_metrics_ready) {
    return;
  }

  const int cardinal_target = radar::kCardinalLabelHeightPx;

  if (displayFontIsSmooth()) {
    s_cardinal_use_vlw = true;
    s_cardinal_vlw_size = findVlwSizeForHeight(cardinal_target);
    const int cardinal_h = measureVlwHeight(s_cardinal_vlw_size);
    const int scale_target = cardinal_h - radar::kScaleBelowCardinalPx;
    s_scale_use_vlw = true;
    s_scale_vlw_size = findVlwSizeForHeight(scale_target);
  } else {
    const lgfx::GFXfont* cardinal_candidates[] = {&fonts::FreeSansBold12pt7b,
                                                  &fonts::FreeSansBold9pt7b};
    s_cardinal_gfx =
        pickGfxFontClosest(cardinal_target, cardinal_candidates, 2);
    s_cardinal_use_vlw = false;

    const int cardinal_h = measureGfxHeight(*s_cardinal_gfx);
    const int scale_target = cardinal_h - radar::kScaleBelowCardinalPx;
    const lgfx::GFXfont* scale_candidates[] = {&fonts::FreeSansBold9pt7b,
                                               &fonts::FreeSansBold12pt7b};
    s_scale_gfx = pickGfxFontClosest(scale_target, scale_candidates, 2);
    s_scale_use_vlw = false;
  }

  applyScaleStyle();
  s_scale_label_h = tft.fontHeight();
  s_scale_label_max_w = 0;
  char label[12];
  for (size_t i = 0; i < radar::kRangePresetCount; ++i) {
    radar::formatRangeLabel(label, sizeof(label), radar::kRangePresets[i].nm);
    const int w = tft.textWidth(label);
    if (w > s_scale_label_max_w) {
      s_scale_label_max_w = w;
    }
  }

  s_label_metrics_ready = true;
}

void initTagLabelMetrics() {
  if (s_tag_label_metrics_ready) {
    return;
  }

  const int target = radar::kAircraftTagLabelHeightPx;
  if (displayFontIsSmooth()) {
    s_tag_use_vlw = true;
    s_tag_vlw_size = findVlwSizeForHeight(target);
  } else {
    const lgfx::GFXfont* tag_candidates[] = {&fonts::FreeSansBold12pt7b,
                                               &fonts::FreeSansBold9pt7b};
    s_tag_gfx = pickGfxFontClosest(target, tag_candidates, 2);
    s_tag_use_vlw = false;
  }

  s_tag_label_metrics_ready = true;
}

void initPalette() {
  radar::kColorBackground = tft.color565(radar::kBgR, radar::kBgG, radar::kBgB);
  radar::kColorGrid = tft.color565(radar::kGridR, radar::kGridG, radar::kGridB);
  radar::kColorLabel = tft.color565(255, 255, 255);
  // GC9A01 BGR panel: swap R/B in color565 so logical red renders red on screen.
  if (config::kDisplayRgbOrder) {
    radar::kColorAircraft =
        tft.color565(radar::kAircraftB, radar::kAircraftG, radar::kAircraftR);
  } else {
    radar::kColorAircraft =
        tft.color565(radar::kAircraftR, radar::kAircraftG, radar::kAircraftB);
  }
  radar::kColorTrackVector =
      tft.color565(radar::kTrackR, radar::kTrackG, radar::kTrackB);
  radar::kColorTagType =
      tft.color565(radar::kTagTypeR, radar::kTagTypeG, radar::kTagTypeB);
  radar::kColorTagAltitude =
      tft.color565(radar::kTagAltR, radar::kTagAltG, radar::kTagAltB);
  radar::kColorRunway =
      tft.color565(radar::kRunwayR, radar::kRunwayG, radar::kRunwayB);
  radar::kColorRunwayLabel = tft.color565(radar::kRunwayLabelR, radar::kRunwayLabelG,
                                          radar::kRunwayLabelB);
  radar::kColorLand =
      tft.color565(radar::kLandR, radar::kLandG, radar::kLandB);
  // Emergency: pure red on both platforms. On native (SDL) we want plain
  // RGB — no swap. On hardware (BGR panel per config::kDisplayRgbOrder)
  // we pre-swap so the panel's own swap yields red.
#ifdef USE_NATIVE
  radar::kColorEmergency = tft.color565(255, 0, 0);
#else
  radar::kColorEmergency = config::kDisplayRgbOrder
      ? tft.color565(0, 0, 255)
      : tft.color565(255, 0, 0);
#endif
}

constexpr float kKmPerDeg = 111.0f;

void offsetKmFromCenter(float lat, float lon, float* dx_km, float* dy_km,
                        float* dist_km) {
  *dx_km =
      static_cast<float>(lon - services::location::lon()) * kKmPerDeg;
  *dy_km =
      static_cast<float>(lat - services::location::lat()) * kKmPerDeg;
  *dist_km = sqrtf((*dx_km) * (*dx_km) + (*dy_km) * (*dy_km));
}

float innerRingMaxKm() {
  const float outer_km = radar::rangeCurrent().outer_km;
  return outer_km * (static_cast<float>(radar::kGridOuterRadius -
                                       radar::kAircraftInsideRingInsetPx) /
                     static_cast<float>(radar::kGridOuterRadius));
}

/** Flat lat/lon as x/y: 1° ≈ 111 km, north = screen up. */
void latLonToScreen(float lat, float lon, int* out_x, int* out_y) {
  const float outer_km = radar::rangeCurrent().outer_km;
  const float px_per_km = static_cast<float>(radar::kGridOuterRadius) / outer_km;

  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);

  *out_x = radar::kCenterX + static_cast<int>(lroundf(dx_km * px_per_km));
  *out_y = radar::kCenterY - static_cast<int>(lroundf(dy_km * px_per_km));
}

bool isInsideOuterRingKm(float dist_km) { return dist_km <= innerRingMaxKm(); }

int distSqFromCenter(int x, int y) {
  const int dx = x - radar::kCenterX;
  const int dy = y - radar::kCenterY;
  return dx * dx + dy * dy;
}

bool isInsideOuterRing(int x, int y) {
  const int max_r = radar::kGridOuterRadius - radar::kAircraftInsideRingInsetPx;
  return distSqFromCenter(x, y) <= max_r * max_r;
}

/** Rim dot from true bearing; always on screen edge (even if target is 50+ km away). */
bool beyondRingEdgeDotFromLatLon(float lat, float lon, int* out_x, int* out_y) {
  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);
  if (dist_km < 0.01f) {
    return false;
  }
  if (isInsideOuterRingKm(dist_km)) {
    return false;
  }

  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int rim_r = radar::kCenterX - radar::kBeyondRingScreenMarginPx;
  const float angle_rad = atan2f(dx_km, dy_km);

  *out_x = cx + static_cast<int>(lroundf(sinf(angle_rad) * rim_r));
  *out_y = cy - static_cast<int>(lroundf(cosf(angle_rad) * rim_r));
  return true;
}

void drawBeyondRingDot(int x, int y) {
  s_draw->fillSmoothCircle(x, y, radar::kBeyondRingDotRadiusPx,
                           radar::kColorAircraft);
}

void clipPointToOuterRing(int x0, int y0, int* x1, int* y1) {
  const int max_r = radar::kGridOuterRadius;
  const int max_r_sq = max_r * max_r;
  if (distSqFromCenter(*x1, *y1) <= max_r_sq) {
    return;
  }

  const int dx = *x1 - x0;
  const int dy = *y1 - y0;
  float t = 1.0f;
  for (int step = 0; step < 20; ++step) {
    const int px = x0 + static_cast<int>(lroundf(dx * t));
    const int py = y0 + static_cast<int>(lroundf(dy * t));
    if (distSqFromCenter(px, py) <= max_r_sq) {
      *x1 = px;
      *y1 = py;
      return;
    }
    t -= 0.05f;
    if (t <= 0.0f) {
      *x1 = x0;
      *y1 = y0;
      return;
    }
  }
}

int speedLineLengthPx(float gs_knots) {
  if (gs_knots <= 0.0f) {
    return 0;
  }

  // Fixed screen scale: 60 s horizon at gs, not tied to current range zoom.
  constexpr float kKmPerKnotPerHorizon =
      1.852f * radar::kAircraftTrackHorizonSec / 3600.0f;
  const float px =
      gs_knots * kKmPerKnotPerHorizon * radar::kGridOuterRadius /
      radar::kAircraftTrackRefOuterKm * radar::kAircraftTrackLengthScale;

  const int len = static_cast<int>(px + 0.5f);
  if (len < radar::kAircraftSpeedLineMinPx) {
    return radar::kAircraftSpeedLineMinPx;
  }
  return len;
}

void noseTip(int cx, int cy, float heading_deg, int* tip_x, int* tip_y) {
  constexpr float kDegToRad = 0.01745329252f;
  const float rad = heading_deg * kDegToRad;
  *tip_x = cx + static_cast<int>(lroundf(sinf(rad) * radar::kAircraftNoseLenPx));
  *tip_y = cy - static_cast<int>(lroundf(cosf(rad) * radar::kAircraftNoseLenPx));
}

void drawHeadingTriangle(int cx, int cy, float heading_deg, uint16_t color) {
  constexpr float kDegToRad = 0.01745329252f;
  const float rad = heading_deg * kDegToRad;
  const float sin_h = sinf(rad);
  const float cos_h = cosf(rad);

  int tip_x = 0;
  int tip_y = 0;
  noseTip(cx, cy, heading_deg, &tip_x, &tip_y);

  const int base_x =
      cx - static_cast<int>(lroundf(sin_h * static_cast<float>(radar::kAircraftTailLenPx)));
  const int base_y =
      cy + static_cast<int>(lroundf(cos_h * static_cast<float>(radar::kAircraftTailLenPx)));

  const int wing_x = static_cast<int>(lroundf(cos_h * radar::kAircraftTailHalfPx));
  const int wing_y = static_cast<int>(lroundf(sin_h * radar::kAircraftTailHalfPx));

  s_draw->fillTriangle(tip_x, tip_y, base_x + wing_x, base_y + wing_y,
                       base_x - wing_x, base_y - wing_y, color);
}

void drawSpeedVector(int cx, int cy, float heading_deg, float track_deg,
                     float gs_knots, uint16_t color) {
  const int len = speedLineLengthPx(gs_knots);
  if (len <= 0) {
    return;
  }

  int tip_x = 0;
  int tip_y = 0;
  noseTip(cx, cy, heading_deg, &tip_x, &tip_y);

  constexpr float kDegToRad = 0.01745329252f;
  const float rad = track_deg * kDegToRad;
  int ex = tip_x + static_cast<int>(lroundf(sinf(rad) * len));
  int ey = tip_y - static_cast<int>(lroundf(cosf(rad) * len));
  clipPointToOuterRing(tip_x, tip_y, &ex, &ey);
  if (ex == tip_x && ey == tip_y) {
    return;
  }
  s_draw->drawWideLine(tip_x, tip_y, ex, ey, radar::kAircraftTrackLineHalfWidth,
                       color);
}

void applyTagStyle() {
  if (s_tag_use_vlw) {
    displayFontSetSmoothSize(*s_draw, s_tag_vlw_size);
  } else {
    displayFontSetBitmap(*s_draw, s_tag_gfx);
  }
}

// Forward-declared below; used by tag placement now.
void datumTopLeftOffset(textdatum_t d, int tw, int th, int* dx, int* dy);
inline bool isEmergency(uint16_t squawk);

// Format altitude in ATC-standard hundreds of feet (e.g. 015 = 1500 ft,
// 350 = FL350). Ground → "GND". Unknown → empty string.
void formatAltitudeShort(int32_t alt_ft, char* out, size_t len) {
  if (len == 0) return;
  if (alt_ft == INT32_MIN) {
    std::snprintf(out, len, "GND");
    return;
  }
  const int hundreds = (alt_ft + 50) / 100;
  std::snprintf(out, len, "%03d", hundreds);
}

// FAA JO 7110.65: display trend indicator when |vertical rate| ≥ 500 fpm.
constexpr float kTrendThresholdFpm = 500.0f;
inline bool showTrend(float vs_fpm) {
  return std::fabs(vs_fpm) >= kTrendThresholdFpm;
}

// Tag mode alternation: flip ONCE per fetch cycle, but OFFSET by ~1.5 s
// from the fetch. Position updates and mode swaps never coincide — each
// event is its own visual beat, spaced ~1.5 s apart:
//
//   t = 0.0  fetch           (positions move, mode = A)
//   t = 1.5  mode-swap  → B
//   t = 3.0  fetch           (positions move, mode still B)
//   t = 4.5  mode-swap  → A
//   t = 6.0  fetch           (positions move, mode still A)
//   ...
//
// So each mode gets a full 3 s dwell and stays put while ONE fetch happens
// inside its window.
constexpr unsigned long kModeToggleOffsetMs = 1500;
inline bool tagShowsAltitude() {
  static bool s_show_alt = true;
  static unsigned long s_toggled_at_fetch = 0;
  const unsigned long fc = services::adsb::fetchCount();
  const unsigned long since = millis() - services::adsb::lastUpdateMs();
  if (fc != s_toggled_at_fetch && since >= kModeToggleOffsetMs) {
    s_show_alt = !s_show_alt;
    s_toggled_at_fetch = fc;
  }
  return s_show_alt;
}

// A small filled triangle: apex UP for climb, apex DOWN for descent. Drawn
// so its baseline aligns with the text baseline it sits next to.
constexpr int kTrendGlyphW = 5;
constexpr int kTrendGlyphH = 6;
constexpr int kTrendGap = 2;  // px between altitude text and triangle
void drawTrendGlyph(int left, int top, bool climb, uint16_t color) {
  const int r = left + kTrendGlyphW - 1;
  const int b = top + kTrendGlyphH - 1;
  const int cx = left + kTrendGlyphW / 2;
  if (climb) {
    s_draw->fillTriangle(cx, top, left, b, r, b, color);
  } else {
    s_draw->fillTriangle(left, top, r, top, cx, b, color);
  }
}

struct TagContent {
  const char* line1;  // callsign (or hex)
  char line2[10];     // formatted alt "015" or type "B738"
  bool line2_is_alt;
  bool draw_trend;
  bool trend_up;
  bool emergency;     // squawk in {7500,7600,7700}
  uint16_t line2_color;
};

constexpr int kEmergencyGap = 2;
inline int emGlyphWidth() { return s_draw->textWidth("EM"); }

TagContent buildTagContent(const services::adsb::Aircraft& p,
                           bool /*show_alt_unused*/) {
  TagContent c{};
  c.line1 = p.callsign[0] != '\0' ? p.callsign : "";
  c.draw_trend = false;
  c.emergency = isEmergency(p.squawk);
  // Altitude is the fundamental thing you want to know about a plane. Always
  // show it when known; fall back to aircraft type only when altitude data is
  // missing. (The previous 3-second altitude/type toggle was confusing —
  // half the time you'd look up and see "B738" and think "where's the
  // altitude?".)
  if (p.alt_ft != INT32_MIN) {
    c.line2_is_alt = true;
    c.line2_color = radar::kColorTagAltitude;
    formatAltitudeShort(p.alt_ft, c.line2, sizeof(c.line2));
    if (showTrend(p.vs_fpm)) {
      c.draw_trend = true;
      c.trend_up = p.vs_fpm > 0;
    }
  } else if (p.type[0] != '\0') {
    c.line2_is_alt = false;
    c.line2_color = radar::kColorTagType;
    std::snprintf(c.line2, sizeof(c.line2), "%s", p.type);
  }
  return c;
}

// --- Placement engine ------------------------------------------------------
// 8 anchor slots around the aircraft symbol; slot 0 = NE and steps CW.
// Each slot is (dx, dy from symbol center to text-box anchor point) plus
// the datum that keeps the text on the OUTWARD side of that anchor.

// Two rings of 8 anchor points each: the inner ring is the preferred
// placement (close leader, short lines). If everything in the inner ring
// collides, the outer ring provides fallback slots at a longer leader.
constexpr uint8_t kSlotCount = 16;

struct SlotDef {
  int dx;
  int dy;
  textdatum_t datum;
};
// Inner ring — ~14 px axis / ~12 px diagonal — icon extends ~12 px so the
// leader is short and the tag reads as clearly attached.
constexpr int kSlotAxis = 14;
constexpr int kSlotDiag = 12;
// Outer ring — ~26 px axis / ~22 px diagonal — used only when the inner
// slot for this preferred direction is taken by another tag.
constexpr int kSlotAxisFar = 26;
constexpr int kSlotDiagFar = 22;
const SlotDef kSlots[kSlotCount] = {
    // Inner ring (slots 0..7) — preferred.
    { kSlotDiag,    -kSlotDiag,    textdatum_t::bottom_left  },  // NE
    { kSlotAxis,     0,             textdatum_t::middle_left  },  // E
    { kSlotDiag,     kSlotDiag,    textdatum_t::top_left     },  // SE
    { 0,             kSlotAxis,    textdatum_t::top_center   },  // S
    {-kSlotDiag,     kSlotDiag,    textdatum_t::top_right    },  // SW
    {-kSlotAxis,     0,             textdatum_t::middle_right },  // W
    {-kSlotDiag,    -kSlotDiag,    textdatum_t::bottom_right },  // NW
    { 0,            -kSlotAxis,    textdatum_t::bottom_center},  // N
    // Outer ring (slots 8..15) — fallback when the inner slot is taken.
    { kSlotDiagFar, -kSlotDiagFar, textdatum_t::bottom_left  },
    { kSlotAxisFar,  0,             textdatum_t::middle_left  },
    { kSlotDiagFar,  kSlotDiagFar, textdatum_t::top_left     },
    { 0,             kSlotAxisFar, textdatum_t::top_center   },
    {-kSlotDiagFar,  kSlotDiagFar, textdatum_t::top_right    },
    {-kSlotAxisFar,  0,             textdatum_t::middle_right },
    {-kSlotDiagFar, -kSlotDiagFar, textdatum_t::bottom_right },
    { 0,            -kSlotAxisFar, textdatum_t::bottom_center},
};

// Per-aircraft hysteresis — remembered across frames, keyed by callsign.
struct TagMemory {
  char callsign[9];
  uint8_t slot;
  unsigned long last_used_ms;
};
constexpr size_t kMaxTagMemory = services::adsb::kMaxAircraft;
TagMemory s_tag_memory[kMaxTagMemory] = {};
size_t s_tag_memory_count = 0;

TagMemory* findMemory(const char* callsign) {
  for (size_t i = 0; i < s_tag_memory_count; ++i) {
    if (strncmp(s_tag_memory[i].callsign, callsign,
                     sizeof(s_tag_memory[i].callsign)) == 0) {
      return &s_tag_memory[i];
    }
  }
  return nullptr;
}

void rememberSlot(const char* callsign, uint8_t slot) {
  TagMemory* m = findMemory(callsign);
  if (m == nullptr) {
    if (s_tag_memory_count >= kMaxTagMemory) return;
    m = &s_tag_memory[s_tag_memory_count++];
  }
  strncpy(m->callsign, callsign, sizeof(m->callsign) - 1);
  m->callsign[sizeof(m->callsign) - 1] = '\0';
  // Only remember the compass direction (0..7); the ring is re-picked each
  // frame so hysteresis snaps back to the inner ring when it clears.
  m->slot = slot % 8;
  m->last_used_ms = millis();
}

// Purge memory entries not touched in the last N seconds so a completed
// flight doesn't force a stale placement on a new aircraft with the same
// (rare) callsign collision.
void gcTagMemory() {
  const unsigned long now = millis();
  size_t out = 0;
  for (size_t i = 0; i < s_tag_memory_count; ++i) {
    if (now - s_tag_memory[i].last_used_ms < 30000) {
      if (out != i) s_tag_memory[out] = s_tag_memory[i];
      ++out;
    }
  }
  s_tag_memory_count = out;
}

// Compute where an aircraft will be a few seconds ahead so tag placement
// doesn't flip between odd/even frames when a plane is skirting a candidate.
constexpr float kPredictSec = 6.0f;
void predictScreenPos(const services::adsb::Aircraft& p, int cur_x, int cur_y,
                      int* out_x, int* out_y) {
  if (p.gs_knots <= 0.0f) {
    *out_x = cur_x;
    *out_y = cur_y;
    return;
  }
  // px/sec derived from current range preset. Same formula as the render
  // pipeline's px_per_km applied over kPredictSec.
  const float outer_km = radar::rangeCurrent().outer_km;
  const float px_per_km =
      static_cast<float>(radar::kGridOuterRadius) / outer_km;
  const float km_per_sec = p.gs_knots * 1.852f / 3600.0f;
  const float dist_px = km_per_sec * kPredictSec * px_per_km;
  constexpr float kDegToRad = 3.14159265f / 180.0f;
  const float rad = p.track_deg * kDegToRad;
  *out_x = cur_x + static_cast<int>(std::lroundf(dist_px * sinf(rad)));
  *out_y = cur_y - static_cast<int>(std::lroundf(dist_px * cosf(rad)));
}

// Which side of the anchor the line sits on. Same rule for both lines within
// a tag; picked from the slot's datum to point AWAY from the aircraft.
enum class TagAlign : uint8_t { Left, Right, Center };

TagAlign alignFromDatum(textdatum_t d) {
  // *_right datums: tag hangs on the LEFT of the aircraft, so lines must be
  // right-aligned to keep line 2 flush with the leader-side edge.
  // Everything else (including vertical *_center slots) uses left-align for
  // visual consistency with the *_left tags.
  switch (d) {
    case textdatum_t::middle_right:
    case textdatum_t::top_right:
    case textdatum_t::bottom_right:
      return TagAlign::Right;
    default:
      return TagAlign::Left;
  }
}

struct TagPlacement {
  int anchor_x;
  int anchor_y;
  int rect_x;    // bounding-box left (widest of the two lines)
  int rect_y;    // bounding-box top
  int rect_w;    // bounding-box width  (= max of line1_w, line2_full_w)
  int rect_h;    // bounding-box height (= 2 line-heights)
  int line1_w;   // actual line-1 text extent
  int line2_w;   // actual line-2 group (text + optional trend) extent
  textdatum_t datum;
  TagAlign align;
  uint8_t slot;
  TagContent content;
};

int tagLine1Width(const TagContent& c) {
  return c.line1[0] != '\0' ? s_draw->textWidth(c.line1) : 0;
}

int tagLine2Width(const TagContent& c) {
  int w = c.line2[0] != '\0' ? s_draw->textWidth(c.line2) : 0;
  if (c.draw_trend) w += kTrendGap + kTrendGlyphW;
  // Reserve room for "EM" in the opposite corner. Bounding-box only —
  // actual glyph positioning happens in drawTagPlacement.
  if (c.emergency) w += kEmergencyGap + emGlyphWidth();
  return w;
}

// The tag's two lines can be different widths, so the "envelope" is an
// L-shape: line 1 on top spanning line1_w, line 2 on bottom spanning
// line2_w, both flush with the anchor-side edge of the bounding box.
// For collision checking we still walk both sub-rects (via labels::add
// twice in draw), but placement quick-rejects on the union so we don't
// have to enumerate combinations here.
bool tryPlaceSlot(uint8_t slot, int ax, int ay, const TagContent& c,
                  int px, int py, TagPlacement* out, bool strict) {
  applyTagStyle();
  const int line_h = s_draw->fontHeight();
  const int l1_w = tagLine1Width(c);
  const int l2_w = tagLine2Width(c);
  const int max_w = std::max(l1_w, l2_w);
  const int h = line_h * 2;
  const SlotDef& s = kSlots[slot];
  const int anchor_x = ax + s.dx;
  const int anchor_y = ay + s.dy;
  int off_x = 0;
  int off_y = 0;
  datumTopLeftOffset(s.datum, max_w, h, &off_x, &off_y);
  const int left = anchor_x + off_x;
  const int top = anchor_y + off_y;
  if (left < 1 || top < 1 ||
      left + max_w >= radar::kSize - 1 || top + h >= radar::kSize - 1) {
    return false;
  }
  const TagAlign align = alignFromDatum(s.datum);
  // Sub-rect for line 2 (the shorter line). If the two lines share the same
  // width no L-shape and the check reduces to the bounding rect.
  int line1_left = left;
  int line2_left = left;
  if (align == TagAlign::Right) {
    line2_left = left + max_w - l2_w;
  } else if (align == TagAlign::Center) {
    line2_left = left + (max_w - l2_w) / 2;
  }
  (void)px;  // predicted position keep-outs are already in labels::
  (void)py;
  // Strict pass avoids everything (labels + aircraft icons/vectors); relaxed
  // pass only avoids labels — used as a fallback so we prefer covering an
  // untagged aircraft icon over covering another tag.
  auto hit = [strict](int rx, int ry, int rw, int rh) {
    return strict ? labels::intersectsAny(rx, ry, rw, rh)
                  : labels::intersects(rx, ry, rw, rh);
  };
  if (hit(line1_left, top, l1_w, line_h)) return false;
  if (l2_w > 0 && hit(line2_left, top + line_h, l2_w, line_h)) return false;
  out->anchor_x = anchor_x;
  out->anchor_y = anchor_y;
  out->rect_x = left;
  out->rect_y = top;
  out->rect_w = max_w;
  out->rect_h = h;
  out->line1_w = l1_w;
  out->line2_w = l2_w;
  out->datum = s.datum;
  out->align = align;
  out->slot = slot;
  out->content = c;
  return true;
}

// Find a good slot. Preference order:
//   1. The callsign's remembered slot (hysteresis — steady placement frame
//      to frame).
//   2. Neighboring inner-ring slots (compass step around from the
//      preferred direction).
//   3. All outer-ring slots (farther anchor, longer leader).
//   4. If everything still collides, accept the remembered slot's overlap
//      rather than dropping the tag entirely.
bool pickTagPlacement(const services::adsb::Aircraft& p, int ax, int ay,
                      int px, int py, const TagContent& c,
                      TagPlacement* out) {
  const TagMemory* mem =
      (p.callsign[0] != '\0') ? findMemory(p.callsign) : nullptr;
  uint8_t start_inner = mem ? (mem->slot % 8) : 0;

  // Strict pass — dodge labels AND aircraft icons/vectors. Inner ring first
  // (short leader), then outer ring (longer leader, still no overlap).
  for (int step = 0; step < 8; ++step) {
    const uint8_t slot = (start_inner + step) % 8;
    if (tryPlaceSlot(slot, ax, ay, c, px, py, out, /*strict=*/true)) return true;
  }
  for (int step = 0; step < 8; ++step) {
    const uint8_t slot = 8 + ((start_inner + step) % 8);
    if (tryPlaceSlot(slot, ax, ay, c, px, py, out, /*strict=*/true)) return true;
  }
  // Relaxed pass — dodge labels only. Allow overlap with untagged aircraft
  // icons / track vectors, per user preference: "labels cover deprioritized
  // unlabeled aircraft rather than other labels".
  for (int step = 0; step < 8; ++step) {
    const uint8_t slot = (start_inner + step) % 8;
    if (tryPlaceSlot(slot, ax, ay, c, px, py, out, /*strict=*/false)) return true;
  }
  for (int step = 0; step < 8; ++step) {
    const uint8_t slot = 8 + ((start_inner + step) % 8);
    if (tryPlaceSlot(slot, ax, ay, c, px, py, out, /*strict=*/false)) return true;
  }
  // Last resort — accept overlapping a label too.
  return tryPlaceSlot(start_inner, ax, ay, c, px, py, out, /*strict=*/false) ||
         tryPlaceSlot(0, ax, ay, c, px, py, out, /*strict=*/false);
}

// Thin leader from icon edge to the tag anchor corner so the eye can
// visually connect a tag to its aircraft. Starts one icon-radius out from
// the symbol center so it doesn't visibly begin inside the triangle.
void drawLeaderLine(int icon_x, int icon_y, int anchor_x, int anchor_y) {
  const float dx = static_cast<float>(anchor_x - icon_x);
  const float dy = static_cast<float>(anchor_y - icon_y);
  const float len = std::sqrt(dx * dx + dy * dy);
  constexpr float kIconRadius = 7.0f;
  if (len <= kIconRadius + 2.0f) return;
  const int sx = icon_x + static_cast<int>(std::lroundf(dx * kIconRadius / len));
  const int sy = icon_y + static_cast<int>(std::lroundf(dy * kIconRadius / len));
  s_draw->drawLine(sx, sy, anchor_x, anchor_y, radar::kColorLabel);
}

// Compute the (x) origin of a text/glyph group of the given width inside a
// max_w-wide row, given the tag alignment. Left-align → 0 offset; right-align
// → flush to right edge; center-align → centered.
int alignOffset(TagAlign a, int max_w, int piece_w) {
  switch (a) {
    case TagAlign::Right:  return max_w - piece_w;
    case TagAlign::Center: return (max_w - piece_w) / 2;
    default:               return 0;
  }
}

void drawTagPlacement(int icon_x, int icon_y, const TagPlacement& tp) {
  applyTagStyle();
  const int line_h = s_draw->fontHeight();
  const TagContent& c = tp.content;
  s_draw->setTextDatum(textdatum_t::top_left);

  const int line1_x = tp.rect_x + alignOffset(tp.align, tp.rect_w, tp.line1_w);
  // For line 2 alignment we want the alt/type group to hug the leader edge
  // even when an EM tag is reserved on the other side. Use text-only width
  // for the alignment so EM sits in the free corner.
  int alt_type_w = c.line2[0] != '\0' ? s_draw->textWidth(c.line2) : 0;
  if (c.draw_trend) alt_type_w += kTrendGap + kTrendGlyphW;
  const int line2_x =
      tp.rect_x + alignOffset(tp.align, tp.rect_w, alt_type_w);

  // Leader connects the icon to the tag's near edge. With aligned lines the
  // "near edge" is a straight vertical (or horizontal for N/S slots) side,
  // and the leader ends flush with it — no dangling gap regardless of which
  // line is longer.
  int leader_end_x = tp.anchor_x;
  int leader_end_y = tp.anchor_y;
  (void)line2_x;  // used for text placement below
  drawLeaderLine(icon_x, icon_y, leader_end_x, leader_end_y);

  if (c.line1[0] != '\0') {
    s_draw->setTextColor(radar::kColorLabel, radar::kColorBackground);
    s_draw->drawString(c.line1, line1_x, tp.rect_y);
  }
  if (c.line2[0] != '\0') {
    const int line2_y = tp.rect_y + line_h;
    s_draw->setTextColor(c.line2_color, radar::kColorBackground);
    s_draw->drawString(c.line2, line2_x, line2_y);
    if (c.draw_trend) {
      const int text_w = s_draw->textWidth(c.line2);
      const int glyph_left = line2_x + text_w + kTrendGap;
      const int glyph_top =
          line2_y + (line_h - kTrendGlyphH) / 2;
      drawTrendGlyph(glyph_left, glyph_top, c.trend_up, c.line2_color);
    }
  }
  if (c.emergency) {
    // "EM" goes in the corner OPPOSITE the alt/type group — free space of
    // the L-shape. For a left-aligned tag alt/type is on the left, so EM
    // sits at the right; for right-aligned, EM sits on the left.
    const int line2_y = tp.rect_y + line_h;
    const int em_w = emGlyphWidth();
    const int em_x = (tp.align == TagAlign::Right)
                         ? tp.rect_x
                         : tp.rect_x + tp.rect_w - em_w;
    s_draw->setTextColor(radar::kColorEmergency, radar::kColorBackground);
    s_draw->drawString("EM", em_x, line2_y);
  }
  // Register the two lines as SEPARATE rects so the L-shape's empty corner
  // is free for other placements (later tags, icons, coastline lines, etc.).
  labels::add(line1_x - 1, tp.rect_y - 1, tp.line1_w + 2, line_h + 2);
  if (tp.line2_w > 0) {
    labels::add(line2_x - 1, tp.rect_y + line_h - 1, tp.line2_w + 2,
                line_h + 2);
  }
}

struct AircraftDrawItem {
  size_t index = 0;
  int x = 0;
  int y = 0;
  int dist_sq = 0;
};

struct BeyondDotDrawItem {
  int x = 0;
  int y = 0;
  int dist_sq = 0;
};

// FAA emergency squawk codes: 7500 hijack, 7600 comm failure, 7700 general
// emergency. Anything with one of these gets red rendering + a priority
// tag slot + an "EM" annotation.
inline bool isEmergency(uint16_t squawk) {
  return squawk == 7500 || squawk == 7600 || squawk == 7700;
}

// "How interesting is this aircraft on a spectator's radar toy?"
// - Altitude in feet: high-flying commercial traffic wins.
// - Ground speed × 20 ft-equivalent: fast movers rise up the list.
// - |vs| ÷ 5: climbers/descenders (takeoff / on-final) get a modest boost.
// Result is a coarse composite that reliably floats jets above pattern
// GA at wide zooms without needing airport lookups or explicit filters.
float clarityScore(const services::adsb::Aircraft& p) {
  const float alt =
      (p.alt_ft == INT32_MIN) ? 0.0f : static_cast<float>(p.alt_ft);
  float score = alt + p.gs_knots * 20.0f + std::fabs(p.vs_fpm) / 5.0f;
  // Emergency squawks always float to the top of the tag budget.
  if (isEmergency(p.squawk)) score += 1.0e9f;
  return score;
}

// Cap the number of full tags per frame, scaled to range preset. Wide views
// (25 nm) → few tags so the interesting traffic stands out; narrow views
// (5 nm) → generous budget because we've zoomed in specifically to poke at
// what's happening locally. Untagged aircraft still draw their symbol +
// track vector so the density is visible; they just don't clutter with text.
int tagBudget() {
  static constexpr int kBudget[] = {20, 15, 10, 6};  // per preset index 0..3
  const uint8_t idx = radar::rangeIndex();
  return kBudget[idx < 4 ? idx : 3];
}

void sortDrawItemsFarFirst(AircraftDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const AircraftDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

void sortBeyondDotsFarFirst(BeyondDotDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const BeyondDotDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

void drawAircraft() {
  initLabelMetrics();
  initTagLabelMetrics();  // calibrates s_tag_vlw_size to kAircraftTagLabelHeightPx

  const size_t n = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();

  AircraftDrawItem items[services::adsb::kMaxAircraft];
  BeyondDotDrawItem dots[services::adsb::kMaxAircraft];
  size_t draw_count = 0;
  size_t dot_count = 0;

  for (size_t i = 0; i < n; ++i) {
    float dx_km = 0.0f;
    float dy_km = 0.0f;
    float dist_km = 0.0f;
    offsetKmFromCenter(planes[i].lat, planes[i].lon, &dx_km, &dy_km, &dist_km);

    if (isInsideOuterRingKm(dist_km)) {
      int x = 0;
      int y = 0;
      latLonToScreen(planes[i].lat, planes[i].lon, &x, &y);
      items[draw_count].index = i;
      items[draw_count].x = x;
      items[draw_count].y = y;
      items[draw_count].dist_sq = distSqFromCenter(x, y);
      ++draw_count;
      continue;
    }

    int dot_x = 0;
    int dot_y = 0;
    if (!beyondRingEdgeDotFromLatLon(planes[i].lat, planes[i].lon, &dot_x,
                                     &dot_y)) {
      continue;
    }
    dots[dot_count].x = dot_x;
    dots[dot_count].y = dot_y;
    dots[dot_count].dist_sq = distSqFromCenter(dot_x, dot_y);
    ++dot_count;
  }

  sortBeyondDotsFarFirst(dots, dot_count);
  for (size_t d = 0; d < dot_count; ++d) {
    drawBeyondRingDot(dots[d].x, dots[d].y);
  }

  sortDrawItemsFarFirst(items, draw_count);
  // First pass: draw ALL aircraft symbols + track vectors and register their
  // keep-out rects (current + predicted position) so tag placement dodges them.
  int pred_x[services::adsb::kMaxAircraft];
  int pred_y[services::adsb::kMaxAircraft];
  for (size_t d = 0; d < draw_count; ++d) {
    const size_t i = items[d].index;
    const int x = items[d].x;
    const int y = items[d].y;
    const bool em = isEmergency(planes[i].squawk);
    drawSpeedVector(x, y, planes[i].nose_deg, planes[i].track_deg,
                    planes[i].gs_knots,
                    em ? radar::kColorEmergency : radar::kColorTrackVector);
    drawHeadingTriangle(x, y, planes[i].nose_deg,
                        em ? radar::kColorEmergency : radar::kColorAircraft);
    predictScreenPos(planes[i], x, y, &pred_x[d], &pred_y[d]);
    // Icon + vector endpoint + predicted position registered as SOFT keep-
    // outs — placement prefers to dodge them but will accept an overlap
    // rather than sitting on another tag.
    labels::addSoft(x - 8, y - 8, 16, 16);
    const int vec_len = speedLineLengthPx(planes[i].gs_knots);
    if (vec_len > 0) {
      constexpr float kDegToRad = 3.14159265f / 180.0f;
      const float rad = planes[i].track_deg * kDegToRad;
      const int ex = x + static_cast<int>(std::lroundf(vec_len * sinf(rad)));
      const int ey = y - static_cast<int>(std::lroundf(vec_len * cosf(rad)));
      labels::addSoft(ex - 4, ey - 4, 8, 8);
    }
    if (pred_x[d] != x || pred_y[d] != y) {
      labels::addSoft(pred_x[d] - 6, pred_y[d] - 6, 12, 12);
    }
  }

  // Second pass: score aircraft by "how interesting" and only tag the top N
  // (budget scales with range preset). Untagged planes still render with
  // symbol + track vector so the density stays visible, they just don't
  // add text clutter to the frame.
  if (!ui::layers::enabled(ui::layers::Layer::AircraftTags)) return;
  struct Scored {
    uint8_t d;
    float score;
  };
  Scored scored[services::adsb::kMaxAircraft];
  size_t scored_n = 0;
  for (size_t d = 0; d < draw_count; ++d) {
    const size_t i = items[d].index;
    if (planes[i].callsign[0] == '\0' && planes[i].type[0] == '\0' &&
        planes[i].alt_ft == INT32_MIN) {
      continue;  // nothing to say about this one
    }
    scored[scored_n++] = {static_cast<uint8_t>(d), clarityScore(planes[i])};
  }
  // Insertion sort by score DESC (small n, same style as other sorts here).
  for (size_t i = 1; i < scored_n; ++i) {
    const Scored key = scored[i];
    size_t j = i;
    while (j > 0 && scored[j - 1].score < key.score) {
      scored[j] = scored[j - 1];
      --j;
    }
    scored[j] = key;
  }

  gcTagMemory();
  // Emergency aircraft (score bumped by 1e9 in clarityScore) always tag
  // regardless of budget. Count them separately so a bunch of emergency
  // squawks can't eat everyone's slot; base budget still applies to normal
  // traffic after.
  size_t emergency_n = 0;
  for (size_t k = 0; k < scored_n; ++k) {
    const size_t i = items[scored[k].d].index;
    if (!isEmergency(planes[i].squawk)) break;
    ++emergency_n;
  }
  const size_t tag_limit =
      std::min(scored_n,
               emergency_n + static_cast<size_t>(tagBudget()));
  for (size_t k = 0; k < tag_limit; ++k) {
    const size_t d = scored[k].d;
    const size_t i = items[d].index;
    TagContent content = buildTagContent(planes[i], /*unused=*/true);
    TagPlacement tp;
    if (pickTagPlacement(planes[i], items[d].x, items[d].y, pred_x[d],
                         pred_y[d], content, &tp)) {
      drawTagPlacement(items[d].x, items[d].y, tp);
      if (planes[i].callsign[0] != '\0') {
        rememberSlot(planes[i].callsign, tp.slot);
      }
    }
  }
}

void applyCardinalStyle() {
  if (s_cardinal_use_vlw) {
    displayFontSetSmoothSize(*s_draw, s_cardinal_vlw_size);
  } else {
    displayFontSetBitmap(*s_draw, s_cardinal_gfx);
  }
}

void applyScaleStyle() {
  if (s_scale_use_vlw) {
    displayFontSetSmoothSize(*s_draw, s_scale_vlw_size);
  } else {
    displayFontSetBitmap(*s_draw, s_scale_gfx);
  }
}

// Given a datum, return (dx, dy) from the anchor point to the top-left
// corner of the resulting text box. Used for fill-rect + occupancy tracking.
void datumTopLeftOffset(textdatum_t d, int tw, int th, int* dx, int* dy) {
  *dx = 0;
  *dy = 0;
  switch (d) {
    case textdatum_t::top_center:    *dx = -tw / 2; break;
    case textdatum_t::top_right:     *dx = -tw;     break;
    case textdatum_t::middle_left:                  *dy = -th / 2; break;
    case textdatum_t::middle_center: *dx = -tw / 2; *dy = -th / 2; break;
    case textdatum_t::middle_right:  *dx = -tw;     *dy = -th / 2; break;
    case textdatum_t::bottom_left:                  *dy = -th;     break;
    case textdatum_t::bottom_center: *dx = -tw / 2; *dy = -th;     break;
    case textdatum_t::bottom_right:  *dx = -tw;     *dy = -th;     break;
    default: break;
  }
}

void drawCardinalLabel(const char* text, int x, int y, textdatum_t datum) {
  applyCardinalStyle();
  const int tw = s_draw->textWidth(text);
  const int th = s_draw->fontHeight();
  int ox = 0;
  int oy = 0;
  datumTopLeftOffset(datum, tw, th, &ox, &oy);
  s_draw->setTextDatum(datum);
  s_draw->setTextColor(radar::kColorLabel, radar::kColorBackground);
  s_draw->drawString(text, x, y);
  labels::add(x + ox, y + oy, tw, th);
}

void drawGridRing(int cx, int cy, int r, uint16_t color) {
  if (r <= 0) {
    return;
  }
  const int thickness =
      std::max(1, static_cast<int>(radar::kGridStrokeHalfWidth * 2.0f));
  for (int i = 0; i < thickness && r - i > 0; ++i) {
    s_draw->drawCircle(cx, cy, r - i, color);
  }
}

void drawRings(int cx, int cy, int outer_radius) {
  for (int i = 1; i <= radar::kRingCount; ++i) {
    const int r = (outer_radius * i) / radar::kRingCount;
    drawGridRing(cx, cy, r, radar::kColorGrid);
  }
}

void drawCrosshairs(int cx, int cy, int radius, uint16_t color) {
  s_draw->drawWideLine(cx, cy - radius, cx, cy + radius,
                       radar::kGridStrokeHalfWidth, color);
  s_draw->drawWideLine(cx - radius, cy, cx + radius, cy,
                       radar::kGridStrokeHalfWidth, color);
}

void drawCardinalLabels() {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int edge = radar::kSize - 1;

  drawCardinalLabel("N", cx, radar::kCardinalNorthOffsetY, textdatum_t::top_center);
  drawCardinalLabel("S", cx, edge + radar::kCardinalSouthOffsetY,
                    textdatum_t::bottom_center);
  drawCardinalLabel("W", 0, cy, textdatum_t::middle_left);
  drawCardinalLabel("E", edge, cy, textdatum_t::middle_right);
}

// Scale label sits INSIDE the outer ring near the top, ~12° east of N by
// default (or ~348° / west of N as second choice). Text fully inside the
// ring so ring lines don't cross through the glyphs (which was making "nm"
// read as "mm"). White color so it visually reads as a label, not another
// grid element. Walks outward from N in symmetric E/W steps if the default
// position collides with cardinals or airport labels.
void drawScaleLabel(int cx, int cy, int outer_radius) {
  char scale_label[12];
  radar::formatCurrentRangeLabel(scale_label, sizeof(scale_label));

  applyScaleStyle();
  const int tw = s_draw->textWidth(scale_label);
  const int th = s_draw->fontHeight();
  const int box_w = tw + 2;
  const int box_h = th + 2;
  constexpr float kPi = 3.14159265f;

  // Text sits fully inside the outer ring — radial-outer edge is 2 px inside
  // the ring line. half_radial blends box_w and box_h by angle so the box
  // fits regardless of orientation.
  auto anchorAt = [&](int theta_deg, int* out_x, int* out_y) {
    const float rad = theta_deg * kPi / 180.0f;
    const float sin_t = std::sin(rad);
    const float cos_t = std::cos(rad);
    const float half_radial =
        std::fabs(sin_t) * (box_w * 0.5f) + std::fabs(cos_t) * (box_h * 0.5f);
    const float er = static_cast<float>(outer_radius) - half_radial - 2.0f;
    *out_x = cx + static_cast<int>(std::lroundf(er * sin_t));
    *out_y = cy - static_cast<int>(std::lroundf(er * cos_t));
  };

  auto trial = [&](int theta_deg, int* out_x, int* out_y) -> bool {
    int ax = 0;
    int ay = 0;
    anchorAt(theta_deg, &ax, &ay);
    const int left = ax - box_w / 2;
    const int top = ay - box_h / 2;
    if (labels::intersects(left, top, box_w, box_h)) return false;
    *out_x = ax;
    *out_y = ay;
    return true;
  };

  int px = 0;
  int py = 0;
  bool found = false;
  // Preferred: 12° east of N. Fallback: 348° (west of N), then step outward
  // symmetrically. 12° gives a few pixels of clearance from the N crosshair
  // for typical range labels (34 px wide) at outer_radius 107.
  constexpr int kBaseDeg = 12;
  constexpr int kStepDeg = 4;
  for (int delta = 0; delta <= 90 && !found; delta += kStepDeg) {
    if (trial(kBaseDeg + delta, &px, &py)) { found = true; break; }
    if (trial(360 - kBaseDeg - delta, &px, &py)) { found = true; break; }
  }
  if (!found) {
    // Continue past E/W into the south half.
    for (int delta = 94; delta <= 180 && !found; delta += kStepDeg) {
      if (trial(kBaseDeg + delta, &px, &py)) { found = true; break; }
      if (trial(360 - kBaseDeg - delta, &px, &py)) { found = true; break; }
    }
  }
  if (!found) {
    anchorAt(kBaseDeg, &px, &py);
  }

  s_draw->setTextDatum(textdatum_t::middle_center);
  // Same green as the grid — it's SA reference, not a focal element.
  s_draw->setTextColor(radar::kColorGrid);
  s_draw->drawString(scale_label, px, py);
  const int left = px - box_w / 2;
  const int top = py - box_h / 2;
  labels::add(left, top, box_w, box_h);
}

template <typename Gfx>
void drawStaticGrid(Gfx& gfx) {
  labels::reset();
  initLabelMetrics();
  const DrawScope scope(gfx);
  displayFontEnsureLoaded(gfx);
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int grid_r = radar::kGridOuterRadius;

  gfx.fillScreen(radar::kColorBackground);
  initPalette();
  // Land goes down FIRST — under the grid, coastline, labels, aircraft.
  // Triangles may spill past the outer ring; the fillArc mask below then
  // paints the ring's exterior back to background, so the disc reads clean.
  land::draw(gfx);
  // Lakes: paint water polygons back to background over the land tint
  // so they read as bodies of water carved into land. Global data from
  // ne_10m_lakes (Section::Water in the tile pyramid).
  water::draw(gfx);
  gfx.fillArc(cx, cy, radar::kSize, grid_r + 1, 0, 360,
              radar::kColorBackground);
  drawRings(cx, cy, grid_r);
  drawCrosshairs(cx, cy, grid_r, radar::kColorGrid);
  // Coastline sits over the land tint (delineates the boundary) and under
  // labels/aircraft. Also carries rivers (folded into Section::Coast by
  // build_tiles.py — same polyline geometry, same render path). No
  // labels to register.
  coastline::draw(gfx);
  // Order matters: airport labels register bounding rects with labels::,
  // then the scale label dodges around them. (N/E/S/W cardinals were
  // removed — north is always up on a radar, so those pixels are wasted.)
  runway::drawLargeAirportRunways(gfx);
  drawScaleLabel(cx, cy, grid_r);
  gfx.setTextDatum(textdatum_t::top_left);
}

bool ensureFrameSprite() {
  if (s_frame_ready) {
    return true;
  }
  // 8bpp palette mode: 240x240 sprite = ~57 KB instead of ~115 KB at 16bpp.
  // The ESP32-C3 SuperMini has no PSRAM and only ~80 KB free heap at
  // steady state (WiFi + HTTPClient + TLS + ArduinoJson eat the rest), so
  // 16bpp allocation reliably fails and drops us into the direct-to-panel
  // fallback — the visible ~5 s clear/redraw flicker on every ADS-B fetch.
  // The radar palette is <32 distinct colors, so 8bpp is lossless for us.
  s_frame.setColorDepth(8);
  if (!s_frame.createSprite(radar::kSize, radar::kSize)) {
    Serial.println("radar: frame sprite alloc failed");
    return false;
  }
  s_frame_ready = true;
  return true;
}

// Double-buffered frame: composite the grid AND aircraft into the off-screen
// sprite, then blit it to the panel in a single pushSprite. Because the panel
// is updated in one pass, labels never show an erase/redraw gap — no flicker.
// The GC9A01 is a physically ROUND 240 px panel — pixels outside a disc of
// radius ~120 are hidden by the bezel. Paint them back to background as a
// final step so (a) the SDL emulator visually matches what hardware shows,
// and (b) anything a code path accidentally drew into the corners doesn't
// mask a real fix. The mask is fillArc from just past the physical panel
// edge to the corner.
constexpr int kPhysicalPanelRadius = 120;

void applyBezelMask(lgfx::LGFXBase& gfx) {
  gfx.fillArc(radar::kCenterX, radar::kCenterY, radar::kSize + 8,
              kPhysicalPanelRadius, 0, 360, radar::kColorBackground);
}

void renderFrame() {
  drawStaticGrid(s_frame);  // opens its own DrawScope(s_frame)
  {
    const DrawScope scope(s_frame);
    drawAircraft();
  }
  applyBezelMask(s_frame);
  s_frame.pushSprite(0, 0);
  tft.setTextDatum(textdatum_t::top_left);
}

}  // namespace

void radarDisplayDraw() {
  initPalette();
  initLabelMetrics();

  if (ensureFrameSprite()) {
    renderFrame();
    return;
  }

  // Fallback when the sprite can't be allocated: draw straight to the panel.
  const DrawScope scope(tft);
  drawStaticGrid(tft);
  drawAircraft();
  applyBezelMask(tft);
  tft.setTextDatum(textdatum_t::top_left);
}

void radarDisplayRefreshAircraft() {
  initPalette();

  if (ensureFrameSprite()) {
    renderFrame();
    return;
  }

  radarDisplayDraw();
}

LGFX_Sprite* radarDisplayFrameSprite() {
  return ensureFrameSprite() ? &s_frame : nullptr;
}

}  // namespace ui
