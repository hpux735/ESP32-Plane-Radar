#include "services/metar_config.h"

#include <Arduino.h>
#include <Preferences.h>

#include <cmath>
#include <cstdlib>

#include "config.h"
#include "services/weather.h"

namespace services::metar_config {
namespace {

constexpr char kNamespace[] = "metar";
constexpr char kKeyLat[] = "lat";
constexpr char kKeyLon[] = "lon";
constexpr char kKeyRadius[] = "rad";

float s_lat = 0.0f;
float s_lon = 0.0f;
float s_radius_nm = 0.0f;

bool finiteLatLon(float lat, float lon) {
  return std::isfinite(lat) && std::isfinite(lon) &&
         lat >= -90.0f && lat <= 90.0f &&
         lon >= -180.0f && lon <= 180.0f;
}

}  // namespace

void init() {
  s_lat = config::kDefaultMetarLat;
  s_lon = config::kDefaultMetarLon;
  s_radius_nm = config::kDefaultMetarRadiusNm;
  Preferences prefs;
  if (!prefs.begin(kNamespace, true)) return;
  const float lat = prefs.getFloat(kKeyLat, s_lat);
  const float lon = prefs.getFloat(kKeyLon, s_lon);
  const float rad = prefs.getFloat(kKeyRadius, s_radius_nm);
  prefs.end();
  if (finiteLatLon(lat, lon)) {
    s_lat = lat;
    s_lon = lon;
  }
  if (std::isfinite(rad) && rad > 0.0f) {
    s_radius_nm = rad;
  }
}

float centerLat() { return s_lat; }
float centerLon() { return s_lon; }
float radiusNm() { return s_radius_nm; }

void saveFromStrings(const char* lat_str, const char* lon_str,
                     const char* radius_str) {
  if (lat_str == nullptr || lon_str == nullptr || radius_str == nullptr) {
    return;
  }
  const float lat = static_cast<float>(std::atof(lat_str));
  const float lon = static_cast<float>(std::atof(lon_str));
  const float rad = static_cast<float>(std::atof(radius_str));
  if (!finiteLatLon(lat, lon) || !std::isfinite(rad) || rad <= 0.0f) {
    Serial.println("metar_config: invalid values, ignoring");
    return;
  }
  s_lat = lat;
  s_lon = lon;
  s_radius_nm = rad;
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  prefs.putFloat(kKeyLat, lat);
  prefs.putFloat(kKeyLon, lon);
  prefs.putFloat(kKeyRadius, rad);
  prefs.end();
  services::weather::invalidate();
  Serial.printf("metar_config: saved center=(%.4f, %.4f) radius=%.1f nm\n",
                lat, lon, rad);
}

}  // namespace services::metar_config
