#pragma once

#include <LovyanGFX.hpp>

// FAA Class B/C/D airspace polygon overlay. Draws each shelf as a dashed
// closed polyline, clipped to the outer radar ring. Colors follow the
// sectional chart convention (B = blue, C = magenta, D = teal).
//
// The dashed style signals "this is a boundary, not a hazard/obstacle."

namespace ui::airspace {

void draw(lgfx::LGFXBase& gfx);

}  // namespace ui::airspace
