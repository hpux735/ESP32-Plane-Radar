"""Tests for scripts/tile_geo.py — the shared geometry helpers used by
every per-layer tile builder.

Coastline/land/water/airport code all depend on these three primitives:
DP simplification, bbox overlap, and polyline-to-bbox clipping. A
regression here would silently corrupt every layer's output, so these
tests are the load-bearing safety net for the new pipeline.
"""
import math

import tile_geo as tg


def test_polyline_bbox_spans_extremes():
    mn_lat, mx_lat, mn_lon, mx_lon = tg.polyline_bbox(
        [(-1.0, 51.0), (2.5, 52.5), (-3.0, 49.75)]
    )
    assert mn_lat == 49.75
    assert mx_lat == 52.5
    assert mn_lon == -3.0
    assert mx_lon == 2.5


def test_bboxes_overlap_touching_share_edge():
    a = (0.0, 10.0, 0.0, 10.0)
    b = (10.0, 20.0, 10.0, 20.0)  # touches at corner
    assert tg.bboxes_overlap(a, b) is True


def test_bboxes_overlap_disjoint_returns_false():
    a = (0.0, 10.0, 0.0, 10.0)
    b = (20.0, 30.0, 20.0, 30.0)
    assert tg.bboxes_overlap(a, b) is False


def test_bboxes_overlap_contained_returns_true():
    outer = (0.0, 10.0, 0.0, 10.0)
    inner = (2.0, 8.0, 3.0, 7.0)
    assert tg.bboxes_overlap(outer, inner) is True


def test_clip_polyline_to_bbox_all_inside_returns_single_chunk():
    bbox = (35.0, 40.0, -125.0, -120.0)
    coords = [(-122.5, 37.5), (-122.4, 37.6), (-122.3, 37.7)]
    assert tg.clip_polyline_to_bbox(coords, bbox) == [coords]


def test_clip_polyline_to_bbox_all_outside_returns_empty():
    bbox = (35.0, 40.0, -125.0, -120.0)
    coords = [(-100.0, 50.0), (-99.0, 51.0)]
    assert tg.clip_polyline_to_bbox(coords, bbox) == []


def test_clip_polyline_to_bbox_splits_on_exit_and_reentry():
    bbox = (35.0, 40.0, -125.0, -120.0)
    coords = [
        (-122.5, 37.5),   # in
        (-122.4, 37.6),   # in
        (-100.0, 50.0),   # out — first chunk closes
        (-99.0, 51.0),    # out
        (-122.3, 37.7),   # in — second chunk starts
        (-122.2, 37.8),   # in
    ]
    chunks = tg.clip_polyline_to_bbox(coords, bbox)
    assert len(chunks) == 2
    assert chunks[0] == [(-122.5, 37.5), (-122.4, 37.6)]
    assert chunks[1] == [(-122.3, 37.7), (-122.2, 37.8)]


def test_clip_polyline_drops_single_inside_point():
    """A polyline that dips in and out with only one inside point never
    forms a drawable segment — should be dropped, not emitted as a
    zero-length line."""
    bbox = (35.0, 40.0, -125.0, -120.0)
    coords = [(-100.0, 50.0), (-122.5, 37.5), (-100.0, 50.0)]
    assert tg.clip_polyline_to_bbox(coords, bbox) == []


def test_dp_simplify_short_polyline_untouched():
    pts = [(0.0, 0.0), (1.0, 1.0)]
    assert tg.dp_simplify(pts, 0.1) == pts


def test_dp_simplify_collinear_middle_dropped():
    pts = [(0.0, 0.0), (1.0, 0.0), (2.0, 0.0), (3.0, 0.0)]
    assert tg.dp_simplify(pts, 0.001) == [(0.0, 0.0), (3.0, 0.0)]


def test_dp_simplify_preserves_peaks_above_tolerance():
    pts = [(0.0, 0.0), (5.0, 10.0), (10.0, 0.0)]
    assert tg.dp_simplify(pts, 1.0) == pts


def test_dp_simplify_removes_wiggles_below_tolerance():
    pts = [(0.0, 0.0), (5.0, 0.05), (10.0, 0.0)]
    assert tg.dp_simplify(pts, 0.1) == [(0.0, 0.0), (10.0, 0.0)]


def test_dp_simplify_no_recursion_limit_on_long_polyline():
    """Iterative DP should handle a 5000-point polyline without hitting
    Python's default recursion depth (1000) — the old script did too."""
    n = 5000
    pts = [(i / n, math.sin(i * 0.01)) for i in range(n)]
    result = tg.dp_simplify(pts, 0.001)
    # Just prove it returned without RecursionError.
    assert 2 <= len(result) <= n


def test_dp_simplify_preserves_endpoints():
    pts = [(0.0, 0.0), (1.0, 0.5), (2.0, 0.0), (3.0, -0.5), (4.0, 0.0)]
    result = tg.dp_simplify(pts, 10.0)  # huge tolerance
    assert result[0] == pts[0]
    assert result[-1] == pts[-1]


def test_sutherland_hodgman_polygon_fully_inside_unchanged():
    bbox = (0.0, 10.0, 0.0, 10.0)
    poly = [(2.0, 2.0), (8.0, 2.0), (8.0, 8.0), (2.0, 8.0)]
    result = tg.sutherland_hodgman_clip(poly, bbox)
    assert result == poly


def test_sutherland_hodgman_polygon_fully_outside_returns_empty():
    bbox = (0.0, 10.0, 0.0, 10.0)
    poly = [(20.0, 20.0), (30.0, 20.0), (30.0, 30.0), (20.0, 30.0)]
    assert tg.sutherland_hodgman_clip(poly, bbox) == []


def test_sutherland_hodgman_polygon_straddling_west_edge_gets_clipped():
    """Polygon crosses lon=0; result must sit entirely within [0, 10]
    on the lon axis."""
    bbox = (0.0, 10.0, 0.0, 10.0)
    poly = [(-5.0, 2.0), (5.0, 2.0), (5.0, 8.0), (-5.0, 8.0)]
    result = tg.sutherland_hodgman_clip(poly, bbox)
    assert len(result) >= 3
    assert all(bbox[2] <= lon <= bbox[3] for lon, _ in result)


def test_sutherland_hodgman_drops_closing_vertex_from_input():
    """A ring that ends with its opening vertex should not double-count
    that vertex in the output."""
    bbox = (0.0, 10.0, 0.0, 10.0)
    poly = [(2.0, 2.0), (8.0, 2.0), (8.0, 8.0), (2.0, 8.0), (2.0, 2.0)]
    result = tg.sutherland_hodgman_clip(poly, bbox)
    assert len(result) == 4  # not 5
    assert result[0] != result[-1]  # not closed


def test_sutherland_hodgman_polygon_entirely_containing_bbox_returns_bbox():
    """A polygon that engulfs the bbox should be clipped down to a
    rectangle matching the bbox corners."""
    bbox = (0.0, 10.0, 0.0, 10.0)
    poly = [(-20.0, -20.0), (20.0, -20.0), (20.0, 20.0), (-20.0, 20.0)]
    result = tg.sutherland_hodgman_clip(poly, bbox)
    lons = {lon for lon, _ in result}
    lats = {lat for _, lat in result}
    assert min(lons) == 0.0 and max(lons) == 10.0
    assert min(lats) == 0.0 and max(lats) == 10.0


def test_perp_dist_point_on_line_is_zero():
    assert tg._perp_dist((5.0, 0.0), (0.0, 0.0), (10.0, 0.0)) == 0.0


def test_perp_dist_degenerate_line_falls_back_to_point_distance():
    # sqrt((5-2)^2 + (6-2)^2) = 5
    assert tg._perp_dist((5.0, 6.0), (2.0, 2.0), (2.0, 2.0)) == 5.0
