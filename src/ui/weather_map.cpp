#include "ui/weather_map.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "data/coastlines.h"
#include "data/land.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/weather.h"
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

// Land: iterate the baked triangles and fill them at weather zoom. Each
// triangle spans three vertices already in the baked cache; we just
// re-project them here. Any triangle entirely outside the projection
// disc is dropped early. Triangles clipping the disc still draw and
// spill past the boundary — the bezel mask at the end catches the
// overflow.
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
    if (!insideDisc(x0, y0) && !insideDisc(x1, y1) && !insideDisc(x2, y2)) {
      continue;
    }
    gfx.fillTriangle(x0, y0, x1, y1, x2, y2, color);
  }
}

// Coastline: iterate polylines, quick-reject those wholly outside the
// disc, drawLine the survivors segment-by-segment. Simpler than a
// full-clip solution and looks fine at this zoom because coastline is
// dense (Peninsula, East Bay, Marin all present).
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
      if (!insideDisc(ax, ay) && !insideDisc(bx, by)) continue;
      gfx.drawLine(ax, ay, bx, by, color);
    }
  }
}

void drawStations(lgfx::LGFXBase& gfx) {
  const services::weather::Station* stations = services::weather::stations();
  const size_t n = services::weather::stationCount();
  displayFontEnsureLoaded(gfx);
  gfx.setTextSize(0.60f);
  gfx.setTextDatum(textdatum_t::top_center);

  for (size_t i = 0; i < n; ++i) {
    int sx = 0, sy = 0;
    projectLatLon(stations[i].lat, stations[i].lon, &sx, &sy);
    if (!insideDisc(sx, sy)) continue;

    const uint16_t color = categoryColor(stations[i].category);
    gfx.fillCircle(sx, sy, 4, color);
    gfx.drawCircle(sx, sy, 4, radar::kColorBackground);  // subtle outline

    // ICAO label above the dot (drop the leading K).
    const char* id = stations[i].icao;
    if (id[0] == 'K' && id[1]) id++;
    gfx.setTextColor(radar::kColorLabel, radar::kColorBackground);
    gfx.drawString(id, sx, sy + 7);
  }
}

void drawFreshness(lgfx::LGFXBase& gfx) {
  const unsigned long last = services::weather::lastUpdateMs();
  gfx.setTextSize(0.40f);
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
  computeFit();
  tft.fillScreen(radar::kColorBackground);
  drawLand(tft);
  drawCoast(tft);
  drawFreshness(tft);
  drawStations(tft);
  // Bezel mask — same as the radar view. Keeps SDL visually matched to
  // the round physical panel.
  tft.fillArc(radar::kCenterX, radar::kCenterY, radar::kSize + 8, 120, 0, 360,
              radar::kColorBackground);
}

}  // namespace ui::weather
