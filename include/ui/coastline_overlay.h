#pragma once

#include <LovyanGFX.hpp>

namespace ui::coastline {

/** Draw coastline polylines within the outer ring. Call after the grid
 *  and before aircraft so the outline is under the traffic. */
void draw(lgfx::LGFXBase& gfx);

}  // namespace ui::coastline
