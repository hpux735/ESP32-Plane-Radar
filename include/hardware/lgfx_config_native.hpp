#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/sdl/Panel_sdl.hpp>

#include "config.h"

// Subclass exposes Panel_sdl's protected `_texturebuf` — the RGB888 buffer
// Panel_sdl composes for SDL_UpdateTexture. Reading it directly gives us
// exactly what SDL is about to render, no LockTexture semantics involved.
class Panel_sdl_pub : public lgfx::Panel_sdl {
 public:
  const void* textureBytes() const { return _texturebuf; }
  int textureWidth() const { return _cfg.panel_width; }
  int textureHeight() const { return _cfg.panel_height; }
};

// Custom LGFX for Panel_sdl. Mirrors the reference LGFX_AutoDetect_sdl class
// (init_impl override to skip HW reset, board_SDL tag).
class LGFX : public lgfx::LGFX_Device {
  Panel_sdl_pub _panel;

  bool init_impl(bool /*use_reset*/, bool use_clear) {
    return lgfx::LGFX_Device::init_impl(false, use_clear);
  }

 public:
  LGFX() {
    auto cfg = _panel.config();
    cfg.memory_width = config::kDisplayWidth;
    cfg.panel_width = config::kDisplayWidth;
    cfg.memory_height = config::kDisplayHeight;
    cfg.panel_height = config::kDisplayHeight;
    _panel.config(cfg);
    _panel.setWindowTitle("Plane Radar (SDL)");
    _panel.setScaling(3, 3);
    setPanel(&_panel);
    _board = lgfx::board_t::board_SDL;
  }

  Panel_sdl_pub& sdlPanel() { return _panel; }
};
