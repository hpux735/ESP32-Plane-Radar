import { afterEach, describe, expect, it, vi } from "vitest";
import { handler } from "./metar.mjs";

function ev(qs = {}) {
  return { queryStringParameters: qs };
}

afterEach(() => {
  vi.unstubAllGlobals();
});

describe("metar proxy — validation", () => {
  it("returns 400 on missing bbox", async () => {
    const r = await handler(ev({}));
    expect(r.statusCode).toBe(400);
    expect(JSON.parse(r.body).error).toMatch(/bbox=/);
  });

  it("returns 400 on wrong-arity bbox", async () => {
    const r = await handler(ev({ bbox: "37,-122" }));
    expect(r.statusCode).toBe(400);
  });

  it("returns 400 when bbox contains non-numeric parts", async () => {
    const r = await handler(ev({ bbox: "37,-122,foo,bar" }));
    expect(r.statusCode).toBe(400);
  });

  it("returns 400 when latitude span exceeds 20", async () => {
    const r = await handler(ev({ bbox: "0,-122,50,-100" }));
    expect(r.statusCode).toBe(400);
    expect(JSON.parse(r.body).error).toMatch(/too large/);
  });

  it("returns 400 when longitude span exceeds 40", async () => {
    const r = await handler(ev({ bbox: "37,-180,45,-100" }));
    expect(r.statusCode).toBe(400);
  });
});

describe("metar proxy — happy path", () => {
  it("proxies the upstream body verbatim on 200", async () => {
    const payload = JSON.stringify([{ icao: "KSFO", raw: "KSFO 010056Z ..." }]);
    const fetchMock = vi.fn(() =>
      Promise.resolve(new Response(payload, { status: 200 })),
    );
    vi.stubGlobal("fetch", fetchMock);

    const r = await handler(ev({ bbox: "37,-123,38,-122" }));
    expect(r.statusCode).toBe(200);
    expect(r.body).toBe(payload);
    expect(r.headers["Access-Control-Allow-Origin"]).toBe("*");
    expect(r.headers["Cache-Control"]).toMatch(/max-age=300/);
    expect(String(fetchMock.mock.calls[0][0])).toContain("aviationweather.gov");
  });

  it("passes the exact bbox through to the upstream URL", async () => {
    const fetchMock = vi.fn(() =>
      Promise.resolve(new Response("[]", { status: 200 })),
    );
    vi.stubGlobal("fetch", fetchMock);

    await handler(ev({ bbox: "37,-123,38,-122" }));
    expect(String(fetchMock.mock.calls[0][0])).toContain("bbox=37,-123,38,-122");
  });
});

describe("metar proxy — failure paths", () => {
  it("returns 502 with the upstream code embedded in the message on non-OK", async () => {
    vi.stubGlobal("fetch", vi.fn(() =>
      Promise.resolve(new Response("boom", { status: 503 })),
    ));

    const r = await handler(ev({ bbox: "37,-123,38,-122" }));
    expect(r.statusCode).toBe(502);
    expect(JSON.parse(r.body).error).toMatch(/upstream 503/);
  });

  it("returns 502 with a fetch-failed error when the request throws", async () => {
    vi.stubGlobal("fetch", vi.fn(() => Promise.reject(new Error("dns lookup"))));

    const r = await handler(ev({ bbox: "37,-123,38,-122" }));
    expect(r.statusCode).toBe(502);
    expect(JSON.parse(r.body).error).toMatch(/fetch failed/);
  });
});
