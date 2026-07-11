#pragma once

// Pure lat/lon math shared between services::weather (production) and
// its unit tests. Extracted so the tests don't need to pull in
// ArduinoJson, HTTPClient, WiFi, or metar_config just to lock the
// distance / bbox formulas.

namespace services::weather::geo {

// Approximate great-circle distance in nautical miles. Uses
// 60 nm/deg latitude and cos(lat) scaling for longitude — good enough
// at the scales the METAR view cares about (< 100 nm).
float distanceNm(float lat1, float lon1, float lat2, float lon2);

// Build a lat/lon bounding box centered on (center_lat, center_lon)
// with radius_nm half-diagonal, inflated 10% so stations that project
// just past the visible map still get fetched.
void makeBbox(float center_lat, float center_lon, float radius_nm,
              float* lat_min, float* lon_min,
              float* lat_max, float* lon_max);

// Great-circle initial bearing from (lat1, lon1) to (lat2, lon2) in
// degrees clockwise from true north, normalized to [0, 360).
float bearingDeg(float lat1, float lon1, float lat2, float lon2);

// Bin a bearing (deg, any real) to one of 8 compass directions.
// Returns a pointer to a static string: "N", "NE", "E", "SE", "S",
// "SW", "W", "NW".
const char* compass8(float bearing_deg);

}  // namespace services::weather::geo
