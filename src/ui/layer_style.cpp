#include "ui/layer_style.h"

#include <Arduino.h>
#include <Preferences.h>

#include <cstring>

namespace ui::layers {
namespace {

constexpr char kPrefsNamespace[] = "layers";

// Defaults: everything on. Order MUST match Layer enum.
constexpr LayerStyle kLayerDefaults[static_cast<size_t>(Layer::kCount)] = {
    {"coastline",     true},
    {"land",          true},
    {"roads",         true},
    {"runways_large", true},
    {"runways_focus", true},
    {"aircraft_tags", true},
};

LayerStyle s_layers[static_cast<size_t>(Layer::kCount)] = {
    kLayerDefaults[0], kLayerDefaults[1], kLayerDefaults[2],
    kLayerDefaults[3], kLayerDefaults[4], kLayerDefaults[5],
};

inline size_t idx(Layer l) { return static_cast<size_t>(l); }

}  // namespace

void init() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) return;
  for (size_t i = 0; i < static_cast<size_t>(Layer::kCount); ++i) {
    s_layers[i].enabled =
        prefs.getBool(kLayerDefaults[i].name, kLayerDefaults[i].enabled);
  }
  prefs.end();
}

bool enabled(Layer layer) { return s_layers[idx(layer)].enabled; }

bool toggle(Layer layer) {
  const size_t i = idx(layer);
  s_layers[i].enabled = !s_layers[i].enabled;
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putBool(kLayerDefaults[i].name, s_layers[i].enabled);
    prefs.end();
  }
  Serial.printf("layer: %s = %s\n", s_layers[i].name,
                s_layers[i].enabled ? "on" : "off");
  return s_layers[i].enabled;
}

const char* name(Layer layer) { return s_layers[idx(layer)].name; }

}  // namespace ui::layers
