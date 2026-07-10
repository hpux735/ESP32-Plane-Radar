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
#include "services/tile_cache.h"
#include "services/wifi_setup.h"
#include "ui/cockpit_screen.h"
#include "ui/layer_style.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"
#include "ui/weather_map.h"

namespace {

// Three-position cycle driven by triple-tap. Single/double taps only mean
// something on Radar; on MetarWeather/Cockpit any tap returns to Radar.
enum class Screen : uint8_t { Radar, MetarWeather, Cockpit };

bool g_radar_visible = false;
Screen g_screen = Screen::Radar;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_adsb_fetch_ms = 0;
unsigned long g_last_non_radar_draw_ms = 0;

void showRadarIfConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    return;
  }
  ui::radarDisplayDraw();
  g_radar_visible = true;
}

void onRangeTap() {
  ui::radar::rangeNext();
  char range_label[12];
  ui::radar::formatCurrentRangeLabel(range_label, sizeof(range_label));
  Serial.printf("Range: %s (outer ~%.0f km)\n", range_label,
                ui::radar::rangeCurrent().outer_km);

  if (g_radar_visible && WiFi.status() == WL_CONNECTED) {
    ui::radarDisplayDraw();
  }
}

void onFocusTap() {
  services::focus::cycle();
  const auto& fp = services::focus::current();
  Serial.printf("Focus: %s\n", fp.name);
  if (g_radar_visible && WiFi.status() == WL_CONNECTED) {
    // Force a fresh fetch at the new center so the display updates fast.
    services::adsb::fetchUpdate(services::location::lat(),
                                services::location::lon(),
                                ui::radar::fetchRadiusKm());
    ui::radarDisplayDraw();
  }
}

void enterMetarWeather() {
  g_screen = Screen::MetarWeather;
  Serial.println("View: METAR weather");
  if (WiFi.status() == WL_CONNECTED) ui::weather::refresh();
  ui::weather::draw();
  g_last_non_radar_draw_ms = millis();
}

void enterCockpit() {
  g_screen = Screen::Cockpit;
  Serial.println("View: cockpit");
  ui::cockpit::refresh();
  ui::cockpit::draw();
  g_last_non_radar_draw_ms = millis();
}

void returnToRadar() {
  g_screen = Screen::Radar;
  Serial.println("View: radar");
  if (g_radar_visible && WiFi.status() == WL_CONNECTED) {
    ui::radarDisplayDraw();
  }
}

void handleBootButton() {
  bootButtonPollLongPress();
  const BootTap ev = bootButtonConsumeEvent();
  if (ev == BootTap::None) return;

  // Cycle: Radar → METAR → Cockpit → Radar. Triple-tap advances forward;
  // any single/double tap on a non-radar screen returns home.
  switch (g_screen) {
    case Screen::Radar:
      switch (ev) {
        case BootTap::Single: onRangeTap(); break;
        case BootTap::Double: onFocusTap(); break;
        case BootTap::Triple: enterMetarWeather(); break;
        case BootTap::None:   break;
      }
      break;
    case Screen::MetarWeather:
      if (ev == BootTap::Triple) enterCockpit();
      else                       returnToRadar();
      break;
    case Screen::Cockpit:
      returnToRadar();
      break;
  }
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
  services::tile_cache::mountAndHydrate();
  services::metar_config::init();
  ui::radar::rangeInit();
  services::focus::init();
  ui::layers::init();
  ui::cockpit::init();
  services::adsb::setPollFn(wifiLoop);

  if (wifiSetupConnect()) {
    showRadarIfConnected();
  }
}

void loop() {
  handleBootButton();
  wifiLoop();
  services::ota::loop();

  if (WiFi.status() != WL_CONNECTED) {
    if (g_radar_visible) {
      Serial.println("WiFi lost — will reconnect");
      g_radar_visible = false;
    }

    if (g_wifi_down_since == 0) {
      g_wifi_down_since = millis();
    }

    const unsigned long down_ms = millis() - g_wifi_down_since;
    if (down_ms >= config::kWifiDownGraceMs &&
        millis() - g_last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
      g_last_reconnect_ms = millis();
      if (wifiReconnect()) {
        g_wifi_down_since = 0;
        showRadarIfConnected();
      }
    }
  } else {
    g_wifi_down_since = 0;
    services::outdoor_temp::loop();
    if (g_screen == Screen::MetarWeather) {
      // Repaint every ~1s so the "n min ago" age updates smoothly;
      // refresh() itself no-ops until the 5 min TTL expires.
      if (millis() - g_last_non_radar_draw_ms >= 1000) {
        g_last_non_radar_draw_ms = millis();
        ui::weather::refresh();
        ui::weather::draw();
      }
    } else if (g_screen == Screen::Cockpit) {
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
