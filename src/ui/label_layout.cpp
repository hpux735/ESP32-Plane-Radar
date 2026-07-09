#include "ui/label_layout.h"

#include <cstddef>
#include <cstdio>

namespace ui::labels {
namespace {

// The radar draws a handful of hard text elements + up to ~kMaxAircraft
// icon keep-outs. 128 fits both comfortably as a single fixed allocation.
constexpr size_t kMaxRects = 128;

Rect s_hard[kMaxRects];
Rect s_soft[kMaxRects];
size_t s_hard_count = 0;
size_t s_soft_count = 0;

// 3 px of "margin" so labels that are strictly touching (or a hair apart)
// still register as colliding — antialiased characters bleed a pixel or two
// and a tiny visible gap makes stacks read as distinct.
constexpr int kMargin = 3;

bool intersectsList(const Rect* rects, size_t count, int x, int y, int w, int h) {
  const int r = x + w + kMargin;
  const int b = y + h + kMargin;
  const int xl = x - kMargin;
  const int yt = y - kMargin;
  for (size_t i = 0; i < count; ++i) {
    const auto& o = rects[i];
    if (o.x < r && xl < o.x + o.w && o.y < b && yt < o.y + o.h) {
      return true;
    }
  }
  return false;
}

}  // namespace

void reset() {
  s_hard_count = 0;
  s_soft_count = 0;
}

void add(int x, int y, int w, int h) {
  if (s_hard_count >= kMaxRects) return;
  s_hard[s_hard_count++] = {x, y, w, h};
}

void addSoft(int x, int y, int w, int h) {
  if (s_soft_count >= kMaxRects) return;
  s_soft[s_soft_count++] = {x, y, w, h};
}

bool intersects(int x, int y, int w, int h) {
  return intersectsList(s_hard, s_hard_count, x, y, w, h);
}

bool intersectsAny(int x, int y, int w, int h) {
  return intersects(x, y, w, h) ||
         intersectsList(s_soft, s_soft_count, x, y, w, h);
}

}  // namespace ui::labels
