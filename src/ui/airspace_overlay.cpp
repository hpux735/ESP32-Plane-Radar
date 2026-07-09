#include "ui/airspace_overlay.h"

#include "data/airspace.h"
#include "ui/layer_style.h"
#include "ui/map_projection.hpp"
#include "ui/radar_theme.h"

namespace ui::airspace {
namespace {

constexpr float kE7 = 1e-7f;

uint16_t classColor(char c) {
  switch (c) {
    case 'B': return radar::kColorAirspaceB;
    case 'C': return radar::kColorAirspaceC;
    case 'D': return radar::kColorAirspaceD;
    default:  return radar::kColorAirspaceD;
  }
}

// Dashed edges: draw only every other polygon edge so the outline reads as
// a boundary marker rather than a solid barrier.
void drawEdge(lgfx::LGFXBase& gfx, int x0, int y0, int x1, int y1,
              uint16_t color) {
  int cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
  if (proj::clipSegmentToDisc(x0, y0, x1, y1, &cx0, &cy0, &cx1, &cy1)) {
    gfx.drawWideLine(cx0, cy0, cx1, cy1, 0.9f, color);
  }
}

}  // namespace

void draw(lgfx::LGFXBase& gfx) {
  if (!ui::layers::enabled(ui::layers::Layer::Airspace)) return;
  using namespace data::airspace;

  for (size_t p = 0; p < kPolygonCount; ++p) {
    const Polygon& poly = kPolygons[p];
    const uint16_t color = classColor(poly.class_letter);

    // Project all vertices into screen space up front so we can iterate
    // edges with wrap-around. 128 is comfortably above the largest ring
    // (SFO shelf 8 has 27 vertices after DP; the FAA raw geometry is
    // much larger but simplification keeps every ring < 60).
    constexpr int kMaxVerts = 128;
    if (poly.count > kMaxVerts) continue;
    int xs[kMaxVerts];
    int ys[kMaxVerts];
    for (uint16_t i = 0; i < poly.count; ++i) {
      const Point& pt = kPoints[poly.start + i];
      const float lat = pt.lat_e7 * kE7;
      const float lon = pt.lon_e7 * kE7;
      proj::latLonToScreen(lat, lon, &xs[i], &ys[i]);
    }

    // Dashed closed loop: draw edges 0-1, 2-3, 4-5, … (skip odd edges).
    for (uint16_t i = 0; i < poly.count; i += 2) {
      const uint16_t j = (i + 1) % poly.count;
      drawEdge(gfx, xs[i], ys[i], xs[j], ys[j], color);
    }
  }
}

}  // namespace ui::airspace
