#pragma once

#include <cstddef>
#include <cstdint>

// Per-layer display style. M4 ships with enable/disable — the color and
// thickness fields exist for M4.5 when a settings UI can drive them, but
// today every layer just reads its constexpr default palette.
//
// Adding a layer:
//   1. Add an entry to `enum class Layer` below.
//   2. Add its default (name + enabled) to kLayerDefaults in layer_style.cpp.
//   3. Wrap that layer's draw call with `if (!ui::layers::enabled(Layer::X))`.
//   4. If native, hook a keyboard mapping in host_main.cpp.

namespace ui::layers {

enum class Layer : uint8_t {
  Coastline,
  Land,
  RunwaysLarge,   // large-airport runway + label overlay
  RunwaysFocus,   // focus GA airport (SQL/HAF/PAO/HWD) when it's the focus
  AircraftTags,   // 2-line data blocks (icons + track vectors always show)
  kCount,
};

struct LayerStyle {
  const char* name;   // "coastline", "land", ...
  bool enabled;
};

/** Load persisted enable flags from Preferences into the in-memory table.
 *  Call once during setup. */
void init();

/** True if the layer should be drawn this frame. */
bool enabled(Layer layer);

/** Flip a layer's enabled flag and persist. Returns the new state. */
bool toggle(Layer layer);

/** Human-readable name for logging / UI. */
const char* name(Layer layer);

}  // namespace ui::layers
