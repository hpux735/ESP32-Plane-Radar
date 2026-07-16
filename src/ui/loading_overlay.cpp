#include "ui/loading_overlay.h"

#include <lgfx/v1/lgfx_fonts.hpp>

#include <cmath>

#include "config.h"
#include "hardware/display.h"
#include "ui/radar_theme.h"

namespace fonts = lgfx::v1::fonts;

namespace {

constexpr int kCenterX = config::kDisplayWidth / 2;
constexpr int kCenterY = config::kDisplayHeight / 2;
constexpr int kSpinnerRadius = 36;
constexpr int kDotCount = 8;
constexpr int kDotRadius = 3;
constexpr int kEraseRadius = 5;
constexpr float kDegToRad = 0.01745329252f;
constexpr float kStepDeg = 20.0f;
constexpr unsigned long kFrameMs = 40;

void eraseDots(float angle_deg) {
  for (int i = 0; i < kDotCount; ++i) {
    const float a = (angle_deg - static_cast<float>(i) * (360.0f / kDotCount)) *
                    kDegToRad;
    const int x = kCenterX + static_cast<int>(std::lround(std::cos(a) * kSpinnerRadius));
    const int y = kCenterY + static_cast<int>(std::lround(std::sin(a) * kSpinnerRadius));
    tft.fillCircle(x, y, kEraseRadius, ui::radar::kColorBackground);
  }
}

void drawDots(float angle_deg) {
  for (int i = 0; i < kDotCount; ++i) {
    const float a = (angle_deg - static_cast<float>(i) * (360.0f / kDotCount)) *
                    kDegToRad;
    const int x = kCenterX + static_cast<int>(std::lround(std::cos(a) * kSpinnerRadius));
    const int y = kCenterY + static_cast<int>(std::lround(std::sin(a) * kSpinnerRadius));
    const int fade = 255 - i * (255 / kDotCount);
    const uint16_t color = tft.color565(0, fade, 0);
    tft.fillSmoothCircle(x, y, kDotRadius, color);
  }
}

void drawLabel(const char* label) {
  tft.setFont(&fonts::Font0);
  tft.setTextSize(1);
  tft.setTextDatum(textdatum_t::top_center);
  tft.setTextColor(ui::radar::kColorGrid, ui::radar::kColorBackground);
  tft.drawString(label, kCenterX, kCenterY + kSpinnerRadius + 12);
  tft.setTextDatum(textdatum_t::top_left);
}

}  // namespace

namespace ui::loading {

void animateBriefly(const char* label, unsigned long duration_ms) {
  tft.fillScreen(ui::radar::kColorBackground);
  drawLabel(label);

  const unsigned long start = millis();
  float angle = -90.0f;

  while (millis() - start < duration_ms) {
    drawDots(angle);
    tft.display();
    delay(kFrameMs);
    eraseDots(angle);
    angle += kStepDeg;
  }

  drawDots(angle);
  tft.display();
}

}  // namespace ui::loading
