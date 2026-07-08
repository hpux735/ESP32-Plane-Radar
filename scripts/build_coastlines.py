#!/usr/bin/env python3
"""Build baked coastline vector data from Natural Earth 1:10m.

Downloads Natural Earth's public-domain 10m coastline GeoJSON, clips to a
bounding box around a chosen center (default: 2125 Bryant St SF), simplifies
with Douglas-Peucker to a tolerance readable at the radar's 240×240 canvas,
and emits src/data/coastlines_data.cpp for the firmware/native build.

Same generator pattern as scripts/build_large_airports.py — committed
generator + committed output = reproducible with no Python runtime dep.

Usage:
  scripts/build_coastlines.py              # SF default
  scripts/build_coastlines.py --center 40.6413,-73.7781 --radius-km 200
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
GEOJSON_CACHE = CACHE_DIR / "ne_10m_coastline.geojson"
OUT_H = ROOT / "include" / "data" / "coastlines.h"
OUT_CPP = ROOT / "src" / "data" / "coastlines_data.cpp"

NATURAL_EARTH_URL = (
    "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/"
    "geojson/ne_10m_coastline.geojson"
)

# Defaults: user home (Mission District SF).
DEFAULT_CENTER_LAT = 37.759
DEFAULT_CENTER_LON = -122.409
DEFAULT_RADIUS_KM = 200.0
DEFAULT_SIMPLIFY_TOL_DEG = 0.002  # ≈ 220 m ≈ 1.5 px at 25 nm range

KM_PER_DEG = 111.0


def download_geojson() -> None:
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    if GEOJSON_CACHE.exists():
        return
    print(f"Downloading {NATURAL_EARTH_URL} → {GEOJSON_CACHE}", file=sys.stderr)
    urllib.request.urlretrieve(NATURAL_EARTH_URL, GEOJSON_CACHE)


def clip_polyline(coords, bbox):
    """Split a polyline at bbox exits — returns a list of sub-polylines."""
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
    """Iterative Douglas-Peucker to avoid recursion-depth issues."""
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
    download_geojson()
    data = json.loads(GEOJSON_CACHE.read_text())

    lat_margin = radius_km / KM_PER_DEG
    lon_margin = radius_km / (KM_PER_DEG * math.cos(math.radians(center_lat)))
    bbox = (
        center_lat - lat_margin,
        center_lat + lat_margin,
        center_lon - lon_margin,
        center_lon + lon_margin,
    )

    polylines: list[list[tuple[float, float]]] = []
    features = data.get("features", [])
    for feat in features:
        geom = feat.get("geometry") or {}
        gtype = geom.get("type")
        raw = geom.get("coordinates") or []
        if gtype == "LineString":
            chunks = [raw]
        elif gtype == "MultiLineString":
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
    all_points: list[tuple[int, int]] = []  # (lat_e7, lon_e7)
    for poly in polylines:
        for lon, lat in poly:
            all_points.append(
                (int(round(lat * 1e7)), int(round(lon * 1e7)))
            )

    total = len(all_points)
    if total > 0xFFFF:
        raise SystemExit(
            f"Point count {total} exceeds uint16 range; tighten the bbox or "
            f"increase the DP tolerance."
        )

    OUT_H.parent.mkdir(parents=True, exist_ok=True)
    OUT_H.write_text(
        "#pragma once\n"
        "\n"
        "#include <cstddef>\n"
        "#include <cstdint>\n"
        "\n"
        "// Baked coastline (Natural Earth 1:10m) clipped around the radar\n"
        "// center. Points are stored as compact int32 micro-degrees.\n"
        "// Regenerate with scripts/build_coastlines.py.\n"
        "\n"
        "namespace data::coastlines {\n"
        "\n"
        "struct Point {\n"
        "  int32_t lat_e7;\n"
        "  int32_t lon_e7;\n"
        "};\n"
        "\n"
        "// A polyline is a slice of kPoints[start .. start+count).\n"
        "struct Polyline {\n"
        "  uint16_t start;\n"
        "  uint16_t count;\n"
        "};\n"
        "\n"
        "extern const Point kPoints[];\n"
        "extern const Polyline kPolylines[];\n"
        "extern const size_t kPointCount;\n"
        "extern const size_t kPolylineCount;\n"
        "\n"
        "}  // namespace data::coastlines\n"
    )

    OUT_CPP.parent.mkdir(parents=True, exist_ok=True)
    lines: list[str] = []
    lines.append("// Generated by scripts/build_coastlines.py — do not edit.")
    lines.append("// Source: Natural Earth 1:10m coastline (public domain,")
    lines.append("//   https://www.naturalearthdata.com/)")
    lines.append(
        f"// Center: ({center_lat:.6f}, {center_lon:.6f})  radius {radius_km:.0f} km"
    )
    lines.append(
        f"// Bbox: lat [{bbox[0]:.4f} .. {bbox[1]:.4f}]  lon [{bbox[2]:.4f} .. {bbox[3]:.4f}]"
    )
    lines.append(
        f"// Simplification tol: {tol_deg}° (~{tol_deg * KM_PER_DEG * 1000:.0f} m)"
    )
    lines.append(f"// Polylines: {len(polylines)}, points: {total}")
    lines.append("")
    lines.append('#include "data/coastlines.h"')
    lines.append("")
    lines.append("namespace data::coastlines {")
    lines.append("")
    lines.append("const Point kPoints[] = {")
    for i in range(0, total, 4):
        chunk = all_points[i : i + 4]
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
    lines.append("}  // namespace data::coastlines")
    OUT_CPP.write_text("\n".join(lines) + "\n")

    print(f"Wrote {OUT_H}")
    print(f"Wrote {OUT_CPP} ({len(polylines)} polylines, {total} points)")


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
