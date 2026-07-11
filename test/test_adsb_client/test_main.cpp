// Tests for services::adsb JSON parsing. Drives ingestPayloadForTest
// directly with canned adsb.fi bodies so no HTTP / TLS / heap check
// runs. Locks the field-picker helpers (pickAltitude, pickGroundSpeed,
// pickSquawk, on-ground filter) end-to-end.

#include <unity.h>
#include <cstring>

#include "services/adsb_client.h"

#define NATIVE_STUBS_DEFINE
#include "../common/native_stubs.h"

namespace adsb = services::adsb;

// A single-aircraft body is enough for most assertions. Wrap the array
// literal in a fresh call — the parser mutates module state that persists
// across tests, so each test starts by ingesting the body it needs.
static const char* singleAircraftBody =
  "{\"ac\":[{"
  "\"hex\":\"ABC123\",\"flight\":\"UAL1234 \","
  "\"lat\":37.62,\"lon\":-122.375,"
  "\"alt_baro\":18000,\"gs\":420,\"track\":90,\"true_heading\":93,"
  "\"baro_rate\":-1200,\"squawk\":\"1200\",\"t\":\"B738\""
  "}]}";

void setUp(void) {
  // Force the aircraft list to empty by ingesting an empty array.
  adsb::ingestPayloadForTest("{\"ac\":[]}", std::strlen("{\"ac\":[]}"));
}

void tearDown(void) {}

void test_parse_populates_aircraft_list(void) {
  TEST_ASSERT_TRUE(adsb::ingestPayloadForTest(
      singleAircraftBody, std::strlen(singleAircraftBody)));
  TEST_ASSERT_EQUAL_UINT(1, adsb::aircraftCount());
  const adsb::Aircraft& a = adsb::aircraftList()[0];
  TEST_ASSERT_EQUAL_FLOAT(37.62f, a.lat);
  TEST_ASSERT_EQUAL_FLOAT(-122.375f, a.lon);
  TEST_ASSERT_EQUAL_INT32(18000, a.alt_ft);
  TEST_ASSERT_EQUAL_FLOAT(420.0f, a.gs_knots);
  // track = 90 but true_heading = 93 → nose_deg picks true_heading.
  TEST_ASSERT_EQUAL_FLOAT(93.0f, a.nose_deg);
  TEST_ASSERT_EQUAL_FLOAT(90.0f, a.track_deg);
  TEST_ASSERT_EQUAL_UINT16(1200, a.squawk);
}

void test_callsign_prefers_flight_over_registration(void) {
  TEST_ASSERT_TRUE(adsb::ingestPayloadForTest(
      singleAircraftBody, std::strlen(singleAircraftBody)));
  // "flight" is "UAL1234 " (trailing space); the parser trims.
  TEST_ASSERT_EQUAL_STRING("UAL1234", adsb::aircraftList()[0].callsign);
}

void test_callsign_falls_back_to_registration_then_hex(void) {
  const char* reg_only =
    "{\"ac\":[{\"hex\":\"AAA\",\"r\":\"N12345\",\"lat\":0,\"lon\":0,\"alt_baro\":10000,\"gs\":100}]}";
  TEST_ASSERT_TRUE(adsb::ingestPayloadForTest(reg_only, std::strlen(reg_only)));
  TEST_ASSERT_EQUAL_STRING("N12345", adsb::aircraftList()[0].callsign);

  const char* hex_only =
    "{\"ac\":[{\"hex\":\"BBBEEE\",\"lat\":0,\"lon\":0,\"alt_baro\":10000,\"gs\":100}]}";
  TEST_ASSERT_TRUE(adsb::ingestPayloadForTest(hex_only, std::strlen(hex_only)));
  TEST_ASSERT_EQUAL_STRING("BBBEEE", adsb::aircraftList()[0].callsign);
}

void test_ground_aircraft_are_filtered_when_flag_is_off(void) {
  // "alt_baro":"ground" → on-ground. kAdsbShowGroundAircraft defaults
  // to false so this should be filtered out.
  const char* ground =
    "{\"ac\":[{\"hex\":\"ABC\",\"lat\":37,\"lon\":-122,\"alt_baro\":\"ground\",\"gs\":0}]}";
  TEST_ASSERT_TRUE(adsb::ingestPayloadForTest(ground, std::strlen(ground)));
  TEST_ASSERT_EQUAL_UINT(0, adsb::aircraftCount());
}

void test_missing_lat_lon_aircraft_are_skipped(void) {
  const char* mixed =
    "{\"ac\":["
    "{\"hex\":\"A\",\"lat\":37,\"lon\":-122,\"alt_baro\":10000,\"gs\":100},"
    "{\"hex\":\"B\",\"alt_baro\":10000,\"gs\":100},"                // no lat/lon
    "{\"hex\":\"C\",\"lat\":38,\"lon\":-123,\"alt_baro\":10000,\"gs\":100}"
    "]}";
  TEST_ASSERT_TRUE(adsb::ingestPayloadForTest(mixed, std::strlen(mixed)));
  TEST_ASSERT_EQUAL_UINT(2, adsb::aircraftCount());
}

void test_malformed_json_returns_false_and_leaves_list_alone(void) {
  TEST_ASSERT_TRUE(adsb::ingestPayloadForTest(
      singleAircraftBody, std::strlen(singleAircraftBody)));
  const size_t before = adsb::aircraftCount();

  const char* junk = "this is not json";
  TEST_ASSERT_FALSE(adsb::ingestPayloadForTest(junk, std::strlen(junk)));
  TEST_ASSERT_EQUAL_UINT(before, adsb::aircraftCount());
}

void test_empty_ac_array_populates_zero_aircraft(void) {
  const char* empty = "{\"ac\":[]}";
  TEST_ASSERT_TRUE(adsb::ingestPayloadForTest(empty, std::strlen(empty)));
  TEST_ASSERT_EQUAL_UINT(0, adsb::aircraftCount());
}

void test_missing_ac_key_populates_zero_aircraft(void) {
  const char* no_ac = "{}";
  TEST_ASSERT_TRUE(adsb::ingestPayloadForTest(no_ac, std::strlen(no_ac)));
  TEST_ASSERT_EQUAL_UINT(0, adsb::aircraftCount());
}

void test_fetchCount_increments_on_successful_parse(void) {
  const unsigned long before = adsb::fetchCount();
  adsb::ingestPayloadForTest(singleAircraftBody,
                             std::strlen(singleAircraftBody));
  TEST_ASSERT_TRUE(adsb::fetchCount() > before);
}

int main(int /*argc*/, char** /*argv*/) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_populates_aircraft_list);
  RUN_TEST(test_callsign_prefers_flight_over_registration);
  RUN_TEST(test_callsign_falls_back_to_registration_then_hex);
  RUN_TEST(test_ground_aircraft_are_filtered_when_flag_is_off);
  RUN_TEST(test_missing_lat_lon_aircraft_are_skipped);
  RUN_TEST(test_malformed_json_returns_false_and_leaves_list_alone);
  RUN_TEST(test_empty_ac_array_populates_zero_aircraft);
  RUN_TEST(test_missing_ac_key_populates_zero_aircraft);
  RUN_TEST(test_fetchCount_increments_on_successful_parse);
  return UNITY_END();
}
