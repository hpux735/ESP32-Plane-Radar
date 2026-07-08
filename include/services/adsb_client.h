#pragma once

#include <cstddef>
#include <cstdint>

namespace services::adsb {

struct Aircraft {
  float lat;
  float lon;
  float nose_deg;
  float track_deg;
  float gs_knots;
  float vs_fpm;   // Barometric or geometric vertical rate, ft/min. 0 if unknown.
  int32_t alt_ft; // Integer altitude in feet, or INT32_MIN for on-ground / unknown.
  char callsign[9];
  char type[5];
};

constexpr size_t kMaxAircraft = 64;

size_t aircraftCount();
const Aircraft* aircraftList();

/** Time (millis()) of the most recent successful fetchUpdate. Used by the
 *  tag mode-toggle to align its flip to the midpoint between fetches so the
 *  text change doesn't race the position update. Returns 0 if none yet. */
unsigned long lastUpdateMs();

/** Hook invoked during long HTTP I/O (e.g. wifiLoop). Optional. */
using PollFn = void (*)();
void setPollFn(PollFn fn);

/** Fetch aircraft within fetch_radius_km of center_lat/lon from adsb.fi. */
bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km);

}  // namespace services::adsb
