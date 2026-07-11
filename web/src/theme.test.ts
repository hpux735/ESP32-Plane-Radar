import { describe, expect, it } from "vitest";
import {
  CENTER_X,
  CENTER_Y,
  COLORS,
  GRID_OUTER_RADIUS,
  KM_PER_DEG,
  PHYSICAL_PANEL_RADIUS,
  RANGE_PRESETS,
  SIZE,
  TRACK_HORIZON_SEC,
  TRACK_LENGTH_SCALE,
  TRACK_MIN_PX,
  TRACK_REF_OUTER_KM,
  WX_COLORS,
} from "./theme";

// Smoke tests for constants. Cheap on their own — they exist because
// the firmware and the web preview both consume these numbers, so a
// typo (a stray digit in an RGB, a range preset that goes downhill)
// would show up as "web doesn't look like the hardware" and be hard
// to trace.

describe("panel geometry", () => {
  it("centers the grid inside the 240×240 canvas", () => {
    expect(SIZE).toBe(240);
    expect(CENTER_X).toBe(SIZE / 2);
    expect(CENTER_Y).toBe(SIZE / 2);
  });

  it("keeps the grid inside the panel radius", () => {
    expect(GRID_OUTER_RADIUS).toBeLessThan(PHYSICAL_PANEL_RADIUS);
  });

  it("uses the standard km-per-degree constant", () => {
    // Rough spherical-Earth approximation — 111 km/deg latitude.
    expect(KM_PER_DEG).toBeGreaterThan(110);
    expect(KM_PER_DEG).toBeLessThan(112);
  });
});

describe("range presets", () => {
  it("has at least three presets", () => {
    expect(RANGE_PRESETS.length).toBeGreaterThanOrEqual(3);
  });

  it("is monotonically increasing in both nm and outerKm", () => {
    for (let i = 1; i < RANGE_PRESETS.length; i++) {
      expect(RANGE_PRESETS[i].nm).toBeGreaterThan(RANGE_PRESETS[i - 1].nm);
      expect(RANGE_PRESETS[i].outerKm).toBeGreaterThan(RANGE_PRESETS[i - 1].outerKm);
    }
  });

  it("each preset's km value matches its nm value at 1.852 km/nm within 1%", () => {
    for (const p of RANGE_PRESETS) {
      const expectedKm = p.nm * 1.852;
      expect(Math.abs(p.outerKm - expectedKm) / expectedKm).toBeLessThan(0.01);
    }
  });
});

describe("radar color palette", () => {
  const rgb = /^rgb\((\d{1,3}),\s*(\d{1,3}),\s*(\d{1,3})\)$/;

  for (const [name, value] of Object.entries(COLORS)) {
    it(`${name} parses as an RGB triple with 0-255 channels`, () => {
      const m = value.match(rgb);
      expect(m, `expected ${name}='${value}' to be rgb(...)`).not.toBeNull();
      const [r, g, b] = m!.slice(1).map(Number);
      for (const ch of [r, g, b]) {
        expect(ch).toBeGreaterThanOrEqual(0);
        expect(ch).toBeLessThanOrEqual(255);
      }
    });
  }
});

describe("weather color palette", () => {
  const rgb = /^rgb\((\d{1,3}),\s*(\d{1,3}),\s*(\d{1,3})\)$/;

  for (const [name, value] of Object.entries(WX_COLORS)) {
    it(`${name} parses as an RGB triple`, () => {
      expect(value).toMatch(rgb);
    });
  }

  it("has entries for each METAR flight category", () => {
    for (const key of ["VFR", "MVFR", "IFR", "LIFR", "Unknown"]) {
      expect(WX_COLORS).toHaveProperty(key);
    }
  });
});

describe("track vector constants", () => {
  it("uses a 60-second horizon and positive scale + minimum length", () => {
    expect(TRACK_HORIZON_SEC).toBe(60);
    expect(TRACK_REF_OUTER_KM).toBeGreaterThan(0);
    expect(TRACK_LENGTH_SCALE).toBeGreaterThan(0);
    expect(TRACK_LENGTH_SCALE).toBeLessThan(1);
    expect(TRACK_MIN_PX).toBeGreaterThan(0);
  });
});
