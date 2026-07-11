import { beforeEach, describe, expect, it } from "vitest";
import { drawWeatherView } from "./weatherView";
import { STATIONS } from "./weather";
import { makeCanvasSpy } from "./testCanvas";
import { COLORS, SIZE, WX_COLORS } from "./theme";
import { state } from "./state";

// Drives the weather view against a hand-populated STATIONS list.
// STATIONS is a live-mutable exported array (see weather.ts) so tests
// can push stations directly instead of round-tripping through fetch.

beforeEach(() => {
  STATIONS.length = 0;
  state.metar = { centerLat: 37.7, centerLon: -122.4, radiusNm: 30 };
});

describe("drawWeatherView — always-on layers", () => {
  it("fills the whole 240×240 background", () => {
    const ctx = makeCanvasSpy();
    drawWeatherView(ctx, []);
    const bg = ctx.callsOf("fillRect").filter(c =>
      c.args[0] === 0 && c.args[1] === 0 && c.args[2] === SIZE && c.args[3] === SIZE,
    );
    expect(bg.length).toBeGreaterThan(0);
    expect(ctx.hasFill(COLORS.background)).toBe(true);
  });

  it("draws a 'no data' freshness label when lastUpdateMs is 0", () => {
    const ctx = makeCanvasSpy();
    drawWeatherView(ctx, []);
    expect(ctx.hasText("no data")).toBe(true);
  });

  it("applies the bezel mask (rect+inverse-arc fill)", () => {
    const ctx = makeCanvasSpy();
    drawWeatherView(ctx, []);
    expect(ctx.countOf("rect")).toBeGreaterThan(0);
  });
});

describe("drawWeatherView — station rendering", () => {
  it("draws a dot in the category color for each in-view station", () => {
    STATIONS.push(
      { icao: "KSFO", lat: 37.62, lon: -122.375, category: "VFR",
        ceilingFtAgl: Infinity, visibilitySm: 10, fetchedAtMs: 0 },
      { icao: "KOAK", lat: 37.72, lon: -122.22, category: "IFR",
        ceilingFtAgl: 600, visibilitySm: 2, fetchedAtMs: 0 },
    );
    const ctx = makeCanvasSpy();
    drawWeatherView(ctx, []);
    // Each station is painted with WX_COLORS[category].
    expect(ctx.hasFill(WX_COLORS.VFR)).toBe(true);
    expect(ctx.hasFill(WX_COLORS.IFR)).toBe(true);
    // Displayed labels drop the leading "K".
    expect(ctx.hasText("SFO")).toBe(true);
    expect(ctx.hasText("OAK")).toBe(true);
  });

  it("filters out stations projected outside the visible disc", () => {
    STATIONS.push(
      // Nagasaki is nowhere near SF Bay — projects far outside disc.
      { icao: "RJFU", lat: 32.9, lon: 129.9, category: "MVFR",
        ceilingFtAgl: 1500, visibilitySm: 5, fetchedAtMs: 0 },
    );
    const ctx = makeCanvasSpy();
    drawWeatherView(ctx, []);
    expect(ctx.hasText("RJFU")).toBe(false);
  });

  it("handles non-K prefixed ICAOs verbatim (no leading char stripped)", () => {
    STATIONS.push(
      { icao: "EGLL", lat: 37.7, lon: -122.4, category: "VFR",
        ceilingFtAgl: Infinity, visibilitySm: 10, fetchedAtMs: 0 },
    );
    // EGLL projects to the center — should render.
    const ctx = makeCanvasSpy();
    drawWeatherView(ctx, []);
    expect(ctx.hasText("EGLL")).toBe(true);
  });
});
