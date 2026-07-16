#pragma once

// Animated loading spinner shown during page transitions. Draws directly
// to the panel (not the sprite) so it appears immediately while the
// heavy network fetch + render runs. The sprite push naturally replaces
// the spinner when the new screen is ready.

namespace ui::loading {

/** Show an animated spinner with a label for duration_ms, then return.
 *  Call this BEFORE the blocking network fetch + render so the user
 *  gets immediate feedback that their tap was registered. The spinner
 *  animates in a tight loop for the requested duration, then the caller
 *  proceeds with the heavy work. The final sprite push replaces the
 *  spinner automatically. */
void animateBriefly(const char* label, unsigned long duration_ms = 200);

}  // namespace ui::loading
