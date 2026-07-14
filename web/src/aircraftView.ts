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

// Clip the segment (x0,y0)->(x1,y1) to the outer radar ring. Mirrors
// clipPointToOuterRing in src/ui/radar_display.cpp:290 so fast-mover track
// vectors don't run past the ring into the bezel.
function clipToOuterRing(
  x0: number, y0: number,
  x1: number, y1: number,
): { x: number; y: number } {
  const R = GRID_OUTER_RADIUS;
  const dxEnd = x1 - CENTER_X, dyEnd = y1 - CENTER_Y;
  if (dxEnd * dxEnd + dyEnd * dyEnd <= R * R) return { x: x1, y: y1 };
  // Segment/circle intersection: find t in [0,1] such that
  // |P0 + t*(P1-P0) - C|^2 = R^2. Since the aircraft (start) sits
  // inside the disc, the smaller positive root is the exit point.
  const px = x0 - CENTER_X, py = y0 - CENTER_Y;
  const rx = x1 - x0,      ry = y1 - y0;
  const a = rx * rx + ry * ry;
  const b = 2 * (px * rx + py * ry);
  const c = px * px + py * py - R * R;
  const disc = b * b - 4 * a * c;
  if (a === 0 || disc < 0) return { x: x1, y: y1 };
  const t = (-b + Math.sqrt(disc)) / (2 * a);
  const tc = Math.max(0, Math.min(1, t));
  return { x: x0 + rx * tc, y: y0 + ry * tc };
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
  const tipX = x + Math.cos(rad) * len;
  const tipY = y + Math.sin(rad) * len;
  const end = clipToOuterRing(x, y, tipX, tipY);
  ctx.strokeStyle = emergency ? COLORS.emergency : COLORS.trackVector;
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(x, y);
  ctx.lineTo(end.x, end.y);
  ctx.stroke();
}

function rectsOverlap(a: Rect, b: Rect): boolean {
  return !(a.x + a.w <= b.x || b.x + b.w <= a.x ||
           a.y + a.h <= b.y || b.y + b.h <= a.y);
}

// Try each of 16 slots around the icon. For each slot pick the tag
// rect that would sit there and check against everything already taken.
// Return the winning rect + orientation info the tag renderer needs.
//
// Alignment rule: tag ALWAYS aligns left or right — never center.
// Aircraft directly above or below the icon still get L/R alignment
// picked by whichever side has more room on the disc. Matches firmware.
interface Placement {
  rect: Rect;
  anchorX: number; anchorY: number;   // tag corner nearest the icon (leader endpoint)
  align: "left" | "right";
}

// Import from theme to know the panel radius for clamping.
import { PHYSICAL_PANEL_RADIUS, CENTER_X, CENTER_Y } from "./theme";

function rectFitsInDisc(r: Rect): boolean {
  // Clamp target: all four corners inside the physical panel (bezel)
  // so the tag doesn't get cut off by the round display edge.
  const corners: [number, number][] = [
    [r.x, r.y], [r.x + r.w, r.y],
    [r.x, r.y + r.h], [r.x + r.w, r.y + r.h],
  ];
  for (const [x, y] of corners) {
    const dx = x - CENTER_X, dy = y - CENTER_Y;
    if (dx * dx + dy * dy > (PHYSICAL_PANEL_RADIUS - 1) * (PHYSICAL_PANEL_RADIUS - 1)) {
      return false;
    }
  }
  return true;
}

function pickPlacement(
  cx: number, cy: number,
  callsignW: number, altW: number,
  taken: Rect[],
): Placement {
  const tagW = Math.max(callsignW, altW, CALLSIGN_MIN_WIDTH);
  // For aircraft near the disc edge, the smart-money slots are the
  // ones that point INWARD (back toward the center). Sort the slot
  // angles by how much they lean toward center from this icon; that
  // way tags for planes near the eastern edge default to west-of-icon
  // placement, tags for northern-edge planes go south, etc.
  const towardCenterAngle = Math.atan2(
    CENTER_X - cx,      // sin(theta) = east/west from N-up
    -(CENTER_Y - cy),   // cos(theta) = north/south
  ) * 180 / Math.PI;
  const angleDelta = (a: number) => {
    let d = a - towardCenterAngle;
    while (d > 180) d -= 360;
    while (d < -180) d += 360;
    return Math.abs(d);
  };
  const sortedAngles = [...SLOT_ANGLES_DEG].sort(
    (a, b) => angleDelta(a) - angleDelta(b),
  );

  interface Try { rect: Rect; align: "left" | "right"; anchorX: number; anchorY: number; inwardness: number }
  const candidates: Try[] = [];
  for (const angle of sortedAngles) {
    const rad = ((angle - 90) * Math.PI) / 180;
    const px = cx + Math.round(Math.cos(rad) * TAG_ANCHOR_R);
    const py = cy + Math.round(Math.sin(rad) * TAG_ANCHOR_R);
    const dx = px - cx;
    // Never "center" — pick whichever side gives more headroom.
    let align: "left" | "right";
    let rectX: number;
    if (dx > 0 || (dx === 0 && cx <= CENTER_X)) {
      align = "left";  rectX = px;
    } else {
      align = "right"; rectX = px - tagW;
    }
    const rectY = py - TAG_HEIGHT / 2;
    const rect: Rect = { x: Math.round(rectX), y: Math.round(rectY), w: tagW, h: TAG_HEIGHT };
    // Inwardness = how much closer to center the tag rect's CENTER is
    // vs. the icon. Higher = better when the icon is near the edge.
    const trcx = rect.x + rect.w / 2, trcy = rect.y + rect.h / 2;
    const tagDist = Math.hypot(trcx - CENTER_X, trcy - CENTER_Y);
    const iconDist = Math.hypot(cx - CENTER_X, cy - CENTER_Y);
    candidates.push({
      rect, align,
      anchorX: align === "left" ? rect.x : rect.x + rect.w,
      anchorY: py < cy ? rect.y + rect.h : rect.y,
      inwardness: iconDist - tagDist,
    });
  }
  // Pass 1: fits in disc AND doesn't collide.
  for (const c of candidates) {
    if (!rectFitsInDisc(c.rect)) continue;
    if (taken.some((t) => rectsOverlap(c.rect, t))) continue;
    return c;
  }
  // Pass 2: fits in disc, allow collision.
  for (const c of candidates) {
    if (rectFitsInDisc(c.rect)) return c;
  }
  // Pass 3: pick the one that pushes the tag most inward (least overflow).
  return [...candidates].sort((a, b) => b.inwardness - a.inwardness)[0];
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

// Tag mode toggle — mirrors the firmware exactly:
//   - Toggle at most once per fetch (guarded by fetchCount).
//   - Only toggle after 1.5 s have passed since the fetch — the mode
//     flip lags the position update, so viewers see:
//     … position update (mode: alt) … 1.5s pause … flip to type
//       … 1.5s pause … position update (mode: type) … 1.5s pause …
//       flip to alt … and so on.
//   - Each mode gets a full 3 s dwell. Position and mode never change
//     at the same instant.
const MODE_TOGGLE_OFFSET_MS = 1500;
let s_showAlt = true;
let s_toggledAtFetch = 0;
function tagShowsAltitude(fetchNum: number, lastUpdateMs: number): boolean {
  const since = Date.now() - lastUpdateMs;
  if (fetchNum !== s_toggledAtFetch && since >= MODE_TOGGLE_OFFSET_MS) {
    s_showAlt = !s_showAlt;
    s_toggledAtFetch = fetchNum;
  }
  return s_showAlt;
}

function drawTag(
  ctx: CanvasRenderingContext2D,
  p: Placement,
  a: Aircraft,
  showAlt: boolean,
): void {
  ctx.font = "8px system-ui, sans-serif";
  ctx.textBaseline = "top";
  ctx.textAlign = p.align;
  const emergency = isEmergency(a);

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

// Aircraft just OUTSIDE the visible range get a small dot on the ring
// edge in their direction, so you know "hey, one's coming from over
// there." Same pattern as beyondRingEdgeDotFromLatLon in the firmware.
function drawBeyondRingDot(
  ctx: CanvasRenderingContext2D,
  view: ViewFrame,
  a: Aircraft,
): boolean {
  const [x, y] = project(view, a.lat, a.lon);
  const dx = x - CENTER_X;
  const dy = y - CENTER_Y;
  const d2 = dx * dx + dy * dy;
  // Inside disc: no beyond-ring dot needed.
  if (d2 <= GRID_OUTER_RADIUS * GRID_OUTER_RADIUS) return false;
  // Too far out (~2x range): don't bother.
  const maxR2 = (GRID_OUTER_RADIUS * 2) * (GRID_OUTER_RADIUS * 2);
  if (d2 > maxR2) return true;   // outside our care but we DID skip
  const d = Math.sqrt(d2);
  const ex = CENTER_X + Math.round(dx / d * (GRID_OUTER_RADIUS - 2));
  const ey = CENTER_Y + Math.round(dy / d * (GRID_OUTER_RADIUS - 2));
  ctx.fillStyle = iconColor(a);
  ctx.beginPath();
  ctx.arc(ex, ey, 2, 0, Math.PI * 2);
  ctx.fill();
  return true;
}

export function drawAircraft(
  ctx: CanvasRenderingContext2D,
  view: ViewFrame,
  aircraft: readonly Aircraft[],
  showTags: boolean,
  fetchNum: number,
  lastUpdateMs: number,
): void {
  const showAlt = tagShowsAltitude(fetchNum, lastUpdateMs);
  const placed: Placed[] = [];
  for (const a of aircraft) {
    if (isOnGround(a)) continue;
    const [x, y] = project(view, a.lat, a.lon);
    const d2 = distSqFromCenter(x, y);
    if (d2 > GRID_OUTER_RADIUS * GRID_OUTER_RADIUS) {
      // Aircraft outside the ring — draw a small dot on the edge in
      // its direction (same as the firmware's beyond-ring dot).
      drawBeyondRingDot(ctx, view, a);
      continue;
    }
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
    drawTag(ctx, p, a, showAlt);
  }
}
