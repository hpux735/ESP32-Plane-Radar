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
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"

// Provided by src/host/host_stubs.cpp
void bootButtonInit();
bool bootButtonConsumeTap();
void bootButtonPollLongPress();

namespace {

constexpr uint8_t kShotFakeGpio = 10;
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

void onRangeTap() {
  ui::radar::rangeNext();
  char label[12];
  ui::radar::formatCurrentRangeLabel(label, sizeof(label));
  std::printf("Range: %s (outer ~%.0f km)\n", label,
              ui::radar::rangeCurrent().outer_km);
  ui::radarDisplayDraw();
}

bool consumeShotKey() {
  static bool prev_pressed = false;
  const bool now = !lgfx::v1::gpio_in(kShotFakeGpio);  // active low
  const bool edge = !prev_pressed && now;
  prev_pressed = now;
  return edge;
}

}  // namespace

void setup() {
  std::printf("Plane Radar — SDL emulator (SPACE=range, S=screenshot)\n");
  bootButtonInit();
  lgfx::v1::gpio_hi(kShotFakeGpio);
  lgfx::Panel_sdl::addKeyCodeMapping(SDLK_s, kShotFakeGpio);
  displayInit();
  services::location::init();
  ui::radar::rangeInit();
  ui::radarDisplayDraw();
  // Kick off an initial fetch so the first frame isn't empty.
  services::adsb::fetchUpdate(services::location::lat(),
                              services::location::lon(),
                              ui::radar::fetchRadiusKm());
}

void loop() {
  static unsigned long last_shot_ms = 0;
  static unsigned long last_adsb_ms = 0;
  bootButtonPollLongPress();
  if (bootButtonConsumeTap()) {
    onRangeTap();
  }
  if (consumeShotKey()) {
    saveScreenshot(kShotPath);
    std::printf("screenshot: %s\n", kShotPath);
  }

  const unsigned long now = millis();
  // adsb.fi's public rate limit is 1 req/s; matching the firmware's 3 s poll.
  if (now - last_adsb_ms >= 3000) {
    last_adsb_ms = now;
    services::adsb::fetchUpdate(services::location::lat(),
                                services::location::lon(),
                                ui::radar::fetchRadiusKm());
  }
  ui::radarDisplayRefreshAircraft();
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
