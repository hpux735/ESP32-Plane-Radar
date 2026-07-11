// Tests for services::weather::geo — the pure lat/lon math the METAR
// bulk-fetch relies on. Zero HTTP, zero ArduinoJson, zero Preferences.
// Pulled out of weather.cpp specifically so the formulas are testable in
// isolation without stubbing the world.

#include <unity.h>
#include <cmath>

#include "services/weather_geo.h"

// weather_geo.cpp doesn't touch the projection layer, but native_test
// compiles common stubs into every binary — include the marker so the
// linker gets services::location::lat/lon.
#define NATIVE_STUBS_DEFINE
#include "../common/native_stubs.h"

using services::weather::geo::distanceNm;
using services::weather::geo::makeBbox;

void setUp(void) {}
void tearDown(void) {}

// ---- distanceNm ------------------------------------------------------

void test_distance_zero_for_same_point(void) {
  TEST_ASSERT_EQUAL_FLOAT(0.0f, distanceNm(37.75f, -122.45f, 37.75f, -122.45f));
}

void test_distance_1deg_lat_is_60nm(void) {
  // 1° of latitude at any longitude scaling ≈ 60 nm.
  const float d = distanceNm(37.0f, -122.0f, 38.0f, -122.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 60.0f, d);
}

void test_distance_scales_longitude_by_cos_lat(void) {
  // 1° of longitude at latitude 60° should be ~30 nm (60 * cos 60°).
  const float d = distanceNm(60.0f, 0.0f, 60.0f, 1.0f);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 30.0f, d);
}

void test_distance_symmetric_within_tolerance(void) {
  // At the same lat, swapping start/end doesn't change distance (the
  // formula uses cos of the *first* argument's lat, which changes the
  // number slightly — verify the difference is small at moderate lat).
  const float a = distanceNm(37.75f, -122.45f, 37.85f, -122.35f);
  const float b = distanceNm(37.85f, -122.35f, 37.75f, -122.45f);
  TEST_ASSERT_FLOAT_WITHIN(0.3f, a, b);
}

// ---- makeBbox --------------------------------------------------------

void test_bbox_centered_on_input_point(void) {
  float lat_min, lon_min, lat_max, lon_max;
  makeBbox(37.75f, -122.45f, 30.0f, &lat_min, &lon_min, &lat_max, &lon_max);
  const float mid_lat = (lat_min + lat_max) / 2.0f;
  const float mid_lon = (lon_min + lon_max) / 2.0f;
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 37.75f, mid_lat);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -122.45f, mid_lon);
}

void test_bbox_grows_with_radius(void) {
  float small_lat_min, small_lon_min, small_lat_max, small_lon_max;
  float big_lat_min, big_lon_min, big_lat_max, big_lon_max;
  makeBbox(0.0f, 0.0f, 10.0f, &small_lat_min, &small_lon_min,
           &small_lat_max, &small_lon_max);
  makeBbox(0.0f, 0.0f, 40.0f, &big_lat_min, &big_lon_min,
           &big_lat_max, &big_lon_max);
  TEST_ASSERT_TRUE((big_lat_max - big_lat_min) >
                   (small_lat_max - small_lat_min));
  TEST_ASSERT_TRUE((big_lon_max - big_lon_min) >
                   (small_lon_max - small_lon_min));
}

void test_bbox_inflates_radius_by_10_percent(void) {
  // 30 nm × 1.1 / 60 = 0.55 deg latitude half-width.
  float lat_min, lon_min, lat_max, lon_max;
  makeBbox(37.75f, -122.45f, 30.0f, &lat_min, &lon_min, &lat_max, &lon_max);
  const float half_lat = (lat_max - lat_min) / 2.0f;
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.55f, half_lat);
}

void test_bbox_longitude_widens_at_high_latitude(void) {
  // At lat 60° cos ≈ 0.5, so pad_lon ≈ 2× pad_lat.
  float lat_min, lon_min, lat_max, lon_max;
  makeBbox(60.0f, 0.0f, 30.0f, &lat_min, &lon_min, &lat_max, &lon_max);
  const float half_lat = (lat_max - lat_min) / 2.0f;
  const float half_lon = (lon_max - lon_min) / 2.0f;
  TEST_ASSERT_TRUE(half_lon > 1.9f * half_lat);
  TEST_ASSERT_TRUE(half_lon < 2.1f * half_lat);
}

int main(int /*argc*/, char** /*argv*/) {
  UNITY_BEGIN();
  RUN_TEST(test_distance_zero_for_same_point);
  RUN_TEST(test_distance_1deg_lat_is_60nm);
  RUN_TEST(test_distance_scales_longitude_by_cos_lat);
  RUN_TEST(test_distance_symmetric_within_tolerance);
  RUN_TEST(test_bbox_centered_on_input_point);
  RUN_TEST(test_bbox_grows_with_radius);
  RUN_TEST(test_bbox_inflates_radius_by_10_percent);
  RUN_TEST(test_bbox_longitude_widens_at_high_latitude);
  return UNITY_END();
}
