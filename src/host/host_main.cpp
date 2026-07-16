// Native SDL entry point. The firmware's Arduino setup()/loop() is in
// src/main.cpp and pulls in ESP32-only APIs (WiFi, HTTPClient), so it is
// excluded from the native build via platformio.ini build_src_filter. Here we
// substitute a slimmer setup/loop that just renders the radar in a Mac window
// and polls the fake BOOT button (SDL SPACE key) to cycle the range preset.

#ifdef USE_NATIVE

#include <SDL.h>
#include <lgfx/v1/platforms/sdl/Panel_sdl.hpp>
#include <lgfx/v1/platforms/sdl/common.hpp>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "hardware/display.h"
#include "host/config_server.h"
#include "services/adsb_client.h"
#include "services/focus_points.h"
#include "services/metar_config.h"
#include "services/outdoor_temp.h"
#include "services/radar_location.h"
#include "services/wifi_setup.h"
#include "ui/cockpit_screen.h"
#include "ui/layer_style.h"
#include "ui/loading_overlay.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"
#include "ui/weather_map.h"

// Loads the pre-baked SF-area tile into TileStore at boot — emulator
// stand-in for the HTTPS fetch that milestone 2 step 11 wires up on
// real hardware. Defined in src/host/host_stubs.cpp.
namespace host { void loadBootstrapTiles(); }

namespace {

constexpr uint8_t kShotFakeGpio = 10;
// Screen ring: 0..focus_count-1 = radar at each focus; then Weather,
// Cockpit; then a dev-only Offline preview position that only the env
// var can reach (not part of the user-facing tap ring). Mirrors the
// firmware in src/main.cpp so the SDL emulator behaves identically.
size_t g_ring_index = 0;
bool g_offline_preview = false;
unsigned long g_last_non_radar_draw_ms = 0;

inline size_t ringLength() { return services::focus::count() + 2; }
inline bool onRadar()   { return !g_offline_preview && g_ring_index < services::focus::count(); }
inline bool onWeather() { return !g_offline_preview && g_ring_index == services::focus::count(); }
inline bool onCockpit() { return !g_offline_preview && g_ring_index == services::focus::count() + 1; }
constexpr const char* kShotPath = "/tmp/plane-radar-screenshot.ppm";
constexpr unsigned long kAutoShotIntervalMs = 200;

// Read _texturebuf (RGB888, packed) — Panel_sdl's own post-sync buffer that
// gets pushed to SDL_UpdateTexture. Bypasses the streaming-texture write-
// only semantics of SDL_LockTexture.
void saveScreenshot(const char* path) {
  auto& panel = tft.sdlPanel();
  const int w = panel.textureWidth();
  const int h = panel.textureHeight();
  const auto* bytes = static_cast<const uint8_t*>(panel.textureBytes());
  if (bytes == nullptr || w <= 0 || h <= 0) return;

  FILE* f = std::fopen(path, "wb");
  if (f == nullptr) {
    std::printf("screenshot: %s: %s\n", path, std::strerror(errno));
    return;
  }
  std::fprintf(f, "P6\n%d %d\n255\n", w, h);
  std::fwrite(bytes, 1, static_cast<size_t>(w) * h * 3, f);
  std::fclose(f);
}

void enterRadar(size_t focus_idx) {
  g_offline_preview = false;
  g_ring_index = focus_idx;
  services::focus::setIndex(focus_idx);
  const auto& fp = services::focus::current();
  std::printf("View: radar @ %s (%.4f, %.4f)\n", fp.name,
              services::location::lat(), services::location::lon());
  // Kick a fresh fetch so the new center's traffic loads immediately.
  services::adsb::fetchUpdate(services::location::lat(),
                              services::location::lon(),
                              ui::radar::fetchRadiusKm());
  ui::radarDisplayDraw();
}

void enterMetarWeather() {
  g_offline_preview = false;
  g_ring_index = services::focus::count();
  std::printf("View: METAR weather\n");
  ui::weather::refresh();
  ui::weather::draw();
  g_last_non_radar_draw_ms = millis();
}

void enterCockpit() {
  g_offline_preview = false;
  g_ring_index = services::focus::count() + 1;
  std::printf("View: cockpit\n");
  ui::cockpit::refresh();
  ui::cockpit::draw();
  g_last_non_radar_draw_ms = millis();
}

// Dev-only preview of the real firmware's offline banner. On hardware the
// banner is triggered by an actual Wi-Fi drop; here we can't easily fake
// that, so an env var opens the same screen for screenshots and design
// review. Not user-facing — parity aid only.
void enterOffline() {
  g_offline_preview = true;
  std::printf("View: offline banner (dev preview)\n");
  statusScreenOffline();
}

// Advance to the next screen in the ring; wraps back to radar 0.
// Shows an animated spinner during the blocking network fetch + render
// so the user gets immediate feedback on their tap.
void advanceRing() {
  if (g_offline_preview) { enterRadar(0); return; }
  const size_t next = (g_ring_index + 1) % ringLength();
  if (next < services::focus::count()) {
    ui::loading::animateBriefly("Radar");
    enterRadar(next);
  } else if (next == services::focus::count()) {
    ui::loading::animateBriefly("Weather");
    enterMetarWeather();
  } else {
    ui::loading::animateBriefly("Cockpit");
    enterCockpit();
  }
}

// Single-tap = adjust the current screen. Mirrors src/main.cpp's
// adjustCurrent(): range cycle on radar, refresh on weather, nothing on
// cockpit. Offline preview treats any tap as "return to radar."
void adjustCurrent() {
  if (g_offline_preview) { enterRadar(0); return; }
  if (onRadar()) {
    ui::radar::rangeNext();
    char label[12];
    ui::radar::formatCurrentRangeLabel(label, sizeof(label));
    std::printf("Range: %s (outer ~%.0f km)\n", label,
                ui::radar::rangeCurrent().outer_km);
    ui::radarDisplayDraw();
    return;
  }
  if (onWeather()) {
    std::printf("Weather: manual refresh\n");
    ui::weather::refresh();
    ui::weather::draw();
    g_last_non_radar_draw_ms = millis();
    return;
  }
  // Cockpit — no-op.
}

bool consumeShotKey() {
  static bool prev_pressed = false;
  const bool now = !lgfx::v1::gpio_in(kShotFakeGpio);  // active low
  const bool edge = !prev_pressed && now;
  prev_pressed = now;
  return edge;
}

// Native layer toggles — 1..7 flip the corresponding layer. Each key is
// bound to its own fake GPIO so the tap-edge helper can debounce it
// cleanly the same way as the boot/screenshot keys.
struct KeyBinding {
  SDL_KeyCode key;
  uint8_t gpio;
  ui::layers::Layer layer;
};
constexpr KeyBinding kLayerKeys[] = {
    {SDLK_1, 20, ui::layers::Layer::Coastline},
    {SDLK_2, 21, ui::layers::Layer::Land},
    {SDLK_3, 24, ui::layers::Layer::RunwaysLarge},
    {SDLK_4, 26, ui::layers::Layer::AircraftTags},
};

bool consumeLayerKey(const KeyBinding& kb) {
  static bool prev[sizeof(kLayerKeys) / sizeof(kLayerKeys[0])] = {};
  const size_t idx = &kb - &kLayerKeys[0];
  const bool now = !lgfx::v1::gpio_in(kb.gpio);
  const bool edge = !prev[idx] && now;
  prev[idx] = now;
  return edge;
}

}  // namespace

void setup() {
  std::printf(
      "Plane Radar — SDL emulator\n"
      "  SPACE  : tap  (single = adjust current screen,\n"
      "                 double = advance ring)\n"
      "           Radar@Home → Radar@Focus2 → Radar@Focus3\n"
      "                     → Weather → Cockpit → wraps back\n"
      "  S      : save screenshot\n"
      "  1..5   : toggle layer (coastline / land /\n"
      "           runways-large / runways-focus / aircraft-tags)\n");
  bootButtonInit();
  lgfx::v1::gpio_hi(kShotFakeGpio);
  lgfx::Panel_sdl::addKeyCodeMapping(SDLK_s, kShotFakeGpio);
  for (const auto& kb : kLayerKeys) {
    lgfx::v1::gpio_hi(kb.gpio);
    lgfx::Panel_sdl::addKeyCodeMapping(kb.key, kb.gpio);
  }
  displayInit();
  services::location::init();
  host::loadBootstrapTiles();  // must precede first radar draw
  services::metar_config::init();
  ui::radar::rangeInit();
  services::focus::init();
  // Load persisted config from emulator_config.json (if present) so
  // restarts remember what you set via the config web UI. Runs after
  // the service init()s so it overrides their defaults. Then start
  // the mirror-of-WiFiManager HTTP server on 127.0.0.1:8080.
  host::config_server::loadPersistedConfig();
  host::config_server::start();
  g_ring_index = services::focus::currentIndex();
  ui::layers::init();
  ui::cockpit::init();
  ui::radarDisplayDraw();
  // Kick off an initial fetch so the first frame isn't empty.
  services::adsb::fetchUpdate(services::location::lat(),
                              services::location::lon(),
                              ui::radar::fetchRadiusKm());
  // Dev shortcut: PLANE_RADAR_WEATHER=1 opens the METAR weather view on
  // boot; PLANE_RADAR_COCKPIT=1 opens the cockpit clock. snap.sh uses
  // these to capture non-radar screens without keyboard input.
  const char* wxvar = std::getenv("PLANE_RADAR_WEATHER");
  if (wxvar && wxvar[0] == '1') enterMetarWeather();
  const char* cpvar = std::getenv("PLANE_RADAR_COCKPIT");
  if (cpvar && cpvar[0] == '1') enterCockpit();
  const char* offvar = std::getenv("PLANE_RADAR_OFFLINE");
  if (offvar && offvar[0] == '1') enterOffline();
}

void loop() {
  static unsigned long last_shot_ms = 0;
  static unsigned long last_adsb_ms = 0;
  bootButtonPollLongPress();
  // Drain any config-server POST /save changes onto this thread before
  // we render, so a mid-frame HTTP write can't race the render code.
  // Redraws the current view to pick up new home/METAR values right
  // away.
  if (host::config_server::applyPending()) {
    if (onRadar()) ui::radarDisplayDraw();
    else if (onWeather()) { ui::weather::refresh(); ui::weather::draw(); }
    else if (onCockpit()) { ui::cockpit::refresh(); ui::cockpit::draw(); }
  }
  const BootTap ev = bootButtonConsumeEvent();
  if (ev == BootTap::Single)      adjustCurrent();
  else if (ev == BootTap::Double) advanceRing();
  if (consumeShotKey()) {
    saveScreenshot(kShotPath);
    std::printf("screenshot: %s\n", kShotPath);
  }
  for (const auto& kb : kLayerKeys) {
    if (consumeLayerKey(kb)) {
      ui::layers::toggle(kb.layer);
      if (onRadar()) ui::radarDisplayDraw();
    }
  }

  const unsigned long now = millis();
  services::outdoor_temp::loop();
  if (onWeather()) {
    if (now - g_last_non_radar_draw_ms >= 1000) {
      g_last_non_radar_draw_ms = now;
      ui::weather::refresh();
      ui::weather::draw();
    }
  } else if (onCockpit()) {
    if (now - g_last_non_radar_draw_ms >= 1000) {
      g_last_non_radar_draw_ms = now;
      ui::cockpit::refresh();
      ui::cockpit::draw();
    }
  } else if (g_offline_preview) {
    // Static banner — no per-frame work.
  } else {
    // adsb.fi's public rate limit is 1 req/s; matching the firmware's 3 s poll.
    if (now - last_adsb_ms >= 3000) {
      last_adsb_ms = now;
      services::adsb::fetchUpdate(services::location::lat(),
                                  services::location::lon(),
                                  ui::radar::fetchRadiusKm());
    }
    ui::radarDisplayRefreshAircraft();
  }
  if (now - last_shot_ms >= kAutoShotIntervalMs) {
    last_shot_ms = now;
    saveScreenshot(kShotPath);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(33));
}

static int user_func(bool* running) {
  setup();
  while (*running) {
    loop();
  }
  return 0;
}

int main(int, char**) {
  // Ask SDL for smooth upscaling. Panel_sdl scales the 240x240 framebuffer
  // 3x for display; without this, nearest-neighbor makes anti-aliased text
  // look jagged.
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
  return lgfx::Panel_sdl::main(user_func);
}

#endif  // USE_NATIVE
