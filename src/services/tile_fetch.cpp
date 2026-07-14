#include "services/tile_fetch.h"

#include <cstdio>
#include <cstring>

#include "data/tile_math.h"
#include "data/tile_store.h"
#include "services/radar_location.h"
#include "services/tile_cache.h"

#ifndef USE_NATIVE
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#endif

namespace services::tile_fetch {

namespace {

// Base URL for the tile pyramid. The refactor plan pins this on the
// existing Netlify deploy — the tile files land at radar.benyaffe.com
// via the deploy-web workflow, no additional infra.
constexpr const char* kTileHostAndPath = "https://radar.benyaffe.com/data/tiles";
// 3 s (down from 10 s). Tile fetches are rare (only when home moves)
// so the timeout hurts less than ADS-B, but same reasoning: blocking
// loop() longer than a couple seconds makes the button feel dead.
constexpr unsigned long kFetchTimeoutMs = 3000;
constexpr size_t kMaxTileBytes = 128 * 1024;  // matches services::tile_cache

// State: last successful fetch key. Guarded by shouldFetch().
bool s_have_last = false;
uint8_t s_last_z = 0;
uint16_t s_last_x = 0;
uint16_t s_last_y = 0;
bool s_pending_retry = false;
// Back-off between retries after a failed fetch. Without this, loop() retries
// on every main-loop tick (~1 ms) — each retry allocates a WiFiClientSecure +
// mbedTLS record buffers (~32 KB) and immediately frees them. On a heap-tight
// device that produces relentless fragmentation until every allocation fails
// (SSL 'Memory allocation failed', fetch guard trips, planes vanish). 30 s
// is enough for the WiFi stack to settle after a marginal-signal event.
constexpr unsigned long kRetryBackoffMs = 30000;
unsigned long s_next_retry_ms = 0;

// Boot-time only: number of quick retry attempts (including the first) before
// falling back to the 30 s runtime back-off. The runtime back-off is safe at
// steady state but painful when home just changed — the LAN portal writes new
// coords, esp_restart()s, and the user expects the new-home tile to load
// promptly. Retrying here while heap is still uncontaminated by the frame
// sprite is far more reliable than the runtime path.
constexpr int kBootFetchAttempts = 3;
constexpr unsigned long kBootFetchRetryDelayMs = 1000;

#ifndef USE_NATIVE

bool downloadTile(uint8_t z, uint16_t x, uint16_t y) {
  if (WiFi.status() != WL_CONNECTED) return false;

  char url[128];
  std::snprintf(url, sizeof(url), "%s/%u/%u/%u.bin", kTileHostAndPath,
                 static_cast<unsigned>(z), static_cast<unsigned>(x),
                 static_cast<unsigned>(y));

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(kFetchTimeoutMs);

  HTTPClient http;
  http.setTimeout(kFetchTimeoutMs);
  if (!http.begin(client, url)) {
    Serial.println("tile_fetch: http.begin failed");
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("tile_fetch: HTTP %d for %s\n", code, url);
    http.end();
    return false;
  }
  const int content_length = http.getSize();
  if (content_length <= 0 ||
      static_cast<size_t>(content_length) > kMaxTileBytes) {
    Serial.printf("tile_fetch: implausible content length %d\n", content_length);
    http.end();
    return false;
  }
  uint8_t* buf = static_cast<uint8_t*>(std::malloc(content_length));
  if (buf == nullptr) {
    Serial.printf("tile_fetch: malloc %d failed\n", content_length);
    http.end();
    return false;
  }
  WiFiClient* stream = http.getStreamPtr();
  int total = 0;
  while (total < content_length && http.connected()) {
    const int chunk = stream->readBytes(buf + total, content_length - total);
    if (chunk <= 0) break;
    total += chunk;
  }
  http.end();
  if (total != content_length) {
    Serial.printf("tile_fetch: short read %d of %d\n", total, content_length);
    std::free(buf);
    return false;
  }
  // Persist to SPIFFS while `buf` is still valid (we own it here).
  services::tile_cache::persist(z, x, y, buf, content_length);
  // Hand buf off to the store — no memcpy, no second malloc. Store owns
  // it from here on; must NOT std::free() after this call.
  // putOwning takes buf on success and frees it on failure. Either way,
  // we must NOT call std::free(buf) after this — that was the whole point
  // of switching from put() to putOwning().
  const bool ok = data::tile::store().putOwning(z, x, y, buf, content_length);
  if (ok) {
    Serial.printf("tile_fetch: got %s (%d bytes)\n", url, content_length);
  }
  return ok;
}

#endif  // !USE_NATIVE

}  // namespace

bool fetchHomeTileSync() {
#ifdef USE_NATIVE
  return true;
#else
  if (WiFi.status() != WL_CONNECTED) return false;
  const uint8_t z = data::tile::kRenderZoom;
  uint16_t x = 0, y = 0;
  data::tile::tileOfLatLon(z, services::location::lat(),
                            services::location::lon(), &x, &y);
  // Already in the RAM cache (either hydrated from SPIFFS or just fetched)?
  const auto probe = data::tile::store().get(z, x, y);
  if (!probe.is_fallback) {
    s_have_last = true;
    s_last_z = z; s_last_x = x; s_last_y = y;
    s_pending_retry = false;
    return true;
  }
  // Fresh fetch — heap is presumed clean here (caller responsibility). A few
  // quick retries before giving up: this runs pre-sprite-alloc where heap is
  // uncontaminated, so retrying is materially more likely to succeed than
  // waiting for the runtime 30 s back-off (which retries against the tighter
  // steady-state heap).
  for (int attempt = 0; attempt < kBootFetchAttempts; ++attempt) {
    if (attempt > 0) delay(kBootFetchRetryDelayMs);
    if (downloadTile(z, x, y)) {
      s_have_last = true;
      s_last_z = z; s_last_x = x; s_last_y = y;
      s_pending_retry = false;
      return true;
    }
  }
  s_pending_retry = true;
  s_next_retry_ms = millis() + kRetryBackoffMs;
  return false;
#endif
}

bool shouldFetch(uint8_t z, double lat, double lon,
                  uint8_t last_z, uint16_t last_x, uint16_t last_y,
                  bool have_last) {
  if (!have_last) return true;
  uint16_t x = 0, y = 0;
  data::tile::tileOfLatLon(z, lat, lon, &x, &y);
  return (z != last_z) || (x != last_x) || (y != last_y);
}

void loop() {
#ifdef USE_NATIVE
  // The emulator has a disk-loaded bootstrap tile; no HTTPS to run.
  return;
#else
  const double lat = services::location::lat();
  const double lon = services::location::lon();
  const uint8_t z = data::tile::kRenderZoom;

  if (!s_pending_retry &&
      !shouldFetch(z, lat, lon, s_last_z, s_last_x, s_last_y, s_have_last)) {
    return;
  }
  // Rate-limit retries. Without this, a single failure spirals into a
  // millisecond-tight busy loop that fragments the heap into unusable dust
  // (each retry allocates a ~32 KB mbedTLS record buffer that fails and
  // frees, eventually starving HTTPClient/adsb.fi fetches entirely).
  if (s_pending_retry && millis() < s_next_retry_ms) {
    return;
  }

  uint16_t x = 0, y = 0;
  data::tile::tileOfLatLon(z, lat, lon, &x, &y);
  if (downloadTile(z, x, y)) {
    s_have_last = true;
    s_last_z = z;
    s_last_x = x;
    s_last_y = y;
    s_pending_retry = false;
  } else {
    // Try again after the back-off window, unless the location changes first.
    s_pending_retry = true;
    s_next_retry_ms = millis() + kRetryBackoffMs;
  }
#endif
}

}  // namespace services::tile_fetch
