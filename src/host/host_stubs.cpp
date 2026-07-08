// Native-only stubs. Compiled only under USE_NATIVE (see platformio.ini
// build_src_filter). Replaces the ESP32-specific services (WiFi/HTTP/NVS/GPIO)
// with just enough surface to make the render path compile and run on desktop.

#ifdef USE_NATIVE

#include <SDL.h>
#include <lgfx/v1/platforms/sdl/Panel_sdl.hpp>
#include <lgfx/v1/platforms/sdl/common.hpp>

#include <cstdint>

#include "services/adsb_client.h"
#include "services/radar_location.h"

// The ESP32 build embeds data/ui_font.vlw as objcopy symbols. On native we
// don't have a valid VLW to embed. We need vlwDataLen() = end - start = 0
// so displayFontInit() skips the load and s_vlw_loaded stays false — otherwise
// a non-zero garbage length trips loadFont() into returning true, then
// radar_display's label-metric calibration iterates against garbage font
// metrics and produces a text size of ~5700 px which wipes the whole radar
// on the first setTextColor(fg, bg) drawString.
//
// C linkage: naming without the leading underscore lets Mach-O's automatic
// underscore prepend produce the `_binary_...` linker symbols the extern in
// display_font.cpp expects (via asm() label). `extern` on the declaration
// forces external linkage (file-scope `const` would otherwise be internal).
// The .set assembly directive aliases _end to _start so their difference is 0.
extern "C" {
extern const uint8_t binary_data_ui_font_vlw_start[];
const uint8_t binary_data_ui_font_vlw_start[] = {0};
}
__asm__(".globl _binary_data_ui_font_vlw_end\n"
        ".set _binary_data_ui_font_vlw_end, _binary_data_ui_font_vlw_start\n");

namespace services::adsb {

static Aircraft s_none[1] = {};
static size_t s_count = 0;
static PollFn s_poll = nullptr;

size_t aircraftCount() { return s_count; }
const Aircraft* aircraftList() { return s_none; }

void setPollFn(PollFn fn) { s_poll = fn; }

bool fetchUpdate(double, double, float) {
  // TODO(M1.5): call adsb.fi via libcurl and populate s_none.
  return false;
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

void init() {}
double lat() { return s_lat; }
double lon() { return s_lon; }
bool saveFromStrings(const char*, const char*) { return false; }
void clear() { s_lat = kNativeHomeLat; s_lon = kNativeHomeLon; }

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

#endif  // USE_NATIVE
