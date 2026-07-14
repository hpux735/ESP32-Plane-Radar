#include "data/tile_store.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "data/fallback_tile.h"

#ifndef USE_NATIVE
#include <SPIFFS.h>
#endif

namespace data::tile {

namespace {

// Read /tile_z_x_y.bin from SPIFFS into a freshly-malloc'd buffer. Returns
// nullptr on any failure (file missing, alloc failed, short read). Caller
// owns the returned buffer.
//
// Kept private to tile_store to avoid a circular include with tile_cache
// (tile_cache already depends on tile_store for hydration writes; the
// tiny amount of duplication here is cheaper than untangling that).
#ifndef USE_NATIVE
uint8_t* readTileFromSpiffs(uint8_t z, uint16_t x, uint16_t y,
                             size_t* out_size) {
  char path[32];
  const int n = std::snprintf(path, sizeof(path), "/tile_%u_%u_%u.bin",
                                static_cast<unsigned>(z),
                                static_cast<unsigned>(x),
                                static_cast<unsigned>(y));
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(path)) return nullptr;
  fs::File f = SPIFFS.open(path, "r");
  if (!f) return nullptr;
  const size_t size = f.size();
  if (size == 0 || size > 128 * 1024) { f.close(); return nullptr; }
  uint8_t* buf = static_cast<uint8_t*>(std::malloc(size));
  if (buf == nullptr) { f.close(); return nullptr; }
  const size_t read = f.read(buf, size);
  f.close();
  if (read != size) { std::free(buf); return nullptr; }
  *out_size = size;
  return buf;
}
#endif

}  // namespace

TileStore& store() {
  static TileStore s_instance;
  return s_instance;
}

TileStore::TileStore() {
  for (auto& e : entries_) {
    e.used = false;
    e.buffer = nullptr;
    e.size = 0;
  }
}

TileStore::~TileStore() {
  clear();
}

int TileStore::findEntry(uint8_t z, uint16_t x, uint16_t y) const {
  for (size_t i = 0; i < kTileCacheCapacity; ++i) {
    const Entry& e = entries_[i];
    if (e.used && e.z == z && e.x == x && e.y == y) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int TileStore::findLruSlot() const {
  // Prefer an unused slot; otherwise the oldest last_used_tick.
  int lru = 0;
  uint32_t oldest = 0xFFFFFFFFu;
  for (size_t i = 0; i < kTileCacheCapacity; ++i) {
    const Entry& e = entries_[i];
    if (!e.used) {
      return static_cast<int>(i);
    }
    if (e.last_used_tick < oldest) {
      oldest = e.last_used_tick;
      lru = static_cast<int>(i);
    }
  }
  return lru;
}

TileBytes TileStore::get(uint8_t z, uint16_t x, uint16_t y) {
  const int idx = findEntry(z, x, y);
  if (idx >= 0) {
    entries_[idx].last_used_tick = ++tick_;
    return TileBytes{entries_[idx].buffer, entries_[idx].size, false};
  }
#ifndef USE_NATIVE
  // Cache miss — try SPIFFS. This is the mechanism that makes endRender()
  // safe to call every frame: the tile disappears from RAM between
  // renders but the next get() re-loads it in ~20 ms.
  size_t size = 0;
  uint8_t* buf = readTileFromSpiffs(z, x, y, &size);
  if (buf != nullptr) {
    // putOwning takes the buffer and caches it under (z,x,y). If it
    // fails (only pre-condition failures, never OOM here since the
    // buffer is already allocated), it frees `buf` itself.
    if (putOwning(z, x, y, buf, size)) {
      return TileBytes{entries_[findEntry(z, x, y)].buffer, size, false};
    }
  }
#endif
  return TileBytes{kFallbackTile, kFallbackTileSize, true};
}

bool TileStore::put(uint8_t z, uint16_t x, uint16_t y,
                     const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0) return false;

  // Update-in-place if this key already exists — otherwise a repeated
  // put() for the same tile would evict a *different* cache entry
  // instead of refreshing itself.
  int slot = findEntry(z, x, y);
  if (slot < 0) {
    slot = findLruSlot();
    Entry& victim = entries_[slot];
    if (victim.used && victim.buffer != nullptr) {
      std::free(victim.buffer);
      victim.buffer = nullptr;
    }
  } else {
    // Same key: reuse the buffer if it fits, otherwise realloc.
    if (entries_[slot].size != size) {
      std::free(entries_[slot].buffer);
      entries_[slot].buffer = nullptr;
    }
  }

  Entry& e = entries_[slot];
  if (e.buffer == nullptr) {
    e.buffer = static_cast<uint8_t*>(std::malloc(size));
    if (e.buffer == nullptr) {
      e.used = false;
      e.size = 0;
      return false;
    }
  }
  std::memcpy(e.buffer, data, size);
  e.used = true;
  e.z = z;
  e.x = x;
  e.y = y;
  e.size = size;
  e.last_used_tick = ++tick_;
  return true;
}

bool TileStore::putOwning(uint8_t z, uint16_t x, uint16_t y,
                          uint8_t* data, size_t size) {
  if (data == nullptr || size == 0) {
    if (data != nullptr) std::free(data);
    return false;
  }
  int slot = findEntry(z, x, y);
  if (slot < 0) {
    slot = findLruSlot();
  }
  Entry& e = entries_[slot];
  // Free any previous buffer at this slot (either an evicted LRU entry
  // or the same key being refreshed).
  if (e.buffer != nullptr) {
    std::free(e.buffer);
    e.buffer = nullptr;
  }
  e.buffer = data;  // take ownership — no memcpy, no second malloc
  e.used = true;
  e.z = z;
  e.x = x;
  e.y = y;
  e.size = size;
  e.last_used_tick = ++tick_;
  return true;
}

size_t TileStore::cachedCount() const {
  size_t n = 0;
  for (const auto& e : entries_) {
    if (e.used) ++n;
  }
  return n;
}

void TileStore::clear() {
  for (auto& e : entries_) {
    if (e.buffer != nullptr) {
      std::free(e.buffer);
      e.buffer = nullptr;
    }
    e.used = false;
    e.size = 0;
  }
  tick_ = 0;
}

void TileStore::endRender() {
  // Same mechanics as clear() but semantically distinct: this fires every
  // render frame, whereas clear() was only meant for test teardown. Leaving
  // the tick counter alone so a follow-up put() still gets a monotonically
  // increasing tick if the caller repopulates.
  for (auto& e : entries_) {
    if (e.buffer != nullptr) {
      std::free(e.buffer);
      e.buffer = nullptr;
    }
    e.used = false;
    e.size = 0;
  }
}

}  // namespace data::tile
