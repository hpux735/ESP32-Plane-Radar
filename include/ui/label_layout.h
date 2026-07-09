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

/** Record a HARD text/label bounding rect. Tag placement will try very hard
 *  not to overlap these. */
void add(int x, int y, int w, int h);

/** Record a SOFT keep-out (aircraft icon, track vector endpoint, predicted
 *  position). Tag placement prefers to dodge these too, but will accept an
 *  overlap before overlapping a HARD rect. */
void addSoft(int x, int y, int w, int h);

/** Overlap check against HARD rects only. */
bool intersects(int x, int y, int w, int h);

/** Overlap check against HARD + SOFT rects. Used for the strict first-pass
 *  placement search. */
bool intersectsAny(int x, int y, int w, int h);

}  // namespace ui::labels
