#include "ui/weather_map.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "data/coastlines.h"
#include "data/land.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/weather.h"
#include "ui/radar_display.h"
#include "ui/radar_theme.h"

namespace ui::weather {
namespace {

// Weather-map projection: auto-fit — the center is the geometric middle
// of the current station set (NOT the user's home/focus), and the scale
// is picked so the farthest station lands ~12 px inside the bezel. This
// packs the fixed airport set as densely as possible into the round
// viewport regardless of which stations are on the list.
constexpr int   kProjectionPx     = 108;    // physical bezel is 120
constexpr int   kLabelMarginPx    = 14;     // reserve for label glyphs
constexpr float kKmPerDeg         = 111.0f;
constexpr float kE7               = 1e-7f;

// Populated by computeFit() on each frame — cheap for 11 stations.
float s_center_lat = 0.0f;
float s_center_lon = 0.0f;
float s_px_per_km  = 1.0f;

// Post-projection screen positions, after collision-nudge. Sized to a
// generous cap because the fixed weather station list never approaches
// this. Filled by placeStations() before draw.
constexpr size_t kMaxStations = 32;
int s_sx[kMaxStations] = {0};
int s_sy[kMaxStations] = {0};
int s_half_w[kMaxStations] = {0};  // measured from the real label glyphs

// Per-station AABB used for label collision. Horizontal half-width is
// measured from the actual rendered label (varies per ICAO — 'W' is
// nearly 2× 'I'). Vertical extent covers the dot on top plus the label
// stacked below. Labels are wider than tall, so an isotropic min-sep
// pushes vertical pairs too far while horizontal pairs (PAO/NUQ/SJC)
// still crash.
constexpr int kDotRadiusPx    = 4;
constexpr int kLabelHeightPx  = 14;
constexpr int kLabelOffsetPx  = 8;   // label baseline offset below dot center
constexpr int kFootprintPadPx = 2;

constexpr int kStationHalfH =
    (kDotRadiusPx + kLabelOffsetPx + kLabelHeightPx + kDotRadiusPx) / 2 +
    kFootprintPadPx;

const char* displayIcao(const char* icao) {
  return (icao[0] == 'K' && icao[1]) ? icao + 1 : icao;
}

void computeFit() {
  const services::weather::Station* st = services::weather::stations();
  const size_t n = services::weather::stationCount();
  if (n == 0) return;
  float min_lat = st[0].lat, max_lat = st[0].lat;
  float min_lon = st[0].lon, max_lon = st[0].lon;
  for (size_t i = 1; i < n; ++i) {
    if (st[i].lat < min_lat) min_lat = st[i].lat;
    if (st[i].lat > max_lat) max_lat = st[i].lat;
    if (st[i].lon < min_lon) min_lon = st[i].lon;
    if (st[i].lon > max_lon) max_lon = st[i].lon;
  }
  s_center_lat = (min_lat + max_lat) * 0.5f;
  s_center_lon = (min_lon + max_lon) * 0.5f;
  // Farthest station in km from the geometric center.
  float max_r_km = 0.0f;
  for (size_t i = 0; i < n; ++i) {
    const float dx = (st[i].lon - s_center_lon) * kKmPerDeg;
    const float dy = (st[i].lat - s_center_lat) * kKmPerDeg;
    const float r  = std::sqrt(dx * dx + dy * dy);
    if (r > max_r_km) max_r_km = r;
  }
  const float budget_px = static_cast<float>(kProjectionPx - kLabelMarginPx);
  s_px_per_km = (max_r_km > 0.0f) ? (budget_px / max_r_km) : 1.0f;
}

uint16_t categoryColor(services::weather::Category c) {
  switch (c) {
    case services::weather::Category::VFR:   return tft.color565( 40, 200,  60);
    case services::weather::Category::MVFR:  return tft.color565( 70, 130, 255);
    case services::weather::Category::IFR:   return tft.color565(240,  70,  70);
    case services::weather::Category::LIFR:  return tft.color565(220,  70, 200);
    default:                                 return tft.color565(120, 120, 120);
  }
}

void projectLatLon(float lat, float lon, int* out_x, int* out_y) {
  const float dx_km = (lon - s_center_lon) * kKmPerDeg;
  const float dy_km = (lat - s_center_lat) * kKmPerDeg;
  *out_x = radar::kCenterX + static_cast<int>(std::lroundf(dx_km * s_px_per_km));
  *out_y = radar::kCenterY - static_cast<int>(std::lroundf(dy_km * s_px_per_km));
}

bool insideDisc(int x, int y) {
  const int dx = x - radar::kCenterX;
  const int dy = y - radar::kCenterY;
  return dx * dx + dy * dy <= kProjectionPx * kProjectionPx;
}

// True when a segment or triangle's screen bbox overlaps the 240×240
// framebuffer at all. Correctly keeps big features whose *vertices*
// project off-screen but whose *interior* still covers the viewport —
// which is exactly what happens at high zoom levels with the baked
// Natural Earth polygons.
bool bboxOverlapsScreen(int min_x, int max_x, int min_y, int max_y) {
  if (max_x < 0 || min_x >= radar::kSize) return false;
  if (max_y < 0 || min_y >= radar::kSize) return false;
  return true;
}

// Land: iterate the baked triangles and fill them at weather zoom.
// Reject only when the triangle's screen bbox is fully off-frame —
// vertex-based "inside the projection disc" tests are wrong at high
// zoom, because a triangle whose three corners all project past the
// bezel can still cover the visible disc entirely (imagine a big
// Central Valley triangle when we're zoomed on the Bay).
void drawLand(lgfx::LGFXBase& gfx) {
  const uint16_t color = radar::kColorLand;
  for (size_t i = 0; i < data::land::kTriangleCount; ++i) {
    const data::land::Triangle& t = data::land::kTriangles[i];
    const data::land::Vertex& v0 = data::land::kVertices[t.v0];
    const data::land::Vertex& v1 = data::land::kVertices[t.v1];
    const data::land::Vertex& v2 = data::land::kVertices[t.v2];
    int x0, y0, x1, y1, x2, y2;
    projectLatLon(v0.lat_e7 * kE7, v0.lon_e7 * kE7, &x0, &y0);
    projectLatLon(v1.lat_e7 * kE7, v1.lon_e7 * kE7, &x1, &y1);
    projectLatLon(v2.lat_e7 * kE7, v2.lon_e7 * kE7, &x2, &y2);
    const int min_x = std::min({x0, x1, x2});
    const int max_x = std::max({x0, x1, x2});
    const int min_y = std::min({y0, y1, y2});
    const int max_y = std::max({y0, y1, y2});
    if (!bboxOverlapsScreen(min_x, max_x, min_y, max_y)) continue;
    gfx.fillTriangle(x0, y0, x1, y1, x2, y2, color);
  }
}

// Coastline: iterate polylines segment-by-segment. Reject only if the
// segment bbox is off-frame — same reasoning as drawLand.
void drawCoast(lgfx::LGFXBase& gfx) {
  const uint16_t color = tft.color565(radar::kBgR + 40, radar::kBgG + 60,
                                      radar::kBgB + 40);
  for (size_t i = 0; i < data::coastlines::kPolylineCount; ++i) {
    const data::coastlines::Polyline& pl = data::coastlines::kPolylines[i];
    for (uint16_t k = 1; k < pl.count; ++k) {
      const data::coastlines::Point& a = data::coastlines::kPoints[pl.start + k - 1];
      const data::coastlines::Point& b = data::coastlines::kPoints[pl.start + k];
      int ax, ay, bx, by;
      projectLatLon(a.lat_e7 * kE7, a.lon_e7 * kE7, &ax, &ay);
      projectLatLon(b.lat_e7 * kE7, b.lon_e7 * kE7, &bx, &by);
      const int min_x = std::min(ax, bx);
      const int max_x = std::max(ax, bx);
      const int min_y = std::min(ay, by);
      const int max_y = std::max(ay, by);
      if (!bboxOverlapsScreen(min_x, max_x, min_y, max_y)) continue;
      gfx.drawLine(ax, ay, bx, by, color);
    }
  }
}

// Project every station, measure its rendered label width, then relax
// overlapping pairs so their AABBs (dot + label) don't touch. Push each
// pair along the axis of *minimum penetration* — labels are wider than
// tall, so a vertical pair (OAK above HWD) needs only a small nudge
// while a horizontal chain (PAO/NUQ/SJC) needs a bigger one. gfx must
// already have the display font loaded and text size set to what
// drawStations() will render, since we measure via textWidth().
void placeStations(lgfx::LGFXBase& gfx) {
  const services::weather::Station* stations = services::weather::stations();
  const size_t n = services::weather::stationCount();
  for (size_t i = 0; i < n && i < kMaxStations; ++i) {
    projectLatLon(stations[i].lat, stations[i].lon, &s_sx[i], &s_sy[i]);
    const int label_w = gfx.textWidth(displayIcao(stations[i].icao));
    s_half_w[i] = std::max(kDotRadiusPx, label_w / 2) + kFootprintPadPx;
  }
  // 8 passes: axis-aware pushes converge more slowly than isotropic ones
  // when a station is boxed in on two sides, but 8 still costs nothing
  // for 11 stations.
  for (int pass = 0; pass < 8; ++pass) {
    bool moved = false;
    for (size_t i = 0; i < n && i < kMaxStations; ++i) {
      for (size_t j = i + 1; j < n && j < kMaxStations; ++j) {
        const int dx = s_sx[j] - s_sx[i];
        const int dy = s_sy[j] - s_sy[i];
        const int need_x = s_half_w[i] + s_half_w[j];
        const int need_y = 2 * kStationHalfH;
        const int ox = need_x - std::abs(dx);
        const int oy = need_y - std::abs(dy);
        if (ox <= 0 || oy <= 0) continue;
        if (ox <= oy) {
          const int sign = (dx >= 0) ? 1 : -1;
          const int push = (ox + 1) / 2;
          s_sx[i] -= sign * push;
          s_sx[j] += sign * push;
        } else {
          const int sign = (dy >= 0) ? 1 : -1;
          const int push = (oy + 1) / 2;
          s_sy[i] -= sign * push;
          s_sy[j] += sign * push;
        }
        moved = true;
      }
    }
    if (!moved) break;
  }
}

// Configures gfx with the label font/size that both placeStations() (for
// textWidth measurements) and drawStations() (for the actual drawString)
// need. Keeps the two paths from drifting.
void configureLabelFont(lgfx::LGFXBase& gfx) {
  displayFontEnsureLoaded(gfx);
  gfx.setTextSize(0.80f);
  gfx.setTextDatum(textdatum_t::top_center);
}

void drawStations(lgfx::LGFXBase& gfx) {
  const services::weather::Station* stations = services::weather::stations();
  const size_t n = services::weather::stationCount();

  for (size_t i = 0; i < n && i < kMaxStations; ++i) {
    const int sx = s_sx[i];
    const int sy = s_sy[i];
    if (!insideDisc(sx, sy)) continue;

    const uint16_t color = categoryColor(stations[i].category);
    gfx.fillCircle(sx, sy, 4, color);
    gfx.drawCircle(sx, sy, 4, radar::kColorBackground);  // subtle outline

    gfx.setTextColor(radar::kColorLabel, radar::kColorBackground);
    gfx.drawString(displayIcao(stations[i].icao), sx, sy + 8);
  }
}

void drawFreshness(lgfx::LGFXBase& gfx) {
  const unsigned long last = services::weather::lastUpdateMs();
  gfx.setTextSize(0.50f);
  gfx.setTextDatum(textdatum_t::top_center);
  gfx.setTextColor(radar::kColorGrid, radar::kColorBackground);
  char buf[24];
  if (last == 0) {
    std::strncpy(buf, "no data", sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
  } else {
    const unsigned long age_s = (millis() - last) / 1000;
    if (age_s < 60) std::snprintf(buf, sizeof(buf), "%lus ago", age_s);
    else            std::snprintf(buf, sizeof(buf), "%lum ago", age_s / 60);
  }
  gfx.drawString(buf, radar::kCenterX, 8);
}

}  // namespace

void refresh() {
  // Cache for 5 min — METARs update ~hourly, but SPECIALs can appear
  // any time and the extra freshness is cheap.
  constexpr unsigned long kTtlMs = 5UL * 60UL * 1000UL;
  const unsigned long last = services::weather::lastUpdateMs();
  const unsigned long now  = millis();
  if (last == 0 || (now - last) >= kTtlMs) {
    services::weather::update();
  }
}

void draw() {
  // Composite into the shared 240×240 sprite, then blit in a single
  // pushSprite. Drawing straight to the panel every ~1 s (as the
  // freshness updater does) produces a visible ~1 Hz flash — the
  // sprite hides the intermediate erase.
  LGFX_Sprite* sp = radarDisplayFrameSprite();
  lgfx::LGFXBase& gfx = sp ? static_cast<lgfx::LGFXBase&>(*sp)
                           : static_cast<lgfx::LGFXBase&>(tft);
  computeFit();
  configureLabelFont(gfx);   // must be set before placeStations() measures
  placeStations(gfx);
  gfx.fillScreen(radar::kColorBackground);
  drawLand(gfx);
  drawCoast(gfx);
  drawFreshness(gfx);
  configureLabelFont(gfx);   // drawFreshness stomps text size
  drawStations(gfx);
  // Bezel mask — same as the radar view. Keeps SDL visually matched to
  // the round physical panel.
  gfx.fillArc(radar::kCenterX, radar::kCenterY, radar::kSize + 8, 120, 0, 360,
              radar::kColorBackground);
  if (sp) sp->pushSprite(0, 0);
}

}  // namespace ui::weather
