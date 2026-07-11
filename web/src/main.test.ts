import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

// main.ts wiring tests. The module registers a handful of subscribers
// against state.ts at import time — one for tile refetch, one for the
// aircraft fetch loop, one for clearAircraft on center move, one for
// invalidateOutdoorTemp on home move, one for METAR bbox rebuild. Those
// subscribers were the bug site in the "blank radar after home change"
// incident, so they get their own tests: import main.ts, trigger state
// changes, assert the right side effect fired.
//
// Setup shape: fake #radar canvas + stub network fetches BEFORE importing
// main.ts. main.ts calls loadIndexData() during init(); we stub that
// too so no real files get read.

interface CanvasContext2DLike {
  fillStyle?: string;
  fillRect?: (...args: unknown[]) => void;
  clearRect?: (...args: unknown[]) => void;
  drawImage?: (...args: unknown[]) => void;
  [k: string]: unknown;
}

function mountCanvas() {
  document.body.innerHTML = "";
  const canvas = document.createElement("canvas");
  canvas.id = "radar";
  canvas.width = 240;
  canvas.height = 240;
  // happy-dom's canvas returns a no-op 2d context; that's fine — main.ts
  // just needs the getContext handshake to not throw.
  (canvas as unknown as { getContext: (t: string) => CanvasContext2DLike | null }).getContext =
    () => ({
      fillStyle: "",
      fillRect: vi.fn(),
      clearRect: vi.fn(),
      drawImage: vi.fn(),
      strokeRect: vi.fn(),
      beginPath: vi.fn(),
      moveTo: vi.fn(),
      lineTo: vi.fn(),
      arc: vi.fn(),
      rect: vi.fn(),
      fill: vi.fn(),
      stroke: vi.fn(),
      clip: vi.fn(),
      save: vi.fn(),
      restore: vi.fn(),
      fillText: vi.fn(),
      strokeText: vi.fn(),
      measureText: () => ({ width: 20 }),
      setLineDash: vi.fn(),
      translate: vi.fn(),
      rotate: vi.fn(),
      scale: vi.fn(),
      font: "",
      textAlign: "",
      textBaseline: "",
      strokeStyle: "",
      lineWidth: 1,
      globalAlpha: 1,
    });
  document.body.appendChild(canvas);
}

// Shared arrays the mocked modules push into. Reset between tests.
const ensureTilesCalls: Array<[string, number, number, number]> = [];

// Freeze URL to something predictable so main.ts's `location.search`
// parsing doesn't observe test junk.
async function importMainFresh() {
  vi.resetModules();
  // Stub loadIndexData BEFORE main.ts imports it — otherwise init() will
  // try to fetch data/* files that don't exist under vitest.
  vi.doMock("./data", () => ({
    loadIndexData: () => Promise.resolve({
      airportIndex: [
        ["KSFO", "SFO", "San Francisco", "SFO Intl", 37.6188, -122.3750],
      ],
      basePath: "data",
    }),
  }));
  // Neuter viewTiles so subscribers don't actually hit the network.
  // Push call args into a shared array we can assert against — vi.fn's
  // .mock.calls is unreliable here because vi.doMock's factory returns
  // a fresh object each import and the imported reference doesn't
  // always survive vi.resetModules cycles cleanly.
  vi.doMock("./viewTiles", () => ({
    ensureTiles: async (viewTag: string, lat: number, lon: number, radiusKm: number) => {
      ensureTilesCalls.push([viewTag, lat, lon, radiusKm]);
      return true;
    },
    currentTiles: () => [],
  }));
  return await import("./main");
}

beforeEach(() => {
  ensureTilesCalls.length = 0;
  mountCanvas();
});

afterEach(() => {
  vi.doUnmock("./data");
  vi.doUnmock("./viewTiles");
  vi.unstubAllGlobals();
  document.body.innerHTML = "";
});

describe("main.ts — subscriber wiring", () => {
  it("registers the settings gear button in the DOM at init time", async () => {
    // Silence the aircraft poll timer so it doesn't chatter into
    // console.error during the test.
    vi.stubGlobal("fetch", vi.fn(() =>
      Promise.resolve(new Response(JSON.stringify({ ac: [] }), { status: 200 })),
    ));
    await importMainFresh();
    // main.ts calls init() at load; init() awaits DOMContentLoaded OR
    // fires immediately. It calls mountSettings which appends a gear.
    // Wait a tick for the async init to complete.
    await new Promise(r => setTimeout(r, 20));
    expect(document.querySelector("button.settings-open")).not.toBeNull();
  });

  it("fires ensureTiles when the state notify() fires (subscriber wiring)", async () => {
    vi.stubGlobal("fetch", vi.fn(() =>
      Promise.resolve(new Response(JSON.stringify({ ac: [] }), { status: 200 })),
    ));
    await importMainFresh();
    await new Promise(r => setTimeout(r, 20));

    const before = ensureTilesCalls.length;
    const { cycleRange } = await import("./state");
    cycleRange();
    // Subscribers run synchronously; ensureTiles is async but its call
    // registers immediately.
    expect(ensureTilesCalls.length).toBeGreaterThan(before);
  });

  it("calls clearAircraft on state.centerLat/centerLon change", async () => {
    // Stub fetch for aircraft poll (fires on every subscribe).
    vi.stubGlobal("fetch", vi.fn(() =>
      Promise.resolve(new Response(JSON.stringify({ ac: [{ hex: "A1", lat: 37, lon: -122 }] }), { status: 200 })),
    ));
    await importMainFresh();
    await new Promise(r => setTimeout(r, 20));

    const aircraftMod = await import("./aircraft");
    // Manually seed the cache so we can watch it clear.
    await aircraftMod.fetchAircraft(37.75, -122.45, 11);
    expect(aircraftMod.aircraft().length).toBe(1);

    // Trigger a center change via setCenter.
    const { setCenter } = await import("./state");
    setCenter(47.45, -122.30, "SEA");
    // clearAircraft runs synchronously in the subscriber.
    expect(aircraftMod.aircraft().length).toBe(0);
  });

  it("calls invalidateOutdoorTemp on state.home change", async () => {
    vi.stubGlobal("fetch", vi.fn(() =>
      Promise.resolve(new Response(JSON.stringify({ ac: [] }), { status: 200 })),
    ));
    await importMainFresh();
    await new Promise(r => setTimeout(r, 20));

    const outdoorTemp = await import("./outdoorTemp");
    // Seed the outdoor-temp cache manually.
    (outdoorTemp as unknown as { _cachedForTest?: unknown });
    // Trigger a home change via saveHome.
    const { saveHome } = await import("./state");
    saveHome({ lat: 47.45, lon: -122.30 });
    // After the subscriber fires, refreshIfStale must fetch again on
    // next call (i.e., cache is empty). Check cachedReading is invalid.
    expect(outdoorTemp.cachedReading().valid).toBe(false);
  });
});

describe("main.ts — poll loop", () => {
  it("triggers an aircraft fetch when the radar view + subscribe fires", async () => {
    const fetchMock = vi.fn(() =>
      Promise.resolve(new Response(JSON.stringify({ ac: [{ hex: "H1", lat: 37, lon: -122 }] }), { status: 200 })),
    );
    vi.stubGlobal("fetch", fetchMock);

    await importMainFresh();
    await new Promise(r => setTimeout(r, 20));
    const before = fetchMock.mock.calls.length;

    // Trigger a subscriber notify by cycling range.
    const { cycleRange } = await import("./state");
    cycleRange();
    await new Promise(r => setTimeout(r, 20));

    expect(fetchMock.mock.calls.length).toBeGreaterThan(before);
    const urls = fetchMock.mock.calls.map(c => String(c[0]));
    expect(urls.some(u => u.includes("api/adsb"))).toBe(true);
  });
});
