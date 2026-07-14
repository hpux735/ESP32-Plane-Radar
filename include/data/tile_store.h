// In-memory tile cache with LRU eviction, backed by the flash-embedded
// fallback tile when nothing more specific is available.
//
// Rendering code asks for a tile at (z, x, y). If it's not cached,
// get() falls back to the world-overview tile in flash — never
// returns nullptr. That means the render path never has to think
// about "map data missing" as a separate state.
//
// Persistence: this layer is RAM-only. Milestone 2 step 9 adds a
// SPIFFS-backed variant that persists across reboots. Milestone 2
// step 10 adds HTTPS fetch that populates the cache.
//
// Not thread-safe: designed to be called only from the render/network
// task on the ESP32 (Arduino loop() thread), same as the existing
// baked-data access.
#pragma once

#include <cstddef>
#include <cstdint>

namespace data::tile {

// Cache capacity. Shrunk from 4 to 1 slot: the 4-slot design was eating
// 90-160 KB of heap (`put` mallocs per-tile at tile_store.cpp:88,
// hydrated from every SPIFFS file at boot in tile_cache.cpp:52-78,
// never evicted unless home moves) and starving the 115 KB radar frame
// sprite. The device sits at a fixed home lat/lon so we only ever
// touch one home-tile at a time; on the rare occasion home moves, the
// old tile evicts and the new one fetches on the next cycle. Fallback
// to the flash-embedded world tile (~14 KB in flash, not RAM) covers
// any cache miss so the render never nulls.
constexpr size_t kTileCacheCapacity = 1;

// Result of a get() call. If `is_fallback` is true, the pointer is
// into the flash-embedded fallback tile — caller must not free() it.
// If false, the pointer is into the RAM cache — same rule (owned by
// TileStore).
struct TileBytes {
  const uint8_t* data;
  size_t size;
  bool is_fallback;
};

// Global singleton — overlays call data::tile::store().get(...) rather
// than each holding their own reference. One-copy design keeps the
// LRU state coherent across all callers.
class TileStore;
TileStore& store();

class TileStore {
 public:
  TileStore();
  ~TileStore();

  // Non-copyable so heap-owned buffers don't get double-freed.
  TileStore(const TileStore&) = delete;
  TileStore& operator=(const TileStore&) = delete;

  // Return the tile at (z, x, y). Falls back to the flash-embedded
  // world tile if not cached. Never returns null pointers.
  TileBytes get(uint8_t z, uint16_t x, uint16_t y);

  // Copy `size` bytes into the cache under the (z, x, y) key. Evicts
  // the least-recently-used entry if capacity is full. Returns false
  // if allocation failed (input tile too big for heap).
  bool put(uint8_t z, uint16_t x, uint16_t y,
           const uint8_t* data, size_t size);

  // How many cache slots currently hold a fetched tile. Fallback is
  // not counted.
  size_t cachedCount() const;

  // Clear the RAM cache (fallback remains). Test-only convenience.
  void clear();

 private:
  struct Entry {
    bool used;
    uint8_t z;
    uint16_t x;
    uint16_t y;
    uint32_t last_used_tick;
    uint8_t* buffer;
    size_t size;
  };

  int findEntry(uint8_t z, uint16_t x, uint16_t y) const;
  int findLruSlot() const;

  Entry entries_[kTileCacheCapacity];
  uint32_t tick_ = 0;
};

}  // namespace data::tile
