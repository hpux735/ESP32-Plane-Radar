#include "services/weather.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef USE_NATIVE
#include <cstdio>
#include <string>
#else
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#endif

#include "services/metar_config.h"
#include "services/weather_category.h"
#include "services/weather_geo.h"

namespace services::weather {

namespace {

// Fixed cap on how many stations we render. The weather map's label-
// placement loop is O(n·candidates·passes); 32 is plenty for a 40 nm
// radius around a busy metro and keeps the arithmetic cheap.
constexpr size_t kMaxStations = 32;

Station s_stations[kMaxStations] = {};
size_t s_station_count = 0;

unsigned long s_last_update_ms = 0;

int parseVisibility(JsonVariantConst v) {
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (s && s[0]) {
      // "10+" → 10; "3" → 3; "2 1/2" → 2 (worst-case conservative).
      return std::atoi(s);
    }
    return 10;
  }
  if (v.is<int>() || v.is<float>() || v.is<double>()) {
    return static_cast<int>(v.as<float>());
  }
  return 10;
}

// Extracts the lowest BKN/OVC base as ceiling; anything else → INT32_MAX.
int32_t parseCeiling(JsonArrayConst clouds) {
  int32_t ceiling = INT32_MAX;
  for (JsonObjectConst layer : clouds) {
    const char* cover = layer["cover"].as<const char*>();
    if (!cover) continue;
    if (std::strcmp(cover, "BKN") == 0 || std::strcmp(cover, "OVC") == 0 ||
        std::strcmp(cover, "VV") == 0) {
      const int32_t base = layer["base"].as<int32_t>();
      if (base < ceiling) ceiling = base;
    }
  }
  return ceiling;
}

void ingestPayload(const char* body, size_t body_len,
                   float center_lat, float center_lon) {
  // 12 kB is enough for ~40 METARs at their typical size. The parser
  // allocates as it deserializes; oversize responses just truncate cleanly.
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body, body_len);
  if (err) {
#ifdef USE_NATIVE
    std::printf("weather: json parse: %s\n", err.c_str());
#else
    Serial.printf("weather: json parse: %s\n", err.c_str());
#endif
    return;
  }
  JsonArrayConst arr = doc.as<JsonArrayConst>();

  // Two-pass populate: gather (distance, index) into a scratch, sort by
  // distance, then take the nearest kMaxStations. Guarantees the closest
  // fields to the user's center always render even in dense metros.
  struct Row { float dist_nm; size_t src_idx; };
  Row rows[kMaxStations * 4];  // upper bound for typical bbox responses
  size_t row_count = 0;

  size_t src_idx = 0;
  for (JsonObjectConst m : arr) {
    const char* icao = m["icaoId"].as<const char*>();
    if (!icao || icao[0] == '\0') { ++src_idx; continue; }
    if (!m["lat"].is<float>() && !m["lat"].is<double>() &&
        !m["lat"].is<int>()) { ++src_idx; continue; }
    const float lat = m["lat"].as<float>();
    const float lon = m["lon"].as<float>();
    if (row_count < sizeof(rows) / sizeof(rows[0])) {
      rows[row_count++] = { services::weather::geo::distanceNm(center_lat, center_lon, lat, lon),
                            src_idx };
    } else {
      // Replace the current farthest if this one is closer.
      size_t worst = 0;
      for (size_t k = 1; k < row_count; ++k) {
        if (rows[k].dist_nm > rows[worst].dist_nm) worst = k;
      }
      const float d = services::weather::geo::distanceNm(center_lat, center_lon, lat, lon);
      if (d < rows[worst].dist_nm) {
        rows[worst] = { d, src_idx };
      }
    }
    ++src_idx;
  }

  std::sort(rows, rows + row_count,
            [](const Row& a, const Row& b) { return a.dist_nm < b.dist_nm; });

  const size_t take = std::min(row_count, kMaxStations);
  // Second pass: keep the JSON entries whose src_idx is in `rows[0..take)`.
  // Sort picks by src_idx first so the walk is O(arr_size + take).
  size_t picks[kMaxStations];
  for (size_t k = 0; k < take; ++k) picks[k] = rows[k].src_idx;
  std::sort(picks, picks + take);

  size_t written = 0;
  size_t next_pick = 0;
  src_idx = 0;
  for (JsonObjectConst m : arr) {
    if (next_pick < take && picks[next_pick] == src_idx) {
      const char* icao = m["icaoId"].as<const char*>();
      Station& st = s_stations[written];
      std::strncpy(st.icao, icao, sizeof(st.icao) - 1);
      st.icao[sizeof(st.icao) - 1] = '\0';
      st.lat = m["lat"].as<float>();
      st.lon = m["lon"].as<float>();
      st.wind_dir_deg   = m["wdir"].is<int>()     ? m["wdir"].as<int>()  : 0;
      st.wind_speed_kt  = m["wspd"].is<int>()     ? m["wspd"].as<int>()  : 0;
      st.visibility_sm  = parseVisibility(m["visib"]);
      st.ceiling_ft_agl = parseCeiling(m["clouds"].as<JsonArrayConst>());
      st.category       = deriveCategory(st.ceiling_ft_agl, st.visibility_sm);
      ++written;
      ++next_pick;
      if (written >= kMaxStations) break;
    }
    ++src_idx;
  }

  s_station_count = written;
  s_last_update_ms =
#ifdef USE_NATIVE
      (unsigned long)millis();
#else
      millis();
#endif
}

}  // namespace

size_t stationCount() { return s_station_count; }
const Station* stations() { return s_stations; }
unsigned long lastUpdateMs() { return s_last_update_ms; }

void invalidate() {
  s_station_count = 0;
  s_last_update_ms = 0;
}

bool update() {
  const float center_lat = services::metar_config::centerLat();
  const float center_lon = services::metar_config::centerLon();
  const float radius_nm  = services::metar_config::radiusNm();
  float lat_min, lon_min, lat_max, lon_max;
  services::weather::geo::makeBbox(center_lat, center_lon, radius_nm,
                                   &lat_min, &lon_min, &lat_max, &lon_max);

  char bbox[64];
  std::snprintf(bbox, sizeof(bbox), "%.4f,%.4f,%.4f,%.4f",
                lat_min, lon_min, lat_max, lon_max);

#ifdef USE_NATIVE
  char cmd[512];
  std::snprintf(cmd, sizeof(cmd),
                "curl -sf --max-time 8 "
                "'https://aviationweather.gov/api/data/metar?bbox=%s&format=json'",
                bbox);
  FILE* pipe = popen(cmd, "r");
  if (!pipe) return false;
  std::string body;
  body.reserve(32 * 1024);
  char buf[4096];
  while (size_t n = std::fread(buf, 1, sizeof(buf), pipe)) {
    body.append(buf, n);
  }
  const int rc = pclose(pipe);
  if (rc != 0 || body.empty()) {
    std::printf("weather: fetch failed rc=%d body=%zu\n", rc, body.size());
    return false;
  }
  ingestPayload(body.data(), body.size(), center_lat, center_lon);
  return true;
#else
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  char url[512];
  std::snprintf(url, sizeof(url),
                "https://aviationweather.gov/api/data/metar?bbox=%s&format=json",
                bbox);
  if (!http.begin(client, url)) return false;
  http.setTimeout(8000);
  const int code = http.GET();
  if (code != 200) {
    Serial.printf("weather: http %d\n", code);
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();
  ingestPayload(payload.c_str(), payload.length(), center_lat, center_lon);
  return true;
#endif
}

}  // namespace services::weather
