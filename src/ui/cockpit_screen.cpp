#include "ui/cockpit_screen.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "data/tile_math.h"
#include "data/tile_reader.h"
#include "data/tile_store.h"
#include "hardware/display.h"
#include "services/env_sensor.h"
#include "services/outdoor_temp.h"
#include "services/radar_location.h"
#include "services/weather.h"
#include "services/weather_geo.h"
#include "ui/radar_display.h"
#include "ui/radar_theme.h"

namespace ui::cockpit {
namespace {

constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

// Colors picked to read well on the same dark radar background so the
// display doesn't visually re-adjust between screens. Slightly warmer
// whites/grays than pure #FFFFFF/#7BEF for a "instrument panel" feel.
// Green matches typical glass-cockpit (Garmin G1000 / PFD) legend text.
uint16_t colWhite() { return tft.color565(230, 232, 235); }
uint16_t colGray()  { return tft.color565( 96,  96, 104); }
uint16_t colAmber() { return tft.color565(255, 190,  40); }
uint16_t colTemp()  { return tft.color565(180, 200, 230); }
uint16_t colGreen() { return tft.color565( 80, 220,  80); }
uint16_t colFrame() { return tft.color565( 60,  90,  60); }

void drawRadialLine(LGFX_Sprite& g, float angle_rad, int inner, int outer,
                    uint16_t color) {
  const float cx = static_cast<float>(radar::kCenterX);
  const float cy = static_cast<float>(radar::kCenterY);
  const float cs = std::cos(angle_rad);
  const float sn = std::sin(angle_rad);
  const int x0 = static_cast<int>(std::lroundf(cx + cs * inner));
  const int y0 = static_cast<int>(std::lroundf(cy + sn * inner));
  const int x1 = static_cast<int>(std::lroundf(cx + cs * outer));
  const int y1 = static_cast<int>(std::lroundf(cy + sn * outer));
  g.drawLine(x0, y0, x1, y1, color);
}

void drawTicks(LGFX_Sprite& g) {
  for (int i = 0; i < 60; ++i) {
    const float angle = (static_cast<float>(i) * 6.0f - 90.0f) * kDegToRad;
    if (i % 5 == 0) {
      drawRadialLine(g, angle, 92, 108, colWhite());
    } else {
      drawRadialLine(g, angle, 92, 100, colGray());
    }
  }
}

void drawSecondSweep(LGFX_Sprite& g, int seconds) {
  const float angle = (static_cast<float>(seconds) * 6.0f - 90.0f) * kDegToRad;
  const uint16_t c = colWhite();
  // Five parallel lines fanned by a small angular offset so the sweep
  // reads as a solid bar without needing anti-aliasing.
  static const float offs[] = {-0.020f, -0.010f, 0.0f, 0.010f, 0.020f};
  for (float d : offs) {
    drawRadialLine(g, angle + d, 88, 108, c);
  }
}

void drawTime(LGFX_Sprite& g, const std::tm& t) {
  // "1" is narrower in Font7 — nudge the anchor left when the hour starts
  // with '1' so the two glyphs land visually centered.
  const int cx = (t.tm_hour >= 10 && t.tm_hour <= 19) ? 115 : 120;
  const int cy = radar::kCenterY;
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
  g.setFont(&fonts::Font7);
  g.setTextDatum(textdatum_t::middle_center);
  g.setTextColor(colWhite(), radar::kColorBackground);
  g.drawString(buf, cx, cy);
  // Small "L" marker in the same face/size as the Zulu line below, so the
  // two annotations read as a matched pair (local vs Zulu).
  const int time_half_w = g.textWidth(buf) / 2;
  g.setFont(&fonts::Font0);
  g.setTextSize(1);
  g.setTextDatum(textdatum_t::middle_left);
  g.setTextColor(colTemp(), radar::kColorBackground);
  g.drawString("L", cx + time_half_w + 3, cy + 12);
  g.setTextDatum(textdatum_t::top_left);
}

void drawZulu(LGFX_Sprite& g, const std::tm& utc) {
  char buf[12];
  std::snprintf(buf, sizeof(buf), "%02d:%02d Z", utc.tm_hour, utc.tm_min);
  g.setFont(&fonts::Font0);
  g.setTextSize(1);
  g.setTextDatum(textdatum_t::top_center);
  g.setTextColor(colTemp(), radar::kColorBackground);
  g.drawString(buf, radar::kCenterX, 142);
  g.setTextDatum(textdatum_t::top_left);
}

void drawFreshness(LGFX_Sprite& g) {
  const unsigned long last = services::weather::lastUpdateMs();
  char buf[24];
  if (last == 0) {
    std::snprintf(buf, sizeof(buf), "no data");
  } else {
    const unsigned long age_ms = millis() - last;
    const unsigned long age_min = age_ms / 60000UL;
    std::snprintf(buf, sizeof(buf), "%lu min ago", age_min);
  }
  g.setFont(&fonts::Font0);
  g.setTextSize(1);
  g.setTextDatum(textdatum_t::top_center);
  g.setTextColor(colGray(), radar::kColorBackground);
  g.drawString(buf, radar::kCenterX, 30);
  g.setTextDatum(textdatum_t::top_left);
}

// "N.n nm N of KSFO" — home's position relative to the nearest airport
// with an instrument approach. Uses the tile pyramid the runway overlay
// already fetches (per-airport IAP flag lives on AirportView.flags). The
// scan is single-tile (~50 nm across at kRenderZoom); more than enough
// for any home location that isn't in the middle of the ocean.
void drawReferencePosition(LGFX_Sprite& g) {
  const double home_lat = services::location::lat();
  const double home_lon = services::location::lon();
  uint16_t tx = 0, ty = 0;
  data::tile::tileOfLatLon(data::tile::kRenderZoom, home_lat, home_lon,
                            &tx, &ty);
  const auto tbytes = data::tile::store().get(data::tile::kRenderZoom, tx, ty);
  if (tbytes.is_fallback) return;
  data::tile::TileReader reader;
  if (!reader.init(tbytes.data, tbytes.size)) return;
  uint32_t sec_len = 0;
  const uint8_t* p =
      reader.sectionBegin(data::tile::Section::Airports, &sec_len);
  if (p == nullptr || sec_len == 0) return;
  const uint8_t* end = p + sec_len;
  uint16_t airport_count = 0;
  if (!data::tile::TileReader::readSectionCount(&p, end, &airport_count)) return;

  float best_dist = 1.0e9f;
  data::tile::AirportView best{};
  bool have_best = false;
  for (uint16_t i = 0; i < airport_count; ++i) {
    data::tile::AirportView view;
    if (!data::tile::TileReader::readAirport(&p, end, &view)) return;
    if (!view.instrumentApproach()) continue;
    const float apt_lat = static_cast<float>(view.lat_e7) / 1.0e7f;
    const float apt_lon = static_cast<float>(view.lon_e7) / 1.0e7f;
    const float d = services::weather::geo::distanceNm(
        static_cast<float>(home_lat), static_cast<float>(home_lon),
        apt_lat, apt_lon);
    if (d < best_dist) { best_dist = d; best = view; have_best = true; }
  }
  if (!have_best) return;

  char buf[32];
  if (best_dist <= 0.1f) {
    std::snprintf(buf, sizeof(buf), "%s", best.ident);
  } else {
    const float apt_lat = static_cast<float>(best.lat_e7) / 1.0e7f;
    const float apt_lon = static_cast<float>(best.lon_e7) / 1.0e7f;
    // Bearing from airport → home reads naturally as "home is X of airport".
    const float brg = services::weather::geo::bearingDeg(
        apt_lat, apt_lon,
        static_cast<float>(home_lat), static_cast<float>(home_lon));
    std::snprintf(buf, sizeof(buf), "%.1f nm %s of %s",
                  static_cast<double>(best_dist),
                  services::weather::geo::compass8(brg),
                  best.ident);
  }

  g.setFont(&fonts::Font0);
  g.setTextSize(1);
  g.setTextDatum(textdatum_t::top_center);
  g.setTextColor(colGray(), radar::kColorBackground);
  g.drawString(buf, radar::kCenterX, 182);
  g.setTextDatum(textdatum_t::top_left);
}

void drawLabelValue(LGFX_Sprite& g, const char* label, const char* value,
                    int y, uint16_t label_color, uint16_t value_color) {
  const int cx = radar::kCenterX;
  g.setTextDatum(textdatum_t::top_center);
  g.setTextSize(1);
  char line[32];
  std::snprintf(line, sizeof(line), "%s  %s", label, value);
  g.setTextColor(value_color, radar::kColorBackground);
  g.drawString(line, cx, y);
  (void)label_color;
  g.setTextDatum(textdatum_t::top_left);
}

void drawSensorBlock(LGFX_Sprite& g) {
  const services::outdoor_temp::Reading oat = services::outdoor_temp::cached();
  const services::env_sensor::Reading env = services::env_sensor::read();

  // "20C 68F" — Celsius first (per pilot convention: METARs report in C),
  // Fahrenheit second for the US-market default read. Web preview mirrors
  // this format in web/src/cockpitView.ts formatTempCF.
  char oat_val[16];
  if (oat.valid) {
    const float cval = (oat.tempF - 32.0f) * 5.0f / 9.0f;
    std::snprintf(oat_val, sizeof(oat_val), "%.0fC %.0fF",
                  std::round(cval), std::round(oat.tempF));
  } else {
    std::snprintf(oat_val, sizeof(oat_val), "--C --F");
  }
  drawLabelValue(g, "OAT", oat_val, 155, colGray(), colTemp());

  // OAT moved down 7 px to y=155 to make room for the Zulu line at 142
  // and the reference-position line at 182. Web preview matches this
  // layout — see web/src/cockpitView.ts drawSensorBlock. CABIN/RH stay
  // below OAT when a BME280 is installed; the reference-position line
  // is only drawn on cockpit's outer path, so it doesn't collide with
  // an inserted CABIN/RH pair.
  if (env.valid) {
    char cabin_val[16];
    char rh_val[16];
    const float cabin_c = (env.tempF - 32.0f) * 5.0f / 9.0f;
    std::snprintf(cabin_val, sizeof(cabin_val), "%.0fC %.0fF",
                  std::round(cabin_c), std::round(env.tempF));
    std::snprintf(rh_val, sizeof(rh_val), "%.0f%%",
                  std::round(env.humidityPct));
    drawLabelValue(g, "CABIN", cabin_val, 168, colGray(), colTemp());
    drawLabelValue(g, "RH",    rh_val,    182, colGray(), colTemp());
  }
}

// Filled triangle arrow rotated to `angle_rad` (0 rad = pointing right).
// Base at (cx, cy), length `len`, half-width `half_w`.
void drawArrow(LGFX_Sprite& g, int cx, int cy, float angle_rad, int len,
               int half_w, uint16_t color) {
  const float cs = std::cos(angle_rad);
  const float sn = std::sin(angle_rad);
  // Tip of the arrow.
  const float tx = cx + cs * len;
  const float ty = cy + sn * len;
  // Base corners: perpendicular to the arrow axis, at the base end.
  const float bx = cx - cs * (len * 0.15f);
  const float by = cy - sn * (len * 0.15f);
  const float px = -sn * half_w;
  const float py =  cs * half_w;
  g.fillTriangle(static_cast<int>(std::lroundf(tx)),
                 static_cast<int>(std::lroundf(ty)),
                 static_cast<int>(std::lroundf(bx + px)),
                 static_cast<int>(std::lroundf(by + py)),
                 static_cast<int>(std::lroundf(bx - px)),
                 static_cast<int>(std::lroundf(by - py)),
                 color);
}

// Garmin-style wind indicator: small arrow showing where the wind is
// BLOWING TO (i.e. 180° from the meteorological "wind from" direction),
// plus digital "270°/12kt" text. Placed above the time so it stays
// clear of the seven-segment glyphs.
void drawWindIndicator(LGFX_Sprite& g) {
  const services::outdoor_temp::Reading r = services::outdoor_temp::cached();
  const int block_cy = 52;
  const int arrow_cx = radar::kCenterX - 30;
  const uint16_t c = colGreen();

  g.setFont(&fonts::Font0);
  g.setTextSize(1);
  g.setTextDatum(textdatum_t::middle_left);
  g.setTextColor(c, radar::kColorBackground);

  if (!r.valid || std::isnan(r.windKts) || std::isnan(r.windDegFrom)) {
    g.drawString("WND --", radar::kCenterX - 18, block_cy);
    g.setTextDatum(textdatum_t::top_left);
    return;
  }

  // Meteorological "wind from" → direction wind is blowing TO is 180° off.
  // Screen angle convention: 0 rad = +x axis (right), and +y is DOWN, so a
  // heading of 0° (north) points UP → screen angle = -90°. Subtract 90°
  // from the compass heading to convert.
  const float going_to_deg = r.windDegFrom + 180.0f;
  const float angle_rad = (going_to_deg - 90.0f) * kDegToRad;
  drawArrow(g, arrow_cx, block_cy, angle_rad, 10, 4, c);

  char buf[16];
  std::snprintf(buf, sizeof(buf), "%03d/%.0fkt",
                static_cast<int>(std::lroundf(r.windDegFrom)) % 360,
                std::round(r.windKts));
  g.drawString(buf, arrow_cx + 14, block_cy);
  g.setTextDatum(textdatum_t::top_left);
}

// PFD-style altimeter setting (Kollsman window): green digital text in a
// thin box. Placed at the bottom center so it doesn't clash with the
// CABIN/RH lines when a BME280 is installed.
void drawBaroIndicator(LGFX_Sprite& g) {
  const services::outdoor_temp::Reading r = services::outdoor_temp::cached();
  const int block_cy = 205;
  const int half_w = 36;
  const int half_h = 8;
  const uint16_t c = colGreen();

  g.drawRect(radar::kCenterX - half_w, block_cy - half_h,
             half_w * 2, half_h * 2, colFrame());

  g.setFont(&fonts::Font0);
  g.setTextSize(1);
  g.setTextDatum(textdatum_t::middle_center);
  g.setTextColor(c, radar::kColorBackground);
  char buf[16];
  if (r.valid && !std::isnan(r.baroInHg)) {
    std::snprintf(buf, sizeof(buf), "%.2f IN", r.baroInHg);
  } else {
    std::snprintf(buf, sizeof(buf), "--.-- IN");
  }
  g.drawString(buf, radar::kCenterX, block_cy);
  g.setTextDatum(textdatum_t::top_left);
}

// Home local time = UTC + Open-Meteo's utc_offset_seconds for the home
// lat/lon. Open-Meteo populates that offset (DST-aware for the fetch
// moment) whenever the fetch URL carries `&timezone=auto`, which
// outdoor_temp.cpp now does. Falls back to raw UTC when no fetch has
// landed yet — better than showing wall-clock ESP32-boot time.
bool localTimeNow(std::tm* out) {
  const std::time_t now = std::time(nullptr);
  // On ESP32, before SNTP has synced, time() returns a very small value
  // (seconds since boot cast forward). Treat anything before 2020-01-01
  // as "not synced yet" so the display shows the "SYNC" placeholder.
  if (now < 1577836800L) return false;
  const long offset =
      services::outdoor_temp::cached().utcOffsetSec;
  const std::time_t local = now + static_cast<std::time_t>(offset);
  std::tm* tm_ptr = std::gmtime(&local);
  if (tm_ptr == nullptr) return false;
  *out = *tm_ptr;
  return true;
}

bool utcTimeNow(std::tm* out) {
  const std::time_t now = std::time(nullptr);
  if (now < 1577836800L) return false;
  std::tm* tm_ptr = std::gmtime(&now);
  if (tm_ptr == nullptr) return false;
  *out = *tm_ptr;
  return true;
}

void drawUnsyncedPlaceholder(LGFX_Sprite& g) {
  g.setFont(&fonts::Font7);
  g.setTextDatum(textdatum_t::middle_center);
  g.setTextColor(colAmber(), radar::kColorBackground);
  g.drawString("--:--", radar::kCenterX, radar::kCenterY);
  g.setFont(&fonts::Font0);
  g.setTextDatum(textdatum_t::top_center);
  g.setTextSize(1);
  g.setTextColor(colAmber(), radar::kColorBackground);
  g.drawString("SYNC", radar::kCenterX, 148);
  g.setTextDatum(textdatum_t::top_left);
}

}  // namespace

void init() {
  services::outdoor_temp::init();
  services::env_sensor::init();
}

void refresh() {
  services::outdoor_temp::loop();
}

void draw() {
  LGFX_Sprite* sp = radarDisplayFrameSprite();
  if (sp == nullptr) return;  // no fallback — cockpit screen requires sprite.
  LGFX_Sprite& g = *sp;

  g.fillScreen(radar::kColorBackground);
  drawTicks(g);
  drawFreshness(g);
  drawWindIndicator(g);

  std::tm t{};
  std::tm utc{};
  if (localTimeNow(&t)) {
    drawTime(g, t);
    if (utcTimeNow(&utc)) drawZulu(g, utc);
    drawSensorBlock(g);
    drawReferencePosition(g);
    drawSecondSweep(g, t.tm_sec);
  } else {
    drawUnsyncedPlaceholder(g);
    drawSensorBlock(g);
    drawReferencePosition(g);
  }

  drawBaroIndicator(g);

  // Round bezel mask — same as radar/weather screens so SDL matches
  // physical panel.
  g.fillArc(radar::kCenterX, radar::kCenterY, radar::kSize + 8, 120, 0, 360,
            radar::kColorBackground);
  g.pushSprite(0, 0);
  // Release tile buffer — cockpit's reference-position marker reads the tile.
  data::tile::store().endRender();
}

}  // namespace ui::cockpit
