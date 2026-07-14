// Unity tests for data::tile::TileStore — the in-memory cache with
// flash-fallback that firmware renders read from.
//
// The tests use synthetic byte buffers (not real tiles) to keep the
// assertions about caching behavior clean; separate tests
// (test_fallback_tile, test_tile_reader) cover byte-format concerns.
//
// Run via `pio test -e native_test`.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unity.h>

#include "data/fallback_tile.h"
#include "data/tile_store.h"

#define NATIVE_STUBS_DEFINE
#include "../common/native_stubs.h"

void setUp(void) {}
void tearDown(void) {}

// Tiny sentinel buffers — the byte content only has to be distinct
// enough for the tests to notice which one was returned.
static const uint8_t kTileA[] = {0xAA, 0xAA, 0xAA, 0xAA};
static const uint8_t kTileB[] = {0xBB, 0xBB, 0xBB, 0xBB};
static const uint8_t kTileC[] = {0xCC, 0xCC, 0xCC, 0xCC};
static const uint8_t kTileD[] = {0xDD, 0xDD, 0xDD, 0xDD};
static const uint8_t kTileE[] = {0xEE, 0xEE, 0xEE, 0xEE};

void test_get_on_empty_cache_returns_fallback(void) {
  data::tile::TileStore s;
  auto b = s.get(7, 20, 37);
  TEST_ASSERT_TRUE(b.is_fallback);
  TEST_ASSERT_EQUAL_PTR(data::tile::kFallbackTile, b.data);
  TEST_ASSERT_EQUAL_UINT32(data::tile::kFallbackTileSize, b.size);
  TEST_ASSERT_EQUAL_UINT32(0, s.cachedCount());
}

void test_put_then_get_returns_cached_bytes(void) {
  data::tile::TileStore s;
  TEST_ASSERT_TRUE(s.put(7, 20, 37, kTileA, sizeof(kTileA)));
  auto b = s.get(7, 20, 37);
  TEST_ASSERT_FALSE(b.is_fallback);
  TEST_ASSERT_EQUAL_UINT32(sizeof(kTileA), b.size);
  TEST_ASSERT_EQUAL_MEMORY(kTileA, b.data, sizeof(kTileA));
  TEST_ASSERT_EQUAL_UINT32(1, s.cachedCount());
}

void test_put_makes_copy_so_caller_buffer_can_change(void) {
  uint8_t caller_buf[] = {0x11, 0x22, 0x33, 0x44};
  data::tile::TileStore s;
  s.put(7, 20, 37, caller_buf, sizeof(caller_buf));
  // Mutate caller's buffer AFTER put(): cached bytes must be unaffected.
  caller_buf[0] = 0x99;
  caller_buf[3] = 0x99;
  auto b = s.get(7, 20, 37);
  TEST_ASSERT_EQUAL_UINT8(0x11, b.data[0]);
  TEST_ASSERT_EQUAL_UINT8(0x44, b.data[3]);
}

void test_put_same_key_twice_updates_in_place(void) {
  data::tile::TileStore s;
  s.put(7, 20, 37, kTileA, sizeof(kTileA));
  s.put(7, 20, 37, kTileB, sizeof(kTileB));
  TEST_ASSERT_EQUAL_UINT32(1, s.cachedCount());
  auto b = s.get(7, 20, 37);
  TEST_ASSERT_EQUAL_MEMORY(kTileB, b.data, sizeof(kTileB));
}

void test_get_wrong_key_falls_back(void) {
  data::tile::TileStore s;
  s.put(7, 20, 37, kTileA, sizeof(kTileA));
  auto b = s.get(7, 20, 99);
  TEST_ASSERT_TRUE(b.is_fallback);
}

void test_cache_evicts_lru_when_full(void) {
  // Written to be capacity-agnostic. Fills the cache with N distinct
  // tiles (N = kTileCacheCapacity), touches the first N-1 so the last
  // one is the LRU, inserts one more, and asserts the LRU entry is
  // gone while the touched ones remain. Works whether the cache is 1
  // slot (heap-tight ESP32-C3 build) or larger.
  const size_t N = data::tile::kTileCacheCapacity;
  const uint8_t* payloads[] = {kTileA, kTileB, kTileC, kTileD, kTileE};
  constexpr size_t payload_len = sizeof(kTileA);
  static_assert(sizeof(payloads) / sizeof(payloads[0]) >= 2,
                "test needs at least 2 distinct payloads");
  TEST_ASSERT_TRUE_MESSAGE(
      N + 1 <= sizeof(payloads) / sizeof(payloads[0]),
      "cache capacity outgrew this test's payload table — extend");
  data::tile::TileStore s;
  for (size_t i = 0; i < N; ++i) {
    s.put(7, 0, static_cast<uint16_t>(i), payloads[i], payload_len);
  }
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(N), s.cachedCount());
  // Touch all except the last so the last is LRU.
  for (size_t i = 0; i + 1 < N; ++i) {
    (void)s.get(7, 0, static_cast<uint16_t>(i));
  }
  // Insert one more — must evict slot (N-1).
  s.put(7, 0, static_cast<uint16_t>(N), payloads[N], payload_len);
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(N), s.cachedCount());
  if (N > 1) {
    for (size_t i = 0; i + 1 < N; ++i) {
      TEST_ASSERT_FALSE(s.get(7, 0, static_cast<uint16_t>(i)).is_fallback);
    }
  }
  TEST_ASSERT_TRUE(s.get(7, 0, static_cast<uint16_t>(N - 1)).is_fallback);
  TEST_ASSERT_FALSE(s.get(7, 0, static_cast<uint16_t>(N)).is_fallback);
}

void test_clear_drops_all_cached_entries(void) {
  const size_t N = data::tile::kTileCacheCapacity;
  data::tile::TileStore s;
  for (size_t i = 0; i < N; ++i) {
    s.put(7, 0, static_cast<uint16_t>(i), kTileA, sizeof(kTileA));
  }
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(N), s.cachedCount());
  s.clear();
  TEST_ASSERT_EQUAL_UINT32(0, s.cachedCount());
  TEST_ASSERT_TRUE(s.get(7, 0, 0).is_fallback);
}

void test_putOwning_takes_ownership_no_memcpy(void) {
  // Caller allocates a fresh buffer, hands it to putOwning, and MUST
  // NOT free it afterwards — store now owns it. Read-back must match.
  auto* buf = static_cast<uint8_t*>(std::malloc(sizeof(kTileA)));
  TEST_ASSERT_NOT_NULL(buf);
  std::memcpy(buf, kTileA, sizeof(kTileA));
  data::tile::TileStore s;
  TEST_ASSERT_TRUE(s.putOwning(7, 20, 37, buf, sizeof(kTileA)));
  auto b = s.get(7, 20, 37);
  TEST_ASSERT_FALSE(b.is_fallback);
  TEST_ASSERT_EQUAL_MEMORY(kTileA, b.data, sizeof(kTileA));
  // Store's cleanup path (destructor) frees the buffer — caller must not.
}

void test_putOwning_frees_buf_on_precondition_failure(void) {
  // Nullptr / zero size must free the buffer (or accept nullptr silently)
  // so callers can hand off unconditionally without leaking.
  data::tile::TileStore s;
  TEST_ASSERT_FALSE(s.putOwning(0, 0, 0, nullptr, 4));   // nullptr → false
  auto* buf = static_cast<uint8_t*>(std::malloc(8));
  TEST_ASSERT_NOT_NULL(buf);
  // Zero-size path: putOwning still owns buf and must free it. If it leaks,
  // valgrind would catch — the assertion here is just that we accept.
  TEST_ASSERT_FALSE(s.putOwning(0, 0, 0, buf, 0));
}

void test_put_reject_null_or_zero(void) {
  data::tile::TileStore s;
  TEST_ASSERT_FALSE(s.put(0, 0, 0, nullptr, 4));
  TEST_ASSERT_FALSE(s.put(0, 0, 0, kTileA, 0));
  TEST_ASSERT_EQUAL_UINT32(0, s.cachedCount());
}

// The native bootstrap-tile hook is what makes endRender() every frame safe
// on the SDL emulator: the tile leaves the RAM cache but stays reachable
// via a static buffer host_stubs registered at boot. Regressing this makes
// the emulator render only the flash outlines from frame 2 onward.
void test_native_bootstrap_survives_endRender(void) {
  data::tile::setHostBootstrapBuffer(7, 20, 37, kTileA, sizeof(kTileA));
  data::tile::TileStore s;
  auto b1 = s.get(7, 20, 37);
  TEST_ASSERT_FALSE(b1.is_fallback);
  TEST_ASSERT_EQUAL_MEMORY(kTileA, b1.data, sizeof(kTileA));
  s.endRender();
  auto b2 = s.get(7, 20, 37);
  TEST_ASSERT_FALSE(b2.is_fallback);
  TEST_ASSERT_EQUAL_MEMORY(kTileA, b2.data, sizeof(kTileA));
  data::tile::setHostBootstrapBuffer(0, 0, 0, nullptr, 0);  // clear
}

void test_native_bootstrap_wrong_key_falls_back(void) {
  data::tile::setHostBootstrapBuffer(7, 20, 37, kTileA, sizeof(kTileA));
  data::tile::TileStore s;
  auto b = s.get(7, 20, 38);
  TEST_ASSERT_TRUE(b.is_fallback);
  data::tile::setHostBootstrapBuffer(0, 0, 0, nullptr, 0);  // clear
}

int main(int /*argc*/, char** /*argv*/) {
  UNITY_BEGIN();
  RUN_TEST(test_get_on_empty_cache_returns_fallback);
  RUN_TEST(test_put_then_get_returns_cached_bytes);
  RUN_TEST(test_put_makes_copy_so_caller_buffer_can_change);
  RUN_TEST(test_put_same_key_twice_updates_in_place);
  RUN_TEST(test_get_wrong_key_falls_back);
  RUN_TEST(test_cache_evicts_lru_when_full);
  RUN_TEST(test_clear_drops_all_cached_entries);
  RUN_TEST(test_putOwning_takes_ownership_no_memcpy);
  RUN_TEST(test_putOwning_frees_buf_on_precondition_failure);
  RUN_TEST(test_put_reject_null_or_zero);
  RUN_TEST(test_native_bootstrap_survives_endRender);
  RUN_TEST(test_native_bootstrap_wrong_key_falls_back);
  return UNITY_END();
}
