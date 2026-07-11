// Tests for services::outdoor_temp — Open-Meteo JSON parsing. Drives
// the pure parser directly (via ingestPayloadForTest) with canned bodies
// so no HTTP / WiFi is involved. Reads results through the public
// cached() accessor.

#include <unity.h>
#include <cmath>
#include <cstring>

#include "services/outdoor_temp.h"

#define NATIVE_STUBS_DEFINE
#include "../common/native_stubs.h"

namespace ot = services::outdoor_temp;

void setUp(void) { ot::init(); }
void tearDown(void) {}

void test_valid_full_payload_parses_all_fields(void) {
  const char* body =
    "{\"current\":{"
    "\"temperature_2m\":72.5,"
    "\"wind_speed_10m\":15,"
    "\"wind_direction_10m\":270,"
    "\"pressure_msl\":1013.25}}";
  TEST_ASSERT_TRUE(ot::ingestPayloadForTest(body, std::strlen(body)));
  const ot::Reading r = ot::cached();
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 72.5f, r.tempF);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 15.0f, r.windKts);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 270.0f, r.windDegFrom);
  // 1013.25 hPa / 33.8639 hPa-per-inHg ≈ 29.92 inHg.
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 29.92f, r.baroInHg);
}

void test_missing_temperature_2m_fails_and_leaves_state_alone(void) {
  const char* body = "{\"current\":{\"wind_speed_10m\":15}}";
  TEST_ASSERT_FALSE(ot::ingestPayloadForTest(body, std::strlen(body)));
  const ot::Reading r = ot::cached();
  TEST_ASSERT_FALSE(r.valid);
}

void test_missing_current_object_fails(void) {
  const char* body = "{}";
  TEST_ASSERT_FALSE(ot::ingestPayloadForTest(body, std::strlen(body)));
  TEST_ASSERT_FALSE(ot::cached().valid);
}

void test_malformed_json_fails(void) {
  const char* body = "this is not json";
  TEST_ASSERT_FALSE(ot::ingestPayloadForTest(body, std::strlen(body)));
  TEST_ASSERT_FALSE(ot::cached().valid);
}

void test_partial_payload_temperature_only_still_valid(void) {
  // Wind + baro are best-effort — the reading is valid as long as
  // temperature parses.
  const char* body = "{\"current\":{\"temperature_2m\":50}}";
  TEST_ASSERT_TRUE(ot::ingestPayloadForTest(body, std::strlen(body)));
  const ot::Reading r = ot::cached();
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 50.0f, r.tempF);
  TEST_ASSERT_TRUE(std::isnan(r.windKts));
  TEST_ASSERT_TRUE(std::isnan(r.baroInHg));
}

void test_wind_string_type_falls_back_to_nan(void) {
  // Wind fields come back as strings in some Open-Meteo variants — the
  // parser only accepts numeric types, everything else falls back to NAN.
  const char* body =
    "{\"current\":{"
    "\"temperature_2m\":60,"
    "\"wind_speed_10m\":\"variable\"}}";
  TEST_ASSERT_TRUE(ot::ingestPayloadForTest(body, std::strlen(body)));
  const ot::Reading r = ot::cached();
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_TRUE(std::isnan(r.windKts));
}

void test_second_successful_parse_overwrites_first(void) {
  const char* body1 = "{\"current\":{\"temperature_2m\":50}}";
  const char* body2 = "{\"current\":{\"temperature_2m\":80}}";
  TEST_ASSERT_TRUE(ot::ingestPayloadForTest(body1, std::strlen(body1)));
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 50.0f, ot::cached().tempF);
  TEST_ASSERT_TRUE(ot::ingestPayloadForTest(body2, std::strlen(body2)));
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 80.0f, ot::cached().tempF);
}

void test_failed_parse_does_not_wipe_prior_valid_reading(void) {
  const char* good = "{\"current\":{\"temperature_2m\":68}}";
  const char* bad  = "not json";
  TEST_ASSERT_TRUE(ot::ingestPayloadForTest(good, std::strlen(good)));
  TEST_ASSERT_FALSE(ot::ingestPayloadForTest(bad, std::strlen(bad)));
  TEST_ASSERT_TRUE(ot::cached().valid);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 68.0f, ot::cached().tempF);
}

int main(int /*argc*/, char** /*argv*/) {
  UNITY_BEGIN();
  RUN_TEST(test_valid_full_payload_parses_all_fields);
  RUN_TEST(test_missing_temperature_2m_fails_and_leaves_state_alone);
  RUN_TEST(test_missing_current_object_fails);
  RUN_TEST(test_malformed_json_fails);
  RUN_TEST(test_partial_payload_temperature_only_still_valid);
  RUN_TEST(test_wind_string_type_falls_back_to_nan);
  RUN_TEST(test_second_successful_parse_overwrites_first);
  RUN_TEST(test_failed_parse_does_not_wipe_prior_valid_reading);
  return UNITY_END();
}
