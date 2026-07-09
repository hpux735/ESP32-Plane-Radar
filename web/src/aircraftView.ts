// Aircraft icons + tags on the radar view. Ports the firmware's
// drawAircraft with as much of the same visual language as possible:
//   - heading-oriented icon
//   - track vector length = 60s horizon at gs, scaled per firmware formula
//   - 2-line data block (callsign / [altitude|type] with trend triangle)
//   - leader line from icon to tag anchor, alignment flips L/R by side
//   - 16-slot radial tag placement, first non-colliding wins
//   - tag budget per range preset; on-ground records dropped

import type { Aircraft } from "./aircraft";
import type { ViewFrame } from "./projection";
import { project, distSqFromCenter } from "./projection";
import {
  COLORS,
  GRID_OUTER_RADIUS,
  TRACK_HORIZON_SEC,
  TRACK_REF_OUTER_KM,
  TRACK_LENGTH_SCALE,
  TRACK_MIN_PX,
} from "./theme";
import { state } from "./state";

interface Rect { x: number; y: number; w: number; h: number }

const ICON_HALF = 6;
const TAG_LINE_HEIGHT = 10;
const TAG_HEIGHT = 22;                // two lines + a bit of padding
const CALLSIGN_MIN_WIDTH = 36;        // roughly enough for "UAL1234"
const LINE2_MIN_WIDTH = 32;           // "037▼" or "A320"
const TAG_ANCHOR_R = 18;              // px from icon center to tag anchor
const TAG_BUDGET = [20, 15, 10, 6];   // matches firmware {5nm, 10nm, 15nm, 25nm}

// 16 slots around the icon, in the same "prefer NE, walk around" order
// the firmware uses. Angles in degrees CW from N.
const SLOT_ANGLES_DEG = [
  45, 315, 90, 270, 0, 180, 135, 225,
  22, 337, 67, 292, 112, 247, 157, 202,
];

function iconColor(a: Aircraft): string {
  if (isEmergency(a)) return COLORS.emergency;
  return COLORS.aircraft;
}

function isEmergency(a: Aircraft): boolean {
  return a.squawk === 7500 || a.squawk === 7600 || a.squawk === 7700;
}

function isOnGround(a: Aircraft): boolean {
  if (a.altFt === null) return true;
  if (a.altFt < 100 && a.gsKnots < 40) return true;
  return false;
}

function clarityScore(a: Aircraft): number {
  if (isEmergency(a)) return 1e9;
  const alt = a.altFt ?? 0;
  return alt + a.gsKnots * 20 + Math.abs(a.vsFpm) / 5;
}

function tagBudget(): number {
  return TAG_BUDGET[state.rangeIdx] ?? 8;
}

function speedLinePx(gs: number): number {
  if (gs <= 0) return 0;
  const kmPerKtPerHorizon = 1.852 * TRACK_HORIZON_SEC / 3600;
  const px =
    gs * kmPerKtPerHorizon * GRID_OUTER_RADIUS / TRACK_REF_OUTER_KM *
    TRACK_LENGTH_SCALE;
  const len = Math.round(px);
  return len < TRACK_MIN_PX ? TRACK_MIN_PX : len;
}

function drawIcon(
  ctx: CanvasRenderingContext2D,
  x: number, y: number,
  headingDeg: number,
  color: string,
): void {
  const rad = ((headingDeg - 90) * Math.PI) / 180;
  const cos = Math.cos(rad), sin = Math.sin(rad);
  const nx = x + Math.round(cos * 6),  ny = y + Math.round(sin * 6);
  const lx = x + Math.round(cos * -4 - sin * 3);
  const ly = y + Math.round(sin * -4 + cos * 3);
  const rx = x + Math.round(cos * -4 + sin * 3);
  const ry = y + Math.round(sin * -4 - cos * 3);
  ctx.fillStyle = color;
  ctx.beginPath();
  ctx.moveTo(nx, ny);
  ctx.lineTo(lx, ly);
  ctx.lineTo(rx, ry);
  ctx.closePath();
  ctx.fill();
}

function drawTrackVector(
  ctx: CanvasRenderingContext2D,
  x: number, y: number,
  trackDeg: number,
  gs: number,
  emergency: boolean,
): void {
  const len = speedLinePx(gs);
  if (len <= 0) return;
  const rad = ((trackDeg - 90) * Math.PI) / 180;
  const dx = Math.cos(rad) * len;
  const dy = Math.sin(rad) * len;
  ctx.strokeStyle = emergency ? COLORS.emergency : COLORS.trackVector;
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(x, y);
  ctx.lineTo(x + dx, y + dy);
  ctx.stroke();
}

function rectsOverlap(a: Rect, b: Rect): boolean {
  return !(a.x + a.w <= b.x || b.x + b.w <= a.x ||
           a.y + a.h <= b.y || b.y + b.h <= a.y);
}

// Try each of 16 slots around the icon. For each slot pick the tag
// rect that would sit there and check against everything already taken.
// Return the winning rect + orientation info the tag renderer needs.
interface Placement {
  rect: Rect;
  anchorX: number; anchorY: number;   // tag corner nearest the icon (leader endpoint)
  align: "left" | "right" | "center";
  labelSide: "top" | "bottom";        // is the tag above or below the icon
}

function pickPlacement(
  cx: number, cy: number,
  callsignW: number, altW: number,
  taken: Rect[],
): Placement {
  const tagW = Math.max(callsignW, altW, CALLSIGN_MIN_WIDTH);
  for (const angle of SLOT_ANGLES_DEG) {
    const rad = ((angle - 90) * Math.PI) / 180;
    const px = cx + Math.round(Math.cos(rad) * TAG_ANCHOR_R);
    const py = cy + Math.round(Math.sin(rad) * TAG_ANCHOR_R);
    // Alignment: tag right of icon → text starts on tag's LEFT edge
    // (align "left"); tag left of icon → align "right"; near-vertical → center.
    const dx = px - cx;
    let align: "left" | "right" | "center";
    let rectX: number;
    if (dx > 4) {
      align = "left";
      rectX = px;
    } else if (dx < -4) {
      align = "right";
      rectX = px - tagW;
    } else {
      align = "center";
      rectX = px - tagW / 2;
    }
    const rectY = py - TAG_HEIGHT / 2;
    const rect: Rect = {
      x: Math.round(rectX), y: Math.round(rectY),
      w: tagW, h: TAG_HEIGHT,
    };
    if (taken.some((t) => rectsOverlap(rect, t))) continue;
    return {
      rect,
      anchorX: dx > 0 ? rect.x : (dx < 0 ? rect.x + rect.w : rect.x + rect.w / 2),
      anchorY: py < cy ? rect.y + rect.h : rect.y,
      align,
      labelSide: py < cy ? "top" : "bottom",
    };
  }
  // Fallback: preferred slot regardless of collision.
  const rad = ((SLOT_ANGLES_DEG[0] - 90) * Math.PI) / 180;
  const px = cx + Math.round(Math.cos(rad) * TAG_ANCHOR_R);
  const py = cy + Math.round(Math.sin(rad) * TAG_ANCHOR_R);
  const rect = {
    x: Math.round(px), y: Math.round(py - TAG_HEIGHT / 2),
    w: tagW, h: TAG_HEIGHT,
  };
  return { rect, anchorX: rect.x, anchorY: rect.y + rect.h, align: "left", labelSide: "bottom" };
}

function formatAltHundreds(altFt: number | null): string {
  if (altFt === null) return "GND";
  const hundreds = Math.round(altFt / 100);
  return String(hundreds).padStart(3, "0");
}

function trendGlyph(vsFpm: number): string {
  if (vsFpm >= 500) return "▲";
  if (vsFpm <= -500) return "▼";
  return "";
}

// Tag mode toggles once per fetch (~3 s), so the alt/type flip lines
// up with the position update rather than fighting it. This is the
// same "latched per fetch" trick the firmware uses.
let s_altModeAtSlot = new Map<string, boolean>();
let s_lastFetchTag = 0;
function tagShowsAltitude(icao: string, fetchNum: number): boolean {
  if (fetchNum !== s_lastFetchTag) {
    s_lastFetchTag = fetchNum;
    // Flip the mode for every callsign at fetch boundary.
    const next = new Map<string, boolean>();
    for (const [k, v] of s_altModeAtSlot) next.set(k, !v);
    s_altModeAtSlot = next;
  }
  const cur = s_altModeAtSlot.get(icao);
  if (cur === undefined) {
    s_altModeAtSlot.set(icao, true);
    return true;
  }
  return cur;
}

function drawTag(
  ctx: CanvasRenderingContext2D,
  p: Placement,
  a: Aircraft,
  fetchNum: number,
): void {
  ctx.font = "8px system-ui, sans-serif";
  ctx.textBaseline = "top";
  ctx.textAlign = p.align;
  const emergency = isEmergency(a);
  const showAlt = tagShowsAltitude(a.callsign, fetchNum);

  const tx =
    p.align === "left" ? p.rect.x + 1 :
    p.align === "right" ? p.rect.x + p.rect.w - 1 :
    p.rect.x + p.rect.w / 2;

  // Line 1 — callsign
  ctx.fillStyle = emergency ? COLORS.emergency : COLORS.label;
  ctx.fillText(a.callsign, tx, p.rect.y + 1);

  // Line 2 — altitude in hundreds (+trend triangle) OR type code
  let line2: string;
  let line2Color: string;
  if (showAlt || !a.type) {
    line2 = formatAltHundreds(a.altFt) + trendGlyph(a.vsFpm);
    line2Color = COLORS.tagAltitude;
  } else {
    line2 = a.type;
    line2Color = COLORS.tagType;
  }
  ctx.fillStyle = emergency ? COLORS.emergency : line2Color;
  ctx.fillText(line2, tx, p.rect.y + 1 + TAG_LINE_HEIGHT);

  // Emergency EM glyph in the free corner (opposite side of the tag
  // from the current alignment).
  if (emergency) {
    const emX =
      p.align === "left" ? p.rect.x + p.rect.w :
      p.align === "right" ? p.rect.x :
      p.rect.x + p.rect.w;
    const oldAlign = p.align;
    ctx.textAlign = oldAlign === "left" ? "right" : "left";
    ctx.fillStyle = COLORS.emergency;
    ctx.fillText("EM", emX, p.rect.y + 1);
  }
}

function drawLeader(
  ctx: CanvasRenderingContext2D,
  iconX: number, iconY: number,
  p: Placement,
): void {
  // Leader from the icon edge (not the center) to the tag anchor.
  const dx = p.anchorX - iconX;
  const dy = p.anchorY - iconY;
  const d = Math.sqrt(dx * dx + dy * dy);
  if (d < 12) return;   // tag right next to icon; no leader needed
  const ux = dx / d, uy = dy / d;
  const startX = iconX + Math.round(ux * ICON_HALF);
  const startY = iconY + Math.round(uy * ICON_HALF);
  ctx.strokeStyle = COLORS.label;
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(startX, startY);
  ctx.lineTo(p.anchorX, p.anchorY);
  ctx.stroke();
}

interface Placed {
  icon: [number, number];
  a: Aircraft;
  d2: number;
  clarity: number;
  tagged: boolean;
}

export function drawAircraft(
  ctx: CanvasRenderingContext2D,
  view: ViewFrame,
  aircraft: readonly Aircraft[],
  showTags: boolean,
  fetchNum: number,
): void {
  const placed: Placed[] = [];
  for (const a of aircraft) {
    if (isOnGround(a)) continue;
    const [x, y] = project(view, a.lat, a.lon);
    const d2 = distSqFromCenter(x, y);
    if (d2 > GRID_OUTER_RADIUS * GRID_OUTER_RADIUS) continue;
    placed.push({ icon: [x, y], a, d2, clarity: clarityScore(a), tagged: false });
  }

  const budget = tagBudget();
  const ranked = [...placed].sort((p, q) => q.clarity - p.clarity);
  for (let i = 0; i < Math.min(budget, ranked.length); i++) {
    ranked[i].tagged = true;
  }

  placed.sort((p, q) => q.d2 - p.d2);

  for (const { icon: [x, y], a } of placed) {
    drawTrackVector(ctx, x, y, a.trackDeg, a.gsKnots, isEmergency(a));
    drawIcon(ctx, x, y, a.noseDeg || a.trackDeg, iconColor(a));
  }

  if (!showTags) return;

  // Icons reserved as HARD keep-outs so tags never land on top of an
  // adjacent icon.
  const taken: Rect[] = placed.map(({ icon: [x, y] }) => ({
    x: x - ICON_HALF, y: y - ICON_HALF, w: ICON_HALF * 2, h: ICON_HALF * 2,
  }));
  // Measure font once — assume monospace-ish width for our small labels.
  ctx.font = "8px system-ui, sans-serif";
  for (const { icon: [x, y], a, tagged } of placed) {
    if (!tagged) continue;
    const callW = Math.max(CALLSIGN_MIN_WIDTH, ctx.measureText(a.callsign).width + 2);
    const line2Text = formatAltHundreds(a.altFt) + (Math.abs(a.vsFpm) >= 500 ? "▼" : "");
    const line2W = Math.max(LINE2_MIN_WIDTH, ctx.measureText(line2Text).width + 2);
    const p = pickPlacement(x, y, callW, line2W, taken);
    taken.push(p.rect);
    drawLeader(ctx, x, y, p);
    drawTag(ctx, p, a, fetchNum);
  }
}
