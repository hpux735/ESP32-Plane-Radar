// Unity tests for services::tile_cache — the SPIFFS filename helpers.
//
// SPIFFS itself can't be exercised from the native env (no partition,
// no fs::File), but the pure filename helpers are the contract that
// firmware-side hydrate/persist depend on. If filenameFor / parseFilename
// stop round-tripping, the persisted cache silently gets orphaned.
//
// Run via `pio test -e native_test`.

#include <cstdint>
#include <cstring>
#include <unity.h>

#include "services/tile_cache.h"

#define NATIVE_STUBS_DEFINE
#include "../common/native_stubs.h"

void setUp(void) {}
void tearDown(void) {}

void test_filenameFor_writes_stable_slash_prefixed_name(void) {
  char buf[64];
  const size_t n = services::tile_cache::filenameFor(7, 20, 37, buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_STRING("/tile_7_20_37.bin", buf);
}

void test_filenameFor_max_dimensions_fit(void) {
  char buf[64];
  const size_t n = services::tile_cache::filenameFor(255, 65535, 65535, buf,
                                                      sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_STRING("/tile_255_65535_65535.bin", buf);
}

void test_filenameFor_rejects_undersized_buffer(void) {
  char buf[10];  // too small even for the smallest name
  const size_t n = services::tile_cache::filenameFor(7, 20, 37, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_UINT32(0, n);
}

void test_filenameFor_rejects_null_buffer(void) {
  TEST_ASSERT_EQUAL_UINT32(0, services::tile_cache::filenameFor(0, 0, 0, nullptr, 64));
}

void test_parseFilename_round_trips(void) {
  char buf[64];
  services::tile_cache::filenameFor(7, 20, 37, buf, sizeof(buf));
  uint8_t z = 0;
  uint16_t x = 0, y = 0;
  TEST_ASSERT_TRUE(services::tile_cache::parseFilename(buf, &z, &x, &y));
  TEST_ASSERT_EQUAL_UINT8(7, z);
  TEST_ASSERT_EQUAL_UINT16(20, x);
  TEST_ASSERT_EQUAL_UINT16(37, y);
}

void test_parseFilename_accepts_basename_without_leading_slash(void) {
  uint8_t z = 0;
  uint16_t x = 0, y = 0;
  TEST_ASSERT_TRUE(
      services::tile_cache::parseFilename("tile_3_4_5.bin", &z, &x, &y));
  TEST_ASSERT_EQUAL_UINT8(3, z);
  TEST_ASSERT_EQUAL_UINT16(4, x);
  TEST_ASSERT_EQUAL_UINT16(5, y);
}

void test_parseFilename_rejects_garbage(void) {
  uint8_t z = 0;
  uint16_t x = 0, y = 0;
  TEST_ASSERT_FALSE(
      services::tile_cache::parseFilename("random.bin", &z, &x, &y));
  TEST_ASSERT_FALSE(
      services::tile_cache::parseFilename(nullptr, &z, &x, &y));
  TEST_ASSERT_FALSE(
      services::tile_cache::parseFilename("/tile_7_20_37.txt", &z, &x, &y));
}

void test_parseFilename_rejects_trailing_junk(void) {
  uint8_t z = 0;
  uint16_t x = 0, y = 0;
  TEST_ASSERT_FALSE(
      services::tile_cache::parseFilename("/tile_7_20_37.bin.tmp", &z, &x, &y));
}

void test_parseFilename_rejects_out_of_range_values(void) {
  uint8_t z = 0;
  uint16_t x = 0, y = 0;
  // z=256 overflows uint8
  TEST_ASSERT_FALSE(
      services::tile_cache::parseFilename("/tile_256_0_0.bin", &z, &x, &y));
  // x=65536 overflows uint16
  TEST_ASSERT_FALSE(
      services::tile_cache::parseFilename("/tile_0_65536_0.bin", &z, &x, &y));
}

int main(int /*argc*/, char** /*argv*/) {
  UNITY_BEGIN();
  RUN_TEST(test_filenameFor_writes_stable_slash_prefixed_name);
  RUN_TEST(test_filenameFor_max_dimensions_fit);
  RUN_TEST(test_filenameFor_rejects_undersized_buffer);
  RUN_TEST(test_filenameFor_rejects_null_buffer);
  RUN_TEST(test_parseFilename_round_trips);
  RUN_TEST(test_parseFilename_accepts_basename_without_leading_slash);
  RUN_TEST(test_parseFilename_rejects_garbage);
  RUN_TEST(test_parseFilename_rejects_trailing_junk);
  RUN_TEST(test_parseFilename_rejects_out_of_range_values);
  return UNITY_END();
}
