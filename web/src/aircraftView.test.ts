import { beforeEach, describe, expect, it } from "vitest";
import { drawAircraft } from "./aircraftView";
import { makeCanvasSpy } from "./testCanvas";
import { makeView } from "./projection";
import { COLORS } from "./theme";
import { state } from "./state";
import type { Aircraft } from "./aircraft";

// See src/testCanvas.ts for the shim decision. These tests observe the
// draw command stream — regressions like "emergency aircraft stopped
// being red" or "tags disappeared" will show up here.

function plane(overrides: Partial<Aircraft> = {}): Aircraft {
  return {
    hex: "AAA001",
    callsign: "UAL1234",
    type: "B738",
    lat: 37.7,
    lon: -122.4,
    altFt: 15_000,
    gsKnots: 350,
    trackDeg: 90,
    noseDeg: 90,
    vsFpm: 0,
    squawk: 1200,
    ...overrides,
  };
}

const SF_VIEW = () => makeView(37.7552, -122.4528, 18.52);  // 10nm outer

beforeEach(() => {
  state.rangeIdx = 1;
});

describe("drawAircraft — filtering", () => {
  it("skips on-ground aircraft entirely (no icon, no tag)", () => {
    const ctx = makeCanvasSpy();
    // altFt = null → on-ground per isOnGround().
    drawAircraft(ctx, SF_VIEW(), [plane({ altFt: null })], true, 1, Date.now());
    // Nothing filled with the aircraft (blue) color.
    expect(ctx.hasFill(COLORS.aircraft)).toBe(false);
    expect(ctx.hasText("UAL1234")).toBe(false);
  });

  it("skips aircraft moving under 40kt below 100ft (still on-ground)", () => {
    const ctx = makeCanvasSpy();
    drawAircraft(ctx, SF_VIEW(),
      [plane({ altFt: 50, gsKnots: 20 })], true, 1, Date.now());
    expect(ctx.hasFill(COLORS.aircraft)).toBe(false);
  });

  it("shows normal in-air aircraft with the standard aircraft color", () => {
    const ctx = makeCanvasSpy();
    drawAircraft(ctx, SF_VIEW(), [plane()], true, 1, Date.now());
    expect(ctx.hasFill(COLORS.aircraft)).toBe(true);
  });
});

describe("drawAircraft — emergency handling", () => {
  it("paints 7500/7600/7700 squawks in emergency red", () => {
    for (const sq of [7500, 7600, 7700]) {
      const ctx = makeCanvasSpy();
      drawAircraft(ctx, SF_VIEW(),
        [plane({ squawk: sq })], true, 1, Date.now());
      expect(ctx.hasFill(COLORS.emergency)).toBe(true);
      expect(ctx.hasText("EM")).toBe(true);
    }
  });

  it("does not mark ordinary VFR squawks as emergency", () => {
    const ctx = makeCanvasSpy();
    drawAircraft(ctx, SF_VIEW(),
      [plane({ squawk: 1200 })], true, 1, Date.now());
    expect(ctx.hasText("EM")).toBe(false);
  });
});

describe("drawAircraft — beyond-ring dots", () => {
  it("draws a dot on the ring edge for aircraft outside the disc but within 2x range", () => {
    // 10nm ≈ 18.5 km. Plane at ~30 km NNE — outside the ring, well
    // inside the "beyond ring" 2x window.
    const ctx = makeCanvasSpy();
    drawAircraft(ctx, SF_VIEW(),
      [plane({ lat: 38.02, lon: -122.4 })],
      true, 1, Date.now());
    // Beyond-ring dot = a small arc + fill; no callsign text.
    expect(ctx.hasText("UAL1234")).toBe(false);
    expect(ctx.countOf("arc")).toBeGreaterThan(0);
  });
});

describe("drawAircraft — tags", () => {
  it("draws callsign text when showTags is true and aircraft is inside the disc", () => {
    const ctx = makeCanvasSpy();
    drawAircraft(ctx, SF_VIEW(), [plane()], true, 1, Date.now());
    expect(ctx.hasText("UAL1234")).toBe(true);
  });

  it("omits callsign text when showTags is false", () => {
    const ctx = makeCanvasSpy();
    drawAircraft(ctx, SF_VIEW(), [plane()], false, 1, Date.now());
    expect(ctx.hasText("UAL1234")).toBe(false);
  });

  it("respects the range-preset tag budget (5nm: 20 tags max)", () => {
    // rangeIdx 0 = 5nm, budget 20. Feed 30 planes; expect ≤20 tags.
    state.rangeIdx = 0;
    const view = makeView(37.7552, -122.4528, 9.26);
    const planes = Array.from({ length: 30 }, (_, i) =>
      plane({
        hex: `H${i}`, callsign: `F${i}`,
        lat: 37.7552 + (i % 5) * 0.005,
        lon: -122.4528 + Math.floor(i / 5) * 0.005,
      }));
    const ctx = makeCanvasSpy();
    drawAircraft(ctx, view, planes, true, 1, Date.now());
    // Callsign fillText calls == number of tagged aircraft. Each tag
    // makes 2 fillText calls (callsign + line 2). So callsigns ≤ 20.
    const uniqueCallsigns = new Set(
      ctx.callsOf("fillText").map(c => c.args[0]).filter(t => typeof t === "string" && /^F\d+$/.test(t as string)),
    );
    expect(uniqueCallsigns.size).toBeLessThanOrEqual(20);
  });
});
