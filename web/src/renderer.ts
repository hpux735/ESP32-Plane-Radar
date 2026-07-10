// Radar frame renderer. Draws in one pass to a 2D canvas context; the
// caller decides when to invoke (input change, animation frame, etc.)

import type { Tile } from "./tile";
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
import { aircraft, fetchCount, lastUpdateMs as aircraftLastUpdateMs } from "./aircraft";
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

// Iterate every land polygon across the currently-loaded tiles and paint
// each ring as a single Canvas path. Canvas can fill non-triangulated
// concave polygons natively (evenodd rule), so no ear-clip on the web
// path — the browser's tessellator handles it.
function drawLand(ctx: CanvasRenderingContext2D, view: ViewFrame, tiles: Tile[]): void {
  if (!state.layers.land) return;
  ctx.fillStyle = COLORS.land;
  ctx.beginPath();
  for (const tile of tiles) {
    for (const ring of tile.land) {
      if (ring.length < 3) continue;
      let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
      const pts: [number, number][] = new Array(ring.length);
      for (let i = 0; i < ring.length; i++) {
        const [lon, lat] = ring[i];
        const [x, y] = project(view, lat, lon);
        pts[i] = [x, y];
        if (x < minX) minX = x; if (x > maxX) maxX = x;
        if (y < minY) minY = y; if (y > maxY) maxY = y;
      }
      // Skip rings entirely off-screen so Canvas doesn't do useless work.
      if (maxX < 0 || minX >= SIZE || maxY < 0 || minY >= SIZE) continue;
      ctx.moveTo(pts[0][0], pts[0][1]);
      for (let i = 1; i < pts.length; i++) ctx.lineTo(pts[i][0], pts[i][1]);
      ctx.closePath();
    }
  }
  // evenodd matches the ear-clip fallback in the firmware and keeps
  // overlapping tile-edge duplicates from cancelling out visually.
  ctx.fill("evenodd");
}

function drawCoastline(ctx: CanvasRenderingContext2D, view: ViewFrame, tiles: Tile[]): void {
  if (!state.layers.coast) return;
  ctx.strokeStyle = COLORS.coastline;
  ctx.lineWidth = 1;
  ctx.beginPath();
  for (const tile of tiles) {
    for (const line of tile.coast) {
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

function drawRunways(ctx: CanvasRenderingContext2D, view: ViewFrame, tiles: Tile[]): void {
  if (!state.layers.runways) return;
  // Show any airport in the current disc regardless of tier — small
  // GA fields (HAF, HWD, SQL, PAO, RHV, NUQ) are exactly what the
  // desk toy audience wants to see, and the disc naturally clips.
  ctx.strokeStyle = COLORS.runway;
  ctx.lineWidth = 2;
  ctx.beginPath();
  for (const tile of tiles) {
    for (const apt of tile.airports) {
      for (const rw of apt.runways) {
        const [x1, y1] = project(view, rw.lat1, rw.lon1);
        const [x2, y2] = project(view, rw.lat2, rw.lon2);
        if (!segmentOnScreen(x1, y1, x2, y2)) continue;
        ctx.moveTo(x1, y1);
        ctx.lineTo(x2, y2);
      }
    }
  }
  ctx.stroke();
  ctx.font = "10px system-ui, sans-serif";
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  ctx.fillStyle = COLORS.runwayLabel;
  const seen = new Set<string>();
  for (const tile of tiles) {
    for (const apt of tile.airports) {
      if (seen.has(apt.ident)) continue;
      seen.add(apt.ident);
      const [x, y] = project(view, apt.lat, apt.lon);
      if (distSqFromCenter(x, y) > GRID_OUTER_RADIUS * GRID_OUTER_RADIUS) continue;
      ctx.fillText(apt.ident, x, y + 6);
    }
  }
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

export function renderFrame(ctx: CanvasRenderingContext2D, tiles: Tile[]): void {
  const view = makeView(state.centerLat, state.centerLon, currentOuterKm());

  fillBackground(ctx);
  clipToOuterDisc(ctx);
  drawLand(ctx, view, tiles);
  drawCoastline(ctx, view, tiles);
  unclip(ctx);

  drawRings(ctx);
  drawRunways(ctx, view, tiles);
  drawScaleLabel(ctx);

  // Aircraft draw last so icons + tags sit above the map. The clip to
  // the outer disc is opened again just for icons to keep them from
  // drawing over the bezel margin (labels are OK past the ring).
  drawAircraft(ctx, view, aircraft(), state.layers.tags,
               fetchCount(), aircraftLastUpdateMs());

  drawBezelMask(ctx);
}
