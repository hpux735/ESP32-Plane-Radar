#pragma once

#include <cstddef>
#include <cstdint>

// Baked major-road polylines (Natural Earth 1:10m). Same encoding as
// coastlines: int32 micro-degrees + Polyline{start,count} into a flat
// vertex array. Regenerate with scripts/build_roads.py.

namespace data::roads {

struct Point {
  int32_t lat_e7;
  int32_t lon_e7;
};

struct Polyline {
  uint16_t start;
  uint16_t count;
};

extern const Point kPoints[];
extern const Polyline kPolylines[];
extern const size_t kPointCount;
extern const size_t kPolylineCount;

}  // namespace data::roads
