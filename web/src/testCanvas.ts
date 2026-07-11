// Canvas shim decision (2026-07-10): spy-based, not real pixels.
//
// The alternatives considered:
//   * `node-canvas`  — native binary, real pixels, snapshot-friendly, but
//     30 MB install + build hooks that break on CI without cairo/pango.
//   * `happy-dom`    — already installed for DOM tests; its canvas is a
//     no-op (methods do nothing, measureText returns 0). Useless for
//     assertions about drawing.
//   * `jest-canvas-mock` — spy-only recorder, no draw output. Same shape
//     as this helper but jest-flavored and unmaintained since 2023.
//
// We went with a hand-rolled spy so the tests can assert on the DRAWING
// COMMAND SEQUENCE (fills, strokes, text, arcs) without a native binary.
// Trade-off: tests catch "renderer stopped calling fillText for airport
// tags" but NOT "text is now 2px too far right." Pixel-accuracy is what
// the SDL emulator + goldens are for; that lives on the firmware side.
//
// Add new recorded methods when a test needs them — do NOT `as any` your
// way out of a missing method.

export interface CanvasCall {
  method: string;
  args: unknown[];
}

export interface StateSet {
  field: string;
  value: unknown;
}

export interface CanvasSpy extends CanvasRenderingContext2D {
  readonly calls: CanvasCall[];
  readonly stateSets: StateSet[];
  callsOf(method: string): CanvasCall[];
  countOf(method: string): number;
  hasFill(color: string): boolean;
  hasStroke(color: string): boolean;
  hasText(text: string): boolean;
}

const RECORDED_METHODS = [
  "fillRect", "clearRect", "strokeRect",
  "beginPath", "closePath", "moveTo", "lineTo", "arc", "rect",
  "fill", "stroke", "clip",
  "save", "restore",
  "fillText", "strokeText",
  "setLineDash",
  "drawImage",
  "translate", "rotate", "scale",
];

const RECORDED_STATE_FIELDS = [
  "fillStyle", "strokeStyle", "lineWidth", "font",
  "textAlign", "textBaseline", "globalAlpha",
];

export function makeCanvasSpy(): CanvasSpy {
  const calls: CanvasCall[] = [];
  const stateSets: StateSet[] = [];
  const state: Record<string, unknown> = {
    fillStyle: "#000",
    strokeStyle: "#000",
    lineWidth: 1,
    font: "10px sans-serif",
    textAlign: "start",
    textBaseline: "alphabetic",
    globalAlpha: 1,
  };

  const spy = {} as Record<string, unknown>;

  for (const m of RECORDED_METHODS) {
    spy[m] = (...args: unknown[]) => {
      calls.push({ method: m, args });
    };
  }

  spy.measureText = (t: string) => ({ width: t.length * 6 });

  for (const f of RECORDED_STATE_FIELDS) {
    Object.defineProperty(spy, f, {
      get: () => state[f],
      set: (v) => { state[f] = v; stateSets.push({ field: f, value: v }); },
    });
  }

  Object.defineProperty(spy, "calls", { get: () => calls });
  Object.defineProperty(spy, "stateSets", { get: () => stateSets });

  spy.callsOf = (m: string) => calls.filter((c) => c.method === m);
  spy.countOf = (m: string) => (spy.callsOf as (m: string) => CanvasCall[])(m).length;
  spy.hasFill = (color: string) =>
    stateSets.some((s) => s.field === "fillStyle" && s.value === color);
  spy.hasStroke = (color: string) =>
    stateSets.some((s) => s.field === "strokeStyle" && s.value === color);
  spy.hasText = (text: string) =>
    calls.some((c) => c.method === "fillText" && c.args[0] === text);

  return spy as unknown as CanvasSpy;
}
