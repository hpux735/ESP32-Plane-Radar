"""Tests for scripts/tile_polygons.py — the land + water layer of the
tile pipeline.

Land and water share the polygon-per-tile builder, so these tests
cover both. Distinct from coastline testing because polygon clipping
(Sutherland-Hodgman) has different failure modes than polyline
splitting: dropped vertices, degenerate results, holes-in-holes.
"""
import tile_polygons as tp
import tile_scheme as ts


def _poly_feat(coords, gtype="Polygon"):
    return {"type": "Feature", "geometry": {"type": gtype, "coordinates": coords}}


# ---------------------------------------------------------------------------
# _extract_polygon_rings
# ---------------------------------------------------------------------------


def test_extract_polygon_rings_polygon_outer_only():
    coords = [
        # outer ring
        [[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0], [0.0, 0.0]],
        # hole ring — must be dropped
        [[0.25, 0.25], [0.75, 0.25], [0.75, 0.75], [0.25, 0.75], [0.25, 0.25]],
    ]
    rings = tp._extract_polygon_rings([_poly_feat(coords)])
    assert len(rings) == 1
    assert rings[0][0] == (0.0, 0.0)


def test_extract_polygon_rings_multipolygon_flattens():
    a = [[[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 0.0]]]
    b = [[[2.0, 2.0], [3.0, 2.0], [3.0, 3.0], [2.0, 2.0]]]
    rings = tp._extract_polygon_rings([_poly_feat([a, b], gtype="MultiPolygon")])
    assert len(rings) == 2


def test_extract_polygon_rings_skips_non_polygon_geometries():
    features = [
        {"type": "Feature", "geometry": {"type": "LineString", "coordinates": [[0, 0], [1, 1]]}},
        {"type": "Feature", "geometry": {"type": "Point", "coordinates": [0, 0]}},
    ]
    assert tp._extract_polygon_rings(features) == []


def test_extract_polygon_rings_drops_degenerate_ring():
    """A ring with fewer than 4 coordinates (3 unique + close) can't
    define a polygon."""
    coords = [[[0.0, 0.0], [1.0, 0.0], [0.0, 0.0]]]  # only 2 unique
    assert tp._extract_polygon_rings([_poly_feat(coords)]) == []


# ---------------------------------------------------------------------------
# distribute_polygon_to_tiles
# ---------------------------------------------------------------------------


def test_short_ring_returns_no_tiles():
    """A 2-point 'polygon' isn't a polygon."""
    assert tp.distribute_polygon_to_tiles([(0.0, 0.0), (1.0, 1.0)], z=7, tol_deg=0.001) == {}


def test_polygon_inside_single_tile_lands_there_only():
    """Small SF polygon — fully inside z=7 tile (20, 37)."""
    ring = [(-122.46, 37.75), (-122.44, 37.75), (-122.44, 37.77), (-122.46, 37.77), (-122.46, 37.75)]
    got = tp.distribute_polygon_to_tiles(ring, z=7, tol_deg=0.001)
    assert set(got.keys()) == {(20, 37)}
    poly = got[(20, 37)]
    # After DP simplify at 0.001° a 4-vertex rectangle is unchanged.
    assert len(poly.points) == 4


def test_polygon_spanning_two_tiles_gets_clipped_into_both():
    """Rectangle spanning the boundary between z=3 tiles (4, ?) and (5, ?)
    at lon=45. Each half must be a valid polygon (>=3 vertices) after
    Sutherland-Hodgman clip."""
    ring = [(44.0, 30.0), (46.0, 30.0), (46.0, 31.0), (44.0, 31.0), (44.0, 30.0)]
    got = tp.distribute_polygon_to_tiles(ring, z=3, tol_deg=0.001)
    xs = {x for x, _ in got.keys()}
    assert 4 in xs and 5 in xs
    for poly in got.values():
        assert len(poly.points) >= 3


def test_polygon_clipping_preserves_area_shape():
    """A polygon fully inside a tile must survive DP simplification
    with at least the vertices needed to describe it."""
    ring = [(-122.45, 37.755), (-122.44, 37.755), (-122.44, 37.760), (-122.45, 37.760)]
    got = tp.distribute_polygon_to_tiles(ring, z=7, tol_deg=0.0001)
    assert (20, 37) in got
    # A rectangle must retain all 4 vertices at a tight tolerance.
    assert len(got[(20, 37)].points) == 4


# ---------------------------------------------------------------------------
# build_polygon_tiles — end-to-end
# ---------------------------------------------------------------------------


def test_build_polygon_tiles_emits_all_zoom_levels_for_touched_areas():
    features = [
        _poly_feat(
            [[[-122.5, 37.75], [-122.4, 37.75], [-122.4, 37.85], [-122.5, 37.85], [-122.5, 37.75]]]
        )
    ]
    tiles = tp.build_polygon_tiles(features)
    zooms_seen = {z for (z, _, _) in tiles.keys()}
    assert zooms_seen == set(ts.ZOOM_LEVELS)


def test_build_polygon_tiles_empty_source_returns_empty_dict():
    assert tp.build_polygon_tiles([]) == {}


def test_build_polygon_tiles_coarser_zooms_have_fewer_points_for_wiggly_boundary():
    """A land polygon with a wiggly coastline should hold fewer points
    at coarser zoom levels — otherwise the coarser tiles are wasting
    bytes."""
    # Wiggly ~200-point closed polygon: circle around SF with radial
    # zig-zag on top of it, ~0.02° base radius so it sits in a single
    # z=7 tile but has enough detail to be worth simplifying.
    import math

    n = 200
    ring = []
    for i in range(n):
        angle = 2 * math.pi * i / n
        r = 0.02 + 0.003 * ((-1) ** i)
        lon = -122.45 + r * math.cos(angle)
        lat = 37.75 + r * math.sin(angle)
        ring.append((lon, lat))
    ring.append(ring[0])
    features = [_poly_feat([ring])]
    tiles = tp.build_polygon_tiles(features)

    def total_points(z: int) -> int:
        return sum(
            len(p.points)
            for (zz, _, _), polys in tiles.items()
            if zz == z
            for p in polys
        )

    n7 = total_points(7)
    n5 = total_points(5)
    n3 = total_points(3)
    assert n7 > 0
    assert n5 <= n7
    assert n3 <= n5
