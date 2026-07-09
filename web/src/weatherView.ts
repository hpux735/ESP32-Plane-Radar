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
import type { MapData } from "./data";
import { segmentOnScreen } from "./projection";

const PROJECTION_PX = 108;
const LABEL_MARGIN_PX = 14;

// Per-station AABB used for label collision — mirrors weather_map.cpp.
// Horizontal half-width is measured from the real label glyphs (varies
// per ICAO); vertical extent covers the dot plus the stacked label.
// Labels are wider than tall, so an isotropic min-sep pushes vertical
// pairs too far while horizontal chains (PAO/NUQ/SJC) still crash.
const DOT_RADIUS_PX = 4;
const LABEL_HEIGHT_PX = 14;
const LABEL_OFFSET_PX = 6;   // label top offset below dot center
const FOOTPRINT_PAD_PX = 2;
const STATION_HALF_H =
  Math.round((DOT_RADIUS_PX + LABEL_OFFSET_PX + LABEL_HEIGHT_PX +
              DOT_RADIUS_PX) / 2) + FOOTPRINT_PAD_PX;
const LABEL_FONT = "bold 12px system-ui, sans-serif";

function displayIcao(icao: string): string {
  return icao.startsWith("K") ? icao.slice(1) : icao;
}

interface Fit {
  centerLat: number;
  centerLon: number;
  pxPerKm: number;
}

function computeFit(): Fit {
  let minLat = STATIONS[0].lat, maxLat = STATIONS[0].lat;
  let minLon = STATIONS[0].lon, maxLon = STATIONS[0].lon;
  for (const s of STATIONS) {
    if (s.lat < minLat) minLat = s.lat;
    if (s.lat > maxLat) maxLat = s.lat;
    if (s.lon < minLon) minLon = s.lon;
    if (s.lon > maxLon) maxLon = s.lon;
  }
  const centerLat = (minLat + maxLat) / 2;
  const centerLon = (minLon + maxLon) / 2;
  let maxR = 0;
  for (const s of STATIONS) {
    const dx = (s.lon - centerLon) * KM_PER_DEG;
    const dy = (s.lat - centerLat) * KM_PER_DEG;
    const r = Math.sqrt(dx * dx + dy * dy);
    if (r > maxR) maxR = r;
  }
  const budget = PROJECTION_PX - LABEL_MARGIN_PX;
  return { centerLat, centerLon, pxPerKm: maxR > 0 ? budget / maxR : 1 };
}

function project(fit: Fit, lat: number, lon: number): [number, number] {
  const dxKm = (lon - fit.centerLon) * KM_PER_DEG;
  const dyKm = (lat - fit.centerLat) * KM_PER_DEG;
  return [
    CENTER_X + Math.round(dxKm * fit.pxPerKm),
    CENTER_Y - Math.round(dyKm * fit.pxPerKm),
  ];
}

// Iterative relaxation on axis-aligned dot+label footprints. Pushes
// each overlapping pair along the axis of minimum penetration so
// vertical pairs (OAK/HWD) get a small y-nudge while horizontal chains
// (PAO/NUQ/SJC) get a bigger x-nudge. ctx must already have LABEL_FONT
// applied so measureText matches what drawStations() renders.
function placeStations(
  ctx: CanvasRenderingContext2D,
  fit: Fit,
): [number, number][] {
  const positions = STATIONS.map((s) => project(fit, s.lat, s.lon));
  const halfW = STATIONS.map((s) => {
    const w = ctx.measureText(displayIcao(s.icao)).width;
    return Math.max(DOT_RADIUS_PX, Math.round(w / 2)) + FOOTPRINT_PAD_PX;
  });
  // 8 passes: axis-aware pushes converge more slowly when a station is
  // boxed in on two sides, but 8 still costs nothing for 11 stations.
  for (let pass = 0; pass < 8; pass++) {
    let moved = false;
    for (let i = 0; i < positions.length; i++) {
      for (let j = i + 1; j < positions.length; j++) {
        const dx = positions[j][0] - positions[i][0];
        const dy = positions[j][1] - positions[i][1];
        const needX = halfW[i] + halfW[j];
        const needY = 2 * STATION_HALF_H;
        const ox = needX - Math.abs(dx);
        const oy = needY - Math.abs(dy);
        if (ox <= 0 || oy <= 0) continue;
        if (ox <= oy) {
          const sign = dx >= 0 ? 1 : -1;
          const push = Math.ceil(ox / 2);
          positions[i][0] -= sign * push;
          positions[j][0] += sign * push;
        } else {
          const sign = dy >= 0 ? 1 : -1;
          const push = Math.ceil(oy / 2);
          positions[i][1] -= sign * push;
          positions[j][1] += sign * push;
        }
        moved = true;
      }
    }
    if (!moved) break;
  }
  return positions;
}

function categoryColor(c: Category): string {
  return WX_COLORS[c];
}

function insideDisc(x: number, y: number): boolean {
  const dx = x - CENTER_X;
  const dy = y - CENTER_Y;
  return dx * dx + dy * dy <= PROJECTION_PX * PROJECTION_PX;
}

function drawLand(ctx: CanvasRenderingContext2D, fit: Fit, data: MapData): void {
  const { vertices, triangles } = data.land;
  ctx.fillStyle = COLORS.land;
  ctx.beginPath();
  for (const [ia, ib, ic] of triangles) {
    const [ax, ay] = project(fit, vertices[ia][1], vertices[ia][0]);
    const [bx, by] = project(fit, vertices[ib][1], vertices[ib][0]);
    const [cx, cy] = project(fit, vertices[ic][1], vertices[ic][0]);
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

function drawCoast(ctx: CanvasRenderingContext2D, fit: Fit, data: MapData): void {
  ctx.strokeStyle = COLORS.coastline;
  ctx.lineWidth = 1;
  ctx.beginPath();
  for (const line of data.coastline) {
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
  positions: [number, number][],
): void {
  for (let i = 0; i < STATIONS.length; i++) {
    const [x, y] = positions[i];
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
    ctx.fillText(displayIcao(s.icao), x, y + LABEL_OFFSET_PX);
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
  data: MapData,
): void {
  const fit = computeFit();
  // Set label font once — placeStations measures glyphs with it and
  // drawStations paints with it. Keeps the two paths from drifting.
  ctx.font = LABEL_FONT;
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  const positions = placeStations(ctx, fit);
  ctx.fillStyle = COLORS.background;
  ctx.fillRect(0, 0, SIZE, SIZE);
  drawLand(ctx, fit, data);
  drawCoast(ctx, fit, data);
  drawFreshness(ctx);
  ctx.font = LABEL_FONT;   // drawFreshness stomps the font
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  drawStations(ctx, positions);
  drawBezelMask(ctx);
}
