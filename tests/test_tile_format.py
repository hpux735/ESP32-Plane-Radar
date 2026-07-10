"""Tests for scripts/tile_format.py — the binary tile format.

Round-trip tests are the primary contract: whatever the encoder produces,
the decoder must parse back into the same values. If either side drifts,
firmware and website will silently disagree on what's in a tile.
"""
import pytest

import tile_format as tf


def test_magic_and_version_are_locked():
    """These bytes end up in every tile file that ships. Changing them
    invalidates every cached tile on every device."""
    assert tf.MAGIC == b"PRT1"
    assert tf.VERSION == 1


def test_section_kinds_are_stable_integers():
    """The section kind byte goes on the wire and into cached files —
    reordering breaks every previously written tile."""
    assert tf.SECTION_COAST == 0
    assert tf.SECTION_LAND == 1
    assert tf.SECTION_WATER == 2
    assert tf.SECTION_AIRPORTS == 3


def test_deg_to_e7_matches_baked_encoding():
    """The firmware's existing baked arrays use int(round(lat * 1e7)).
    We must match, or a future comparison between old-baked and new-fetched
    coordinates could disagree by 1 ULP."""
    assert tf.deg_to_e7(37.7552) == 377552000
    assert tf.deg_to_e7(-122.4528) == -1224528000
    assert tf.deg_to_e7(0.0) == 0


def test_e7_to_deg_round_trips_to_seven_places():
    for deg in (37.7552, -122.4528, 0.0, 89.9999999, -89.9999999):
        assert tf.e7_to_deg(tf.deg_to_e7(deg)) == pytest.approx(deg, abs=1e-7)


def test_empty_tile_encodes_and_decodes():
    t = tf.Tile(z=7, x=20, y=37)
    data = tf.encode(t)
    back = tf.decode(data)
    assert (back.z, back.x, back.y) == (7, 20, 37)
    assert back.coast == [] and back.land == [] and back.water == []
    assert back.airports == []


def test_round_trip_coastline_preserves_points_to_e7_precision():
    poly = tf.Polyline(points=[(-122.5, 37.5), (-122.4, 37.6), (-122.3, 37.7)])
    t = tf.Tile(z=7, x=20, y=37, coast=[poly])
    back = tf.decode(tf.encode(t))
    assert len(back.coast) == 1
    got = back.coast[0].points
    assert len(got) == 3
    for (glon, glat), (elon, elat) in zip(got, poly.points):
        assert glon == pytest.approx(elon, abs=1e-7)
        assert glat == pytest.approx(elat, abs=1e-7)


def test_round_trip_preserves_all_sections_independently():
    """Each section should decode back to the same content, and cross-
    contamination between kinds would be a serious bug."""
    coast = [tf.Polyline([(-1.0, 51.0), (0.0, 51.5)])]
    land = [tf.Polyline([(-1.0, 51.0), (0.0, 51.5), (-1.0, 52.0), (-1.0, 51.0)])]
    water = [tf.Polyline([(0.0, 51.0), (0.5, 51.2), (0.5, 51.0), (0.0, 51.0)])]
    airports = [
        tf.Airport(
            ident="EGLL",
            lat=51.4706,
            lon=-0.461941,
            tier=3,
            instrument_approach=True,
            runways=[
                tf.Runway(51.4642, -0.4342, 51.4772, -0.4894),
                tf.Runway(51.4783, -0.4342, 51.4652, -0.4894),
            ],
        )
    ]
    t = tf.Tile(z=5, x=15, y=10, coast=coast, land=land, water=water, airports=airports)
    back = tf.decode(tf.encode(t))
    assert len(back.coast) == 1 and len(back.coast[0].points) == 2
    assert len(back.land) == 1 and len(back.land[0].points) == 4
    assert len(back.water) == 1
    assert len(back.airports) == 1
    a = back.airports[0]
    assert a.ident == "EGLL"
    assert a.tier == 3
    assert a.instrument_approach is True
    assert len(a.runways) == 2


def test_airport_flags_round_trip_all_combinations():
    for tier in (0, 1, 2, 3):
        for iap in (False, True):
            apt = tf.Airport(ident="TEST", lat=0.0, lon=0.0, tier=tier, instrument_approach=iap)
            back = tf.decode(tf.encode(tf.Tile(z=0, x=0, y=0, airports=[apt])))
            assert back.airports[0].tier == tier
            assert back.airports[0].instrument_approach is iap


def test_short_ident_pads_and_round_trips():
    """Non-ICAO airports have shorter idents (e.g. 'X1'). Padding then
    strip must give back the original string, not 'X1\\x00...'."""
    apt = tf.Airport(ident="X1", lat=0.0, lon=0.0)
    back = tf.decode(tf.encode(tf.Tile(z=0, x=0, y=0, airports=[apt])))
    assert back.airports[0].ident == "X1"


def test_ident_longer_than_eight_is_rejected():
    apt = tf.Airport(ident="TOOLONG9", lat=0.0, lon=0.0)  # 8 chars, fits
    tf.encode(tf.Tile(z=0, x=0, y=0, airports=[apt]))
    apt2 = tf.Airport(ident="NINELETTER", lat=0.0, lon=0.0)  # 10 chars
    with pytest.raises(ValueError):
        tf.encode(tf.Tile(z=0, x=0, y=0, airports=[apt2]))


def test_bad_magic_is_rejected():
    with pytest.raises(ValueError, match="bad magic"):
        tf.decode(b"XXXX" + b"\x00" * 8)


def test_bad_version_is_rejected():
    """A future tile format v2 must fail loudly on a v1 parser, not
    silently return garbage."""
    header = tf.HEADER_STRUCT.pack(tf.MAGIC, 99, 0, 0, 0, 0, 0)
    with pytest.raises(ValueError, match="unsupported version"):
        tf.decode(header)


def test_truncated_section_is_rejected():
    """Parser must not read past the end of the buffer on a corrupt tile."""
    good = tf.encode(tf.Tile(z=0, x=0, y=0, coast=[tf.Polyline([(0.0, 0.0), (1.0, 1.0)])]))
    # Chop the last few bytes of the payload.
    with pytest.raises(ValueError):
        tf.decode(good[:-4])


def test_unknown_section_kinds_are_silently_ignored():
    """Forward compat: a v1 parser fed a tile with a future-added section
    kind must return what it recognizes rather than crash."""
    header = tf.HEADER_STRUCT.pack(tf.MAGIC, tf.VERSION, 0, 0, 0, 1, 0)
    payload = b"junk"
    offset = tf.HEADER_STRUCT.size + tf.INDEX_STRUCT.size
    index = tf.INDEX_STRUCT.pack(77, offset, len(payload))  # kind=77
    data = header + index + payload
    back = tf.decode(data)
    assert back.coast == [] and back.airports == []


def test_polyline_bbox_matches_extremes():
    poly = tf.Polyline(points=[(-1.0, 51.0), (2.5, 52.5), (-3.0, 49.75)])
    mn_lat, mn_lon, mx_lat, mx_lon = poly.bbox_e7()
    assert tf.e7_to_deg(mn_lat) == pytest.approx(49.75)
    assert tf.e7_to_deg(mx_lat) == pytest.approx(52.5)
    assert tf.e7_to_deg(mn_lon) == pytest.approx(-3.0)
    assert tf.e7_to_deg(mx_lon) == pytest.approx(2.5)


def test_section_offset_beyond_64kb_round_trips():
    """z=3 tiles cover 45°×22.5° — enough to hold a whole continent
    of coast+land+airports. First-cut format used a uint16 section
    offset (64 KB cap) which exploded on the initial real-data bake.
    Regression: ensure a tile large enough to push the airport
    section past 64 KB still encodes and decodes cleanly.
    """
    # 5000 coastline points ≈ 5000 × 8 = 40 KB, plus overhead. Combined
    # with a matching land polygon this pushes the airport section
    # comfortably past 64 KB.
    coast_pts = [(i * 0.001, i * 0.001) for i in range(5000)]
    land_pts = [(i * 0.001, i * 0.001) for i in range(5000)]
    airport = tf.Airport(ident="ZZZZ", lat=0.0, lon=0.0)
    t = tf.Tile(
        z=3, x=4, y=3,
        coast=[tf.Polyline(coast_pts)],
        land=[tf.Polyline(land_pts)],
        airports=[airport],
    )
    data = tf.encode(t)
    assert len(data) > 65_536  # confirms the airport section really is past 64 KB
    back = tf.decode(data)
    assert len(back.coast[0].points) == 5000
    assert len(back.land[0].points) == 5000
    assert back.airports[0].ident == "ZZZZ"


def test_large_polyline_round_trips_correctly():
    """A 1000-point polyline stresses the offset math and section length
    encoding."""
    pts = [(i * 0.001, i * 0.001) for i in range(1000)]
    t = tf.Tile(z=7, x=20, y=37, coast=[tf.Polyline(pts)])
    back = tf.decode(tf.encode(t))
    assert len(back.coast[0].points) == 1000
    got = back.coast[0].points
    for (glon, glat), (elon, elat) in zip(got, pts):
        assert glon == pytest.approx(elon, abs=1e-7)
        assert glat == pytest.approx(elat, abs=1e-7)
