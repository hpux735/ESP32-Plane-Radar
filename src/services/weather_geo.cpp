#include "services/weather_geo.h"

#include <cmath>

namespace services::weather::geo {

namespace {
constexpr float kNmPerDeg = 60.0f;
constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
}  // namespace

float distanceNm(float lat1, float lon1, float lat2, float lon2) {
  const float cos_lat = std::cos(lat1 * kDegToRad);
  const float dlat_nm = (lat2 - lat1) * kNmPerDeg;
  const float dlon_nm = (lon2 - lon1) * kNmPerDeg * cos_lat;
  return std::sqrt(dlat_nm * dlat_nm + dlon_nm * dlon_nm);
}

void makeBbox(float center_lat, float center_lon, float radius_nm,
              float* lat_min, float* lon_min,
              float* lat_max, float* lon_max) {
  const float pad_deg = (radius_nm * 1.1f) / kNmPerDeg;
  const float cos_lat = std::cos(center_lat * kDegToRad);
  const float pad_lon = (cos_lat > 0.01f) ? pad_deg / cos_lat : pad_deg;
  *lat_min = center_lat - pad_deg;
  *lat_max = center_lat + pad_deg;
  *lon_min = center_lon - pad_lon;
  *lon_max = center_lon + pad_lon;
}

float bearingDeg(float lat1, float lon1, float lat2, float lon2) {
  const float phi1 = lat1 * kDegToRad;
  const float phi2 = lat2 * kDegToRad;
  const float dlon = (lon2 - lon1) * kDegToRad;
  const float y = std::sin(dlon) * std::cos(phi2);
  const float x = std::cos(phi1) * std::sin(phi2) -
                  std::sin(phi1) * std::cos(phi2) * std::cos(dlon);
  const float brg = std::atan2(y, x) / kDegToRad;
  return std::fmod(brg + 360.0f, 360.0f);
}

const char* compass8(float bearing_deg) {
  static const char* kDirs[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  const float normalized = std::fmod(std::fmod(bearing_deg, 360.0f) + 360.0f, 360.0f);
  const int idx = static_cast<int>(std::lroundf(normalized / 45.0f)) % 8;
  return kDirs[idx];
}

}  // namespace services::weather::geo
