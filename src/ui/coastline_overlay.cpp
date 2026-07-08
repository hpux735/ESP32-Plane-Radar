#include "ui/coastline_overlay.h"

#include <cstdio>

#include "data/coastlines.h"
#include "ui/layer_style.h"
#include "ui/map_projection.hpp"
#include "ui/radar_theme.h"

namespace ui::coastline {
namespace {

// Coastline is grid-family clutter — dim green, thin, sits below labels
// and aircraft. Uses the same palette entry as the grid rings for now
// (M4/M6 will move this to per-layer style + clarity gating).
inline uint16_t coastColor() { return radar::kColorGrid; }

}  // namespace

void draw(lgfx::LGFXBase& gfx) {
  if (!ui::layers::enabled(ui::layers::Layer::Coastline)) return;
  using namespace data::coastlines;
  for (size_t i = 0; i < kPolylineCount; ++i) {
    const Polyline& pl = kPolylines[i];
    if (pl.count < 2) continue;

    int prev_x = 0;
    int prev_y = 0;
    proj::latLonToScreen(proj::e7ToDeg(kPoints[pl.start].lat_e7),
                         proj::e7ToDeg(kPoints[pl.start].lon_e7),
                         &prev_x, &prev_y);
    for (uint16_t j = 1; j < pl.count; ++j) {
      int x = 0;
      int y = 0;
      const Point& p = kPoints[pl.start + j];
      proj::latLonToScreen(proj::e7ToDeg(p.lat_e7), proj::e7ToDeg(p.lon_e7),
                           &x, &y);
      int cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
      if (proj::clipSegmentToDisc(prev_x, prev_y, x, y, &cx0, &cy0, &cx1, &cy1)) {
        gfx.drawLine(cx0, cy0, cx1, cy1, coastColor());
      }
      prev_x = x;
      prev_y = y;
    }
  }
}

}  // namespace ui::coastline
