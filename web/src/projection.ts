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

/** Great-circle initial bearing from (lat1,lon1) to (lat2,lon2), in
 *  degrees clockwise from true north, normalized to [0, 360). */
export function bearingDeg(lat1: number, lon1: number, lat2: number, lon2: number): number {
  const toRad = Math.PI / 180;
  const phi1 = lat1 * toRad;
  const phi2 = lat2 * toRad;
  const dLon = (lon2 - lon1) * toRad;
  const y = Math.sin(dLon) * Math.cos(phi2);
  const x = Math.cos(phi1) * Math.sin(phi2) -
            Math.sin(phi1) * Math.cos(phi2) * Math.cos(dLon);
  const brg = (Math.atan2(y, x) * 180) / Math.PI;
  return (brg + 360) % 360;
}

/** Approximate magnetic declination at (lat, lon) in degrees. Positive =
 *  East (magnetic north is east of true north). Tilted-dipole model using
 *  the north geomagnetic pole (~80.65°N, 72.68°W, epoch ~2020). Accuracy
 *  varies regionally — ~2-5° off in the Americas (dominantly dipolar),
 *  ~10-15° off over Europe / North Atlantic (non-dipole "European
 *  anomaly"). Enough for an 8-point compass reading in most of the
 *  northern hemisphere; not sufficient for navigation. Mirrors
 *  services::weather::geo::magneticDeclinationDeg on firmware. */
export function magneticDeclinationDeg(lat: number, lon: number): number {
  const toRad = Math.PI / 180;
  const POLE_LAT = 80.65;
  const POLE_LON = -72.68;
  const phi = lat * toRad;
  const phiP = POLE_LAT * toRad;
  const dLam = (POLE_LON - lon) * toRad;
  const y = Math.sin(dLam);
  const x = Math.cos(phi) * Math.tan(phiP) - Math.sin(phi) * Math.cos(dLam);
  return (Math.atan2(y, x) * 180) / Math.PI;
}

/** Bin a bearing in degrees to one of 8 compass directions. */
export function compass8(deg: number): "N" | "NE" | "E" | "SE" | "S" | "SW" | "W" | "NW" {
  const dirs = ["N", "NE", "E", "SE", "S", "SW", "W", "NW"] as const;
  const idx = Math.round(((deg % 360 + 360) % 360) / 45) % 8;
  return dirs[idx];
}
