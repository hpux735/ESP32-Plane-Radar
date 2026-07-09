#pragma once

#include <LovyanGFX.hpp>

namespace ui::roads {

/** Draw major-road polylines clipped to the outer ring. Draw AFTER land
 *  + coastline so roads sit on top of the landmass. */
void draw(lgfx::LGFXBase& gfx);

}  // namespace ui::roads
