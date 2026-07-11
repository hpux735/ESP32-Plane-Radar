#include "ui/weather_map.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "data/tile_math.h"
#include "data/tile_reader.h"
#include "data/tile_store.h"
#include "geo/ear_clip.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/metar_config.h"
#include "services/weather.h"
#include "ui/radar_display.h"
#include "ui/radar_theme.h"

namespace ui::weather {
namespace {

// Weather-map projection: user-configurable center + radius (see
// services::metar_config). Radius is expressed in nautical miles; scale
// is chosen so a station at exactly that radius lands just inside the
// bezel with room for its label. Stations beyond the radius project
// past the disc and are filtered out by insideDisc() below.
constexpr int   kProjectionPx     = 108;    // physical bezel is 120
constexpr int   kLabelMarginPx    = 14;     // reserve for label glyphs
constexpr float kKmPerDeg         = 111.0f;
constexpr float kKmPerNm          = 1.852f;
constexpr float kDegToRad         = 3.14159265358979323846f / 180.0f;
constexpr float kE7               = 1e-7f;

// Populated by computeFit() on each frame from services::metar_config.
float s_center_lat = 0.0f;
float s_center_lon = 0.0f;
float s_cos_center = 1.0f;
float s_px_per_km  = 1.0f;

// Dots stay at their true projected positions — never nudged. Labels
// are moved into free space around the dot (8 candidate slots), with a
// leader line drawn whenever the label ends up somewhere other than
// the default "below the dot" position.
constexpr size_t kMaxStations = 32;
int s_sx[kMaxStations] = {0};
int s_sy[kMaxStations] = {0};
int s_half_w[kMaxStations] = {0};

constexpr int kDotRadiusPx   = 4;
constexpr int kLabelHeightPx = 14;
constexpr int kLabelGapPx    = 4;   // space between dot edge and label edge
constexpr int kDotBboxPadPx  = 1;

// Label placement: cand=0 is the default (below the dot, no leader).
// Any other slot gets a thin leader line back to the dot.
struct Placement {
  int label_x, label_y;   // top_center anchor passed to drawString
  int cand;
};
Placement s_place[kMaxStations] = {};

const char* displayIcao(const char* icao) {
  return (icao[0] == 'K' && icao[1]) ? icao + 1 : icao;
}

// 8 candidate offsets (top_center anchor of the label) relative to the
// dot center. Ordered by preference — 0 is directly below (no leader),
// then above, then the four cardinals with the label vertically
// centered on the dot, then the four diagonals.
struct CandOffset { int dx, dy; };
void computeCandidates(int halfW, CandOffset out[8]) {
  const int dr = kDotRadiusPx;
  const int gap = kLabelGapPx;
  const int lh = kLabelHeightPx;
  const int vdn = dr + gap;               // label top: below dot
  const int vup = -dr - gap - lh;         // label top: above dot
  const int hr  = dr + gap + halfW;       // label center-x: right of dot
  const int hl  = -dr - gap - halfW;      // label center-x: left of dot
  const int vmid = -lh / 2;               // label top: vertically centered on dot
  out[0] = {0,   vdn};   // below (default, no leader)
  out[1] = {0,   vup};   // above
  out[2] = {hr,  vmid};  // right
  out[3] = {hl,  vmid};  // left
  out[4] = {hr,  vdn};   // below-right
  out[5] = {hl,  vdn};   // below-left
  out[6] = {hr,  vup};   // above-right
  out[7] = {hl,  vup};   // above-left
}

int overlapArea(int ax1, int ay1, int ax2, int ay2,
                int bx1, int by1, int bx2, int by2) {
  const int w = std::min(ax2, bx2) - std::max(ax1, bx1);
  const int h = std::min(ay2, by2) - std::max(ay1, by1);
  return (w > 0 && h > 0) ? w * h : 0;
}

void computeFit() {
  s_center_lat = services::metar_config::centerLat();
  s_center_lon = services::metar_config::centerLon();
  s_cos_center = std::cos(s_center_lat * kDegToRad);
  const float radius_km = services::metar_config::radiusNm() * kKmPerNm;
  const float budget_px = static_cast<float>(kProjectionPx - kLabelMarginPx);
  s_px_per_km = (radius_km > 0.0f) ? (budget_px / radius_km) : 1.0f;
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
  const float dx_km = (lon - s_center_lon) * kKmPerDeg * s_cos_center;
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

// Ear-clip scratch shared by land polygon triangulation. Sized to
// match land_overlay.cpp — the weather map draws from the same z=7
// tiles at the same 0.002° simplification, so worst-case polygon
// vertex counts are identical.
constexpr size_t kMaxPolyVerts = 1024;
static geo::Vertex s_polyVerts[kMaxPolyVerts];
static uint16_t s_earClipScratch[2 * kMaxPolyVerts];
static uint16_t s_triBuf[3 * (kMaxPolyVerts - 2)];

// Enumerate the (up to 4) z=7 tiles that cover the weather map's
// current viewport. Center + radius maps to a bbox of ±radius_km in
// both lat and lon (equirectangular, so lon step widens toward the
// poles); one lookup per bbox corner catches every tile the visible
// disc can touch even when the center sits on a tile boundary.
struct TileId { uint16_t x, y; };
size_t visibleTiles(TileId out[4]) {
  const float radius_km = services::metar_config::radiusNm() * kKmPerNm;
  const float dlat = radius_km / kKmPerDeg;
  const float dlon = (s_cos_center > 1e-4f)
                       ? radius_km / (kKmPerDeg * s_cos_center)
                       : 0.0f;
  const double corners[4][2] = {
      {static_cast<double>(s_center_lat + dlat), static_cast<double>(s_center_lon - dlon)},
      {static_cast<double>(s_center_lat + dlat), static_cast<double>(s_center_lon + dlon)},
      {static_cast<double>(s_center_lat - dlat), static_cast<double>(s_center_lon - dlon)},
      {static_cast<double>(s_center_lat - dlat), static_cast<double>(s_center_lon + dlon)},
  };
  size_t n = 0;
  for (const auto& c : corners) {
    uint16_t tx = 0, ty = 0;
    data::tile::tileOfLatLon(data::tile::kRenderZoom, c[0], c[1], &tx, &ty);
    bool dup = false;
    for (size_t i = 0; i < n; ++i) {
      if (out[i].x == tx && out[i].y == ty) { dup = true; break; }
    }
    if (!dup) out[n++] = {tx, ty};
  }
  return n;
}

// Project a polygon of tile vertices through the weather map's own
// scale/center, ear-clip it into triangles, and fill each.
void drawTileLandPolygon(lgfx::LGFXBase& gfx,
                         const data::tile::PolylineView& view, uint16_t color) {
  if (view.point_count < 3 || view.point_count > kMaxPolyVerts) return;
  for (uint16_t i = 0; i < view.point_count; ++i) {
    int32_t lat_e7 = 0, lon_e7 = 0;
    view.getPoint(i, &lat_e7, &lon_e7);
    // ear_clip treats .x/.y as flat coords — pack lon into x, lat into
    // y so the winding test matches an equirectangular projection.
    s_polyVerts[i].x = lon_e7;
    s_polyVerts[i].y = lat_e7;
  }
  const int tri_count = geo::triangulate(
      s_polyVerts, view.point_count, s_triBuf,
      sizeof(s_triBuf) / sizeof(s_triBuf[0]), s_earClipScratch);
  if (tri_count <= 0) return;
  for (int t = 0; t < tri_count; ++t) {
    int x[3], y[3];
    for (int k = 0; k < 3; ++k) {
      const uint16_t vi = s_triBuf[t * 3 + k];
      projectLatLon(s_polyVerts[vi].y * kE7, s_polyVerts[vi].x * kE7,
                    &x[k], &y[k]);
    }
    const int min_x = std::min({x[0], x[1], x[2]});
    const int max_x = std::max({x[0], x[1], x[2]});
    const int min_y = std::min({y[0], y[1], y[2]});
    const int max_y = std::max({y[0], y[1], y[2]});
    if (!bboxOverlapsScreen(min_x, max_x, min_y, max_y)) continue;
    gfx.fillTriangle(x[0], y[0], x[1], y[1], x[2], y[2], color);
  }
}

// Land + coast: pull each tile covering the visible viewport from
// TileStore, then draw its Land / Coast sections through the weather
// map's projection.
void drawLand(lgfx::LGFXBase& gfx) {
  const uint16_t color = radar::kColorLand;
  TileId tiles[4];
  const size_t nt = visibleTiles(tiles);
  for (size_t ti = 0; ti < nt; ++ti) {
    const auto bytes = data::tile::store().get(data::tile::kRenderZoom,
                                                tiles[ti].x, tiles[ti].y);
    data::tile::TileReader reader;
    if (!reader.init(bytes.data, bytes.size)) continue;
    uint32_t sec_len = 0;
    const uint8_t* p =
        reader.sectionBegin(data::tile::Section::Land, &sec_len);
    if (p == nullptr || sec_len == 0) continue;
    const uint8_t* end = p + sec_len;
    uint16_t poly_count = 0;
    if (!data::tile::TileReader::readSectionCount(&p, end, &poly_count)) continue;
    for (uint16_t i = 0; i < poly_count; ++i) {
      data::tile::PolylineView view;
      if (!data::tile::TileReader::readPolyline(&p, end, &view)) break;
      drawTileLandPolygon(gfx, view, color);
    }
  }
}

void drawCoast(lgfx::LGFXBase& gfx) {
  const uint16_t color = tft.color565(radar::kBgR + 40, radar::kBgG + 60,
                                      radar::kBgB + 40);
  TileId tiles[4];
  const size_t nt = visibleTiles(tiles);
  for (size_t ti = 0; ti < nt; ++ti) {
    const auto bytes = data::tile::store().get(data::tile::kRenderZoom,
                                                tiles[ti].x, tiles[ti].y);
    data::tile::TileReader reader;
    if (!reader.init(bytes.data, bytes.size)) continue;
    uint32_t sec_len = 0;
    const uint8_t* p =
        reader.sectionBegin(data::tile::Section::Coast, &sec_len);
    if (p == nullptr || sec_len == 0) continue;
    const uint8_t* end = p + sec_len;
    uint16_t poly_count = 0;
    if (!data::tile::TileReader::readSectionCount(&p, end, &poly_count)) continue;
    for (uint16_t i = 0; i < poly_count; ++i) {
      data::tile::PolylineView view;
      if (!data::tile::TileReader::readPolyline(&p, end, &view)) break;
      if (view.point_count < 2) continue;
      int32_t lat_e7 = 0, lon_e7 = 0;
      view.getPoint(0, &lat_e7, &lon_e7);
      int prev_x = 0, prev_y = 0;
      projectLatLon(lat_e7 * kE7, lon_e7 * kE7, &prev_x, &prev_y);
      for (uint16_t k = 1; k < view.point_count; ++k) {
        view.getPoint(k, &lat_e7, &lon_e7);
        int x = 0, y = 0;
        projectLatLon(lat_e7 * kE7, lon_e7 * kE7, &x, &y);
        const int min_x = std::min(prev_x, x);
        const int max_x = std::max(prev_x, x);
        const int min_y = std::min(prev_y, y);
        const int max_y = std::max(prev_y, y);
        if (bboxOverlapsScreen(min_x, max_x, min_y, max_y)) {
          gfx.drawLine(prev_x, prev_y, x, y, color);
        }
        prev_x = x;
        prev_y = y;
      }
    }
  }
}

// Project every station to its true screen pixel and measure the
// label's real rendered width. Dots don't move from here — only labels.
// gfx must already have the display font loaded and text size set to
// what drawStations() will render, since we measure via textWidth().
void projectStations(lgfx::LGFXBase& gfx) {
  const services::weather::Station* stations = services::weather::stations();
  const size_t n = services::weather::stationCount();
  for (size_t i = 0; i < n && i < kMaxStations; ++i) {
    projectLatLon(stations[i].lat, stations[i].lon, &s_sx[i], &s_sy[i]);
    const int lw = gfx.textWidth(displayIcao(stations[i].icao));
    s_half_w[i] = lw / 2 + 1;   // 1 px pad so touching bboxes count as overlap
  }
}

// Overlap of a candidate label bbox against every OTHER station's dot
// and currently-placed label. Lower is better; 0 is a clean slot.
int labelScore(size_t i, int cand, size_t n) {
  CandOffset offs[8];
  computeCandidates(s_half_w[i], offs);
  const int lx = s_sx[i] + offs[cand].dx;
  const int ly = s_sy[i] + offs[cand].dy;
  const int L = lx - s_half_w[i];
  const int R = lx + s_half_w[i];
  const int T = ly;
  const int B = ly + kLabelHeightPx;
  int score = 0;
  for (size_t j = 0; j < n && j < kMaxStations; ++j) {
    if (j == i) continue;
    const int DL = s_sx[j] - kDotRadiusPx - kDotBboxPadPx;
    const int DT = s_sy[j] - kDotRadiusPx - kDotBboxPadPx;
    const int DRt = s_sx[j] + kDotRadiusPx + kDotBboxPadPx;
    const int DB = s_sy[j] + kDotRadiusPx + kDotBboxPadPx;
    score += overlapArea(L, T, R, B, DL, DT, DRt, DB);
    const int LL = s_place[j].label_x - s_half_w[j];
    const int LR = s_place[j].label_x + s_half_w[j];
    const int LT = s_place[j].label_y;
    const int LB = s_place[j].label_y + kLabelHeightPx;
    score += overlapArea(L, T, R, B, LL, LT, LR, LB);
  }
  return score;
}

// Assign each station's label to the least-overlapping candidate slot.
// Iterate a handful of passes so late-placed labels can push earlier
// ones off their first choice.
void placeLabels() {
  const size_t n = services::weather::stationCount();
  for (size_t i = 0; i < n && i < kMaxStations; ++i) {
    CandOffset offs[8];
    computeCandidates(s_half_w[i], offs);
    s_place[i].cand    = 0;
    s_place[i].label_x = s_sx[i] + offs[0].dx;
    s_place[i].label_y = s_sy[i] + offs[0].dy;
  }
  for (int pass = 0; pass < 4; ++pass) {
    bool changed = false;
    for (size_t i = 0; i < n && i < kMaxStations; ++i) {
      int best_cand  = s_place[i].cand;
      int best_score = labelScore(i, best_cand, n);
      for (int c = 0; c < 8; ++c) {
        if (c == best_cand) continue;
        const int s = labelScore(i, c, n);
        // Strict '<' so ties keep the lower-index candidate (defaults win).
        if (s < best_score) { best_score = s; best_cand = c; }
      }
      if (best_cand != s_place[i].cand) {
        CandOffset offs[8];
        computeCandidates(s_half_w[i], offs);
        s_place[i].cand    = best_cand;
        s_place[i].label_x = s_sx[i] + offs[best_cand].dx;
        s_place[i].label_y = s_sy[i] + offs[best_cand].dy;
        changed = true;
      }
    }
    if (!changed) break;
  }
}

// Endpoint of the leader on the label side: the point where the ray
// from the dot to the label center crosses the label bbox. Gives a
// clean, un-crossing leader terminus.
void leaderEndpoint(int dot_x, int dot_y, int label_x, int label_y,
                    int half_w, int* out_x, int* out_y) {
  const int cx = label_x;
  const int cy = label_y + kLabelHeightPx / 2;
  const int hh = kLabelHeightPx / 2;
  const int dx = dot_x - cx;
  const int dy = dot_y - cy;
  if (dx == 0 && dy == 0) { *out_x = cx; *out_y = cy; return; }
  const float tx = (dx == 0) ? 1e9f
                             : static_cast<float>(half_w) / std::abs(dx);
  const float ty = (dy == 0) ? 1e9f
                             : static_cast<float>(hh) / std::abs(dy);
  const float t = std::min(tx, ty);
  *out_x = cx + static_cast<int>(std::lroundf(t * dx));
  *out_y = cy + static_cast<int>(std::lroundf(t * dy));
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
  // Bright enough to be seen against the dark navy bg without competing
  // with the label glyphs. Matched to the coastline greenish-teal so the
  // leaders read as map furniture, not as a data channel.
  const uint16_t leader_color = tft.color565(90, 130, 110);

  // Pass 1: leaders first so dots/labels paint on top of the lines.
  for (size_t i = 0; i < n && i < kMaxStations; ++i) {
    if (!insideDisc(s_sx[i], s_sy[i])) continue;
    if (s_place[i].cand == 0) continue;
    int ex, ey;
    leaderEndpoint(s_sx[i], s_sy[i], s_place[i].label_x, s_place[i].label_y,
                   s_half_w[i], &ex, &ey);
    gfx.drawLine(s_sx[i], s_sy[i], ex, ey, leader_color);
  }

  // Pass 2: dots + labels.
  for (size_t i = 0; i < n && i < kMaxStations; ++i) {
    const int sx = s_sx[i];
    const int sy = s_sy[i];
    if (!insideDisc(sx, sy)) continue;

    const uint16_t color = categoryColor(stations[i].category);
    gfx.fillCircle(sx, sy, kDotRadiusPx, color);
    gfx.drawCircle(sx, sy, kDotRadiusPx, radar::kColorBackground);

    gfx.setTextColor(radar::kColorLabel, radar::kColorBackground);
    gfx.drawString(displayIcao(stations[i].icao),
                   s_place[i].label_x, s_place[i].label_y);
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
    // Always minutes (0 for a fresh fetch). Matches the web preview at
    // web/src/weatherView.ts formatFreshness — the old "Xs ago" branch
    // caused a sub-minute flicker on both platforms.
    const unsigned long age_min = (millis() - last) / 60000UL;
    std::snprintf(buf, sizeof(buf), "%lu min ago", age_min);
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
  configureLabelFont(gfx);   // must be set before textWidth measurement
  projectStations(gfx);
  placeLabels();
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
