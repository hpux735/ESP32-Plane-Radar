#!/usr/bin/env python3
"""Build baked road vector data from Natural Earth 1:10m.

Uses ne_10m_roads (public domain). Filters to major routes — Interstate,
US highway, State highway — inside a bbox around the radar center. Same
generator pattern as build_coastlines.py: DP simplify, int32 micro-degree
encoding, emit src/data/roads_data.cpp.

Natural Earth road features carry a "class" attribute:
    "Major Highway", "Secondary Highway", "Bypass", "Beltway",
    "Track", "Road", ...
plus a "type" like "Interstate", "US", "State", "Federal", ...
"""
from __future__ import annotations

import argparse
import json
import math
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CACHE_DIR = ROOT / ".local-data"
ROADS_GEOJSON = CACHE_DIR / "ne_10m_roads.geojson"
OUT_H = ROOT / "include" / "data" / "roads.h"
OUT_CPP = ROOT / "src" / "data" / "roads_data.cpp"

ROADS_URL = (
    "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/"
    "geojson/ne_10m_roads.geojson"
)

DEFAULT_CENTER_LAT = 37.759
DEFAULT_CENTER_LON = -122.409
DEFAULT_RADIUS_KM = 200.0
DEFAULT_SIMPLIFY_TOL_DEG = 0.003  # ~330 m; roads can be a bit coarser than coast

KM_PER_DEG = 111.0

# Natural Earth's roads use "type" (not "class") for the road category.
# The Bay Area picks up US-101 / I-280 / I-880 / I-80 / I-580 / 92 / 84
# / 24 as "Major Highway" or "Secondary Highway".
KEEP_TYPES = frozenset({
    "Major Highway",
    "Secondary Highway",
})


def download(url: str, cache: Path) -> None:
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    if cache.exists():
        return
    print(f"Downloading {url} → {cache}", file=sys.stderr)
    urllib.request.urlretrieve(url, cache)


def clip_polyline(coords, bbox):
    """Split a linestring at bbox exits — returns list of clipped substrings."""
    min_lat, max_lat, min_lon, max_lon = bbox
    out: list[list[tuple[float, float]]] = []
    current: list[tuple[float, float]] = []
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


def _perp_dist(p, a, b):
    ax, ay = a
    bx, by = b
    px, py = p
    den = math.hypot(by - ay, bx - ax)
    if den == 0:
        return math.hypot(px - ax, py - ay)
    num = abs((by - ay) * px - (bx - ax) * py + bx * ay - by * ax)
    return num / den


def dp_simplify(points, tol):
    if len(points) < 3:
        return list(points)
    keep = [False] * len(points)
    keep[0] = True
    keep[-1] = True
    stack = [(0, len(points) - 1)]
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


def build(center_lat, center_lon, radius_km, tol_deg):
    download(ROADS_URL, ROADS_GEOJSON)
    data = json.loads(ROADS_GEOJSON.read_text())

    lat_margin = radius_km / KM_PER_DEG
    lon_margin = radius_km / (KM_PER_DEG * math.cos(math.radians(center_lat)))
    bbox = (
        center_lat - lat_margin,
        center_lat + lat_margin,
        center_lon - lon_margin,
        center_lon + lon_margin,
    )

    polylines: list[list[tuple[float, float]]] = []
    for feat in data.get("features", []):
        props = feat.get("properties") or {}
        road_type = props.get("type") or ""
        if road_type not in KEEP_TYPES:
            continue
        geom = feat.get("geometry") or {}
        t = geom.get("type")
        raw = geom.get("coordinates") or []
        if t == "LineString":
            chunks = [raw]
        elif t == "MultiLineString":
            chunks = raw
        else:
            continue
        for coords in chunks:
            for clipped in clip_polyline(coords, bbox):
                simplified = dp_simplify(clipped, tol_deg)
                if len(simplified) >= 2:
                    polylines.append(simplified)
    return polylines, bbox


def emit(polylines, bbox, center_lat, center_lon, radius_km, tol_deg):
    all_pts: list[tuple[int, int]] = []
    for poly in polylines:
        for lon, lat in poly:
            all_pts.append((int(round(lat * 1e7)), int(round(lon * 1e7))))
    total = len(all_pts)
    if total > 0xFFFF:
        raise SystemExit(
            f"vertex count {total} exceeds uint16 range; loosen the DP tolerance."
        )

    OUT_H.parent.mkdir(parents=True, exist_ok=True)
    OUT_H.write_text(
        "#pragma once\n\n"
        "#include <cstddef>\n"
        "#include <cstdint>\n\n"
        "// Baked major-road polylines (Natural Earth 1:10m). Same encoding as\n"
        "// coastlines: int32 micro-degrees + Polyline{start,count} into a flat\n"
        "// vertex array. Regenerate with scripts/build_roads.py.\n\n"
        "namespace data::roads {\n\n"
        "struct Point {\n"
        "  int32_t lat_e7;\n"
        "  int32_t lon_e7;\n"
        "};\n\n"
        "struct Polyline {\n"
        "  uint16_t start;\n"
        "  uint16_t count;\n"
        "};\n\n"
        "extern const Point kPoints[];\n"
        "extern const Polyline kPolylines[];\n"
        "extern const size_t kPointCount;\n"
        "extern const size_t kPolylineCount;\n\n"
        "}  // namespace data::roads\n"
    )

    lines: list[str] = []
    lines.append("// Generated by scripts/build_roads.py — do not edit.")
    lines.append("// Source: Natural Earth 1:10m roads (public domain).")
    lines.append(
        f"// Center: ({center_lat:.6f}, {center_lon:.6f})  radius {radius_km:.0f} km"
    )
    lines.append(
        f"// Bbox: lat [{bbox[0]:.4f} .. {bbox[1]:.4f}]  lon [{bbox[2]:.4f} .. {bbox[3]:.4f}]"
    )
    lines.append(
        f"// Simplify tol: {tol_deg}° (~{tol_deg * KM_PER_DEG * 1000:.0f} m)"
    )
    lines.append(f"// Polylines: {len(polylines)}, points: {total}")
    lines.append("")
    lines.append('#include "data/roads.h"')
    lines.append("")
    lines.append("namespace data::roads {")
    lines.append("")
    lines.append("const Point kPoints[] = {")
    for i in range(0, total, 4):
        chunk = all_pts[i : i + 4]
        pieces = " ".join(f"{{{lat}, {lon}}}," for (lat, lon) in chunk)
        lines.append(f"    {pieces}")
    lines.append("};")
    lines.append("")
    lines.append("const Polyline kPolylines[] = {")
    start = 0
    for poly in polylines:
        lines.append(f"    {{{start}, {len(poly)}}},")
        start += len(poly)
    lines.append("};")
    lines.append("")
    lines.append(f"const size_t kPointCount = {total};")
    lines.append(f"const size_t kPolylineCount = {len(polylines)};")
    lines.append("")
    lines.append("}  // namespace data::roads")
    OUT_CPP.parent.mkdir(parents=True, exist_ok=True)
    OUT_CPP.write_text("\n".join(lines) + "\n")

    print(
        f"Wrote {OUT_H.name} + {OUT_CPP.name} "
        f"({len(polylines)} polylines, {total} points)"
    )


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--center", default=f"{DEFAULT_CENTER_LAT},{DEFAULT_CENTER_LON}")
    p.add_argument("--radius-km", type=float, default=DEFAULT_RADIUS_KM)
    p.add_argument("--tol-deg", type=float, default=DEFAULT_SIMPLIFY_TOL_DEG)
    args = p.parse_args()
    lat_str, lon_str = args.center.split(",")
    lat = float(lat_str)
    lon = float(lon_str)
    polylines, bbox = build(lat, lon, args.radius_km, args.tol_deg)
    emit(polylines, bbox, lat, lon, args.radius_km, args.tol_deg)


if __name__ == "__main__":
    main()
