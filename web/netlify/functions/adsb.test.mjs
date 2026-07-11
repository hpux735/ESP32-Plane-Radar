import { afterEach, describe, expect, it, vi } from "vitest";
import { handler } from "./adsb.mjs";

// The proxy has two-tier retry: airplanes.live first, opendata.adsb.fi
// fallback. These tests lock the primary/fallback behavior so a silent
// API change won't just blank the client.

function ev(qs = {}) {
  return { queryStringParameters: qs };
}

afterEach(() => {
  vi.unstubAllGlobals();
});

describe("adsb proxy — validation", () => {
  it("returns 400 when lat/lon/nm are missing", async () => {
    const r = await handler(ev({}));
    expect(r.statusCode).toBe(400);
    expect(JSON.parse(r.body).error).toMatch(/required numeric/);
  });

  it("returns 400 when nm is out of range (>250)", async () => {
    const r = await handler(ev({ lat: "37", lon: "-122", nm: "500" }));
    expect(r.statusCode).toBe(400);
    expect(JSON.parse(r.body).error).toMatch(/out of range/);
  });

  it("returns 400 when nm is zero or negative", async () => {
    const r = await handler(ev({ lat: "37", lon: "-122", nm: "-5" }));
    expect(r.statusCode).toBe(400);
  });
});

describe("adsb proxy — happy path", () => {
  it("returns the primary upstream body verbatim on 200", async () => {
    const payload = JSON.stringify({ ac: [{ hex: "AAA", lat: 37, lon: -122 }] });
    const fetchMock = vi.fn(() =>
      Promise.resolve(new Response(payload, { status: 200 })),
    );
    vi.stubGlobal("fetch", fetchMock);

    const r = await handler(ev({ lat: "37.75", lon: "-122.45", nm: "10" }));
    expect(r.statusCode).toBe(200);
    expect(r.body).toBe(payload);
    expect(r.headers["Access-Control-Allow-Origin"]).toBe("*");
    expect(fetchMock).toHaveBeenCalledTimes(1);
    // Primary is airplanes.live.
    expect(String(fetchMock.mock.calls[0][0])).toContain("airplanes.live");
  });
});

describe("adsb proxy — fallback logic", () => {
  it("falls back to adsb.fi when the primary returns 5xx", async () => {
    const primaryFail = new Response("upstream fail", { status: 503 });
    const fallbackOk = new Response(JSON.stringify({ ac: [] }), { status: 200 });
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(primaryFail)
      .mockResolvedValueOnce(fallbackOk);
    vi.stubGlobal("fetch", fetchMock);

    const r = await handler(ev({ lat: "37.75", lon: "-122.45", nm: "10" }));
    expect(r.statusCode).toBe(200);
    expect(fetchMock).toHaveBeenCalledTimes(2);
    expect(String(fetchMock.mock.calls[1][0])).toContain("adsb.fi");
  });

  it("falls back to adsb.fi when the primary throws (network error)", async () => {
    const fetchMock = vi.fn()
      .mockRejectedValueOnce(new Error("connect refused"))
      .mockResolvedValueOnce(new Response(JSON.stringify({ ac: [] }), { status: 200 }));
    vi.stubGlobal("fetch", fetchMock);

    const r = await handler(ev({ lat: "37.75", lon: "-122.45", nm: "10" }));
    expect(r.statusCode).toBe(200);
    expect(fetchMock).toHaveBeenCalledTimes(2);
  });

  it("returns 502 when both upstreams fail", async () => {
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(new Response("", { status: 500 }))
      .mockResolvedValueOnce(new Response("", { status: 502 }));
    vi.stubGlobal("fetch", fetchMock);

    const r = await handler(ev({ lat: "37.75", lon: "-122.45", nm: "10" }));
    expect(r.statusCode).toBe(502);
    expect(JSON.parse(r.body).error).toMatch(/upstream 502/);
  });

  it("returns 502 with sentinel 599 when both upstreams throw", async () => {
    const fetchMock = vi.fn()
      .mockRejectedValueOnce(new Error("down"))
      .mockRejectedValueOnce(new Error("down"));
    vi.stubGlobal("fetch", fetchMock);

    const r = await handler(ev({ lat: "37.75", lon: "-122.45", nm: "10" }));
    expect(r.statusCode).toBe(502);
    expect(JSON.parse(r.body).error).toMatch(/upstream 599/);
  });
});

describe("adsb proxy — URL construction", () => {
  it("formats lat/lon to 4 decimals and nm to 1 decimal in upstream URL", async () => {
    const fetchMock = vi.fn(() =>
      Promise.resolve(new Response("{}", { status: 200 })),
    );
    vi.stubGlobal("fetch", fetchMock);

    await handler(ev({ lat: "37.755234", lon: "-122.452891", nm: "10" }));
    const url = String(fetchMock.mock.calls[0][0]);
    expect(url).toContain("/37.7552/");
    expect(url).toContain("/-122.4529/");
    expect(url).toContain("/10.0");
  });
});
