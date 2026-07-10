"""End-to-end tests for scripts/build_tiles.py — the pipeline entry
point.

The pure per-layer builders each have their own suite. These tests
prove the entry point:
  * merges layers into complete tiles keyed by (z, x, y)
  * drops empty tiles (no data at any layer)
  * emits deterministic bytes (byte-identical output for byte-identical input)
  * round-trips through tile_format.encode/decode
"""
from pathlib import Path

import build_tiles as bt
import tile_format as tf
import tile_scheme as ts


def _line_feat(coords):
    return {"type": "Feature", "geometry": {"type": "LineString", "coordinates": coords}}


def _poly_feat(rings):
    return {"type": "Feature", "geometry": {"type": "Polygon", "coordinates": rings}}


def _apt_row(ident="KSFO", atype="large_airport", lat="37.6188", lon="-122.375",
             scheduled="yes"):
    return {
        "ident": ident,
        "type": atype,
        "latitude_deg": lat,
        "longitude_deg": lon,
        "scheduled_service": scheduled,
    }


# ---------------------------------------------------------------------------
# load_iap_set — the FAA input parser
# ---------------------------------------------------------------------------


def test_load_iap_set_strips_comments_and_blank_lines(tmp_path):
    p = tmp_path / "iap.txt"
    p.write_text(
        "# comment\n"
        "\n"
        "KSFO\n"
        "  KOAK  \n"
        "# another comment\n"
        "khaf\n"
    )
    got = bt.load_iap_set(p)
    assert got == {"KSFO", "KOAK", "KHAF"}


def test_load_iap_set_missing_file_is_empty_set(tmp_path):
    assert bt.load_iap_set(tmp_path / "does-not-exist.txt") == set()


def test_load_iap_set_uppercases_everything(tmp_path):
    p = tmp_path / "iap.txt"
    p.write_text("kJfK\n")
    assert bt.load_iap_set(p) == {"KJFK"}


def test_shipped_iap_list_is_non_empty():
    """The bootstrapped file must at least contain the Bay Area focus
    airports referenced by the user's memory — otherwise a fresh pipeline
    run would silently drop the small-airport IAP force-includes."""
    got = bt.load_iap_set()
    assert "KHAF" in got
    assert "KSQL" in got
    assert "KSFO" in got


# ---------------------------------------------------------------------------
# build_all_tiles — merge behavior
# ---------------------------------------------------------------------------


def _small_dataset():
    """A minimal set of source features that produces at least one tile
    at every zoom level with at least one layer populated."""
    return dict(
        coast_features=[_line_feat([(-122.5, 37.75), (-122.4, 37.80), (-122.3, 37.85)])],
        land_features=[
            _poly_feat(
                [[[-122.5, 37.75], [-122.4, 37.75], [-122.4, 37.85], [-122.5, 37.85], [-122.5, 37.75]]]
            )
        ],
        island_features=[],
        water_features=[
            _poly_feat(
                [[[-122.45, 37.78], [-122.44, 37.78], [-122.44, 37.79], [-122.45, 37.79], [-122.45, 37.78]]]
            )
        ],
        airport_rows=[_apt_row()],
        runway_rows=[],
        iap_icaos=set(),
    )


def test_build_all_tiles_merges_layers_by_tile_key():
    """A tile that has coast + land + airport in the source should
    end up with all three layers populated in the merged output."""
    tiles = bt.build_all_tiles(**_small_dataset())
    z7_tiles = {(t.x, t.y): t for t in tiles if t.z == 7}
    # SF-area z=7 tile (20, 37) should have coast + land + airport all.
    assert (20, 37) in z7_tiles
    t = z7_tiles[(20, 37)]
    assert t.coast and t.land and t.airports


def test_build_all_tiles_returns_deterministic_z_x_y_order():
    tiles = bt.build_all_tiles(**_small_dataset())
    keys = [(t.z, t.x, t.y) for t in tiles]
    assert keys == sorted(keys)


def test_build_all_tiles_produces_byte_identical_output_for_same_input():
    """Two runs with identical inputs must yield byte-identical
    tile bytes — otherwise every deploy would churn the CDN even
    without data changes."""
    tiles_a = bt.build_all_tiles(**_small_dataset())
    tiles_b = bt.build_all_tiles(**_small_dataset())
    a_bytes = b"".join(tf.encode(t) for t in tiles_a)
    b_bytes = b"".join(tf.encode(t) for t in tiles_b)
    assert a_bytes == b_bytes


def test_build_all_tiles_drops_empty_tiles():
    """A dataset that produces no coast, land, water, or airports at
    a given (z, x, y) must NOT appear in the output — otherwise we'd
    write ~16k empty ocean files at z=7."""
    # Only feed a coastline feature; the tile of SFO should appear at
    # every zoom, but tiles nowhere near SF must not.
    tiles = bt.build_all_tiles(
        coast_features=[_line_feat([(-122.5, 37.75), (-122.3, 37.85)])],
        land_features=[],
        island_features=[],
        water_features=[],
        airport_rows=[],
        runway_rows=[],
        iap_icaos=set(),
    )
    for t in tiles:
        assert t.coast or t.land or t.water or t.airports


def test_build_all_tiles_respects_zoom_levels_filter():
    tiles = bt.build_all_tiles(**_small_dataset(), zoom_levels=(7,))
    assert {t.z for t in tiles} == {7}


def test_build_all_tiles_islands_merge_into_land_layer():
    """Islands live in a separate Natural Earth file but should
    render identically to the main landmass — merged into the same
    SECTION_LAND payload."""
    ring = [[-100.0, 30.0], [-99.0, 30.0], [-99.0, 31.0], [-100.0, 31.0], [-100.0, 30.0]]
    tiles = bt.build_all_tiles(
        coast_features=[],
        land_features=[],
        island_features=[_poly_feat([ring])],
        water_features=[],
        airport_rows=[],
        runway_rows=[],
        iap_icaos=set(),
    )
    assert any(t.land for t in tiles)


# ---------------------------------------------------------------------------
# write_tiles — file layout + decoder round-trip
# ---------------------------------------------------------------------------


def test_write_tiles_lays_out_z_x_y_directory_structure(tmp_path):
    tiles = bt.build_all_tiles(**_small_dataset())
    count, total_bytes = bt.write_tiles(tiles, tmp_path)
    assert count == len(tiles)
    assert total_bytes > 0
    # Every emitted file must live under tiles/{z}/{x}/{y}.bin.
    for t in tiles:
        path = tmp_path / ts.tile_relative_path(t.z, t.x, t.y)
        assert path.exists()


def test_write_tiles_bytes_round_trip_via_decoder(tmp_path):
    tiles = bt.build_all_tiles(**_small_dataset())
    bt.write_tiles(tiles, tmp_path)
    for t in tiles:
        path = tmp_path / ts.tile_relative_path(t.z, t.x, t.y)
        parsed = tf.decode(path.read_bytes())
        assert (parsed.z, parsed.x, parsed.y) == (t.z, t.x, t.y)
        assert len(parsed.coast) == len(t.coast)
        assert len(parsed.land) == len(t.land)
        assert len(parsed.water) == len(t.water)
        assert len(parsed.airports) == len(t.airports)


def test_write_tiles_empty_input_writes_nothing(tmp_path):
    count, total = bt.write_tiles([], tmp_path)
    assert (count, total) == (0, 0)
    assert list(tmp_path.iterdir()) == []
