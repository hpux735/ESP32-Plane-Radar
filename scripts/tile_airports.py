"""Per-tile airport pipeline.

Inputs: OurAirports CSVs (airports, runways, navaids). Emits
{(z, x, y): [Airport, ...]} for the tile pyramid.

Filtering:
  * Keep every airport worldwide with a 4-letter ICAO code that is
    either a large airport, a medium airport, or a small airport with
    scheduled service.
  * Force-include any airport that looks IFR-capable — see
    `iap_idents_from_openflight_data()` below for the two signals we
    combine (lighted runway = RNAV/GPS-approach-capable; on-field
    navaid = VOR/NDB/DME-approach-capable).
  * Drop obvious heliports.

The IAP source is passed in as a pre-computed set of idents rather
than derived inline, so the pipeline module stays pure and testable;
the actual CSV fetches live in the build_tiles.py entry point.
"""
from __future__ import annotations

from typing import Iterable

import tile_format as tf
import tile_scheme as ts

AIRPORT_TIER = {
    "large_airport": 3,
    "medium_airport": 2,
    "small_airport": 1,
}


def iap_idents_from_openflight_data(
    runways: Iterable[dict],
    navaids: Iterable[dict],
) -> set[str]:
    """Best-effort "has a published instrument approach" set, derived
    from the two OurAirports CSVs that carry that signal today.

    Combines two proxies because no single field says "has an IAP":

    * **Lighted, non-closed runway** (from runways.csv). Every airport
      that supports night or IFR operations lights its primary runway,
      and virtually every modern lighted runway has at least an RNAV
      (GPS) approach published to it — RNAV is a satellite-only
      approach that needs no ground equipment, so it exists at almost
      every real airport with an IFR-approved runway.

    * **On-field navaid** (from navaids.csv). Any VOR, NDB, DME,
      TACAN, or VOR-DME whose `associated_airport` is this airport
      almost always supports a VOR / NDB / DME approach at the field.
      OurAirports' navaids.csv doesn't include ILS/LOC records, but
      those airports are covered by the lighting signal above.

    Together the two signals catch the union of RNAV, VOR, NDB, and
    ILS approach airports globally. False positives (lighted field
    with no published approach) are rare and harmless — the map just
    draws one extra dot at high zoom."""
    out: set[str] = set()
    # Signal 1 — lighted runways.
    for r in runways:
        if (r.get("closed") or "").strip() == "1":
            continue
        if (r.get("lighted") or "").strip() != "1":
            continue
        ident = (r.get("airport_ident") or "").strip().upper()
        if ident:
            out.add(ident)
    # Signal 2 — on-field navaids.
    for n in navaids:
        ident = (n.get("associated_airport") or "").strip().upper()
        if ident:
            out.add(ident)
    return out


def _is_h_designator(s: str) -> bool:
    if not s or s[0] != "H":
        return False
    rest = s[1:]
    return not rest or rest[0] in "-_" or rest.isdigit()


def _is_helipad(row: dict) -> bool:
    """Same rule as scripts/build_large_airports.py — H-prefix idents +
    <2500 ft length. See tests/test_airport_builder.py for the fixture
    matrix that pins this down."""
    le = (row.get("le_ident") or "").strip().upper()
    he = (row.get("he_ident") or "").strip().upper()
    if not _is_h_designator(le) and not _is_h_designator(he):
        return False
    try:
        length_ft = int(row.get("length_ft") or 0)
    except ValueError:
        length_ft = 0
    if _is_h_designator(le) and _is_h_designator(he):
        return True
    return length_ft < 2500


def _parse_float(s: str | None) -> float | None:
    if s is None:
        return None
    s = s.strip()
    if not s:
        return None
    try:
        return float(s)
    except ValueError:
        return None


def _valid_icao(ident: str) -> bool:
    """OurAirports uses 4-letter ICAO identifiers for airports on the
    global ICAO grid. Airports without an ICAO grid entry get a longer
    'local code' — we skip those since the device only knows how to
    label 4-letter codes."""
    return len(ident) == 4 and ident.isalnum()


def _runways_by_airport(runways: Iterable[dict]) -> dict[str, list[tf.Runway]]:
    out: dict[str, list[tf.Runway]] = {}
    for r in runways:
        if _is_helipad(r):
            continue
        ident = (r.get("airport_ident") or "").strip()
        if not ident:
            continue
        lat1 = _parse_float(r.get("le_latitude_deg"))
        lon1 = _parse_float(r.get("le_longitude_deg"))
        lat2 = _parse_float(r.get("he_latitude_deg"))
        lon2 = _parse_float(r.get("he_longitude_deg"))
        if None in (lat1, lon1, lat2, lon2):
            continue
        out.setdefault(ident, []).append(
            tf.Runway(lat1=lat1, lon1=lon1, lat2=lat2, lon2=lon2)
        )
    return out


def build_airports(
    airports: Iterable[dict],
    runways: Iterable[dict],
    iap_icaos: Iterable[str] = (),
) -> list[tf.Airport]:
    """Filter and hydrate the OurAirports rows into tf.Airport records.
    Returns airports in stable order sorted by (-tier, ident) so a
    given input always produces the same output — important for
    reproducible tile builds.
    """
    iap_set = {code.strip().upper() for code in iap_icaos if code}
    rw_by_apt = _runways_by_airport(runways)

    result: list[tf.Airport] = []
    for a in airports:
        ident = (a.get("ident") or "").strip().upper()
        if not _valid_icao(ident):
            continue
        atype = a.get("type", "")
        tier = AIRPORT_TIER.get(atype, 0)
        # Tier-0 rows are heliports, seaplane bases, balloonports, and
        # closed airports — the AIRPORT_TIER dict deliberately doesn't
        # score them, and this radar shows fixed-wing traffic only. Skip
        # them regardless of any other signal (an IAP flag can only
        # ever elevate a *small* airport, never revive a heliport).
        if tier == 0:
            continue
        has_iap = ident in iap_set
        scheduled = (a.get("scheduled_service") or "").strip().lower() == "yes"
        # tier 2 / 3 → always keep. tier 1 → keep only if scheduled
        # service or a published instrument approach elevates it.
        keep = tier >= 2 or (tier == 1 and (scheduled or has_iap))
        if not keep:
            continue
        lat = _parse_float(a.get("latitude_deg"))
        lon = _parse_float(a.get("longitude_deg"))
        if lat is None or lon is None:
            continue
        result.append(
            tf.Airport(
                ident=ident,
                lat=lat,
                lon=lon,
                tier=tier,
                instrument_approach=has_iap,
                runways=rw_by_apt.get(ident, []),
            )
        )

    result.sort(key=lambda a: (-a.tier, a.ident))
    return result


def build_airport_tiles(
    airports: Iterable[dict],
    runways: Iterable[dict],
    iap_icaos: Iterable[str] = (),
    zoom_levels: Iterable[int] = ts.ZOOM_LEVELS,
) -> dict[tuple[int, int, int], list[tf.Airport]]:
    """One airport lands in exactly one tile at each zoom (the one
    containing its lat/lon)."""
    apts = build_airports(airports, runways, iap_icaos)
    result: dict[tuple[int, int, int], list[tf.Airport]] = {}
    for a in apts:
        for z in zoom_levels:
            x, y = ts.tile_of(z, a.lat, a.lon)
            result.setdefault((z, x, y), []).append(a)
    return result
