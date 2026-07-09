// Radar frame renderer. Draws in one pass to a 2D canvas context; the
// caller decides when to invoke (input change, animation frame, etc.)

import type { MapData } from "./data";
import { selectMap } from "./data";
import { makeView, project, segmentOnScreen, distSqFromCenter, type ViewFrame } from "./projection";
import {
  CENTER_X,
  CENTER_Y,
  COLORS,
  GRID_OUTER_RADIUS,
  PHYSICAL_PANEL_RADIUS,
  SIZE,
} from "./theme";
import { state, currentOuterKm, currentRangeLabel } from "./state";
import { aircraft, fetchCount } from "./aircraft";
import { drawAircraft } from "./aircraftView";

// ---------------------------------------------------------------------------
// Layer drawing
// ---------------------------------------------------------------------------

function fillBackground(ctx: CanvasRenderingContext2D): void {
  ctx.fillStyle = COLORS.background;
  ctx.fillRect(0, 0, SIZE, SIZE);
}

function clipToOuterDisc(ctx: CanvasRenderingContext2D): void {
  ctx.save();
  ctx.beginPath();
  ctx.arc(CENTER_X, CENTER_Y, GRID_OUTER_RADIUS, 0, Math.PI * 2);
  ctx.clip();
}

function unclip(ctx: CanvasRenderingContext2D): void {
  ctx.restore();
}

function drawLand(ctx: CanvasRenderingContext2D, view: ViewFrame, land: MapData["land"]): void {
  if (!state.layers.land) return;
  const { vertices, triangles } = land;
  ctx.fillStyle = COLORS.land;
  ctx.beginPath();
  for (const [ia, ib, ic] of triangles) {
    const [ax, ay] = project(view, vertices[ia][1], vertices[ia][0]);
    const [bx, by] = project(view, vertices[ib][1], vertices[ib][0]);
    const [cx, cy] = project(view, vertices[ic][1], vertices[ic][0]);
    const minX = Math.min(ax, bx, cx);
    const maxX = Math.max(ax, bx, cx);
    const minY = Math.min(ay, by, cy);
    const maxY = Math.max(ay, by, cy);
    if (maxX < 0 || minX >= SIZE || maxY < 0 || minY >= SIZE) continue;
    ctx.moveTo(ax, ay);
    ctx.lineTo(bx, by);
    ctx.lineTo(cx, cy);
    ctx.closePath();
  }
  ctx.fill();
}

function drawCoastline(ctx: CanvasRenderingContext2D, view: ViewFrame, coastline: MapData["coastline"]): void {
  if (!state.layers.coast) return;
  ctx.strokeStyle = COLORS.coastline;
  ctx.lineWidth = 1;
  ctx.beginPath();
  for (const line of coastline) {
    let prev: [number, number] | null = null;
    for (const [lon, lat] of line) {
      const p = project(view, lat, lon);
      if (prev && segmentOnScreen(prev[0], prev[1], p[0], p[1])) {
        ctx.moveTo(prev[0], prev[1]);
        ctx.lineTo(p[0], p[1]);
      }
      prev = p;
    }
  }
  ctx.stroke();
}

function drawRoads(ctx: CanvasRenderingContext2D, view: ViewFrame, roads: MapData["roads"]): void {
  if (!state.layers.roads) return;
  ctx.strokeStyle = COLORS.road;
  ctx.lineWidth = 1.4;
  ctx.beginPath();
  for (const road of roads) {
    let prev: [number, number] | null = null;
    for (const [lon, lat] of road.points) {
      const p = project(view, lat, lon);
      if (prev && segmentOnScreen(prev[0], prev[1], p[0], p[1])) {
        ctx.moveTo(prev[0], prev[1]);
        ctx.lineTo(p[0], p[1]);
      }
      prev = p;
    }
  }
  ctx.stroke();
}

function drawRings(ctx: CanvasRenderingContext2D): void {
  ctx.strokeStyle = COLORS.grid;
  ctx.lineWidth = 1;
  ctx.beginPath();
  for (const r of [Math.round(GRID_OUTER_RADIUS * 0.25),
                   Math.round(GRID_OUTER_RADIUS * 0.5),
                   Math.round(GRID_OUTER_RADIUS * 0.75),
                   GRID_OUTER_RADIUS]) {
    ctx.moveTo(CENTER_X + r, CENTER_Y);
    ctx.arc(CENTER_X, CENTER_Y, r, 0, Math.PI * 2);
  }
  // Crosshairs
  ctx.moveTo(CENTER_X, CENTER_Y - GRID_OUTER_RADIUS);
  ctx.lineTo(CENTER_X, CENTER_Y + GRID_OUTER_RADIUS);
  ctx.moveTo(CENTER_X - GRID_OUTER_RADIUS, CENTER_Y);
  ctx.lineTo(CENTER_X + GRID_OUTER_RADIUS, CENTER_Y);
  ctx.stroke();
}

function drawRunways(ctx: CanvasRenderingContext2D, view: ViewFrame, data: MapData): void {
  if (!state.layers.runways) return;
  // Show any airport in the current disc regardless of tier — small
  // GA fields (HAF, HWD, SQL, PAO, RHV, NUQ) are exactly what the
  // desk toy audience wants to see, and the disc naturally clips.
  ctx.strokeStyle = COLORS.runway;
  ctx.lineWidth = 2;
  ctx.beginPath();
  for (const [, apt] of Object.entries(data.airports)) {
    for (const rw of apt.runways) {
      const [x1, y1] = project(view, rw.lat1, rw.lon1);
      const [x2, y2] = project(view, rw.lat2, rw.lon2);
      if (!segmentOnScreen(x1, y1, x2, y2)) continue;
      ctx.moveTo(x1, y1);
      ctx.lineTo(x2, y2);
    }
  }
  ctx.stroke();
  ctx.font = "10px system-ui, sans-serif";
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  ctx.fillStyle = COLORS.runwayLabel;
  for (const [icao, apt] of Object.entries(data.airports)) {
    const [x, y] = project(view, apt.lat, apt.lon);
    if (distSqFromCenter(x, y) > GRID_OUTER_RADIUS * GRID_OUTER_RADIUS) continue;
    ctx.fillText(icao, x, y + 6);
  }
}

function drawCenterDot(ctx: CanvasRenderingContext2D): void {
  ctx.fillStyle = COLORS.centerDot;
  ctx.beginPath();
  ctx.arc(CENTER_X, CENTER_Y, 2, 0, Math.PI * 2);
  ctx.fill();
}

// Scale label — small, near the top, using kBaseDeg=12° offset from N
// like the firmware does. Green (grid color) so it reads as a reference,
// not a focal element.
function drawScaleLabel(ctx: CanvasRenderingContext2D): void {
  const label = currentRangeLabel();
  const angleDeg = 12;
  const rad = (angleDeg * Math.PI) / 180;
  const r = GRID_OUTER_RADIUS - 10;
  const x = CENTER_X + Math.round(r * Math.sin(rad));
  const y = CENTER_Y - Math.round(r * Math.cos(rad));
  ctx.font = "9px system-ui, sans-serif";
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillStyle = COLORS.grid;
  ctx.fillText(label, x, y);
}

// Paint the corner region outside the physical panel radius back to
// background — matches the round hardware bezel visually.
function drawBezelMask(ctx: CanvasRenderingContext2D): void {
  ctx.fillStyle = COLORS.background;
  ctx.beginPath();
  ctx.rect(0, 0, SIZE, SIZE);
  ctx.arc(CENTER_X, CENTER_Y, PHYSICAL_PANEL_RADIUS, 0, Math.PI * 2, true);
  ctx.fill("evenodd");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

export function renderFrame(ctx: CanvasRenderingContext2D, data: MapData): void {
  const view = makeView(state.centerLat, state.centerLon, currentOuterKm());

  fillBackground(ctx);
  // Pick the appropriate map layers: high-detail Bay Area if the
  // current center is in-bounds, else CONUS 50m coarse base.
  const map = selectMap(data, state.centerLat, state.centerLon);
  // Land is CLIPPED to the outer disc; the coastline/roads sit over it and
  // also inside the disc. Order matches drawStaticGrid in the firmware.
  clipToOuterDisc(ctx);
  drawLand(ctx, view, map.land);
  drawCoastline(ctx, view, map.coastline);
  drawRoads(ctx, view, map.roads);
  unclip(ctx);

  drawRings(ctx);
  drawRunways(ctx, view, data);
  drawCenterDot(ctx);
  drawScaleLabel(ctx);

  // Aircraft draw last so icons + tags sit above the map. The clip to
  // the outer disc is opened again just for icons to keep them from
  // drawing over the bezel margin (labels are OK past the ring).
  drawAircraft(ctx, view, aircraft(), state.layers.tags, fetchCount());

  drawBezelMask(ctx);
}
