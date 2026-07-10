"""Binary tile format encoder + decoder.

One file per (z, x, y) tile. Little-endian throughout because the ESP32
target is little-endian. Points are packed int32 microdegrees (lat*1e7,
lon*1e7) — the same encoding the current baked C arrays already use.

Layout:

    Header (12 bytes)
      magic        4 bytes  = b"PRT1"
      version      uint8    = 1
      z            uint8
      x            uint16
      y            uint16
      section_count uint8
      reserved     1 byte   (0)

    Section index (12 bytes per entry, section_count entries)
      kind         uint8    (see SECTION_* constants)
      reserved     3 bytes  (0 — reserved for future flags)
      offset       uint32   (bytes from start of file)
      length       uint32

    z=3 tiles (each covers 45°×22.5°) can pack a full continent of
    coastline+land into hundreds of KB — the offset field is uint32
    so an oversized section doesn't corrupt the header.

    Section payloads follow, in the order given by the index. Each
    payload's format is decided by its section kind:

    * SECTION_COAST / SECTION_LAND / SECTION_WATER  — polyline set
        polyline_count  uint16
        for each polyline:
          point_count   uint16
          bbox_min_lat  int32   (E7)
          bbox_min_lon  int32   (E7)
          bbox_max_lat  int32   (E7)
          bbox_max_lon  int32   (E7)
          for each point:
            lat_e7      int32
            lon_e7      int32

    * SECTION_AIRPORTS
        airport_count   uint16
        for each airport:
          lat_e7        int32
          lon_e7        int32
          flags         uint8   (bits 0-1: tier 0/1/2/3,
                                 bit 2:    instrument_approach,
                                 bit 3:    has_runways [not used yet])
          ident         8 bytes ASCII (null-padded)
          runway_count  uint8
          for each runway:
            lat1_e7     int32
            lon1_e7     int32
            lat2_e7     int32
            lon2_e7     int32

The section index means the parser can skip payload types it doesn't
care about — useful because the firmware and the website want the same
tile but render different subsets.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Iterable

MAGIC = b"PRT1"
VERSION = 1

SECTION_COAST = 0
SECTION_LAND = 1
SECTION_WATER = 2
SECTION_AIRPORTS = 3

HEADER_STRUCT = struct.Struct("<4sBBHHBB")   # 12 bytes
INDEX_STRUCT = struct.Struct("<B3xII")       # 12 bytes: kind, pad, offset, length


def deg_to_e7(deg: float) -> int:
    """Round a degree value to signed micro-degrees (1e-7 deg units)."""
    return int(round(deg * 1e7))


def e7_to_deg(e7: int) -> float:
    return e7 / 1e7


@dataclass
class Polyline:
    """A single polyline; a bbox is derived at encode-time."""

    points: list[tuple[float, float]] = field(default_factory=list)  # (lon, lat)

    def bbox_e7(self) -> tuple[int, int, int, int]:
        lats = [p[1] for p in self.points]
        lons = [p[0] for p in self.points]
        return (
            deg_to_e7(min(lats)),
            deg_to_e7(min(lons)),
            deg_to_e7(max(lats)),
            deg_to_e7(max(lons)),
        )


@dataclass
class Runway:
    lat1: float
    lon1: float
    lat2: float
    lon2: float


@dataclass
class Airport:
    ident: str                      # up to 8 ASCII chars
    lat: float
    lon: float
    tier: int = 0                   # 0=none, 1=small, 2=medium, 3=large
    instrument_approach: bool = False
    runways: list[Runway] = field(default_factory=list)

    def flags_byte(self) -> int:
        flags = self.tier & 0b11
        if self.instrument_approach:
            flags |= 0b100
        if self.runways:
            flags |= 0b1000
        return flags


def _encode_polylines(polylines: Iterable[Polyline]) -> bytes:
    polys = list(polylines)
    out = bytearray()
    out += struct.pack("<H", len(polys))
    for poly in polys:
        pts = poly.points
        if len(pts) > 0xFFFF:
            raise ValueError(f"polyline has {len(pts)} points, exceeds uint16")
        out += struct.pack("<H", len(pts))
        if pts:
            mn_lat, mn_lon, mx_lat, mx_lon = poly.bbox_e7()
        else:
            mn_lat = mn_lon = mx_lat = mx_lon = 0
        out += struct.pack("<iiii", mn_lat, mn_lon, mx_lat, mx_lon)
        for lon, lat in pts:
            out += struct.pack("<ii", deg_to_e7(lat), deg_to_e7(lon))
    return bytes(out)


def _decode_polylines(data: bytes) -> list[Polyline]:
    off = 0
    (count,) = struct.unpack_from("<H", data, off)
    off += 2
    polys: list[Polyline] = []
    for _ in range(count):
        (point_count,) = struct.unpack_from("<H", data, off)
        off += 2
        # Skip bbox (client can re-derive if it cares).
        off += 16
        pts: list[tuple[float, float]] = []
        for _ in range(point_count):
            lat_e7, lon_e7 = struct.unpack_from("<ii", data, off)
            off += 8
            pts.append((e7_to_deg(lon_e7), e7_to_deg(lat_e7)))
        polys.append(Polyline(pts))
    return polys


def _encode_airports(airports: Iterable[Airport]) -> bytes:
    apts = list(airports)
    out = bytearray()
    out += struct.pack("<H", len(apts))
    for a in apts:
        if len(a.ident) > 8:
            raise ValueError(f"airport ident {a.ident!r} exceeds 8 bytes")
        ident_bytes = a.ident.encode("ascii").ljust(8, b"\x00")
        out += struct.pack("<ii", deg_to_e7(a.lat), deg_to_e7(a.lon))
        out += struct.pack("<B", a.flags_byte())
        out += ident_bytes
        if len(a.runways) > 0xFF:
            raise ValueError(f"airport {a.ident} has too many runways")
        out += struct.pack("<B", len(a.runways))
        for r in a.runways:
            out += struct.pack(
                "<iiii",
                deg_to_e7(r.lat1),
                deg_to_e7(r.lon1),
                deg_to_e7(r.lat2),
                deg_to_e7(r.lon2),
            )
    return bytes(out)


def _decode_airports(data: bytes) -> list[Airport]:
    off = 0
    (count,) = struct.unpack_from("<H", data, off)
    off += 2
    airports: list[Airport] = []
    for _ in range(count):
        lat_e7, lon_e7 = struct.unpack_from("<ii", data, off)
        off += 8
        (flags,) = struct.unpack_from("<B", data, off)
        off += 1
        ident_bytes = data[off : off + 8]
        off += 8
        (rwy_count,) = struct.unpack_from("<B", data, off)
        off += 1
        runways: list[Runway] = []
        for _ in range(rwy_count):
            la1, lo1, la2, lo2 = struct.unpack_from("<iiii", data, off)
            off += 16
            runways.append(
                Runway(e7_to_deg(la1), e7_to_deg(lo1), e7_to_deg(la2), e7_to_deg(lo2))
            )
        ident = ident_bytes.rstrip(b"\x00").decode("ascii")
        airports.append(
            Airport(
                ident=ident,
                lat=e7_to_deg(lat_e7),
                lon=e7_to_deg(lon_e7),
                tier=flags & 0b11,
                instrument_approach=bool(flags & 0b100),
                runways=runways,
            )
        )
    return airports


@dataclass
class Tile:
    z: int
    x: int
    y: int
    coast: list[Polyline] = field(default_factory=list)
    land: list[Polyline] = field(default_factory=list)
    water: list[Polyline] = field(default_factory=list)
    airports: list[Airport] = field(default_factory=list)


def encode(tile: Tile) -> bytes:
    """Serialize a tile to bytes."""
    sections: list[tuple[int, bytes]] = []
    if tile.coast:
        sections.append((SECTION_COAST, _encode_polylines(tile.coast)))
    if tile.land:
        sections.append((SECTION_LAND, _encode_polylines(tile.land)))
    if tile.water:
        sections.append((SECTION_WATER, _encode_polylines(tile.water)))
    if tile.airports:
        sections.append((SECTION_AIRPORTS, _encode_airports(tile.airports)))

    if not (0 <= tile.z <= 255):
        raise ValueError(f"zoom {tile.z} out of range")
    if not (0 <= tile.x <= 0xFFFF and 0 <= tile.y <= 0xFFFF):
        raise ValueError(f"tile ({tile.x}, {tile.y}) out of uint16 range")

    header = HEADER_STRUCT.pack(
        MAGIC, VERSION, tile.z, tile.x, tile.y, len(sections), 0
    )
    index_size = INDEX_STRUCT.size * len(sections)
    payload_start = len(header) + index_size

    index = bytearray()
    payload = bytearray()
    offset = payload_start
    for kind, data in sections:
        index += INDEX_STRUCT.pack(kind, offset, len(data))
        payload += data
        offset += len(data)

    return bytes(header) + bytes(index) + bytes(payload)


def decode(data: bytes) -> Tile:
    """Parse a tile from bytes. Raises ValueError on malformed input."""
    if len(data) < HEADER_STRUCT.size:
        raise ValueError("tile shorter than header")
    magic, version, z, x, y, section_count, _reserved = HEADER_STRUCT.unpack_from(
        data, 0
    )
    if magic != MAGIC:
        raise ValueError(f"bad magic: {magic!r}")
    if version != VERSION:
        raise ValueError(f"unsupported version: {version}")

    tile = Tile(z=z, x=x, y=y)
    off = HEADER_STRUCT.size
    entries: list[tuple[int, int, int]] = []
    for _ in range(section_count):
        kind, sec_off, sec_len = INDEX_STRUCT.unpack_from(data, off)
        off += INDEX_STRUCT.size
        entries.append((kind, sec_off, sec_len))

    for kind, sec_off, sec_len in entries:
        if sec_off + sec_len > len(data):
            raise ValueError(
                f"section {kind} at offset {sec_off}+{sec_len} exceeds file "
                f"size {len(data)}"
            )
        payload = data[sec_off : sec_off + sec_len]
        if kind == SECTION_COAST:
            tile.coast = _decode_polylines(payload)
        elif kind == SECTION_LAND:
            tile.land = _decode_polylines(payload)
        elif kind == SECTION_WATER:
            tile.water = _decode_polylines(payload)
        elif kind == SECTION_AIRPORTS:
            tile.airports = _decode_airports(payload)
        # Unknown kinds are silently skipped — forward compat.

    return tile
