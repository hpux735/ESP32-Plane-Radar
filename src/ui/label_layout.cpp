#include "ui/label_layout.h"

#include <cstddef>

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

bool intersects(int x, int y, int w, int h) {
  const int r = x + w;
  const int b = y + h;
  for (size_t i = 0; i < s_count; ++i) {
    const auto& o = s_rects[i];
    if (o.x < r && x < o.x + o.w && o.y < b && y < o.y + o.h) {
      return true;
    }
  }
  return false;
}

}  // namespace ui::labels
