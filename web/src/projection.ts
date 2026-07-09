// Lat/lon → screen projection. Mirrors the firmware's ui::proj primitives
// (map_projection.hpp/.cpp) but takes the projection center + scale as
// explicit args instead of reading global radar state.

import {
  CENTER_X,
  CENTER_Y,
  GRID_OUTER_RADIUS,
  KM_PER_DEG,
} from "./theme";

export interface ViewFrame {
  centerLat: number;
  centerLon: number;
  outerKm: number;     // outer ring in km
  pxPerKm: number;     // derived: GRID_OUTER_RADIUS / outerKm
}

export function makeView(centerLat: number, centerLon: number, outerKm: number): ViewFrame {
  return {
    centerLat,
    centerLon,
    outerKm,
    pxPerKm: GRID_OUTER_RADIUS / outerKm,
  };
}

/** Project lat/lon to screen pixel coords. Returns [x, y]. */
export function project(view: ViewFrame, lat: number, lon: number): [number, number] {
  const cosCenterLat = Math.cos(view.centerLat * Math.PI / 180);
  const dxKm = (lon - view.centerLon) * KM_PER_DEG * cosCenterLat;
  const dyKm = (lat - view.centerLat) * KM_PER_DEG;
  return [
    CENTER_X + Math.round(dxKm * view.pxPerKm),
    CENTER_Y - Math.round(dyKm * view.pxPerKm),
  ];
}

/** Squared distance in pixels from the given point to the radar center. */
export function distSqFromCenter(x: number, y: number): number {
  const dx = x - CENTER_X;
  const dy = y - CENTER_Y;
  return dx * dx + dy * dy;
}

/** True if the segment (x0,y0)→(x1,y1) *bounding box* overlaps the 240×240
 *  framebuffer. Used as a cheap reject for map features; correct at all
 *  zooms (unlike a vertex-in-disc check, which drops big features whose
 *  interior covers the disc but whose corners project past it). */
export function segmentOnScreen(x0: number, y0: number, x1: number, y1: number): boolean {
  const minX = Math.min(x0, x1);
  const maxX = Math.max(x0, x1);
  const minY = Math.min(y0, y1);
  const maxY = Math.max(y0, y1);
  if (maxX < 0 || minX >= 240) return false;
  if (maxY < 0 || minY >= 240) return false;
  return true;
}
