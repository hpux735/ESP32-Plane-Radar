#pragma once

#include <cstddef>
#include <cstdint>

// Baked coastline (Natural Earth 1:10m) clipped around the radar
// center. Points are stored as compact int32 micro-degrees.
// Regenerate with scripts/build_coastlines.py.

namespace data::coastlines {

struct Point {
  int32_t lat_e7;
  int32_t lon_e7;
};

// A polyline is a slice of kPoints[start .. start+count).
struct Polyline {
  uint16_t start;
  uint16_t count;
};

extern const Point kPoints[];
extern const Polyline kPolylines[];
extern const size_t kPointCount;
extern const size_t kPolylineCount;

}  // namespace data::coastlines
