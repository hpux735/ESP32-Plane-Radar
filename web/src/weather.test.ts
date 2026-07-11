import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import {
  ceilingFromClouds,
  deriveCategory,
  distanceNm,
  invalidate,
  lastUpdateMs,
  rebuildStations,
  refreshIfStale,
  STATIONS,
  updateAll,
} from "./weather";

describe("deriveCategory (FAA flight rules)", () => {
  it("returns VFR when no ceiling and >5 sm visibility", () => {
    expect(deriveCategory(Infinity, 10)).toBe("VFR");
  });

  it("returns MVFR at ceiling 3000 ft (upper MVFR boundary)", () => {
    expect(deriveCategory(3000, 10)).toBe("MVFR");
  });

  it("returns VFR at ceiling 3001 ft (just above MVFR)", () => {
    expect(deriveCategory(3001, 10)).toBe("VFR");
  });

  it("returns IFR at ceiling 999 ft (just below IFR/MVFR boundary)", () => {
    expect(deriveCategory(999, 10)).toBe("IFR");
  });

  it("returns LIFR at ceiling 499 ft (just below IFR/LIFR boundary)", () => {
    expect(deriveCategory(499, 10)).toBe("LIFR");
  });

  it("returns MVFR at visibility 5 sm with high ceiling", () => {
    expect(deriveCategory(Infinity, 5)).toBe("MVFR");
  });

  it("returns IFR at visibility <3 sm with high ceiling", () => {
    expect(deriveCategory(Infinity, 2)).toBe("IFR");
  });

  it("returns LIFR at visibility <1 sm with high ceiling", () => {
    expect(deriveCategory(Infinity, 0)).toBe("LIFR");
  });

  it("picks the WORST of ceiling and visibility categories", () => {
    // Ceiling VFR (5000), vis LIFR (0.5) → LIFR wins.
    expect(deriveCategory(5000, 0)).toBe("LIFR");
    // Ceiling IFR (800), vis VFR (10) → IFR wins.
    expect(deriveCategory(800, 10)).toBe("IFR");
  });
});

describe("ceilingFromClouds", () => {
  it("returns Infinity when clouds is null or empty", () => {
    expect(ceilingFromClouds(null)).toBe(Infinity);
    expect(ceilingFromClouds(undefined)).toBe(Infinity);
    expect(ceilingFromClouds([])).toBe(Infinity);
  });

  it("ignores FEW and SCT layers (only BKN/OVC/VV count as ceiling)", () => {
    // FEW/SCT never count as ceiling regardless of base altitude.
    const clouds = [
      { base: 1000, cover: "FEW" },
      { base: 2000, cover: "SCT" },
    ];
    expect(ceilingFromClouds(clouds)).toBe(Infinity);
  });

  it("returns the lowest BKN layer in feet", () => {
    // aviationweather.gov cloud bases are already in feet AGL.
    const clouds = [
      { base: 2000, cover: "BKN" },
      { base: 1000, cover: "BKN" },
    ];
    expect(ceilingFromClouds(clouds)).toBe(1000);
  });

  it("counts OVC as a ceiling", () => {
    const clouds = [{ base: 500, cover: "OVC" }];
    expect(ceilingFromClouds(clouds)).toBe(500);
  });

  it("counts VV (vertical visibility) as a ceiling", () => {
    const clouds = [{ base: 100, cover: "VV" }];
    expect(ceilingFromClouds(clouds)).toBe(100);
  });

  it("is case-insensitive on layer type", () => {
    const clouds = [{ base: 1000, cover: "bkn" }];
    expect(ceilingFromClouds(clouds)).toBe(1000);
  });
});

describe("distanceNm", () => {
  it("is zero for the same point", () => {
    expect(distanceNm(37.7, -122.4, 37.7, -122.4)).toBe(0);
  });

  it("returns ~60 nm for 1° of latitude", () => {
    // Same longitude, 1° apart in latitude.
    expect(distanceNm(37.0, -122.0, 38.0, -122.0)).toBeCloseTo(60, 1);
  });

  it("scales longitude by cos(lat)", () => {
    // 1° of longitude at the equator ≈ 60 nm; at 60°N ≈ 30 nm.
    const equatorDist = distanceNm(0, 0, 0, 1);
    const highLatDist = distanceNm(60, 0, 60, 1);
    expect(equatorDist).toBeCloseTo(60, 1);
    expect(highLatDist).toBeCloseTo(30, 1);
  });
});

// ---- rebuildStations + updateAll ------------------------------------
// These tests drive the async fetch loop with a stubbed fetch so the
// full contract of "user changes bbox → next fetch hits the new URL →
// nearest stations populate STATIONS" is locked, not just the pure
// helpers underneath.

function stubJsonResponse(rows: unknown, ok = true, status = 200) {
  const fetchMock = vi.fn(() => Promise.resolve(new Response(
    JSON.stringify(rows), { status: ok ? 200 : status },
  )));
  vi.stubGlobal("fetch", fetchMock);
  return fetchMock;
}

const SFO_METAR = { icaoId: "KSFO", lat: 37.62, lon: -122.375, visib: "10+", clouds: [] };
const OAK_METAR = { icaoId: "KOAK", lat: 37.72, lon: -122.22, visib: 3, clouds: [{ base: 900, cover: "BKN" }] };
const SEA_METAR = { icaoId: "KSEA", lat: 47.45, lon: -122.30, visib: 10, clouds: [] };

beforeEach(() => {
  STATIONS.length = 0;
  invalidate();
});

afterEach(() => {
  vi.unstubAllGlobals();
});

describe("rebuildStations + updateAll", () => {
  it("no-op when rebuildStations hasn't set a bbox yet", async () => {
    const fetchMock = stubJsonResponse([]);
    await updateAll();
    expect(fetchMock).not.toHaveBeenCalled();
    expect(STATIONS.length).toBe(0);
  });

  it("fetches /api/metar with the bbox from rebuildStations", async () => {
    const fetchMock = stubJsonResponse([SFO_METAR]);
    rebuildStations([], 37.75, -122.45, 30);
    await updateAll();
    expect(fetchMock).toHaveBeenCalledTimes(1);
    const url = String(fetchMock.mock.calls[0][0]);
    expect(url).toContain("/api/metar?bbox=");
    // 30 nm / 60 (nm/deg) = 0.5° lat half-width.
    expect(url).toContain("37.2500");
    expect(url).toContain("38.2500");
  });

  it("populates STATIONS with the parsed rows on success", async () => {
    stubJsonResponse([SFO_METAR, OAK_METAR]);
    rebuildStations([], 37.75, -122.45, 30);
    await updateAll();
    expect(STATIONS.length).toBe(2);
    const bySfo = STATIONS.find(s => s.icao === "KSFO")!;
    expect(bySfo.category).toBe("VFR");
    const byOak = STATIONS.find(s => s.icao === "KOAK")!;
    // BKN 900 → IFR by ceiling; vis 3 → MVFR. Worst wins → IFR.
    expect(byOak.category).toBe("IFR");
    expect(byOak.ceilingFtAgl).toBe(900);
  });

  it("stamps lastUpdateMs on a successful fetch", async () => {
    stubJsonResponse([SFO_METAR]);
    const before = Date.now();
    rebuildStations([], 37.75, -122.45, 30);
    await updateAll();
    expect(lastUpdateMs()).toBeGreaterThanOrEqual(before);
  });

  it("keeps only the nearest 32 stations when the upstream returns more", async () => {
    // Synthesize 40 stations spread around the bbox center.
    const rows = Array.from({ length: 40 }, (_, i) => ({
      icaoId: `K${String(i).padStart(3, "0")}`,
      lat: 37.75 + (i - 20) * 0.05,
      lon: -122.45,
      visib: 10,
      clouds: [],
    }));
    stubJsonResponse(rows);
    rebuildStations([], 37.75, -122.45, 30);
    await updateAll();
    expect(STATIONS.length).toBe(32);
    // The nearest station (index 20, at the center) should be present.
    expect(STATIONS.some(s => s.icao === "K020")).toBe(true);
  });

  it("swallows and warns on a non-200 upstream, leaving STATIONS empty", async () => {
    const warn = vi.spyOn(console, "warn").mockImplementation(() => {});
    stubJsonResponse("upstream boom", false, 503);
    rebuildStations([], 37.75, -122.45, 30);
    await updateAll();
    expect(STATIONS.length).toBe(0);
    expect(warn).toHaveBeenCalled();
    warn.mockRestore();
  });

  it("swallows and warns on a thrown fetch (network down)", async () => {
    const warn = vi.spyOn(console, "warn").mockImplementation(() => {});
    vi.stubGlobal("fetch", vi.fn(() => Promise.reject(new Error("dns lookup"))));
    rebuildStations([], 37.75, -122.45, 30);
    await updateAll();
    expect(STATIONS.length).toBe(0);
    expect(warn).toHaveBeenCalled();
    warn.mockRestore();
  });

  it("filters out rows missing icaoId or lat/lon", async () => {
    stubJsonResponse([
      SFO_METAR,
      { icaoId: "", lat: 37.7, lon: -122.4 },
      { icaoId: "K001", lat: undefined, lon: -122.4 },
    ]);
    rebuildStations([], 37.75, -122.45, 30);
    await updateAll();
    expect(STATIONS.length).toBe(1);
    expect(STATIONS[0].icao).toBe("KSFO");
  });

  it("clamps parsed visibility to [0, 10] and treats '10+' as 10", async () => {
    stubJsonResponse([
      { icaoId: "KA", lat: 37.75, lon: -122.45, visib: "10+", clouds: [] },
      { icaoId: "KB", lat: 37.76, lon: -122.45, visib: 15, clouds: [] },
      { icaoId: "KC", lat: 37.77, lon: -122.45, visib: -3, clouds: [] },
    ]);
    rebuildStations([], 37.75, -122.45, 30);
    await updateAll();
    const byIcao = new Map(STATIONS.map(s => [s.icao, s]));
    expect(byIcao.get("KA")!.visibilitySm).toBe(10);
    expect(byIcao.get("KB")!.visibilitySm).toBe(10);
    expect(byIcao.get("KC")!.visibilitySm).toBe(0);
  });
});

describe("refreshIfStale", () => {
  it("fetches when lastUpdateMs is 0 (never fetched)", async () => {
    const fetchMock = stubJsonResponse([SFO_METAR]);
    rebuildStations([], 37.75, -122.45, 30);
    await refreshIfStale();
    expect(fetchMock).toHaveBeenCalledTimes(1);
  });

  it("skips a second call within the 5-min TTL", async () => {
    const fetchMock = stubJsonResponse([SFO_METAR]);
    rebuildStations([], 37.75, -122.45, 30);
    await refreshIfStale();  // populates STATIONS + timer
    await refreshIfStale();  // still fresh — no-op
    expect(fetchMock).toHaveBeenCalledTimes(1);
  });

  it("refetches after invalidate()", async () => {
    const fetchMock = stubJsonResponse([SFO_METAR]);
    rebuildStations([], 37.75, -122.45, 30);
    await refreshIfStale();
    invalidate();
    await refreshIfStale();
    expect(fetchMock).toHaveBeenCalledTimes(2);
  });

  it("rebuildStations invalidates the freshness timer", async () => {
    const fetchMock = stubJsonResponse([SFO_METAR]);
    rebuildStations([], 37.75, -122.45, 30);
    await refreshIfStale();
    // Move the bbox — should mark stale immediately.
    rebuildStations([], 47.4, -122.3, 30);
    await refreshIfStale();
    expect(fetchMock).toHaveBeenCalledTimes(2);
    // Second URL is the Seattle bbox, not the SF one.
    const url2 = String(fetchMock.mock.calls[1][0]);
    expect(url2).toContain("47.");
  });
});

// Round-trip: verify STATIONS holds ONLY the last-fetched center's data
// after two rebuildStations() calls with different centers — the exact
// scenario the whole "blank radar after home change" bug hit.
describe("STATIONS lifecycle", () => {
  it("wholesale-replaces after a rebuildStations + refresh with a new center", async () => {
    stubJsonResponse([SFO_METAR, OAK_METAR]);
    rebuildStations([], 37.75, -122.45, 30);
    await updateAll();
    expect(STATIONS.map(s => s.icao)).toEqual(expect.arrayContaining(["KSFO", "KOAK"]));

    vi.unstubAllGlobals();
    stubJsonResponse([SEA_METAR]);
    rebuildStations([], 47.4, -122.3, 30);
    await updateAll();
    expect(STATIONS.map(s => s.icao)).toEqual(["KSEA"]);
  });
});
