/**
 * Plane Radar — WiFi setup, then radar UI on the round GC9A01 display.
 */

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/focus_points.h"
#include "services/metar_config.h"
#include "services/ota_update.h"
#include "services/outdoor_temp.h"
#include "services/radar_location.h"
#include "services/tap_sensor.h"
#include "services/tile_cache.h"
#include "services/tile_fetch.h"
#include "services/wifi_setup.h"
#include "ui/cockpit_screen.h"
#include "ui/layer_style.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"
#include "ui/weather_map.h"

namespace {

// Two-gesture UX. The whole app is a ring of screens navigated with
// double-tap; single-tap adjusts whatever's on the current screen.
//   Radar#N (one per focus airport) → Weather → Cockpit → wraps.
// Position 0 is the "Home" focus (services::location's saved home);
// positions 1..N-1 mirror the focus ring's user-editable airports.

bool g_radar_visible = false;
bool g_offline_banner_drawn = false;
size_t g_ring_index = 0;  // 0..focus_count-1 = radar; then Weather, Cockpit
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_adsb_fetch_ms = 0;
unsigned long g_last_non_radar_draw_ms = 0;

inline size_t ringLength() {
  // N radar slots (one per focus airport) + weather + cockpit.
  return services::focus::count() + 2;
}
inline bool onRadar()   { return g_ring_index < services::focus::count(); }
inline bool onWeather() { return g_ring_index == services::focus::count(); }
inline bool onCockpit() { return g_ring_index == services::focus::count() + 1; }

void showRadarIfConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    return;
  }
  ui::radarDisplayDraw();
  g_radar_visible = true;
}

void enterRadar(size_t focus_idx) {
  services::focus::setIndex(focus_idx);
  const auto& fp = services::focus::current();
  Serial.printf("View: radar @ %s\n", fp.name);
  if (WiFi.status() == WL_CONNECTED) {
    // Force a fresh fetch at the new center so the first frame has traffic.
    services::adsb::fetchUpdate(services::location::lat(),
                                services::location::lon(),
                                ui::radar::fetchRadiusKm());
  }
  showRadarIfConnected();
}

void enterMetarWeather() {
  Serial.println("View: METAR weather");
  if (WiFi.status() == WL_CONNECTED) ui::weather::refresh();
  ui::weather::draw();
  g_last_non_radar_draw_ms = millis();
}

void enterCockpit() {
  Serial.println("View: cockpit");
  ui::cockpit::refresh();
  ui::cockpit::draw();
  g_last_non_radar_draw_ms = millis();
}

// Advance to the next screen in the ring: 3 radar slots → weather →
// cockpit → wrap. Skips the render for the destination screen up-front;
// the loop's per-frame path will keep it painted from there.
void advanceRing() {
  g_ring_index = (g_ring_index + 1) % ringLength();
  if (onRadar())        enterRadar(g_ring_index);
  else if (onWeather()) enterMetarWeather();
  else if (onCockpit()) enterCockpit();
}

// Single-tap = adjust the current screen in place.
//   Radar   → cycle range preset.
//   Weather → force an immediate METAR refresh.
//   Cockpit → no-op (no user-tunable state).
void adjustCurrent() {
  if (onRadar()) {
    ui::radar::rangeNext();
    char range_label[12];
    ui::radar::formatCurrentRangeLabel(range_label, sizeof(range_label));
    Serial.printf("Range: %s (outer ~%.0f km)\n", range_label,
                  ui::radar::rangeCurrent().outer_km);
    if (g_radar_visible && WiFi.status() == WL_CONNECTED) {
      ui::radarDisplayDraw();
    }
    return;
  }
  if (onWeather()) {
    Serial.println("Weather: manual refresh");
    if (WiFi.status() == WL_CONNECTED) ui::weather::refresh();
    ui::weather::draw();
    g_last_non_radar_draw_ms = millis();
    return;
  }
  // Cockpit — no-op.
}

void handleBootButton() {
  bootButtonPollLongPress();
  const BootTap ev = bootButtonConsumeEvent();
  if (ev == BootTap::None) return;
  if (ev == BootTap::Single) adjustCurrent();
  else                        advanceRing();  // Double
}

void fetchAndDrawAircraft() {
  const float fetch_km = ui::radar::fetchRadiusKm();
  if (!services::adsb::fetchUpdate(services::location::lat(),
                                   services::location::lon(), fetch_km)) {
    handleBootButton();
    return;
  }
  ui::radarDisplayRefreshAircraft();
  handleBootButton();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Plane Radar");

  bootButtonInit();
  displayInit();
  if (wifiShowsSetupScreenOnBoot()) {
    statusScreenPortal();
  }
  services::location::init();
  services::tile_cache::mountSpiffs();
  services::metar_config::init();
  ui::radar::rangeInit();
  services::focus::init();
  // Boot the ring at whichever focus was persisted so returning users see
  // their last radar view. Weather / cockpit are always reached via
  // double-tap and never persisted as boot state.
  g_ring_index = services::focus::currentIndex();
  ui::layers::init();
  ui::cockpit::init();
  // Optional case-tap accelerometer. Silent no-op if the chip isn't
  // wired; BOOT-button gestures continue to work.
  services::tap_sensor::init();
  services::adsb::setPollFn(wifiLoop);

  if (wifiSetupConnect()) {
    // Boot-time tile fetch BEFORE the 58 KB 8bpp sprite is allocated.
    // Order matters: sprite alloc takes a 58 KB contiguous chunk that
    // fragments the heap enough to break the 32 KB mbedTLS record
    // buffer + 32 KB tile buffer transient the fetch needs. Fetching
    // first (with ~200+ KB free and largely un-fragmented) lets the
    // tile land in SPIFFS + the RAM store.
    Serial.printf("boot: pre-sprite tile fetch (heap free=%u largest=%u)\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    if (services::tile_fetch::fetchHomeTileSync()) {
      Serial.println("boot: home tile ready");
    } else {
      Serial.println("boot: home tile fetch failed — will retry in main loop");
    }
    // NOW pre-alloc the frame sprite. Deferring past first render caused
    // the "sequential per-element repaint" flicker — sprite alloc kept
    // failing intermittently as heap fragmentation shifted around, and
    // every failure dropped the render into the direct-panel fallback.
    // Doing it here (post-fetch, pre-render) is the sweet spot: the tile
    // buffer is already accounted for, and the sprite alloc doesn't have
    // to compete with mbedTLS for a contiguous 32 KB chunk anymore.
    Serial.printf("boot: pre-alloc sprite (heap free=%u largest=%u)\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    if (!ui::radarDisplayPreallocateFrameSprite()) {
      Serial.printf("boot: frame sprite pre-alloc FAILED (free=%u largest=%u) "
                    "— renders will flicker\n",
                    ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    }
    showRadarIfConnected();
  }
}

void loop() {
  // Poll the tap sensor before dispatching, so any knock latched since
  // the last tick is visible to bootButtonConsumeEvent().
  services::tap_sensor::poll();
  handleBootButton();
  wifiLoop();
  // ArduinoOTA server disabled: on ESP32-C3 with no PSRAM the ~10 KB
  // resident UDP/TCP listener + mDNS record was significant heap pressure.
  // Firmware updates go through the WiFiManager /update endpoint (curl POST
  // to http://plane-radar.local/u) instead — same effect, no always-on
  // server. See src/services/ota_update.cpp — the setHostname NVS write
  // path is still exposed for future re-enablement.
  // services::ota::loop();

  if (WiFi.status() != WL_CONNECTED) {
    if (g_radar_visible) {
      Serial.println("WiFi lost — will reconnect");
      g_radar_visible = false;
    }

    if (g_wifi_down_since == 0) {
      g_wifi_down_since = millis();
    }

    const unsigned long down_ms = millis() - g_wifi_down_since;
    // After the grace window, replace the stale radar frame with a clear
    // "no network" banner so the desk toy never sits on a half-working
    // screen. Drawn once; reset when Wi-Fi comes back.
    if (down_ms >= config::kWifiDownGraceMs && !g_offline_banner_drawn) {
      statusScreenOffline();
      g_offline_banner_drawn = true;
    }
    if (down_ms >= config::kWifiDownGraceMs &&
        millis() - g_last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
      g_last_reconnect_ms = millis();
      if (wifiReconnect()) {
        g_wifi_down_since = 0;
        g_offline_banner_drawn = false;
        showRadarIfConnected();
      }
    }
  } else {
    g_wifi_down_since = 0;
    g_offline_banner_drawn = false;
    services::outdoor_temp::loop();
    // Kicks a tile download only when the location tile has changed.
    services::tile_fetch::loop();
    if (onWeather()) {
      // Repaint every ~1s so the "n min ago" age updates smoothly;
      // refresh() itself no-ops until the 5 min TTL expires.
      if (millis() - g_last_non_radar_draw_ms >= 1000) {
        g_last_non_radar_draw_ms = millis();
        ui::weather::refresh();
        ui::weather::draw();
      }
    } else if (onCockpit()) {
      // Repaint every ~1s for a smooth second-sweep.
      if (millis() - g_last_non_radar_draw_ms >= 1000) {
        g_last_non_radar_draw_ms = millis();
        ui::cockpit::refresh();
        ui::cockpit::draw();
      }
    } else if (!g_radar_visible) {
      showRadarIfConnected();
    } else if (millis() - g_last_adsb_fetch_ms >= config::kAdsbFetchIntervalMs) {
      g_last_adsb_fetch_ms = millis();
      fetchAndDrawAircraft();
    }
  }

  delay(10);
}
