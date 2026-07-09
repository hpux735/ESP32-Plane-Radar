#!/usr/bin/env python3
"""Bake FAA Class B/C/D airspace polygons around a chosen center.

Fetches the FAA's Class_Airspace GeoJSON (public domain, updated per 56-day
AIRAC cycle) once into .local-data/, filters to features whose lateral
footprint overlaps a bbox around the center, keeps only CLASS in {B, C, D},
DP-simplifies each polygon ring, and emits src/data/airspace_data.cpp.

Unlike a wedding-cake circle approximation, this preserves the real GPS
polygon shape — the FAA moved Class B off DME arcs to GPS polygons years
ago, and each altitude shelf is its own distinct polygon.

Usage:
  scripts/build_airspace.py                        # SF default
  scripts/build_airspace.py --center 40.6,-73.7 --radius-km 200
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
GEOJSON_CACHE = CACHE_DIR / "faa_class_airspace.geojson"
OUT_H = ROOT / "include" / "data" / "airspace.h"
OUT_CPP = ROOT / "src" / "data" / "airspace_data.cpp"

# FAA ArcGIS Hub — Class Airspace layer, GeoJSON export.
FAA_URL = (
    "https://hub.arcgis.com/api/v3/datasets/"
    "c6a62360338e408cb1512366ad61559e_0/downloads/data"
    "?format=geojson&spatialRefId=4326&where=1=1"
)

DEFAULT_CENTER_LAT = 37.759
DEFAULT_CENTER_LON = -122.409
DEFAULT_RADIUS_KM = 150.0
DEFAULT_SIMPLIFY_TOL_DEG = 0.001  # ≈ 110 m — polygon edges are shorter than
                                  # coastline so we simplify less aggressively.

KM_PER_DEG = 111.0


def download_geojson() -> None:
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    if GEOJSON_CACHE.exists():
        return
    print(f"Downloading FAA Class Airspace GeoJSON (~580 MB)…", file=sys.stderr)
    print(f"  {FAA_URL}", file=sys.stderr)
    urllib.request.urlretrieve(FAA_URL, GEOJSON_CACHE)


def iter_points(coords):
    """Recursively yield [lon, lat, ...] points from nested coord arrays."""
    if not coords:
        return
    if isinstance(coords[0], (int, float)):
        yield coords
    else:
        for sub in coords:
            yield from iter_points(sub)


def bbox_of(coords):
    lats = []
    lons = []
    for p in iter_points(coords):
        lons.append(p[0])
        lats.append(p[1])
    return (min(lats), max(lats), min(lons), max(lons)) if lats else None


def overlaps(feat_bbox, view_bbox):
    fmin_lat, fmax_lat, fmin_lon, fmax_lon = feat_bbox
    vmin_lat, vmax_lat, vmin_lon, vmax_lon = view_bbox
    if fmax_lat < vmin_lat or fmin_lat > vmax_lat:
        return False
    if fmax_lon < vmin_lon or fmin_lon > vmax_lon:
        return False
    return True


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


def geometry_rings(geom):
    """Yield each outer ring of a Polygon or MultiPolygon as a list of (lon, lat)."""
    gtype = geom.get("type")
    coords = geom.get("coordinates") or []
    if gtype == "Polygon":
        polys = [coords]
    elif gtype == "MultiPolygon":
        polys = coords
    else:
        return
    for poly in polys:
        if not poly:
            continue
        # First linear ring is the outer boundary; the FAA airspaces don't
        # use interior holes for the geometry types we care about.
        outer = poly[0]
        yield [(p[0], p[1]) for p in outer]


def build(center_lat, center_lon, radius_km, tol_deg):
    download_geojson()
    print("loading FAA GeoJSON…", file=sys.stderr, flush=True)
    with open(GEOJSON_CACHE) as f:
        fc = json.load(f)

    lat_margin = radius_km / KM_PER_DEG
    lon_margin = radius_km / (KM_PER_DEG * math.cos(math.radians(center_lat)))
    view_bbox = (
        center_lat - lat_margin,
        center_lat + lat_margin,
        center_lon - lon_margin,
        center_lon + lon_margin,
    )

    polygons = []  # list of dicts with class/name/lower/upper/ring
    for feat in fc.get("features", []):
        props = feat.get("properties") or {}
        cls = props.get("CLASS")
        if cls not in ("B", "C", "D"):
            continue
        geom = feat.get("geometry")
        if not geom:
            continue
        fbbox = bbox_of(geom.get("coordinates") or [])
        if not fbbox or not overlaps(fbbox, view_bbox):
            continue
        for ring in geometry_rings(geom):
            simplified = dp_simplify(ring, tol_deg)
            if len(simplified) < 3:
                continue
            polygons.append({
                "class": cls,
                "name": (props.get("NAME") or "").strip()[:40],
                "icao": (props.get("ICAO_ID") or "").strip(),
                "lower_ft": int(props.get("LOWER_VAL") or 0),
                "upper_ft": int(props.get("UPPER_VAL") or 0),
                "lower_code": props.get("LOWER_CODE") or "",
                "ring": simplified,
            })

    return polygons, view_bbox


def emit(polygons, view_bbox, center_lat, center_lon, radius_km, tol_deg):
    all_points = []  # (lat_e7, lon_e7)
    for poly in polygons:
        for lon, lat in poly["ring"]:
            all_points.append((int(round(lat * 1e7)), int(round(lon * 1e7))))
    total = len(all_points)
    if total > 0xFFFF:
        raise SystemExit(
            f"Point count {total} exceeds uint16 range; tighten bbox or "
            f"increase --tol-deg."
        )

    OUT_H.parent.mkdir(parents=True, exist_ok=True)
    OUT_H.write_text(
        "#pragma once\n"
        "\n"
        "#include <cstddef>\n"
        "#include <cstdint>\n"
        "\n"
        "// Baked FAA Class B/C/D airspace polygons around the radar center.\n"
        "// Each shelf is its own polygon (SFO Class B alone has ~11 shelves,\n"
        "// one per altitude tier). Points are int32 micro-degrees.\n"
        "// Regenerate with scripts/build_airspace.py.\n"
        "\n"
        "namespace data::airspace {\n"
        "\n"
        "struct Point {\n"
        "  int32_t lat_e7;\n"
        "  int32_t lon_e7;\n"
        "};\n"
        "\n"
        "// A closed polygon ring is kPoints[start .. start+count).\n"
        "// The last vertex is NOT a repeat of the first — the renderer\n"
        "// wraps.\n"
        "struct Polygon {\n"
        "  uint16_t start;\n"
        "  uint16_t count;\n"
        "  char     class_letter;   // 'B', 'C', or 'D'\n"
        "  int16_t  lower_ft;       // 0 = surface\n"
        "  int16_t  upper_ft;\n"
        "};\n"
        "\n"
        "extern const Point   kPoints[];\n"
        "extern const Polygon kPolygons[];\n"
        "extern const size_t  kPointCount;\n"
        "extern const size_t  kPolygonCount;\n"
        "\n"
        "}  // namespace data::airspace\n"
    )

    OUT_CPP.parent.mkdir(parents=True, exist_ok=True)
    lines = []
    lines.append("// Generated by scripts/build_airspace.py — do not edit.")
    lines.append("// Source: FAA Class Airspace GeoJSON (public domain, ArcGIS Hub")
    lines.append("//   c6a62360338e408cb1512366ad61559e).")
    lines.append(
        f"// Center: ({center_lat:.6f}, {center_lon:.6f})  radius {radius_km:.0f} km"
    )
    lines.append(
        f"// Bbox: lat [{view_bbox[0]:.4f} .. {view_bbox[1]:.4f}]  "
        f"lon [{view_bbox[2]:.4f} .. {view_bbox[3]:.4f}]"
    )
    lines.append(
        f"// Simplification tol: {tol_deg}° (~{tol_deg * KM_PER_DEG * 1000:.0f} m)"
    )
    lines.append(f"// Polygons: {len(polygons)}, points: {total}")
    lines.append("")
    lines.append('#include "data/airspace.h"')
    lines.append("")
    lines.append("namespace data::airspace {")
    lines.append("")
    lines.append("const Point kPoints[] = {")
    for i in range(0, total, 4):
        chunk = all_points[i : i + 4]
        pieces = " ".join(f"{{{lat}, {lon}}}," for (lat, lon) in chunk)
        lines.append(f"    {pieces}")
    lines.append("};")
    lines.append("")
    lines.append("const Polygon kPolygons[] = {")
    start = 0
    for poly in polygons:
        cnt = len(poly["ring"])
        lines.append(
            f"    {{{start}, {cnt}, '{poly['class']}', "
            f"{poly['lower_ft']}, {poly['upper_ft']}}},  // "
            f"{poly['icao']} {poly['name']}"
        )
        start += cnt
    lines.append("};")
    lines.append("")
    lines.append(f"const size_t kPointCount = {total};")
    lines.append(f"const size_t kPolygonCount = {len(polygons)};")
    lines.append("")
    lines.append("}  // namespace data::airspace")
    OUT_CPP.write_text("\n".join(lines) + "\n")

    print(f"Wrote {OUT_H}")
    print(f"Wrote {OUT_CPP} ({len(polygons)} polygons, {total} points)")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--center", default=f"{DEFAULT_CENTER_LAT},{DEFAULT_CENTER_LON}")
    p.add_argument("--radius-km", type=float, default=DEFAULT_RADIUS_KM)
    p.add_argument("--tol-deg", type=float, default=DEFAULT_SIMPLIFY_TOL_DEG)
    args = p.parse_args()

    lat_str, lon_str = args.center.split(",")
    lat = float(lat_str)
    lon = float(lon_str)
    polygons, view_bbox = build(lat, lon, args.radius_km, args.tol_deg)
    emit(polygons, view_bbox, lat, lon, args.radius_km, args.tol_deg)


if __name__ == "__main__":
    main()
