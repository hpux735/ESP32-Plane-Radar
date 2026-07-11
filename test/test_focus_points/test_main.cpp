// Tests for services::focus — the double-tap airport ring persisted in
// NVS as a JSON array. Uses the in-memory Preferences shim so save/load
// round-trips work headless.

#include <unity.h>
#include <cstring>

#include <Preferences.h>
#include "services/focus_points.h"

#define NATIVE_STUBS_DEFINE
#include "../common/native_stubs.h"

// isOverrideActive() lives in src/services/host_test_stubs.cpp — a
// test-only accessor over the same state services::location::setOverride
// mutates. Declared here so the tests can read it.
namespace services::location { bool isOverrideActive(); }

namespace fp = services::focus;

void wipeNvs() {
  Preferences p;
  p.begin("focus", false);
  p.remove("ring");
  p.remove("idx");
  p.end();
}

void setUp(void) { wipeNvs(); }
void tearDown(void) {}

// ---- init + defaults ------------------------------------------------

void test_init_falls_back_to_baked_default_ring(void) {
  fp::init();
  // Home + SFO + OAK = 3 slots.
  TEST_ASSERT_EQUAL_UINT(3, fp::count());
  TEST_ASSERT_EQUAL_STRING("Home", fp::current().name);
  TEST_ASSERT_TRUE(fp::current().is_home);
}

void test_init_uses_saved_ring_when_present(void) {
  fp::saveRingJson("[{\"name\":\"SEA\",\"lat\":47.45,\"lon\":-122.30,\"range_idx\":2}]");
  fp::init();
  // Home slot + the one saved airport.
  TEST_ASSERT_EQUAL_UINT(2, fp::count());
  fp::setIndex(1);
  TEST_ASSERT_EQUAL_STRING("SEA", fp::current().name);
  TEST_ASSERT_EQUAL_FLOAT(47.45, fp::current().lat);
  TEST_ASSERT_EQUAL_UINT8(2, fp::current().default_range_idx);
}

// ---- JSON validation ------------------------------------------------

void test_save_ring_refuses_invalid_json_and_next_init_uses_fallback(void) {
  fp::saveRingJson("this is not json");
  fp::init();
  // Should have picked up the baked fallback, not junk.
  TEST_ASSERT_EQUAL_UINT(3, fp::count());
}

void test_save_ring_refuses_non_array_json(void) {
  fp::saveRingJson("{\"foo\":\"bar\"}");
  fp::init();
  TEST_ASSERT_EQUAL_UINT(3, fp::count());
}

void test_ring_entries_with_out_of_range_coords_are_skipped(void) {
  fp::saveRingJson(
    "[{\"name\":\"BAD\",\"lat\":999,\"lon\":0,\"range_idx\":1},"
    " {\"name\":\"OK\",\"lat\":40,\"lon\":-70,\"range_idx\":1}]");
  fp::init();
  // Home + only the valid entry.
  TEST_ASSERT_EQUAL_UINT(2, fp::count());
  fp::setIndex(1);
  TEST_ASSERT_EQUAL_STRING("OK", fp::current().name);
}

void test_ring_size_is_capped(void) {
  // Build a JSON array with 20 entries — the ring cap is 16 (kMaxRingSize)
  // including Home, so we should end up with exactly 16.
  std::string big = "[";
  for (int i = 0; i < 20; ++i) {
    if (i) big += ",";
    big += "{\"name\":\"A";
    big += std::to_string(i);
    big += "\",\"lat\":40,\"lon\":-70,\"range_idx\":1}";
  }
  big += "]";
  fp::saveRingJson(big.c_str());
  fp::init();
  TEST_ASSERT_EQUAL_UINT(16, fp::count());
}

// ---- setIndex + persistence ----------------------------------------

void test_setIndex_persists_and_re_init_restores(void) {
  fp::init();
  fp::setIndex(2);   // OAK
  TEST_ASSERT_EQUAL_UINT(2, fp::currentIndex());
  fp::init();        // re-load from NVS
  TEST_ASSERT_EQUAL_UINT(2, fp::currentIndex());
}

void test_setIndex_ignores_out_of_range(void) {
  fp::init();
  fp::setIndex(1);
  fp::setIndex(99);  // ignored
  TEST_ASSERT_EQUAL_UINT(1, fp::currentIndex());
}

void test_setIndex_applies_location_override_for_non_home(void) {
  fp::init();
  fp::setIndex(1);   // SFO
  TEST_ASSERT_TRUE(services::location::isOverrideActive());
}

void test_setIndex_clears_location_override_for_home(void) {
  fp::init();
  fp::setIndex(1);
  fp::setIndex(0);   // Home
  TEST_ASSERT_FALSE(services::location::isOverrideActive());
}

// ---- currentRingJson round-trip ------------------------------------

void test_currentRingJson_skips_home_and_roundtrips(void) {
  fp::init();
  String out = fp::currentRingJson();
  TEST_ASSERT_TRUE(out.length() > 0);
  // Home is synthetic — never serialized.
  TEST_ASSERT_NULL(std::strstr(out.c_str(), "Home"));
  // Baked SFO / OAK survive.
  TEST_ASSERT_NOT_NULL(std::strstr(out.c_str(), "SFO"));
  TEST_ASSERT_NOT_NULL(std::strstr(out.c_str(), "OAK"));

  // Feed the serialized ring back in — should still validate.
  fp::saveRingJson(out.c_str());
  fp::init();
  TEST_ASSERT_EQUAL_UINT(3, fp::count());
}

int main(int /*argc*/, char** /*argv*/) {
  UNITY_BEGIN();
  RUN_TEST(test_init_falls_back_to_baked_default_ring);
  RUN_TEST(test_init_uses_saved_ring_when_present);
  RUN_TEST(test_save_ring_refuses_invalid_json_and_next_init_uses_fallback);
  RUN_TEST(test_save_ring_refuses_non_array_json);
  RUN_TEST(test_ring_entries_with_out_of_range_coords_are_skipped);
  RUN_TEST(test_ring_size_is_capped);
  RUN_TEST(test_setIndex_persists_and_re_init_restores);
  RUN_TEST(test_setIndex_ignores_out_of_range);
  RUN_TEST(test_setIndex_applies_location_override_for_non_home);
  RUN_TEST(test_setIndex_clears_location_override_for_home);
  RUN_TEST(test_currentRingJson_skips_home_and_roundtrips);
  return UNITY_END();
}
