import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import {
  aircraft,
  clearAircraft,
  fetchAircraft,
  fetchCount,
  lastError,
} from "./aircraft";

// These tests guard the fetch-supersede race that caused the "no
// airplanes after home change" bug: two fetches for different centers
// running concurrently must not clobber each other, and clearAircraft()
// must drop the cached list so old-center planes don't linger.

interface DeferredResponse {
  resolve: (r: Response) => void;
  reject: (err: Error) => void;
}

function deferredFetch(): {
  fetchMock: ReturnType<typeof vi.fn>;
  pending: DeferredResponse[];
} {
  const pending: DeferredResponse[] = [];
  const fetchMock = vi.fn(() =>
    new Promise<Response>((resolve, reject) => {
      pending.push({ resolve, reject });
    }),
  );
  vi.stubGlobal("fetch", fetchMock);
  return { fetchMock, pending };
}

function adsbResponse(planes: Array<{ hex: string; lat: number; lon: number }>): Response {
  return new Response(JSON.stringify({ ac: planes }), { status: 200 });
}

beforeEach(() => {
  // Reset module state — each test starts with an empty cache.
  clearAircraft();
});

afterEach(() => {
  vi.unstubAllGlobals();
});

describe("fetchAircraft — generation-counter supersede", () => {
  it("keeps only the LAST-STARTED fetch's payload when resolved out of order", async () => {
    const { pending } = deferredFetch();

    // Kick off two overlapping fetches, second one for a different center.
    const p1 = fetchAircraft(37.75, -122.45, 11);   // Sutro
    const p2 = fetchAircraft(16.90, 96.13, 11);     // Yangon

    // Resolve OUT OF ORDER — Sutro (first) arrives after Yangon (second).
    pending[1].resolve(adsbResponse([{ hex: "YGN1", lat: 16.9, lon: 96.1 }]));
    await p2;

    pending[0].resolve(adsbResponse([{ hex: "SUT1", lat: 37.7, lon: -122.4 }]));
    await p1;

    // The stale Sutro response must NOT overwrite the fresh Yangon list.
    const list = aircraft();
    expect(list.length).toBe(1);
    expect(list[0].hex).toBe("YGN1");
  });

  it("does not increment fetchCount for a superseded fetch", async () => {
    const { pending } = deferredFetch();
    const startCount = fetchCount();

    const p1 = fetchAircraft(37.75, -122.45, 11);
    const p2 = fetchAircraft(16.90, 96.13, 11);

    pending[1].resolve(adsbResponse([{ hex: "YGN1", lat: 16.9, lon: 96.1 }]));
    await p2;
    pending[0].resolve(adsbResponse([{ hex: "SUT1", lat: 37.7, lon: -122.4 }]));
    await p1;

    // Only the winning (second-started) fetch bumps the count.
    expect(fetchCount()).toBe(startCount + 1);
  });
});

describe("fetchAircraft — error handling", () => {
  it("preserves the last-good list when a subsequent fetch returns non-OK", async () => {
    vi.stubGlobal("fetch", vi.fn(() =>
      Promise.resolve(adsbResponse([{ hex: "OK1", lat: 37.7, lon: -122.4 }])),
    ));
    await fetchAircraft(37.75, -122.45, 11);
    expect(aircraft().length).toBe(1);

    // Second call errors out.
    vi.unstubAllGlobals();
    vi.stubGlobal("fetch", vi.fn(() =>
      Promise.resolve(new Response("boom", { status: 500 })),
    ));
    await fetchAircraft(37.75, -122.45, 11);

    // Old list is retained so the UI doesn't blank on a transient error.
    expect(aircraft().length).toBe(1);
    expect(aircraft()[0].hex).toBe("OK1");
    expect(lastError()).toMatch(/HTTP 500/);
  });

  it("preserves the last-good list on a thrown exception", async () => {
    vi.stubGlobal("fetch", vi.fn(() =>
      Promise.resolve(adsbResponse([{ hex: "OK1", lat: 37.7, lon: -122.4 }])),
    ));
    await fetchAircraft(37.75, -122.45, 11);
    expect(aircraft().length).toBe(1);

    vi.unstubAllGlobals();
    vi.stubGlobal("fetch", vi.fn(() => Promise.reject(new Error("net down"))));
    await fetchAircraft(37.75, -122.45, 11);

    expect(aircraft().length).toBe(1);
    expect(lastError()).toMatch(/net down/);
  });
});

describe("clearAircraft", () => {
  it("empties the cached list", async () => {
    vi.stubGlobal("fetch", vi.fn(() =>
      Promise.resolve(adsbResponse([{ hex: "OK1", lat: 37.7, lon: -122.4 }])),
    ));
    await fetchAircraft(37.75, -122.45, 11);
    expect(aircraft().length).toBe(1);
    clearAircraft();
    expect(aircraft().length).toBe(0);
    expect(lastError()).toBeNull();
  });

  it("causes an in-flight fetch to discard its result", async () => {
    const { pending } = deferredFetch();

    const p = fetchAircraft(37.75, -122.45, 11);
    clearAircraft();
    // The in-flight fetch's gen no longer matches — its response must not
    // resurrect the (now cleared) aircraft list.
    pending[0].resolve(adsbResponse([{ hex: "STALE", lat: 37.7, lon: -122.4 }]));
    await p;

    expect(aircraft().length).toBe(0);
  });
});

describe("URL construction", () => {
  it("includes lat/lon/nm in the /api/adsb query", async () => {
    const fetchMock = vi.fn((_input: RequestInfo | URL) =>
      Promise.resolve(adsbResponse([])),
    );
    vi.stubGlobal("fetch", fetchMock);
    await fetchAircraft(16.9073, 96.1332, 11);
    const url = String(fetchMock.mock.calls[0]?.[0] ?? "");
    expect(url).toMatch(/api\/adsb/);
    expect(url).toContain("lat=16.9073");
    expect(url).toContain("lon=96.1332");
    expect(url).toContain("nm=11.0");
  });
});
