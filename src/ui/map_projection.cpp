#include "ui/map_projection.hpp"

#include <algorithm>
#include <cmath>

#include "services/radar_location.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"

namespace ui::proj {
namespace {

constexpr float kKmPerDeg = 111.0f;
constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

}  // namespace

float e7ToDeg(int32_t e7) { return static_cast<float>(e7) * 1e-7f; }

void offsetKmFromCenter(float lat, float lon, float* dx_km, float* dy_km,
                        float* dist_km) {
  const float center_lat_rad =
      static_cast<float>(services::location::lat()) * kDegToRad;
  *dx_km = static_cast<float>(lon - services::location::lon()) * kKmPerDeg *
           std::cos(center_lat_rad);
  *dy_km = static_cast<float>(lat - services::location::lat()) * kKmPerDeg;
  *dist_km = std::sqrt((*dx_km) * (*dx_km) + (*dy_km) * (*dy_km));
}

void latLonToScreen(float lat, float lon, int* out_x, int* out_y) {
  const float outer_km = radar::rangeCurrent().outer_km;
  const float px_per_km =
      static_cast<float>(radar::kGridOuterRadius) / outer_km;

  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);

  *out_x = radar::kCenterX +
           static_cast<int>(std::lroundf(dx_km * px_per_km));
  *out_y = radar::kCenterY -
           static_cast<int>(std::lroundf(dy_km * px_per_km));
}

int distSqFromCenter(int x, int y) {
  const int dx = x - radar::kCenterX;
  const int dy = y - radar::kCenterY;
  return dx * dx + dy * dy;
}

void clipPointToOuterRing(int x0, int y0, int* x1, int* y1) {
  const int max_r = radar::kGridOuterRadius;
  const int max_r_sq = max_r * max_r;
  if (distSqFromCenter(*x1, *y1) <= max_r_sq) {
    return;
  }

  const int dx = *x1 - x0;
  const int dy = *y1 - y0;
  float t = 1.0f;
  for (int step = 0; step < 20; ++step) {
    const int px = x0 + static_cast<int>(std::lroundf(dx * t));
    const int py = y0 + static_cast<int>(std::lroundf(dy * t));
    if (distSqFromCenter(px, py) <= max_r_sq) {
      *x1 = px;
      *y1 = py;
      return;
    }
    t -= 0.05f;
    if (t <= 0.0f) {
      *x1 = x0;
      *y1 = y0;
      return;
    }
  }
}

// Full line-circle clip: returns the sub-segment lying inside the outer
// ring. Correctly handles the "both endpoints outside, segment crosses"
// case that clipPointToOuterRing collapses.
bool clipSegmentToDisc(int x0, int y0, int x1, int y1,
                       int* out_x0, int* out_y0,
                       int* out_x1, int* out_y1) {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int r = radar::kGridOuterRadius;
  const int r_sq = r * r;
  const bool p0_in = distSqFromCenter(x0, y0) <= r_sq;
  const bool p1_in = distSqFromCenter(x1, y1) <= r_sq;

  if (p0_in && p1_in) {
    *out_x0 = x0;
    *out_y0 = y0;
    *out_x1 = x1;
    *out_y1 = y1;
    return true;
  }

  // Solve |P0 + t*(P1-P0) - C|² = r² for t: quadratic a t² + b t + c = 0.
  const float dx = static_cast<float>(x1 - x0);
  const float dy = static_cast<float>(y1 - y0);
  const float fx = static_cast<float>(x0 - cx);
  const float fy = static_cast<float>(y0 - cy);
  const float a = dx * dx + dy * dy;
  if (a == 0.0f) return false;
  const float b = 2.0f * (fx * dx + fy * dy);
  const float c = fx * fx + fy * fy - static_cast<float>(r_sq);
  const float disc = b * b - 4.0f * a * c;
  if (disc < 0.0f) return false;
  const float sq = std::sqrt(disc);
  const float inv2a = 1.0f / (2.0f * a);
  float t0 = (-b - sq) * inv2a;
  float t1 = (-b + sq) * inv2a;
  if (t0 > t1) std::swap(t0, t1);

  // If both endpoints outside, use the two ring intersections directly.
  // If one is inside, cap the outside end at the corresponding intersection.
  float ta = p0_in ? 0.0f : t0;
  float tb = p1_in ? 1.0f : t1;
  if (tb < 0.0f || ta > 1.0f) return false;
  if (ta < 0.0f) ta = 0.0f;
  if (tb > 1.0f) tb = 1.0f;
  if (ta > tb) return false;

  *out_x0 = x0 + static_cast<int>(std::lroundf(dx * ta));
  *out_y0 = y0 + static_cast<int>(std::lroundf(dy * ta));
  *out_x1 = x0 + static_cast<int>(std::lroundf(dx * tb));
  *out_y1 = y0 + static_cast<int>(std::lroundf(dy * tb));
  return true;
}

bool segmentIntersectsDisc(int x0, int y0, int x1, int y1) {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int r = radar::kGridOuterRadius;
  const int r_sq = r * r;

  if (distSqFromCenter(x0, y0) <= r_sq ||
      distSqFromCenter(x1, y1) <= r_sq) {
    return true;
  }

  const int dx = x1 - x0;
  const int dy = y1 - y0;
  const int fx = x0 - cx;
  const int fy = y0 - cy;
  const int a = dx * dx + dy * dy;
  if (a == 0) {
    return false;
  }
  const int b = 2 * (fx * dx + fy * dy);
  const int c = fx * fx + fy * fy - r_sq;
  int disc = b * b - 4 * a * c;
  if (disc < 0) {
    return false;
  }
  disc = static_cast<int>(std::sqrt(static_cast<float>(disc)));
  const float inv2a = 1.0f / (2.0f * static_cast<float>(a));
  const float t0 = (-static_cast<float>(b) - disc) * inv2a;
  const float t1 = (-static_cast<float>(b) + disc) * inv2a;
  return (t0 >= 0.0f && t0 <= 1.0f) || (t1 >= 0.0f && t1 <= 1.0f);
}

}  // namespace ui::proj
