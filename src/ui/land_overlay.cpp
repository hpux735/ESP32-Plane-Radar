#include "ui/land_overlay.h"

#include "data/land.h"
#include "ui/layer_style.h"
#include "ui/map_projection.hpp"
#include "ui/radar_theme.h"

namespace ui::land {

void draw(lgfx::LGFXBase& gfx) {
  if (!ui::layers::enabled(ui::layers::Layer::Land)) return;
  using namespace data::land;
  const uint16_t color = radar::kColorLand;
  for (size_t i = 0; i < kTriangleCount; ++i) {
    const Triangle& t = kTriangles[i];
    int x[3];
    int y[3];
    const uint16_t vidx[3] = {t.v0, t.v1, t.v2};
    for (int k = 0; k < 3; ++k) {
      const Vertex& v = kVertices[vidx[k]];
      proj::latLonToScreen(proj::e7ToDeg(v.lat_e7), proj::e7ToDeg(v.lon_e7),
                           &x[k], &y[k]);
    }
    // Quick reject: all 3 verts off the same side of the 240×240 canvas.
    if ((x[0] < 0 && x[1] < 0 && x[2] < 0) ||
        (x[0] > 239 && x[1] > 239 && x[2] > 239) ||
        (y[0] < 0 && y[1] < 0 && y[2] < 0) ||
        (y[0] > 239 && y[1] > 239 && y[2] > 239)) {
      continue;
    }
    gfx.fillTriangle(x[0], y[0], x[1], y[1], x[2], y[2], color);
  }
}

}  // namespace ui::land
