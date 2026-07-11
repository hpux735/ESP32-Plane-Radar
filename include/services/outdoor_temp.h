#pragma once

// Cached outdoor conditions from Open-Meteo (no key required). Fetched
// against the user's configured home location — see services::location —
// on a 15 min cadence. Non-blocking after the first successful fetch:
// the cockpit screen always reads whatever's cached and the fetch itself
// is driven by a call to loop() from the main loop.
//
// Populates temperature (F), wind speed (knots) + direction (° from),
// and station pressure (inHg — Kollsman altimeter setting).

namespace services::outdoor_temp {

struct Reading {
  float tempF;         // degrees Fahrenheit
  float windKts;       // knots
  float windDegFrom;   // degrees, meteorological convention (from where wind blows)
  float baroInHg;      // altimeter setting in inches of mercury (Kollsman)
  bool  valid;         // true after any successful fetch
  unsigned long age_ms;  // millis since last successful fetch
  // UTC offset (seconds) at the home location for the moment of the
  // fetch, DST-aware. Set from Open-Meteo's `utc_offset_seconds` field.
  // Zero when the reading is invalid or the API hasn't populated it —
  // callers should treat 0 as "unknown, fall back to UTC".
  long utcOffsetSec;
};

/** Cheap idempotent — safe to call at boot before WiFi. */
void init();

/** Kick a background fetch if the cache is stale. Blocking HTTP GET on the
 *  caller's thread; caps at ~7 s via HTTPClient timeout. Safe to call every
 *  loop iteration — will no-op until the interval elapses. */
void loop();

/** Latest cached reading. `valid` is false until the first successful fetch. */
Reading cached();

/** Parse an Open-Meteo `/v1/forecast` JSON body into the cached reading
 *  state, bypassing HTTP. Returns false + leaves state alone on parse
 *  errors or missing temperature_2m. Exposed for tests — production
 *  code goes through loop() → doFetch(). */
bool ingestPayloadForTest(const char* body, unsigned long body_len);

}  // namespace services::outdoor_temp
