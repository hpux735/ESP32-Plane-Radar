#pragma once

#include <cstddef>
#include <cstdint>

namespace services::focus {

// Where the radar is currently centered. Focus 0 is always "Home" —
// pulls lat/lon from services::location's stored user home. The rest are
// fixed airports the user cycles through with a BOOT tap. Range preset
// index (0..3 → 5/10/15/25 nm) auto-applies on focus change so a small
// GA field defaults to 5 nm while a busy Class B defaults to 10 nm.
struct FocusPoint {
  const char* name;         // Short display label: "Home", "SFO", ...
  double lat;               // Ignored when is_home = true.
  double lon;               // Ignored when is_home = true.
  uint8_t default_range_idx;
  bool is_home;
};

/** Load persistent focus index from flash (defaults to Home). */
void init();

/** Advance to the next focus in the ring, apply its lat/lon + default
 *  range preset. Persists the new index. */
void cycle();

/** Current focus in the ring. Home reads lat/lon from services::location. */
const FocusPoint& current();

/** Total number of focus points. */
size_t count();

/** How long the on-screen focus-name overlay should stay up after the
 *  most recent cycle(). 0 if it should be hidden. */
unsigned long overlayRemainingMs();

}  // namespace services::focus
