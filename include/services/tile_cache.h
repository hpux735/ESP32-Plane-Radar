// SPIFFS-backed persistence for the tile cache.
//
// The 896 KB SPIFFS partition (see partitions/plane_radar.csv) is
// otherwise unused; this module gives it a purpose. Once the HTTPS
// fetcher populates a tile, persist() writes it there so it survives
// power cycles. Tiles are read from SPIFFS on-demand at each render
// (see data::tile::TileStore::get) rather than pre-hydrated at boot,
// because holding tiles in RAM across renders fragmented the heap
// past the mbedTLS handshake budget.
//
// ESP32-only. The native emulator loads its bootstrap tile from disk
// directly (see src/host/host_stubs.cpp::loadBootstrapTiles), so
// this file is excluded from the native build via build_src_filter.
#pragma once

#include <cstddef>
#include <cstdint>

namespace services::tile_cache {

// Mount the SPIFFS partition (formatting on first-ever boot). No-op
// beyond mounting: hydration into RAM is not done here — the tile
// store re-reads from SPIFFS per render (see the module comment). If
// the partition refuses to mount, the device falls back to the
// flash-embedded overview tile until the next successful fetch.
void mountSpiffs();

// Write `size` bytes to SPIFFS under a name derived from (z, x, y).
// Overwrites any prior file at that name. Returns false on I/O
// failure (partition full, mount failed, etc.).
bool persist(uint8_t z, uint16_t x, uint16_t y,
             const uint8_t* data, size_t size);

// Pure helper — pack (z, x, y) into a stable SPIFFS filename that
// mountAndHydrate() knows how to reverse. Returns the number of
// bytes written (excluding the trailing NUL), or 0 if out_len was
// too small. Buffer must be at least 32 bytes.
size_t filenameFor(uint8_t z, uint16_t x, uint16_t y, char* out, size_t out_len);

// Pure helper — parse a filename back into (z, x, y). Returns true
// on match, false otherwise (e.g. random garbage in the partition).
bool parseFilename(const char* name, uint8_t* z, uint16_t* x, uint16_t* y);

}  // namespace services::tile_cache
