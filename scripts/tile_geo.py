"""Pure geometry helpers shared across the tile builder modules.

Kept separate from build_coastlines.py (which will be deleted in a
later milestone) so the new tile pipeline doesn't inherit a dependency
that's about to disappear.
"""
from __future__ import annotations

import math

Point = tuple[float, float]  # (lon, lat)


def polyline_bbox(coords: list[Point]) -> tuple[float, float, float, float]:
    """Min/max lat and lon of a polyline. (min_lat, max_lat, min_lon, max_lon)."""
    lats = [p[1] for p in coords]
    lons = [p[0] for p in coords]
    return min(lats), max(lats), min(lons), max(lons)


def bboxes_overlap(
    a: tuple[float, float, float, float],
    b: tuple[float, float, float, float],
) -> bool:
    """Both bboxes in (min_lat, max_lat, min_lon, max_lon) form."""
    return not (a[1] < b[0] or a[0] > b[1] or a[3] < b[2] or a[2] > b[3])


def clip_polyline_to_bbox(
    coords: list[Point], bbox: tuple[float, float, float, float]
) -> list[list[Point]]:
    """Split a polyline at bbox exits, keeping only sub-polylines with
    >=2 points inside the bbox. bbox = (min_lat, max_lat, min_lon, max_lon).

    This mirrors the current build_coastlines.py behavior exactly so
    the visual output of the new pipeline matches the baked Bay Area
    coastline it will replace.
    """
    min_lat, max_lat, min_lon, max_lon = bbox
    out: list[list[Point]] = []
    current: list[Point] = []
    for lon, lat in coords:
        if min_lat <= lat <= max_lat and min_lon <= lon <= max_lon:
            current.append((lon, lat))
        elif len(current) >= 2:
            out.append(current)
            current = []
        else:
            current = []
    if len(current) >= 2:
        out.append(current)
    return out


def sutherland_hodgman_clip(
    polygon: list[Point], bbox: tuple[float, float, float, float]
) -> list[Point]:
    """Clip a polygon against an axis-aligned bbox. Returns the clipped
    polygon (open — first point NOT repeated at the end), or [] if the
    result is degenerate (<3 vertices).

    Standard Sutherland-Hodgman: iterate the four bbox edges, at each
    step keep vertices on the inside of that edge, plus intersection
    points for each edge crossing. bbox is (min_lat, max_lat, min_lon,
    max_lon) — matching clip_polyline_to_bbox for consistency.

    Points are (lon, lat) as everywhere else in this module.
    """
    min_lat, max_lat, min_lon, max_lon = bbox
    if not polygon:
        return []

    # Drop the closing vertex if the caller included it — we'll operate
    # on an open sequence and rely on wrap-around in the edge loop.
    poly = list(polygon)
    if len(poly) >= 2 and poly[0] == poly[-1]:
        poly = poly[:-1]

    def _clip_against(
        pts: list[Point],
        inside: "callable",
        intersect: "callable",
    ) -> list[Point]:
        if not pts:
            return []
        out: list[Point] = []
        n = len(pts)
        for i in range(n):
            curr = pts[i]
            prev = pts[i - 1]  # -1 → last (wrap)
            curr_in = inside(curr)
            prev_in = inside(prev)
            if curr_in:
                if not prev_in:
                    out.append(intersect(prev, curr))
                out.append(curr)
            elif prev_in:
                out.append(intersect(prev, curr))
        return out

    def _lerp(a: Point, b: Point, t: float) -> Point:
        return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)

    def _at_lon(a: Point, b: Point, lon: float) -> Point:
        # Solve for t such that a.lon + t*(b.lon - a.lon) == lon.
        dx = b[0] - a[0]
        t = 0.0 if dx == 0 else (lon - a[0]) / dx
        return _lerp(a, b, t)

    def _at_lat(a: Point, b: Point, lat: float) -> Point:
        dy = b[1] - a[1]
        t = 0.0 if dy == 0 else (lat - a[1]) / dy
        return _lerp(a, b, t)

    # West edge: keep lon >= min_lon
    poly = _clip_against(
        poly,
        inside=lambda p: p[0] >= min_lon,
        intersect=lambda a, b: _at_lon(a, b, min_lon),
    )
    # East edge: keep lon <= max_lon
    poly = _clip_against(
        poly,
        inside=lambda p: p[0] <= max_lon,
        intersect=lambda a, b: _at_lon(a, b, max_lon),
    )
    # South edge: keep lat >= min_lat
    poly = _clip_against(
        poly,
        inside=lambda p: p[1] >= min_lat,
        intersect=lambda a, b: _at_lat(a, b, min_lat),
    )
    # North edge: keep lat <= max_lat
    poly = _clip_against(
        poly,
        inside=lambda p: p[1] <= max_lat,
        intersect=lambda a, b: _at_lat(a, b, max_lat),
    )

    if len(poly) < 3:
        return []
    return poly


def _perp_dist(p: Point, a: Point, b: Point) -> float:
    ax, ay = a
    bx, by = b
    px, py = p
    den = math.hypot(by - ay, bx - ax)
    if den == 0:
        return math.hypot(px - ax, py - ay)
    num = abs((by - ay) * px - (bx - ax) * py + bx * ay - by * ax)
    return num / den


def dp_simplify(points: list[Point], tol: float) -> list[Point]:
    """Iterative Douglas-Peucker (avoids recursion-depth issues on long
    polylines). Preserves endpoints; drops interior points closer than
    `tol` to the line between their neighbors on either side.
    """
    if len(points) < 3:
        return list(points)
    keep = [False] * len(points)
    keep[0] = True
    keep[-1] = True
    stack: list[tuple[int, int]] = [(0, len(points) - 1)]
    while stack:
        lo, hi = stack.pop()
        if hi - lo < 2:
            continue
        dmax = 0.0
        idx = -1
        for i in range(lo + 1, hi):
            d = _perp_dist(points[i], points[lo], points[hi])
            if d > dmax:
                dmax = d
                idx = i
        if dmax > tol and idx >= 0:
            keep[idx] = True
            stack.append((lo, idx))
            stack.append((idx, hi))
    return [p for p, k in zip(points, keep) if k]
