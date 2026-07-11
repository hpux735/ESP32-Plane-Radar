import { describe, expect, it } from "vitest";
import { drawCockpitView } from "./cockpitView";
import { makeCanvasSpy } from "./testCanvas";
import { PHYSICAL_PANEL_RADIUS, SIZE } from "./theme";

// The cockpit view reads the outdoor-temp cache directly. When the
// cache is empty (as in a fresh test), it draws the "no data" fallback
// labels. That's fine — the tests here observe structure, not values.

describe("drawCockpitView", () => {
  it("fills the whole background rectangle", () => {
    const ctx = makeCanvasSpy();
    drawCockpitView(ctx);
    const bg = ctx.callsOf("fillRect").filter(c =>
      c.args[0] === 0 && c.args[1] === 0 && c.args[2] === SIZE && c.args[3] === SIZE,
    );
    expect(bg.length).toBeGreaterThan(0);
  });

  it("draws 60 rim tick marks around the clock face", () => {
    const ctx = makeCanvasSpy();
    drawCockpitView(ctx);
    // Each tick = one moveTo + one lineTo on the strokes path. 60 ticks
    // plus a handful of other lines (second sweep, arrow legs, baro
    // frame). Expect at least 60 moveTos.
    expect(ctx.countOf("moveTo")).toBeGreaterThanOrEqual(60);
  });

  it("renders a 24-hour HH:MM local clock string (never AM/PM, never '--:--')", () => {
    const ctx = makeCanvasSpy();
    drawCockpitView(ctx);
    const textCalls = ctx.callsOf("fillText").map(c => String(c.args[0]));
    // Big local time is now just HH:MM — the "L" suffix is drawn as a
    // separate small fillText call. Hour must be 00-23 (24h clock).
    const timeCall = textCalls.find(t => /^\d{2}:\d{2}$/.test(t));
    expect(timeCall).toBeDefined();
    const hh = parseInt(timeCall!.slice(0, 2), 10);
    expect(hh).toBeGreaterThanOrEqual(0);
    expect(hh).toBeLessThanOrEqual(23);
    expect(textCalls.every(t => !/AM|PM/i.test(t))).toBe(true);
    expect(textCalls.every(t => t !== "--:--")).toBe(true);
  });

  it("draws a small 'L' marker separate from the big HH:MM time", () => {
    const ctx = makeCanvasSpy();
    drawCockpitView(ctx);
    const textCalls = ctx.callsOf("fillText").map(c => String(c.args[0]));
    expect(textCalls).toContain("L");
  });

  it("renders a Zulu time string in HH:MM Z format", () => {
    const ctx = makeCanvasSpy();
    drawCockpitView(ctx);
    const textCalls = ctx.callsOf("fillText").map(c => String(c.args[0]));
    expect(textCalls.some(t => /^\d{2}:\d{2} Z$/.test(t))).toBe(true);
  });

  it("shows the OAT label even with an invalid weather reading", () => {
    const ctx = makeCanvasSpy();
    drawCockpitView(ctx);
    const textCalls = ctx.callsOf("fillText").map(c => String(c.args[0]));
    expect(textCalls.some(t => t.startsWith("OAT"))).toBe(true);
  });

  it("draws a baro block with either a value or the '--.-- IN' placeholder", () => {
    const ctx = makeCanvasSpy();
    drawCockpitView(ctx);
    const textCalls = ctx.callsOf("fillText").map(c => String(c.args[0]));
    expect(textCalls.some(t => /IN$/.test(t))).toBe(true);
  });

  it("applies the round-bezel mask (same shape the other views use)", () => {
    const ctx = makeCanvasSpy();
    drawCockpitView(ctx);
    // The bezel arc uses PHYSICAL_PANEL_RADIUS as its radius.
    const arcs = ctx.callsOf("arc");
    expect(arcs.some(a => a.args[2] === PHYSICAL_PANEL_RADIUS)).toBe(true);
  });
});
