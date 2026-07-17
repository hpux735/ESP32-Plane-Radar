#include "hardware/display.h"

#include "hardware/display_font.h"

LGFX tft;

void displayInit() {
  tft.init();
  tft.setRotation(0);
  tft.setBrightness(255);
  tft.setTextWrap(false);
  displayFontInit();
}

void displaySetPowered(bool on) {
  // The Super Mini GC9A01 boards ship with the backlight tied to VCC
  // (no BL pin in lgfx_config.hpp), so tft.setBrightness(0) is a
  // no-op and doesn't actually darken the screen. Achieve the same
  // effect two ways instead:
  //   1) Paint the framebuffer black — the LEDs still emit, but every
  //      pixel is off so the screen looks dark.
  //   2) Send the GC9A01 panel-sleep command via LovyanGFX so the
  //      controller stops driving the pixels (belt + braces).
  // On wake we reverse both: exit panel sleep and hand control back to
  // the caller, which re-renders whichever screen the ring is on.
  if (on) {
    tft.wakeup();
    tft.setBrightness(255);
  } else {
    tft.fillScreen(TFT_BLACK);
    tft.setBrightness(0);
    tft.sleep();
  }
}
