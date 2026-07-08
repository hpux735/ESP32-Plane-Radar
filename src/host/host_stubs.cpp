// Native-only stubs. Compiled only under USE_NATIVE (see platformio.ini
// build_src_filter). Replaces the ESP32-specific services (WiFi/HTTP/NVS/GPIO)
// with just enough surface to make the render path compile and run on desktop.

#ifdef USE_NATIVE

#include <SDL.h>
#include <lgfx/v1/platforms/sdl/Panel_sdl.hpp>
#include <lgfx/v1/platforms/sdl/common.hpp>

#include <ArduinoJson.h>

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "config.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "services/wifi_setup.h"

// The ESP32 build embeds data/ui_font.vlw via `board_build.embed_files`
// which creates objcopy symbols. On native, embed the same bytes with
// .incbin in inline asm — matches the exact linker symbols display_font.cpp
// looks up via asm() labels ("_binary_data_ui_font_vlw_start" /
// "_binary_data_ui_font_vlw_end" on Mach-O).
//
// The path is relative to the PlatformIO build cwd (repo root).
__asm__(
    ".section __DATA,__const\n"
    ".globl _binary_data_ui_font_vlw_start\n"
    "_binary_data_ui_font_vlw_start:\n"
    ".incbin \"data/ui_font.vlw\"\n"
    ".globl _binary_data_ui_font_vlw_end\n"
    "_binary_data_ui_font_vlw_end:\n");

namespace services::adsb {

// Native ADS-B fetch: shells out to /usr/bin/curl (universally available on
// macOS/Linux) and parses adsb.fi's response with ArduinoJson. Same JSON
// schema handling as src/services/adsb_client.cpp — deliberate duplication
// so the native env doesn't need HTTPClient/WiFiClientSecure shims.

static Aircraft s_aircraft[kMaxAircraft];
static size_t s_count = 0;
static PollFn s_poll = nullptr;
static unsigned long s_last_update_ms = 0;
static unsigned long s_fetch_count = 0;

size_t aircraftCount() { return s_count; }
const Aircraft* aircraftList() { return s_aircraft; }
unsigned long lastUpdateMs() { return s_last_update_ms; }
unsigned long fetchCount() { return s_fetch_count; }

void setPollFn(PollFn fn) { s_poll = fn; }

namespace {

constexpr float kKmPerNm = 1.852f;

bool readJsonFloat(JsonObjectConst obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    *out = obj[key].as<float>();
    return true;
  }
  return false;
}

float pickNoseHeading(JsonObjectConst p) {
  float v = 0.0f;
  if (readJsonFloat(p, "true_heading", &v)) return v;
  if (readJsonFloat(p, "mag_heading", &v)) return v;
  if (readJsonFloat(p, "track", &v)) return v;
  if (readJsonFloat(p, "dir", &v)) return v;
  return 0.0f;
}

float pickTrackHeading(JsonObjectConst p) {
  float v = 0.0f;
  if (readJsonFloat(p, "track", &v)) return v;
  if (readJsonFloat(p, "true_heading", &v)) return v;
  if (readJsonFloat(p, "mag_heading", &v)) return v;
  if (readJsonFloat(p, "dir", &v)) return v;
  return 0.0f;
}

float pickGroundSpeed(JsonObjectConst p) {
  float v = 0.0f;
  if (readJsonFloat(p, "gs", &v)) return v;
  if (readJsonFloat(p, "tas", &v)) return v;
  if (readJsonFloat(p, "ias", &v)) return v;
  return 0.0f;
}

bool isOnGround(JsonObjectConst p) {
  if (!p["alt_baro"].is<const char*>()) return false;
  return std::strcmp(p["alt_baro"].as<const char*>(), "ground") == 0;
}

uint16_t pickSquawk(JsonObjectConst p) {
  if (p["squawk"].is<const char*>()) {
    const char* s = p["squawk"].as<const char*>();
    if (s && s[0] != '\0') return static_cast<uint16_t>(std::atoi(s));
  }
  return 0;
}

void copyStringTrimmed(JsonObjectConst obj, const char* key, char* out,
                       size_t out_len) {
  out[0] = '\0';
  if (out_len == 0 || !obj[key].is<const char*>()) return;
  const char* s = obj[key].as<const char*>();
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') --n;
  std::memcpy(out, s, n);
  out[n] = '\0';
}

int32_t pickAltitudeFt(JsonObjectConst p) {
  if (p["alt_baro"].is<const char*>() &&
      std::strcmp(p["alt_baro"].as<const char*>(), "ground") == 0) {
    return INT32_MIN;
  }
  float alt = 0.0f;
  if (readJsonFloat(p, "alt_baro", &alt) ||
      readJsonFloat(p, "alt_geom", &alt)) {
    return static_cast<int32_t>(std::lroundf(alt));
  }
  return INT32_MIN;
}

float pickVerticalRate(JsonObjectConst p) {
  float v = 0.0f;
  if (readJsonFloat(p, "baro_rate", &v)) return v;
  if (readJsonFloat(p, "geom_rate", &v)) return v;
  return 0.0f;
}

}  // namespace

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  const float nm = fetch_radius_km / kKmPerNm;
  char cmd[512];
  // -s silent, -f fail on HTTP error, --max-time 5 total budget.
  std::snprintf(cmd, sizeof(cmd),
                "curl -sf --max-time 5 "
                "'https://opendata.adsb.fi/api/v3/lat/%.6f/lon/%.6f/dist/%.1f'",
                center_lat, center_lon, nm);
  FILE* pipe = popen(cmd, "r");
  if (pipe == nullptr) return false;

  std::string body;
  body.reserve(64 * 1024);
  char buf[4096];
  while (size_t n = std::fread(buf, 1, sizeof(buf), pipe)) {
    body.append(buf, n);
  }
  const int rc = pclose(pipe);
  if (rc != 0 || body.empty()) {
    std::printf("adsb: fetch failed rc=%d body=%zu\n", rc, body.size());
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    std::printf("adsb: json parse: %s\n", err.c_str());
    return false;
  }

  JsonArrayConst ac = doc["ac"].as<JsonArrayConst>();
  if (ac.isNull()) {
    s_count = 0;
    return true;
  }

  size_t n = 0;
  for (JsonObjectConst plane : ac) {
    if (n >= kMaxAircraft) break;
    if (!plane["lat"].is<float>() || !plane["lon"].is<float>()) continue;
    if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) continue;

    s_aircraft[n].lat = plane["lat"].as<float>();
    s_aircraft[n].lon = plane["lon"].as<float>();
    s_aircraft[n].nose_deg = pickNoseHeading(plane);
    s_aircraft[n].track_deg = pickTrackHeading(plane);
    s_aircraft[n].gs_knots = pickGroundSpeed(plane);
    // Callsign: flight (dispatch) → registration / tail → hex transponder.
    copyStringTrimmed(plane, "flight", s_aircraft[n].callsign,
                      sizeof(s_aircraft[n].callsign));
    if (s_aircraft[n].callsign[0] == '\0') {
      copyStringTrimmed(plane, "r", s_aircraft[n].callsign,
                        sizeof(s_aircraft[n].callsign));
    }
    if (s_aircraft[n].callsign[0] == '\0') {
      copyStringTrimmed(plane, "hex", s_aircraft[n].callsign,
                        sizeof(s_aircraft[n].callsign));
    }
    copyStringTrimmed(plane, "t", s_aircraft[n].type,
                      sizeof(s_aircraft[n].type));
    s_aircraft[n].alt_ft = pickAltitudeFt(plane);
    s_aircraft[n].vs_fpm = pickVerticalRate(plane);
    s_aircraft[n].squawk = pickSquawk(plane);
    ++n;
  }
  s_count = n;
  s_last_update_ms = millis();
  ++s_fetch_count;
  std::printf("adsb: %zu aircraft in %.1f nm\n", n, nm);
  return true;
}

}  // namespace services::adsb

namespace services::location {

// Default center for the SDL emulator: user's home in the Mission (SF 94110).
// The real firmware persists this via Preferences and lets the WiFiManager
// portal edit it; here it is compiled in.
static constexpr double kNativeHomeLat = 37.7590;
static constexpr double kNativeHomeLon = -122.4093;

static double s_lat = kNativeHomeLat;
static double s_lon = kNativeHomeLon;
static bool s_override_active = false;
static double s_override_lat = 0.0;
static double s_override_lon = 0.0;

void init() {}
double lat() { return s_override_active ? s_override_lat : s_lat; }
double lon() { return s_override_active ? s_override_lon : s_lon; }
bool saveFromStrings(const char*, const char*) { return false; }
void clear() { s_lat = kNativeHomeLat; s_lon = kNativeHomeLon; }
void setOverride(double lat, double lon) {
  s_override_lat = lat;
  s_override_lon = lon;
  s_override_active = true;
}
void clearOverride() { s_override_active = false; }

}  // namespace services::location

// --- BOOT button: SPACE key on the SDL window latches a tap. --------------
// Panel_sdl's addKeyCodeMapping wires an SDL key to an emulated GPIO whose
// state can then be read via gpio_in(). BOOT is active-LOW on the hardware,
// so pressed = low. The emulated GPIO array defaults to 0, so we explicitly
// raise the pin at init to represent "not pressed".

namespace {

constexpr uint8_t kBootFakeGpio = 9;
bool s_prev_pressed = false;
bool s_tap_latched = false;

bool isBootPressed() {
  return !lgfx::v1::gpio_in(kBootFakeGpio);  // active low
}

}  // namespace

void bootButtonInit() {
  lgfx::v1::gpio_hi(kBootFakeGpio);
  lgfx::Panel_sdl::addKeyCodeMapping(SDLK_SPACE, kBootFakeGpio);
}

bool bootButtonConsumeTap() {
  const bool now = isBootPressed();
  if (!s_prev_pressed && now) {
    s_tap_latched = true;
  }
  s_prev_pressed = now;
  if (s_tap_latched) {
    s_tap_latched = false;
    return true;
  }
  return false;
}

void bootButtonPollLongPress() {
  // No-op on desktop; long-press triggers WiFi reset on hardware, meaningless
  // when we have no persistent WiFi credentials to reset.
}

// Same double-tap discriminator as the firmware. Native uses SPACE (BOOT
// pin fake GPIO) for taps.
BootTap bootButtonConsumeEvent() {
  constexpr unsigned long kDoubleTapWindowMs = 250;
  static bool s_pending_single = false;
  static unsigned long s_first_tap_ms = 0;

  const bool tap = bootButtonConsumeTap();
  const unsigned long now = millis();

  if (tap) {
    if (s_pending_single && (now - s_first_tap_ms) <= kDoubleTapWindowMs) {
      s_pending_single = false;
      return BootTap::Double;
    }
    s_pending_single = true;
    s_first_tap_ms = now;
    return BootTap::None;
  }
  if (s_pending_single && (now - s_first_tap_ms) > kDoubleTapWindowMs) {
    s_pending_single = false;
    return BootTap::Single;
  }
  return BootTap::None;
}

#endif  // USE_NATIVE
