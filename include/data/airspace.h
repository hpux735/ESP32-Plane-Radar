#pragma once

#include <cstddef>
#include <cstdint>

// Baked FAA Class B/C/D airspace polygons around the radar center.
// Each shelf is its own polygon (SFO Class B alone has ~11 shelves,
// one per altitude tier). Points are int32 micro-degrees.
// Regenerate with scripts/build_airspace.py.

namespace data::airspace {

struct Point {
  int32_t lat_e7;
  int32_t lon_e7;
};

// A closed polygon ring is kPoints[start .. start+count).
// The last vertex is NOT a repeat of the first — the renderer
// wraps.
struct Polygon {
  uint16_t start;
  uint16_t count;
  char     class_letter;   // 'B', 'C', or 'D'
  int16_t  lower_ft;       // 0 = surface
  int16_t  upper_ft;
};

extern const Point   kPoints[];
extern const Polygon kPolygons[];
extern const size_t  kPointCount;
extern const size_t  kPolygonCount;

}  // namespace data::airspace
