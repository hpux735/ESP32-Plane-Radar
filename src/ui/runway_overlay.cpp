#include "ui/runway_overlay.h"

#include <lgfx/v1/lgfx_fonts.hpp>

#include <cmath>
#include <cstdlib>

#include "data/tile_math.h"
#include "data/tile_reader.h"
#include "data/tile_store.h"
#include "hardware/display_font.h"
#include "services/radar_location.h"
#include "ui/label_layout.h"
#include "ui/layer_style.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"

#include <cstring>

namespace fonts = lgfx::v1::fonts;

namespace ui::runway {
namespace {

constexpr float kKmPerDeg = 111.0f;
constexpr size_t kMaxAirportLabels = 32;

bool s_runway_label_ready = false;
bool s_runway_label_use_vlw = false;
float s_runway_label_vlw_size = 0.38f;
const lgfx::GFXfont* s_runway_label_gfx = &fonts::FreeSansBold12pt7b;

int measureVlwHeight(lgfx::LGFXBase& gfx, float size) {
  gfx.setTextSize(size);
  return gfx.fontHeight();
}

float findVlwSizeForHeight(lgfx::LGFXBase& gfx, int target_px) {
  float lo = 0.2f;
  float hi = 1.2f;
  for (int i = 0; i < 14; ++i) {
    const float mid = (lo + hi) * 0.5f;
    if (measureVlwHeight(gfx, mid) < target_px) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return hi;
}

void initRunwayLabelStyle(lgfx::LGFXBase& gfx) {
  if (s_runway_label_ready) {
    return;
  }

  const int target = radar::kRunwayLabelHeightPx;
  if (displayFontIsSmooth()) {
    s_runway_label_use_vlw = true;
    s_runway_label_vlw_size = findVlwSizeForHeight(gfx, target);
  } else {
    s_runway_label_gfx = &fonts::FreeSansBold12pt7b;
    s_runway_label_use_vlw = false;
  }
  s_runway_label_ready = true;
}

void applyRunwayLabelStyle(lgfx::LGFXBase& gfx) {
  if (s_runway_label_use_vlw) {
    displayFontSetSmoothSize(gfx, s_runway_label_vlw_size);
  } else {
    displayFontSetBitmap(gfx, s_runway_label_gfx);
  }
}

float e7ToDeg(int32_t e7) { return static_cast<float>(e7) * 1e-7f; }

void offsetKmFromCenter(float lat, float lon, float* dx_km, float* dy_km,
                        float* dist_km) {
  *dx_km =
      static_cast<float>(lon - services::location::lon()) * kKmPerDeg;
  *dy_km =
      static_cast<float>(lat - services::location::lat()) * kKmPerDeg;
  *dist_km = sqrtf((*dx_km) * (*dx_km) + (*dy_km) * (*dy_km));
}

void latLonToScreen(float lat, float lon, int* out_x, int* out_y) {
  const float outer_km = radar::rangeCurrent().outer_km;
  const float px_per_km =
      static_cast<float>(radar::kGridOuterRadius) / outer_km;

  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);

  *out_x = radar::kCenterX + static_cast<int>(lroundf(dx_km * px_per_km));
  *out_y = radar::kCenterY - static_cast<int>(lroundf(dy_km * px_per_km));
}

int distSqFromCenter(int x, int y) {
  const int dx = x - radar::kCenterX;
  const int dy = y - radar::kCenterY;
  return dx * dx + dy * dy;
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

bool segmentIntersectsDisc(int x0, int y0, int x1, int y1) {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int r = radar::kGridOuterRadius;
  const int r_sq = r * r;

  if (distSqFromCenter(x0, y0) <= r_sq || distSqFromCenter(x1, y1) <= r_sq) {
    return true;
  }

  const int dx = x1 - x0;
  const int dy = y1 - y0;
  const int fx = x0 - cx;
  const int fy = y0 - cy;
  const int a = dx * dx + dy * dy;
  if (a == 0) {
    return false;
  }
  const int b = 2 * (fx * dx + fy * dy);
  const int c = fx * fx + fy * fy - r_sq;
  int disc = b * b - 4 * a * c;
  if (disc < 0) {
    return false;
  }
  disc = static_cast<int>(sqrtf(static_cast<float>(disc)));
  const float inv2a = 1.0f / (2.0f * static_cast<float>(a));
  const float t0 = (-static_cast<float>(b) - disc) * inv2a;
  const float t1 = (-static_cast<float>(b) + disc) * inv2a;
  return (t0 >= 0.0f && t0 <= 1.0f) || (t1 >= 0.0f && t1 <= 1.0f);
}

// Convert a text datum to the (dx, dy) offset from the anchor point to the
// top-left corner of the resulting text box. Used to compute the fill rect
// and to register the label with the layout registry.
void datumOffset(textdatum_t datum, int tw, int th, int* dx, int* dy) {
  *dx = 0;
  *dy = 0;
  switch (datum) {
    case textdatum_t::top_center:    *dx = -tw / 2; break;
    case textdatum_t::top_right:     *dx = -tw;     break;
    case textdatum_t::middle_left:                  *dy = -th / 2; break;
    case textdatum_t::middle_center: *dx = -tw / 2; *dy = -th / 2; break;
    case textdatum_t::middle_right:  *dx = -tw;     *dy = -th / 2; break;
    case textdatum_t::bottom_left:                  *dy = -th;     break;
    case textdatum_t::bottom_center: *dx = -tw / 2; *dy = -th;     break;
    case textdatum_t::bottom_right:  *dx = -tw;     *dy = -th;     break;
    default: break;  // top_left: (0, 0)
  }
}

void drawAirportIdentLabel(lgfx::LGFXBase& gfx, const char* ident, int mx,
                           int my, textdatum_t datum) {
  const int tw = gfx.textWidth(ident);
  const int th = gfx.fontHeight();
  constexpr int kPadX = 2;
  constexpr int kPadY = 1;

  int ox = 0;
  int oy = 0;
  datumOffset(datum, tw, th, &ox, &oy);
  const int left = mx + ox - kPadX;
  const int top = my + oy - kPadY;
  const int w = tw + kPadX * 2;
  const int h = th + kPadY * 2;
  gfx.fillRect(left, top, w, h, radar::kColorBackground);
  gfx.setTextDatum(datum);
  gfx.setTextColor(radar::kColorRunwayLabel, radar::kColorBackground);
  gfx.drawString(ident, mx, my);
  labels::add(left, top, w, h);
}

template <typename RunwayT>
bool drawRunwayLineT(lgfx::LGFXBase& gfx, const RunwayT& rw) {
  const float le_lat = e7ToDeg(rw.le_lat_e7);
  const float le_lon = e7ToDeg(rw.le_lon_e7);
  const float he_lat = e7ToDeg(rw.he_lat_e7);
  const float he_lon = e7ToDeg(rw.he_lon_e7);

  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;
  latLonToScreen(le_lat, le_lon, &x0, &y0);
  latLonToScreen(he_lat, he_lon, &x1, &y1);

  if (!segmentIntersectsDisc(x0, y0, x1, y1)) {
    return false;
  }

  clipPointToOuterRing(x0, y0, &x1, &y1);
  clipPointToOuterRing(x1, y1, &x0, &y0);

  gfx.drawWideLine(x0, y0, x1, y1, radar::kRunwayLineHalfWidth,
                   radar::kColorRunway);
  return true;
}

void offsetLabelFromCenter(int ax, int ay, int* lx, int* ly) {
  const int dx = ax - radar::kCenterX;
  const int dy = ay - radar::kCenterY;
  const float len = sqrtf(static_cast<float>(dx * dx + dy * dy));
  const int gap = radar::kRunwayLabelGapPx;
  if (len < 1.0f) {
    *lx = ax;
    *ly = ay - gap;
    return;
  }
  *lx = ax + static_cast<int>(lroundf(dx / len * static_cast<float>(gap)));
  *ly = ay + static_cast<int>(lroundf(dy / len * static_cast<float>(gap)));
}

void clipPointOntoOuterRing(int* x, int* y) {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int r = radar::kGridOuterRadius;
  const int dx = *x - cx;
  const int dy = *y - cy;
  const int d_sq = dx * dx + dy * dy;
  const int r_sq = r * r;
  if (d_sq <= r_sq || d_sq == 0) {
    return;
  }
  const float scale = static_cast<float>(r) / sqrtf(static_cast<float>(d_sq));
  *x = cx + static_cast<int>(lroundf(static_cast<float>(dx) * scale));
  *y = cy + static_cast<int>(lroundf(static_cast<float>(dy) * scale));
}

// Place label vertically offset from the marker on the side further from
// center. This keeps text off horizontal runway lines (major runways are
// often near-E/W) and prevents E/W-side airports from having their labels
// pushed off-screen by a full outward-radial datum.
//   Marker in top half (dy < 0)  → label ABOVE marker → bottom_center datum
//   Marker in bottom half (dy > 0) → label BELOW marker → top_center datum
// Prefer placement AWAY from center (above marker if marker is in top half,
// below if bottom half). If that would collide with a previously-drawn
// label (cardinal or another airport), flip to the other side. Fallback:
// use preferred side and accept overlay.
template <typename AirportT>
void drawAirportLabelT(lgfx::LGFXBase& gfx, const AirportT& ap) {
  int ax = 0;
  int ay = 0;
  latLonToScreen(e7ToDeg(ap.lat_e7), e7ToDeg(ap.lon_e7), &ax, &ay);
  clipPointOntoOuterRing(&ax, &ay);

  const int dy = ay - radar::kCenterY;
  constexpr int kGap = 12;
  const int tw = gfx.textWidth(ap.ident);
  const int th = gfx.fontHeight();
  const int box_w = tw + 4;   // matches drawAirportIdentLabel padding
  const int box_h = th + 2;

  auto rectFor = [&](bool below, int* rx, int* ry) {
    // top-left of text box for the given orientation
    *rx = ax - box_w / 2;
    *ry = below ? (ay + kGap - 1) : (ay - kGap - th - 1);
  };

  const bool prefer_below = (dy >= 0);
  int rx = 0;
  int ry = 0;
  rectFor(prefer_below, &rx, &ry);
  bool below = prefer_below;
  if (labels::intersects(rx, ry, box_w, box_h)) {
    int rx2 = 0, ry2 = 0;
    rectFor(!prefer_below, &rx2, &ry2);
    if (!labels::intersects(rx2, ry2, box_w, box_h)) below = !prefer_below;
  }

  const int label_y = below ? (ay + kGap) : (ay - kGap);
  const textdatum_t datum =
      below ? textdatum_t::top_center : textdatum_t::bottom_center;
  drawAirportIdentLabel(gfx, ap.ident, ax, label_y, datum);
}

}  // namespace

// Adapter shapes so the existing drawRunwayLineT / drawAirportLabelT
// templates work against tile-parsed data without copying the whole
// template body.
struct TileRunway {
  int32_t le_lat_e7;
  int32_t le_lon_e7;
  int32_t he_lat_e7;
  int32_t he_lon_e7;
};

struct TileAirport {
  int32_t lat_e7;
  int32_t lon_e7;
  char ident[9];
};

// Tile-backed pass: iterate airports in the SECTION_AIRPORTS payload
// of the current-location tile, draw runways for airports in range,
// then labels. Ignores the RunwaysLarge / RunwaysFocus split from the
// baked path — the tile is a single unified airport list.
void drawRunwaysFromTile(lgfx::LGFXBase& gfx,
                          const data::tile::TileBytes& bytes,
                          float radius_km) {
  data::tile::TileReader reader;
  if (!reader.init(bytes.data, bytes.size)) return;
  uint32_t sec_len = 0;
  const uint8_t* p = reader.sectionBegin(data::tile::Section::Airports, &sec_len);
  if (p == nullptr || sec_len == 0) return;
  const uint8_t* end = p + sec_len;
  uint16_t airport_count = 0;
  if (!data::tile::TileReader::readSectionCount(&p, end, &airport_count)) return;

  TileAirport labeled[kMaxAirportLabels];
  size_t label_count = 0;

  for (uint16_t i = 0; i < airport_count; ++i) {
    data::tile::AirportView view;
    if (!data::tile::TileReader::readAirport(&p, end, &view)) return;

    float dx_km = 0.0f;
    float dy_km = 0.0f;
    float dist_km = 0.0f;
    offsetKmFromCenter(e7ToDeg(view.lat_e7), e7ToDeg(view.lon_e7), &dx_km,
                       &dy_km, &dist_km);
    if (dist_km > radius_km) continue;

    bool drew_any_runway = false;
    for (uint8_t r = 0; r < view.runway_count; ++r) {
      int32_t la1 = 0, lo1 = 0, la2 = 0, lo2 = 0;
      view.getRunway(r, &la1, &lo1, &la2, &lo2);
      TileRunway rw = {la1, lo1, la2, lo2};
      if (drawRunwayLineT(gfx, rw)) drew_any_runway = true;
    }

    if (drew_any_runway && label_count < kMaxAirportLabels) {
      TileAirport& row = labeled[label_count++];
      row.lat_e7 = view.lat_e7;
      row.lon_e7 = view.lon_e7;
      std::memcpy(row.ident, view.ident, sizeof(row.ident));
    }
  }

  if (label_count == 0) return;
  initRunwayLabelStyle(gfx);
  applyRunwayLabelStyle(gfx);
  for (size_t i = 0; i < label_count; ++i) {
    drawAirportLabelT(gfx, labeled[i]);
  }
}

void drawLargeAirportRunways(lgfx::LGFXBase& gfx) {
  if (!radar::showRunways()) {
    return;
  }
  const bool large_on = ui::layers::enabled(ui::layers::Layer::RunwaysLarge);
  const bool focus_on = ui::layers::enabled(ui::layers::Layer::RunwaysFocus);
  if (!large_on && !focus_on) return;
  displayFontEnsureLoaded(gfx);
  const float radius_km = radar::fetchRadiusKm();

  // Look up the tile covering the current location and dispatch to
  // the tile-backed drawer. If no tile is cached (fallback in flash
  // has no airport data), the render simply skips — the runway layer
  // is quiet until a tile arrives. That's the intended offline state.
  uint16_t tx = 0, ty = 0;
  data::tile::tileOfLatLon(data::tile::kRenderZoom,
                            services::location::lat(),
                            services::location::lon(), &tx, &ty);
  const auto tbytes = data::tile::store().get(data::tile::kRenderZoom, tx, ty);
  if (tbytes.is_fallback) return;
  drawRunwaysFromTile(gfx, tbytes, radius_km);
  (void)large_on;
  (void)focus_on;
}

}  // namespace ui::runway
