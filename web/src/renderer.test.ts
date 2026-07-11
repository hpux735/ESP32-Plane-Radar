import { beforeEach, describe, expect, it } from "vitest";
import { renderFrame } from "./renderer";
import { makeCanvasSpy } from "./testCanvas";
import { COLORS, SIZE } from "./theme";
import type { Tile } from "./tile";
import { state } from "./state";
import { clearAircraft } from "./aircraft";

// These tests observe drawing COMMANDS, not pixels. See
// src/testCanvas.ts for the shim decision + tradeoffs.

const EMPTY_TILE: Tile = {
  z: 7, x: 20, y: 37,
  coast: [],
  land: [],
  water: [],
  airports: [],
};

const SF_TILE: Tile = {
  z: 7, x: 20, y: 37,
  coast: [[[-122.5, 37.7], [-122.4, 37.8]]],
  land: [[[-122.6, 37.6], [-122.3, 37.6], [-122.3, 37.9], [-122.6, 37.9], [-122.6, 37.6]]],
  water: [],
  airports: [
    {
      ident: "KSFO", lat: 37.6188, lon: -122.3750, tier: 3, instrumentApproach: true,
      runways: [{ lat1: 37.6, lon1: -122.4, lat2: 37.6, lon2: -122.38 }],
    },
  ],
};

beforeEach(() => {
  state.centerLat = 37.7552;
  state.centerLon = -122.4528;
  state.rangeIdx = 1;
  state.layers = { coast: true, land: true, runways: true, tags: true };
  clearAircraft();
});

describe("renderFrame — always-on layers", () => {
  it("fills the whole 240×240 canvas with the background color", () => {
    const ctx = makeCanvasSpy();
    renderFrame(ctx, [EMPTY_TILE]);
    const fills = ctx.callsOf("fillRect");
    expect(fills.some(c =>
      c.args[0] === 0 && c.args[1] === 0 && c.args[2] === SIZE && c.args[3] === SIZE,
    )).toBe(true);
    expect(ctx.hasFill(COLORS.background)).toBe(true);
  });

  it("draws the four grid rings + crosshairs", () => {
    const ctx = makeCanvasSpy();
    renderFrame(ctx, [EMPTY_TILE]);
    // Four arcs (grid rings). arc() is also called for clip + bezel — so
    // check it's called at least 4+ times.
    expect(ctx.countOf("arc")).toBeGreaterThanOrEqual(4);
    expect(ctx.hasStroke(COLORS.grid)).toBe(true);
  });

  it("draws a range-preset scale label somewhere", () => {
    const ctx = makeCanvasSpy();
    renderFrame(ctx, [EMPTY_TILE]);
    // Default rangeIdx=1 → 10nm label.
    expect(ctx.hasText("10nm")).toBe(true);
  });

  it("paints the bezel mask so corner pixels revert to background", () => {
    const ctx = makeCanvasSpy();
    renderFrame(ctx, [EMPTY_TILE]);
    // The bezel path draws rect(0,0,SIZE,SIZE) then evenodd-fills with
    // an inverse arc — so we should see BOTH a rect() call AND a fill.
    expect(ctx.countOf("rect")).toBeGreaterThan(0);
    expect(ctx.countOf("fill")).toBeGreaterThan(0);
  });
});

describe("renderFrame — layer toggles", () => {
  it("draws runway strokes when tiles have airports and runways layer is on", () => {
    const ctx = makeCanvasSpy();
    renderFrame(ctx, [SF_TILE]);
    expect(ctx.hasStroke(COLORS.runway)).toBe(true);
    expect(ctx.hasText("KSFO")).toBe(true);
  });

  it("skips runway drawing when the runways layer is off", () => {
    state.layers.runways = false;
    const ctx = makeCanvasSpy();
    renderFrame(ctx, [SF_TILE]);
    expect(ctx.hasStroke(COLORS.runway)).toBe(false);
    expect(ctx.hasText("KSFO")).toBe(false);
  });

  it("draws land polygon fills when land layer is on and tile has land", () => {
    const ctx = makeCanvasSpy();
    renderFrame(ctx, [SF_TILE]);
    expect(ctx.hasFill(COLORS.land)).toBe(true);
  });

  it("skips land drawing when the land layer is off", () => {
    state.layers.land = false;
    const ctx = makeCanvasSpy();
    renderFrame(ctx, [SF_TILE]);
    expect(ctx.hasFill(COLORS.land)).toBe(false);
  });

  it("draws coastline strokes when coast layer is on and tile has coast", () => {
    const ctx = makeCanvasSpy();
    renderFrame(ctx, [SF_TILE]);
    expect(ctx.hasStroke(COLORS.coastline)).toBe(true);
  });

  it("skips coast drawing when the coast layer is off", () => {
    state.layers.coast = false;
    const ctx = makeCanvasSpy();
    renderFrame(ctx, [SF_TILE]);
    expect(ctx.hasStroke(COLORS.coastline)).toBe(false);
  });
});

describe("renderFrame — empty tile set", () => {
  it("still paints background + rings + scale label with zero tiles", () => {
    const ctx = makeCanvasSpy();
    renderFrame(ctx, []);
    expect(ctx.hasFill(COLORS.background)).toBe(true);
    expect(ctx.hasStroke(COLORS.grid)).toBe(true);
    expect(ctx.hasText("10nm")).toBe(true);
  });
});
