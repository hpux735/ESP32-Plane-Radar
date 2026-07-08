#include "ui/label_layout.h"

#include <cstddef>
#include <cstdio>

namespace ui::labels {
namespace {

// The radar draws at most a handful of fixed text elements per frame:
// 4 cardinals + up to a couple dozen airport labels. 32 is plenty and keeps
// this a fixed-size allocation (no dynamic memory on the embedded target).
constexpr size_t kMaxRects = 48;

Rect s_rects[kMaxRects];
size_t s_count = 0;

}  // namespace

void reset() { s_count = 0; }

void add(int x, int y, int w, int h) {
  if (s_count >= kMaxRects) return;
  s_rects[s_count++] = {x, y, w, h};
}

// 3 px of "margin" so labels that are strictly touching (or a hair apart)
// still register as colliding — antialiased characters bleed a pixel or two
// and a tiny visible gap makes stacks read as distinct.
bool intersects(int x, int y, int w, int h) {
  constexpr int kMargin = 3;
  const int r = x + w + kMargin;
  const int b = y + h + kMargin;
  const int xl = x - kMargin;
  const int yt = y - kMargin;
  for (size_t i = 0; i < s_count; ++i) {
    const auto& o = s_rects[i];
    if (o.x < r && xl < o.x + o.w && o.y < b && yt < o.y + o.h) {
      return true;
    }
  }
  return false;
}

}  // namespace ui::labels
