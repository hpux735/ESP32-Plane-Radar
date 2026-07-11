"""Tests for scripts/tile_airports.py — the airport layer of the tile
pipeline.

The filter rules are the load-bearing part: get them wrong and the map
either goes empty (over-strict filter) or bloats to include every
private grass strip (over-permissive filter). These tests pin down
the boundaries.
"""
import tile_airports as ta
import tile_scheme as ts


def _apt(ident="KSFO", atype="large_airport", lat="37.6188", lon="-122.375",
         scheduled="yes"):
    return {
        "ident": ident,
        "type": atype,
        "latitude_deg": lat,
        "longitude_deg": lon,
        "scheduled_service": scheduled,
    }


def _rwy(airport="KSFO", le_lat="37.61", le_lon="-122.38",
         he_lat="37.62", he_lon="-122.37", le="28L", he="10R",
         length="10000"):
    return {
        "airport_ident": airport,
        "le_latitude_deg": le_lat,
        "le_longitude_deg": le_lon,
        "he_latitude_deg": he_lat,
        "he_longitude_deg": he_lon,
        "le_ident": le,
        "he_ident": he,
        "length_ft": length,
    }


# ---------------------------------------------------------------------------
# iap_idents_from_openflight_data — instrument-approach superset
# ---------------------------------------------------------------------------


def _rwrow(airport="KSFO", lighted="1", closed="0"):
    return {
        "airport_ident": airport,
        "lighted": lighted,
        "closed": closed,
    }


def _nvrow(associated="KSFO", ntype="VOR"):
    return {
        "associated_airport": associated,
        "type": ntype,
    }


# --- Signal 1: lighted runway (RNAV-approach proxy) -------------------


def test_lighted_runway_makes_airport_iap():
    """Runway with lights on = airport supports night/IFR ops = force-
    include on the map. Every modern lighted runway has at least an
    RNAV (GPS) approach published to it."""
    got = ta.iap_idents_from_openflight_data([_rwrow("KJFK", lighted="1")], [])
    assert got == {"KJFK"}


def test_unlighted_runway_alone_does_not_qualify():
    """Grass strips and daylight-only fields without any navaid on
    the field don't count."""
    got = ta.iap_idents_from_openflight_data([_rwrow("KAAA", lighted="0")], [])
    assert got == set()


def test_closed_runway_does_not_qualify():
    """A lighted runway on a closed airport shouldn't force-include
    the airport — the field isn't operational."""
    got = ta.iap_idents_from_openflight_data(
        [_rwrow("KAAA", lighted="1", closed="1")], []
    )
    assert got == set()


# --- Signal 2: on-field navaid (VOR/NDB/DME-approach proxy) ------------


def test_on_field_vor_makes_airport_iap():
    """A VOR whose associated_airport is set almost always means that
    airport has a VOR approach — the whole point of colocating the
    VOR with the airport."""
    got = ta.iap_idents_from_openflight_data(
        [], [_nvrow("KCCR", "VOR")]
    )
    assert got == {"KCCR"}


def test_on_field_ndb_makes_airport_iap():
    """NDB approaches are old but still published at hundreds of
    smaller fields worldwide."""
    got = ta.iap_idents_from_openflight_data(
        [], [_nvrow("KHAF", "NDB")]
    )
    assert got == {"KHAF"}


def test_on_field_dme_or_tacan_qualifies():
    """DME and TACAN colocated with an airport indicate an approach
    that uses distance info — force-include."""
    got = ta.iap_idents_from_openflight_data(
        [],
        [_nvrow("KAAA", "DME"), _nvrow("KBBB", "TACAN"),
         _nvrow("KCCC", "VOR-DME")],
    )
    assert got == {"KAAA", "KBBB", "KCCC"}


def test_enroute_navaid_with_no_associated_airport_dropped():
    """Enroute VORs (out in the middle of nowhere) have no
    associated_airport — they must not create phantom empty-string
    idents in the IAP set."""
    got = ta.iap_idents_from_openflight_data(
        [], [_nvrow("", "VOR"), {"type": "NDB"}]
    )
    assert got == set()


# --- Combined behavior -------------------------------------------------


def test_signals_union_when_both_present():
    """Two airports: one caught by the lighting signal, one caught by
    the navaid signal. Result includes both."""
    got = ta.iap_idents_from_openflight_data(
        [_rwrow("KJFK", lighted="1")],
        [_nvrow("KCCR", "VOR")],
    )
    assert got == {"KJFK", "KCCR"}


def test_airport_caught_by_both_signals_appears_once():
    """Lighted runway AND on-field VOR at the same airport → still one
    entry in the set."""
    got = ta.iap_idents_from_openflight_data(
        [_rwrow("KSFO", lighted="1")],
        [_nvrow("KSFO", "VOR")],
    )
    assert got == {"KSFO"}


def test_ident_uppercased_from_both_sources():
    """Both signals normalize to uppercase so the set matches what
    build_airports() compares airport idents against."""
    got = ta.iap_idents_from_openflight_data(
        [_rwrow("ksfo", lighted="1")],
        [_nvrow("koak", "VOR")],
    )
    assert got == {"KSFO", "KOAK"}


# ---------------------------------------------------------------------------
# build_airports filter matrix
# ---------------------------------------------------------------------------


def test_large_airport_always_kept():
    apts = ta.build_airports([_apt(atype="large_airport")], [])
    assert [a.ident for a in apts] == ["KSFO"]


def test_medium_airport_always_kept():
    apts = ta.build_airports(
        [_apt(ident="KOAK", atype="medium_airport", scheduled="no")], []
    )
    assert [a.ident for a in apts] == ["KOAK"]


def test_small_airport_with_scheduled_service_kept():
    apts = ta.build_airports(
        [_apt(ident="KAAA", atype="small_airport", scheduled="yes")], []
    )
    assert [a.ident for a in apts] == ["KAAA"]


def test_small_airport_without_scheduled_service_dropped():
    apts = ta.build_airports(
        [_apt(ident="KAAA", atype="small_airport", scheduled="no")], []
    )
    assert apts == []


def test_iap_flag_force_includes_otherwise_filtered_airport():
    """A tiny GA strip with no scheduled service but a published
    instrument approach must appear on the map — instrument-rated
    pilots plan to it."""
    apts = ta.build_airports(
        [_apt(ident="KHAF", atype="small_airport", scheduled="no")],
        [],
        iap_icaos=["KHAF"],
    )
    assert len(apts) == 1
    assert apts[0].instrument_approach is True


def test_iap_flag_does_not_revive_heliport():
    """Regression: a hospital heliport with a lighted pad ends up in
    the IAP set (lighted-runway signal), but the type filter must
    keep it out anyway. Otherwise CN02 (SF VA Med Center Heliport),
    7CL0 / 7CL1 (Children's Hospital Oakland) all show up on the
    plane radar despite being hospital pads."""
    apts = ta.build_airports(
        [_apt(ident="7CL1", atype="heliport", scheduled="no")],
        [],
        iap_icaos=["7CL1"],
    )
    assert apts == []


def test_iap_flag_does_not_revive_seaplane_base():
    """Seaplane bases aren't fixed-wing airports for this radar."""
    apts = ta.build_airports(
        [_apt(ident="KABC", atype="seaplane_base", scheduled="no")],
        [],
        iap_icaos=["KABC"],
    )
    assert apts == []


def test_iap_flag_does_not_revive_closed_airport():
    """Closed airports must never appear — active runways only."""
    apts = ta.build_airports(
        [_apt(ident="KABC", atype="closed", scheduled="no")],
        [],
        iap_icaos=["KABC"],
    )
    assert apts == []


def test_iap_flag_does_not_revive_balloonport():
    """Balloonports are novelties, not radar targets."""
    apts = ta.build_airports(
        [_apt(ident="KABC", atype="balloonport", scheduled="no")],
        [],
        iap_icaos=["KABC"],
    )
    assert apts == []


def test_iap_set_case_insensitive():
    apts = ta.build_airports(
        [_apt(ident="KHAF", atype="small_airport", scheduled="no")],
        [],
        iap_icaos=["khaf"],
    )
    assert len(apts) == 1


def test_non_iap_airport_has_iap_flag_false():
    apts = ta.build_airports(
        [_apt(atype="large_airport")], [], iap_icaos=[]
    )
    assert apts[0].instrument_approach is False


def test_non_icao_ident_is_dropped():
    """OurAirports uses local codes (e.g. '4Q7', 'US-1234') for airports
    without an ICAO grid entry. The device only knows how to label
    4-letter codes so we skip those upstream."""
    apts = ta.build_airports(
        [_apt(ident="4Q7", atype="small_airport", scheduled="yes")], []
    )
    assert apts == []


def test_short_ident_is_dropped():
    apts = ta.build_airports(
        [_apt(ident="KSF", atype="large_airport")], []
    )
    assert apts == []


def test_five_char_ident_is_dropped():
    apts = ta.build_airports(
        [_apt(ident="KSFO1", atype="large_airport")], []
    )
    assert apts == []


def test_missing_coords_row_is_dropped():
    apts = ta.build_airports(
        [_apt(lat="", lon="")], []
    )
    assert apts == []


# ---------------------------------------------------------------------------
# Runway attachment
# ---------------------------------------------------------------------------


def test_runway_attached_to_matching_airport():
    apts = ta.build_airports(
        [_apt(atype="large_airport", ident="KSFO")],
        [_rwy(airport="KSFO")],
    )
    assert len(apts) == 1
    assert len(apts[0].runways) == 1
    r = apts[0].runways[0]
    assert (r.lat1, r.lon1) == (37.61, -122.38)


def test_heliport_pad_row_ignored():
    """A runway row with H-designator idents + short length is a helipad,
    not a runway."""
    apts = ta.build_airports(
        [_apt(atype="large_airport", ident="KSFO")],
        [
            _rwy(airport="KSFO", le="H1", he="H2", length="50"),
            _rwy(airport="KSFO"),
        ],
    )
    assert len(apts[0].runways) == 1


def test_missing_runway_coord_dropped():
    apts = ta.build_airports(
        [_apt(atype="large_airport", ident="KSFO")],
        [_rwy(airport="KSFO", he_lat="")],
    )
    assert apts[0].runways == []


# ---------------------------------------------------------------------------
# Deterministic ordering
# ---------------------------------------------------------------------------


def test_output_ordering_is_deterministic():
    """Sorted by (-tier, ident) so the same input always produces the
    same tile bytes — otherwise every re-run of the pipeline would
    change the deploy hash even with no data changes."""
    rows = [
        _apt(ident="KAAA", atype="small_airport"),   # tier 1 + scheduled
        _apt(ident="KOAK", atype="medium_airport"),  # tier 2
        _apt(ident="KSFO", atype="large_airport"),   # tier 3
        _apt(ident="KBBB", atype="medium_airport"),  # tier 2
    ]
    apts = ta.build_airports(rows, [])
    assert [a.ident for a in apts] == ["KSFO", "KBBB", "KOAK", "KAAA"]


# ---------------------------------------------------------------------------
# Tile bucketing
# ---------------------------------------------------------------------------


def test_airport_lands_in_one_tile_per_zoom():
    """One airport = one tile at each zoom level."""
    tiles = ta.build_airport_tiles(
        [_apt(atype="large_airport", lat="37.7552", lon="-122.4528")], []
    )
    z_seen = {z for (z, _, _) in tiles.keys()}
    assert z_seen == set(ts.ZOOM_LEVELS)
    for (z, x, y), apts in tiles.items():
        assert len(apts) == 1
        # Cross-check the (x,y) matches tile_of.
        assert (x, y) == ts.tile_of(z, 37.7552, -122.4528)


def test_two_airports_in_same_tile_both_appear():
    tiles = ta.build_airport_tiles(
        [
            _apt(ident="KSFO", lat="37.6188", lon="-122.375"),
            _apt(ident="KOAK", atype="medium_airport", lat="37.7213", lon="-122.2208"),
        ],
        [],
    )
    # Both are within the same z=3 tile (western US, tile (2, 3)).
    # Confirm at least one tile at z=3 contains both.
    counts = {
        (x, y): len(apts)
        for (z, x, y), apts in tiles.items()
        if z == 3
    }
    assert max(counts.values()) == 2


def test_two_airports_in_different_tiles_split():
    """SF and NYC land in different tiles at every zoom level."""
    tiles = ta.build_airport_tiles(
        [
            _apt(ident="KSFO", lat="37.6188", lon="-122.375"),
            _apt(ident="KJFK", lat="40.6413", lon="-73.7781"),
        ],
        [],
    )
    for z in ts.ZOOM_LEVELS:
        tiles_at_z = [(x, y) for (zz, x, y), _ in tiles.items() if zz == z]
        assert len(set(tiles_at_z)) == 2
