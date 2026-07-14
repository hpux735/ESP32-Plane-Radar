#include "services/tile_cache.h"

#include <cstdio>
#include <cstring>

#include "data/tile_store.h"

#ifndef USE_NATIVE
#include <Arduino.h>
#include <SPIFFS.h>
#endif

namespace services::tile_cache {

using data::tile::kMaxTileBytes;

size_t filenameFor(uint8_t z, uint16_t x, uint16_t y, char* out, size_t out_len) {
  // Flat SPIFFS layout: /tile_<z>_<x>_<y>.bin
  // Max length: strlen("/tile_255_65535_65535.bin") = 25 + NUL = 26. Round up.
  if (out == nullptr || out_len < 32) return 0;
  const int n = std::snprintf(out, out_len, "/tile_%u_%u_%u.bin",
                                static_cast<unsigned>(z),
                                static_cast<unsigned>(x),
                                static_cast<unsigned>(y));
  if (n <= 0 || static_cast<size_t>(n) >= out_len) return 0;
  return static_cast<size_t>(n);
}

bool parseFilename(const char* name, uint8_t* z, uint16_t* x, uint16_t* y) {
  if (name == nullptr) return false;
  // Support both "/tile_..." (SPIFFS absolute) and "tile_..." (basename).
  const char* p = (name[0] == '/') ? name + 1 : name;
  unsigned zv = 0, xv = 0, yv = 0;
  int consumed = 0;
  const int rc = std::sscanf(p, "tile_%u_%u_%u.bin%n", &zv, &xv, &yv, &consumed);
  if (rc != 3) return false;
  // Reject trailing junk — the whole name must be consumed.
  if (p[consumed] != '\0') return false;
  if (zv > 0xFF || xv > 0xFFFF || yv > 0xFFFF) return false;
  *z = static_cast<uint8_t>(zv);
  *x = static_cast<uint16_t>(xv);
  *y = static_cast<uint16_t>(yv);
  return true;
}

#ifndef USE_NATIVE

void mountSpiffs() {
  if (!SPIFFS.begin(true)) {
    Serial.println("tile_cache: SPIFFS.begin failed");
    return;
  }
  Serial.printf("tile_cache: SPIFFS mounted (%u used of %u total bytes)\n",
                 static_cast<unsigned>(SPIFFS.usedBytes()),
                 static_cast<unsigned>(SPIFFS.totalBytes()));
  // Deliberately NOT pre-loading tiles into RAM here — the tile store
  // loads from SPIFFS on-demand at the first get() of each render and
  // frees on endRender(). Boot-time hydration would just be wasted
  // heap work that immediately gets freed by the first endRender().
}

bool persist(uint8_t z, uint16_t x, uint16_t y,
             const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0 || size > kMaxTileBytes) return false;
  char path[32];
  if (filenameFor(z, x, y, path, sizeof(path)) == 0) return false;
  fs::File f = SPIFFS.open(path, "w");
  if (!f) {
    Serial.printf("tile_cache: SPIFFS.open(%s, \"w\") failed\n", path);
    return false;
  }
  const size_t written = f.write(data, size);
  f.close();
  if (written != size) {
    Serial.printf("tile_cache: short write to %s (%u of %u)\n",
                   path, static_cast<unsigned>(written),
                   static_cast<unsigned>(size));
    return false;
  }
  return true;
}

#else  // USE_NATIVE

// Emulator variant: bootstrap tile is loaded in host_stubs; no-op here.
void mountSpiffs() {}

bool persist(uint8_t /*z*/, uint16_t /*x*/, uint16_t /*y*/,
             const uint8_t* /*data*/, size_t /*size*/) {
  return true;
}

#endif

}  // namespace services::tile_cache
