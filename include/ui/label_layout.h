#pragma once

// Frame-local registry of text/label bounding rects. Populated by the
// first-pass drawers (cardinals, airport labels), consumed by later
// dodging code (scale label) so it can walk to a clear spot without
// obscuring earlier text.

namespace ui::labels {

struct Rect {
  int x, y, w, h;
};

/** Clear the registry — call at the start of each full grid redraw. */
void reset();

/** Record a text bounding rect. Overlapping additions are fine. */
void add(int x, int y, int w, int h);

/** True if the given rect overlaps any previously-added rect. */
bool intersects(int x, int y, int w, int h);

}  // namespace ui::labels
