"""Per-tile polygon pipeline — used for both land (Natural Earth 10m
land + minor islands) and water (Natural Earth 10m lakes, and later
OSM water polygons for tidal cutouts).

Land and water share this pipeline because they're the same shape of
data: polygonal outlines. What differs is only which section they land
in on the tile file (SECTION_LAND vs SECTION_WATER) and which color
the renderer paints them with.
"""
from __future__ import annotations

import math
from typing import Iterable

import tile_format as tf
import tile_geo as tg
import tile_scheme as ts

Coords = list[tuple[float, float]]


def _extract_polygon_rings(features: Iterable[dict]) -> list[Coords]:
    """Flatten Polygon + MultiPolygon features into a list of outer rings.

    Holes are silently dropped — for radar-map purposes, a lake inside
    a landmass is drawn as a separate water polygon on top, not as a
    hole in the land polygon. Keeps the tile format flat.
    """
    out: list[Coords] = []
    for feat in features:
        geom = feat.get("geometry") or {}
        gtype = geom.get("type")
        raw = geom.get("coordinates") or []
        if gtype == "Polygon":
            polys = [raw]
        elif gtype == "MultiPolygon":
            polys = raw
        else:
            continue
        for poly in polys:
            if not poly:
                continue
            outer = [(float(p[0]), float(p[1])) for p in poly[0]]
            if len(outer) >= 4:  # a valid polygon needs >=3 unique + close
                out.append(outer)
    return out


def _tile_x_range(z: int, min_lon: float, max_lon: float) -> range:
    n = ts.tiles_per_side(z)
    lon_span = 360.0 / n
    x_lo = max(0, min(n - 1, int(math.floor((min_lon + 180.0) / lon_span))))
    edge = min(max_lon, 180.0 - 1e-9)
    x_hi = max(0, min(n - 1, int(math.floor((edge + 180.0) / lon_span))))
    return range(x_lo, x_hi + 1)


def _tile_y_range(z: int, min_lat: float, max_lat: float) -> range:
    n = ts.tiles_per_side(z)
    lat_span = 180.0 / n
    y_lo = max(0, min(n - 1, int(math.floor((90.0 - max_lat) / lat_span))))
    y_hi = max(0, min(n - 1, int(math.floor((90.0 - min_lat) / lat_span))))
    return range(y_lo, y_hi + 1)


def distribute_polygon_to_tiles(
    ring: Coords,
    z: int,
    tol_deg: float,
) -> dict[tuple[int, int], tf.Polyline]:
    """Clip one polygon to each tile it touches at zoom z, simplify.

    Returns {(x, y): Polyline} — one polygon fragment per tile. The
    stored polyline is open (first point NOT repeated at the end); the
    renderer is expected to close it before filling.
    """
    if len(ring) < 3:
        return {}
    min_lat, max_lat, min_lon, max_lon = tg.polyline_bbox(ring)
    result: dict[tuple[int, int], tf.Polyline] = {}
    for x in _tile_x_range(z, min_lon, max_lon):
        for y in _tile_y_range(z, min_lat, max_lat):
            bounds = ts.tile_bounds(z, x, y).as_bbox()
            clipped = tg.sutherland_hodgman_clip(ring, bounds)
            if len(clipped) < 3:
                continue
            # Simplify while it's still a closed polygon — DP treats it
            # as an open sequence but wraps around visually since the
            # renderer closes it.
            simplified = tg.dp_simplify(clipped, tol_deg)
            if len(simplified) < 3:
                continue
            result[(x, y)] = tf.Polyline(list(simplified))
    return result


def build_polygon_tiles(
    features: Iterable[dict],
    zoom_levels: Iterable[int] = ts.ZOOM_LEVELS,
) -> dict[tuple[int, int, int], list[tf.Polyline]]:
    """Distribute polygon features across the tile pyramid.

    Same return type as tile_coastline.build_coastline_tiles — a
    dict keyed on (z, x, y). Multiple polygons may land in the same
    tile.
    """
    rings = _extract_polygon_rings(features)
    result: dict[tuple[int, int, int], list[tf.Polyline]] = {}
    for z in zoom_levels:
        tol = ts.SIMPLIFY_TOL_DEG[z]
        for ring in rings:
            for (x, y), poly in distribute_polygon_to_tiles(ring, z, tol).items():
                result.setdefault((z, x, y), []).append(poly)
    return result
