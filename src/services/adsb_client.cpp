#include "services/adsb_client.h"

// HTTPClient / WiFiClientSecure are ESP32-only. Under UNIT_TEST
// (pio test -e native_test) we skip the fetchUpdate implementation and
// only compile the pure parse path so tests don't need HTTP host shims.
#ifndef UNIT_TEST
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#endif

#include <ArduinoJson.h>

#include <climits>
#include <cmath>
#include <cstring>

#include "config.h"

namespace services::adsb {

namespace {

constexpr char kApiBase[] = "https://opendata.adsb.fi/api/v3/lat/";
constexpr float kKmPerNm = 1.852f;
constexpr int kConnectAttemptMs = 200;
constexpr unsigned long kRequestTimeoutMs = 10000;

Aircraft s_aircraft[kMaxAircraft];
size_t s_aircraft_count = 0;
PollFn s_poll_fn = nullptr;
unsigned long s_last_update_ms = 0;
unsigned long s_fetch_count = 0;

// Streaming-parse filter — built once, reused. Whitelists only the keys we
// read below so the whole body never lives in RAM. Keep in sync with the
// field lookups in pickNoseHeading/pickTrackHeading/pickGroundSpeed/
// pickAltitudeFt/pickVerticalRate/pickSquawk/isOnGround/fillTagFields.
JsonDocument s_filter;
bool s_filter_built = false;

void ensureFilterBuilt() {
  if (s_filter_built) return;
  JsonObject ac0 = s_filter["ac"][0].to<JsonObject>();
  ac0["lat"] = true;
  ac0["lon"] = true;
  ac0["flight"] = true;
  ac0["r"] = true;
  ac0["hex"] = true;
  ac0["t"] = true;
  ac0["alt_baro"] = true;
  ac0["alt_geom"] = true;
  ac0["baro_rate"] = true;
  ac0["geom_rate"] = true;
  ac0["squawk"] = true;
  ac0["true_heading"] = true;
  ac0["mag_heading"] = true;
  ac0["track"] = true;
  ac0["dir"] = true;
  ac0["gs"] = true;
  ac0["tas"] = true;
  ac0["ias"] = true;
  s_filter_built = true;
}

constexpr size_t kMinFreeHeapForFetch = 50000;

void pollNetwork() {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

#ifndef UNIT_TEST
int performGetWithPoll(HTTPClient& http) {
  http.setConnectTimeout(kConnectAttemptMs);
  const unsigned long deadline = millis() + kRequestTimeoutMs;
  while (millis() < deadline) {
    pollNetwork();
    const int code = http.GET();
    if (code > 0) {
      return code;
    }
    if (code != HTTPC_ERROR_CONNECTION_REFUSED &&
        code != HTTPC_ERROR_NOT_CONNECTED) {
      return code;
    }
    delay(5);
  }
  return HTTPC_ERROR_READ_TIMEOUT;
}
#endif  // !UNIT_TEST

float kmToNauticalMiles(float km) { return km / kKmPerNm; }

bool readJsonFloat(const JsonObject& obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    *out = obj[key].as<float>();
    return true;
  }
  return false;
}

float pickNoseHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickTrackHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickGroundSpeed(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "gs", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "tas", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "ias", &v)) {
    return v;
  }
  return 0.0f;
}

bool isOnGround(const JsonObject& plane) {
  if (!plane["alt_baro"].is<const char*>()) {
    return false;
  }
  return strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0;
}

void copyJsonStringTrimmed(const JsonObject& obj, const char* key, char* out,
                           size_t out_len) {
  out[0] = '\0';
  if (out_len == 0 || !obj[key].is<const char*>()) {
    return;
  }
  const char* s = obj[key].as<const char*>();
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

int32_t pickAltitudeFt(const JsonObject& plane) {
  if (plane["alt_baro"].is<const char*>() &&
      strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0) {
    return INT32_MIN;  // on-ground sentinel
  }
  float alt = 0.0f;
  if (readJsonFloat(plane, "alt_baro", &alt) ||
      readJsonFloat(plane, "alt_geom", &alt)) {
    return static_cast<int32_t>(lroundf(alt));
  }
  return INT32_MIN;
}

float pickVerticalRate(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "baro_rate", &v)) return v;
  if (readJsonFloat(plane, "geom_rate", &v)) return v;
  return 0.0f;
}

uint16_t pickSquawk(const JsonObject& plane) {
  if (plane["squawk"].is<const char*>()) {
    const char* s = plane["squawk"].as<const char*>();
    if (s && s[0] != '\0') return static_cast<uint16_t>(atoi(s));
  }
  return 0;
}

size_t populateFromArray(JsonArray ac) {
  if (ac.isNull()) return 0;
  size_t n = 0;
  for (JsonObject plane : ac) {
    if (n >= kMaxAircraft) break;
    if (!plane["lat"].is<float>() || !plane["lon"].is<float>()) continue;
    if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) continue;
    s_aircraft[n].lat = plane["lat"].as<float>();
    s_aircraft[n].lon = plane["lon"].as<float>();
    s_aircraft[n].nose_deg = pickNoseHeading(plane);
    s_aircraft[n].track_deg = pickTrackHeading(plane);
    s_aircraft[n].gs_knots = pickGroundSpeed(plane);
    // fillTagFields is defined below — forward-declare so it's visible
    // here.
    void fillTagFields(Aircraft*, const JsonObject&);
    fillTagFields(&s_aircraft[n], plane);
    ++n;
  }
  return n;
}

void fillTagFields(Aircraft* ac, const JsonObject& plane) {
  // Callsign preference: flight (dispatch callsign, e.g. UAL1234) →
  // registration / tail number (e.g. N12345) → hex ICAO transponder code
  // as last resort. adsb.fi reports registration in "r".
  copyJsonStringTrimmed(plane, "flight", ac->callsign, sizeof(ac->callsign));
  if (ac->callsign[0] == '\0') {
    copyJsonStringTrimmed(plane, "r", ac->callsign, sizeof(ac->callsign));
  }
  if (ac->callsign[0] == '\0') {
    copyJsonStringTrimmed(plane, "hex", ac->callsign, sizeof(ac->callsign));
  }
  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  ac->alt_ft = pickAltitudeFt(plane);
  ac->vs_fpm = pickVerticalRate(plane);
  ac->squawk = pickSquawk(plane);
}

}  // namespace

void setPollFn(PollFn fn) { s_poll_fn = fn; }

size_t aircraftCount() { return s_aircraft_count; }

const Aircraft* aircraftList() { return s_aircraft; }

unsigned long lastUpdateMs() { return s_last_update_ms; }

unsigned long fetchCount() { return s_fetch_count; }

#ifndef UNIT_TEST
bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  // Bail early when the heap is tight — the previous full-body-into-String
  // path used to crash with NoMemory/EmptyInput on busy sectors (SFO Bravo).
  // With streaming + filter the parse itself is much smaller, but the TLS
  // client still allocates a few KB, so leave a safety margin.
  if (ESP.getFreeHeap() < kMinFreeHeapForFetch) {
    Serial.printf("adsb: skip (low heap %u)\n",
                  static_cast<unsigned>(ESP.getFreeHeap()));
    return false;
  }

  const float dist_nm = kmToNauticalMiles(fetch_radius_km);

  String url = kApiBase;
  url += String(center_lat, 6);
  url += "/lon/";
  url += String(center_lon, 6);
  url += "/dist/";
  url += String(dist_nm, 1);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("adsb: http.begin failed");
    return false;
  }

  http.setTimeout(kRequestTimeoutMs);
  const int code = performGetWithPoll(http);
  if (code != HTTP_CODE_OK) {
    Serial.printf("adsb: HTTP %d\n", code);
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    Serial.println("adsb: no stream");
    http.end();
    return false;
  }

  ensureFilterBuilt();

  JsonDocument doc;
  const DeserializationError err = deserializeJson(
      doc, *stream, DeserializationOption::Filter(s_filter));
  http.end();
  if (err) {
    Serial.printf("adsb: JSON parse error: %s\n", err.c_str());
    return false;
  }

  s_aircraft_count = populateFromArray(doc["ac"].as<JsonArray>());
  s_last_update_ms = millis();
  ++s_fetch_count;
  Serial.printf("adsb: %u aircraft\n", static_cast<unsigned>(s_aircraft_count));
  return true;
}
#endif  // !UNIT_TEST

bool ingestPayloadForTest(const char* body, unsigned long body_len) {
  ensureFilterBuilt();
  JsonDocument doc;
  const DeserializationError err = deserializeJson(
      doc, body, static_cast<size_t>(body_len),
      DeserializationOption::Filter(s_filter));
  if (err) return false;
  s_aircraft_count = populateFromArray(doc["ac"].as<JsonArray>());
  ++s_fetch_count;
  return true;
}

}  // namespace services::adsb
