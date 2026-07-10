// Weather-map view — mirror of src/ui/weather_map.cpp on the firmware.
// Auto-fits the current station set into the visible disc, nudges dots
// apart so labels are legible, draws land + coastline underneath for
// spatial context.

import {
  CENTER_X,
  CENTER_Y,
  COLORS,
  KM_PER_DEG,
  PHYSICAL_PANEL_RADIUS,
  SIZE,
  WX_COLORS,
} from "./theme";
import { STATIONS, lastUpdateMs, type Category } from "./weather";
import { segmentOnScreen } from "./projection";
import { state } from "./state";
import type { Tile } from "./tile";

const KM_PER_NM = 1.852;

const PROJECTION_PX = 108;
const LABEL_MARGIN_PX = 14;

// Dots stay at their true projected positions — never nudged. Labels
// are moved into free space around the dot (8 candidate slots), with a
// leader line drawn whenever the label ends up somewhere other than
// the default "below the dot" position. Mirrors weather_map.cpp.
const DOT_RADIUS_PX = 4;
const LABEL_HEIGHT_PX = 14;
const LABEL_GAP_PX = 4;
const DOT_BBOX_PAD_PX = 1;
const LABEL_FONT = "bold 12px system-ui, sans-serif";

function displayIcao(icao: string): string {
  return icao.startsWith("K") ? icao.slice(1) : icao;
}

interface Placement {
  labelX: number;
  labelY: number;
  cand: number;
}

// 8 candidate offsets (top_center anchor of the label) relative to the
// dot center. Ordered by preference — 0 is directly below (no leader),
// then above, then the cardinals, then the diagonals.
function candidateOffsets(halfW: number): { dx: number; dy: number }[] {
  const dr = DOT_RADIUS_PX;
  const gap = LABEL_GAP_PX;
  const lh = LABEL_HEIGHT_PX;
  const vdn = dr + gap;
  const vup = -dr - gap - lh;
  const hr = dr + gap + halfW;
  const hl = -dr - gap - halfW;
  const vmid = -Math.round(lh / 2);
  return [
    { dx: 0, dy: vdn },     // below (default, no leader)
    { dx: 0, dy: vup },     // above
    { dx: hr, dy: vmid },   // right
    { dx: hl, dy: vmid },   // left
    { dx: hr, dy: vdn },    // below-right
    { dx: hl, dy: vdn },    // below-left
    { dx: hr, dy: vup },    // above-right
    { dx: hl, dy: vup },    // above-left
  ];
}

function overlapArea(
  ax1: number, ay1: number, ax2: number, ay2: number,
  bx1: number, by1: number, bx2: number, by2: number,
): number {
  const w = Math.min(ax2, bx2) - Math.max(ax1, bx1);
  const h = Math.min(ay2, by2) - Math.max(ay1, by1);
  return w > 0 && h > 0 ? w * h : 0;
}

interface Fit {
  centerLat: number;
  centerLon: number;
  cosCenter: number;
  pxPerKm: number;
}

// Reads center + radius from state.metar (user-editable via the settings
// overlay). Radius maps to just inside the bezel; stations beyond the
// radius project past the visible disc and are filtered out by insideDisc.
function computeFit(): Fit {
  const centerLat = state.metar.centerLat;
  const centerLon = state.metar.centerLon;
  const cosCenter = Math.cos(centerLat * Math.PI / 180);
  const radiusKm = state.metar.radiusNm * KM_PER_NM;
  const budget = PROJECTION_PX - LABEL_MARGIN_PX;
  const pxPerKm = radiusKm > 0 ? budget / radiusKm : 1;
  return { centerLat, centerLon, cosCenter, pxPerKm };
}

function project(fit: Fit, lat: number, lon: number): [number, number] {
  const dxKm = (lon - fit.centerLon) * KM_PER_DEG * fit.cosCenter;
  const dyKm = (lat - fit.centerLat) * KM_PER_DEG;
  return [
    CENTER_X + Math.round(dxKm * fit.pxPerKm),
    CENTER_Y - Math.round(dyKm * fit.pxPerKm),
  ];
}

// Project every station to its true screen pixel and measure the
// label's real rendered width. Dots don't move — only labels.
function projectStations(
  ctx: CanvasRenderingContext2D,
  fit: Fit,
): { dots: [number, number][]; halfW: number[] } {
  const dots = STATIONS.map((s) => project(fit, s.lat, s.lon));
  const halfW = STATIONS.map(
    (s) => Math.round(ctx.measureText(displayIcao(s.icao)).width / 2) + 1,
  );
  return { dots, halfW };
}

// Assign each station's label to the least-overlapping candidate slot
// among 8 positions around the dot. Iterate a few passes so late-placed
// labels can push earlier ones off their first choice.
function placeLabels(
  dots: [number, number][],
  halfW: number[],
): Placement[] {
  const n = dots.length;
  const places: Placement[] = dots.map(([x, y], i) => {
    const off = candidateOffsets(halfW[i])[0];
    return { labelX: x + off.dx, labelY: y + off.dy, cand: 0 };
  });
  const scoreLabel = (i: number, cand: number): number => {
    const off = candidateOffsets(halfW[i])[cand];
    const lx = dots[i][0] + off.dx;
    const ly = dots[i][1] + off.dy;
    const L = lx - halfW[i];
    const R = lx + halfW[i];
    const T = ly;
    const B = ly + LABEL_HEIGHT_PX;
    let s = 0;
    for (let j = 0; j < n; j++) {
      if (j === i) continue;
      const [dx, dy] = dots[j];
      s += overlapArea(
        L, T, R, B,
        dx - DOT_RADIUS_PX - DOT_BBOX_PAD_PX,
        dy - DOT_RADIUS_PX - DOT_BBOX_PAD_PX,
        dx + DOT_RADIUS_PX + DOT_BBOX_PAD_PX,
        dy + DOT_RADIUS_PX + DOT_BBOX_PAD_PX,
      );
      s += overlapArea(
        L, T, R, B,
        places[j].labelX - halfW[j], places[j].labelY,
        places[j].labelX + halfW[j], places[j].labelY + LABEL_HEIGHT_PX,
      );
    }
    return s;
  };
  for (let pass = 0; pass < 4; pass++) {
    let changed = false;
    for (let i = 0; i < n; i++) {
      let bestCand = places[i].cand;
      let bestScore = scoreLabel(i, bestCand);
      for (let c = 0; c < 8; c++) {
        if (c === bestCand) continue;
        const s = scoreLabel(i, c);
        // Strict '<' so ties keep the lower-index candidate (defaults win).
        if (s < bestScore) { bestScore = s; bestCand = c; }
      }
      if (bestCand !== places[i].cand) {
        const off = candidateOffsets(halfW[i])[bestCand];
        places[i] = {
          labelX: dots[i][0] + off.dx,
          labelY: dots[i][1] + off.dy,
          cand: bestCand,
        };
        changed = true;
      }
    }
    if (!changed) break;
  }
  return places;
}

// Endpoint of the leader on the label side: where the ray from the dot
// to the label center crosses the label bbox.
function leaderEndpoint(
  dotX: number, dotY: number,
  labelX: number, labelY: number,
  halfW: number,
): [number, number] {
  const cx = labelX;
  const cy = labelY + LABEL_HEIGHT_PX / 2;
  const hh = LABEL_HEIGHT_PX / 2;
  const dx = dotX - cx;
  const dy = dotY - cy;
  if (dx === 0 && dy === 0) return [cx, cy];
  const tx = dx === 0 ? Infinity : halfW / Math.abs(dx);
  const ty = dy === 0 ? Infinity : hh / Math.abs(dy);
  const t = Math.min(tx, ty);
  return [Math.round(cx + t * dx), Math.round(cy + t * dy)];
}

function categoryColor(c: Category): string {
  return WX_COLORS[c];
}

function insideDisc(x: number, y: number): boolean {
  const dx = x - CENTER_X;
  const dy = y - CENTER_Y;
  return dx * dx + dy * dy <= PROJECTION_PX * PROJECTION_PX;
}

// Fill every tile land polygon at weather-map zoom. Canvas handles
// concave rings natively via the evenodd rule — no ear-clip needed
// (that's the firmware's problem because SPI has no polygon fill).
function drawTileLand(ctx: CanvasRenderingContext2D, fit: Fit,
                      tiles: Tile[]): void {
  ctx.fillStyle = COLORS.land;
  ctx.beginPath();
  for (const tile of tiles) {
    for (const ring of tile.land) {
      if (ring.length < 3) continue;
      let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
      const pts: [number, number][] = new Array(ring.length);
      for (let i = 0; i < ring.length; i++) {
        const [lon, lat] = ring[i];
        const [x, y] = project(fit, lat, lon);
        pts[i] = [x, y];
        if (x < minX) minX = x; if (x > maxX) maxX = x;
        if (y < minY) minY = y; if (y > maxY) maxY = y;
      }
      if (maxX < 0 || minX >= SIZE || maxY < 0 || minY >= SIZE) continue;
      ctx.moveTo(pts[0][0], pts[0][1]);
      for (let i = 1; i < pts.length; i++) ctx.lineTo(pts[i][0], pts[i][1]);
      ctx.closePath();
    }
  }
  ctx.fill("evenodd");
}

function drawTileCoast(ctx: CanvasRenderingContext2D, fit: Fit,
                       tiles: Tile[]): void {
  ctx.strokeStyle = COLORS.coastline;
  ctx.lineWidth = 1;
  ctx.beginPath();
  for (const tile of tiles) {
    for (const line of tile.coast) {
      let prev: [number, number] | null = null;
      for (const [lon, lat] of line) {
        const p = project(fit, lat, lon);
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

function drawFreshness(ctx: CanvasRenderingContext2D): void {
  const last = lastUpdateMs();
  let text = "no data";
  if (last > 0) {
    const ageS = Math.round((Date.now() - last) / 1000);
    text = ageS < 60 ? `${ageS}s ago` : `${Math.floor(ageS / 60)}m ago`;
  }
  ctx.font = "9px system-ui, sans-serif";
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  ctx.fillStyle = COLORS.grid;
  ctx.fillText(text, CENTER_X, 8);
}

function drawStations(
  ctx: CanvasRenderingContext2D,
  dots: [number, number][],
  halfW: number[],
  places: Placement[],
): void {
  // Pass 1: leaders under everything else.
  ctx.strokeStyle = "rgb(90, 130, 110)";
  ctx.lineWidth = 1;
  ctx.beginPath();
  for (let i = 0; i < STATIONS.length; i++) {
    const [dx, dy] = dots[i];
    if (!insideDisc(dx, dy)) continue;
    if (places[i].cand === 0) continue;
    const [ex, ey] = leaderEndpoint(
      dx, dy, places[i].labelX, places[i].labelY, halfW[i],
    );
    ctx.moveTo(dx, dy);
    ctx.lineTo(ex, ey);
  }
  ctx.stroke();

  // Pass 2: dots + labels.
  for (let i = 0; i < STATIONS.length; i++) {
    const [x, y] = dots[i];
    if (!insideDisc(x, y)) continue;
    const s = STATIONS[i];
    ctx.fillStyle = categoryColor(s.category);
    ctx.beginPath();
    ctx.arc(x, y, DOT_RADIUS_PX, 0, Math.PI * 2);
    ctx.fill();
    ctx.strokeStyle = COLORS.background;
    ctx.lineWidth = 1;
    ctx.stroke();
    ctx.fillStyle = COLORS.label;
    ctx.fillText(displayIcao(s.icao), places[i].labelX, places[i].labelY);
  }
}

function drawBezelMask(ctx: CanvasRenderingContext2D): void {
  ctx.fillStyle = COLORS.background;
  ctx.beginPath();
  ctx.rect(0, 0, SIZE, SIZE);
  ctx.arc(CENTER_X, CENTER_Y, PHYSICAL_PANEL_RADIUS, 0, Math.PI * 2, true);
  ctx.fill("evenodd");
}

export function drawWeatherView(
  ctx: CanvasRenderingContext2D,
  tiles: Tile[],
): void {
  const fit = computeFit();
  // Set label font once — projectStations measures glyphs with it and
  // drawStations paints with it. Keeps the two paths from drifting.
  ctx.font = LABEL_FONT;
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  const { dots, halfW } = projectStations(ctx, fit);
  const places = placeLabels(dots, halfW);
  ctx.fillStyle = COLORS.background;
  ctx.fillRect(0, 0, SIZE, SIZE);
  drawTileLand(ctx, fit, tiles);
  drawTileCoast(ctx, fit, tiles);
  drawFreshness(ctx);
  ctx.font = LABEL_FONT;   // drawFreshness stomps the font
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  drawStations(ctx, dots, halfW, places);
  drawBezelMask(ctx);
}
