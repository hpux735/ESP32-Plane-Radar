#pragma once

#include <cstdint>

// Shared lat/lon → radar-screen projection primitives. Extracted from the
// runway-overlay module so new overlays (coastline, water, roads, airspace)
// use the same math and clipping.
//
// Coordinate system: flat lat/lon with 1° ≈ 111 km. North is screen up. All
// distances internally in km. Screen coords in pixels of the 240×240 canvas.

namespace ui::proj {

/** Decode a compact int32 micro-degree encoding (deg × 1e7) → float degrees. */
float e7ToDeg(int32_t e7);

/** Offset (dx_km east, dy_km north) and distance from the radar center to
 *  the given lat/lon. Uses the currently-configured radar center. */
void offsetKmFromCenter(float lat, float lon, float* dx_km, float* dy_km,
                        float* dist_km);

/** Project lat/lon to screen pixel coords in the 240×240 canvas, scaled
 *  by the current range preset's outer_km. */
void latLonToScreen(float lat, float lon, int* out_x, int* out_y);

/** Squared distance in pixels from the given point to the radar center. */
int distSqFromCenter(int x, int y);

/** If (x1, y1) is outside the outer ring, move it inward along the (x0, y0)
 *  → (x1, y1) segment until it lands on the ring. Uses a 20-step bisection
 *  (good enough for pixel-precision display). */
void clipPointToOuterRing(int x0, int y0, int* x1, int* y1);

/** True if the segment (x0,y0)→(x1,y1) intersects the outer-ring disc.
 *  Used as a quick-reject before per-segment draw for vector overlays. */
bool segmentIntersectsDisc(int x0, int y0, int x1, int y1);

/** Clip a segment to the outer-ring disc. Handles all three cases:
 *  both endpoints inside → no change; one endpoint outside → clip that one
 *  to the ring boundary; both outside but segment crosses ring → replace
 *  both endpoints with the two ring-boundary intersections. Returns false
 *  and leaves out-values unchanged if the segment doesn't intersect. */
bool clipSegmentToDisc(int x0, int y0, int x1, int y1,
                       int* out_x0, int* out_y0,
                       int* out_x1, int* out_y1);

}  // namespace ui::proj
