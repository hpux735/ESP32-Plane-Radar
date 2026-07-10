// Live METAR fetch + flight-category compute. Uses the National Weather
// Service's api.weather.gov, which sends CORS `Access-Control-Allow-
// Origin: *` — so we call it directly from the browser, no proxy.
//
// One request per station (there's no bulk endpoint on NWS like there
// is on aviationweather.gov), fired in parallel. Cached for 5 min.
//
// The station list itself is dynamic: rebuildStations() takes the
// current METAR center/radius and picks the nearest airports (from
// airport_index.json) that fall inside the visible disc. That's how the
// map populates around JFK, ORD, or wherever the user points it — the
// firmware equivalent (src/services/weather.cpp) uses aviationweather's
// bbox endpoint; the browser can't (no CORS), so we filter locally.

import type { AirportIndexRow } from "./data";

export type Category = "VFR" | "MVFR" | "IFR" | "LIFR" | "Unknown";

export interface Station {
  icao: string;
  lat: number;
  lon: number;
  category: Category;
  ceilingFtAgl: number;    // Infinity if none reported
  visibilitySm: number;    // 10 for "10+"
  fetchedAtMs: number;     // 0 if never
}

// Fixed cap on how many stations we render / fetch. Kept modest so we
// don't blast NWS with 100+ parallel HTTP calls in a dense metro; 32
// mirrors the firmware weather.cpp cap for visual parity.
const MAX_STATIONS = 32;

// Exported as an array so weatherView.ts's `import { STATIONS }`
// stays a live reference. Mutated in place by rebuildStations().
export const STATIONS: Station[] = [];

let lastFleetUpdateMs = 0;

export function lastUpdateMs(): number {
  return lastFleetUpdateMs;
}

/** Reset the fresh-data cache so refreshIfStale() will refetch. Call
 *  after rebuildStations() or when the user forces a manual refresh. */
export function invalidate(): void {
  lastFleetUpdateMs = 0;
}

// Great-circle-ish distance in nautical miles. 1° latitude ≈ 60 nm;
// longitude scales by cos(lat). Good enough at radar zoom.
const NM_PER_DEG = 60;
function distanceNm(lat1: number, lon1: number, lat2: number, lon2: number): number {
  const cosLat = Math.cos((lat1 * Math.PI) / 180);
  const dLatNm = (lat2 - lat1) * NM_PER_DEG;
  const dLonNm = (lon2 - lon1) * NM_PER_DEG * cosLat;
  return Math.hypot(dLatNm, dLonNm);
}

/** Rebuild STATIONS to the (up to MAX_STATIONS) closest airports to
 *  (centerLat, centerLon) that fall within radiusNm × 1.1 (small pad so
 *  edge stations still render). Preserves already-fetched category data
 *  for stations that survive the rebuild. */
export function rebuildStations(
  airportIndex: AirportIndexRow[],
  centerLat: number,
  centerLon: number,
  radiusNm: number,
): void {
  const cutoff = radiusNm * 1.1;
  const scratch: { icao: string; lat: number; lon: number; dist: number }[] = [];
  for (const [icao, , , , lat, lon] of airportIndex) {
    // NWS observations endpoint only accepts ICAO-formatted ids
    // (4 letters, first char is region). The baked index is already
    // ICAO-keyed but skip anything malformed just in case.
    if (!icao || icao.length < 3) continue;
    const d = distanceNm(centerLat, centerLon, lat, lon);
    if (d > cutoff) continue;
    scratch.push({ icao, lat, lon, dist: d });
  }
  scratch.sort((a, b) => a.dist - b.dist);
  const pick = scratch.slice(0, MAX_STATIONS);

  // Preserve prior fetched data for stations that survive the rebuild —
  // avoids a category-flash back to Unknown while the next fetch lands.
  const prior = new Map(STATIONS.map((s) => [s.icao, s] as const));
  STATIONS.length = 0;
  for (const p of pick) {
    const old = prior.get(p.icao);
    STATIONS.push(old ?? {
      icao: p.icao,
      lat: p.lat,
      lon: p.lon,
      category: "Unknown",
      ceilingFtAgl: Infinity,
      visibilitySm: 0,
      fetchedAtMs: 0,
    });
  }
}

// FAA rules — worst-of ceiling and visibility wins.
function deriveCategory(ceilingFt: number, visibilitySm: number): Category {
  const noCeiling = !isFinite(ceilingFt);
  const c = noCeiling ? "VFR"
    : ceilingFt < 500 ? "LIFR"
    : ceilingFt < 1000 ? "IFR"
    : ceilingFt <= 3000 ? "MVFR"
    : "VFR";
  const v = visibilitySm < 1 ? "LIFR"
    : visibilitySm < 3 ? "IFR"
    : visibilitySm <= 5 ? "MVFR"
    : "VFR";
  const order: Record<Category, number> = { VFR: 0, MVFR: 1, IFR: 2, LIFR: 3, Unknown: -1 };
  return order[c] > order[v] ? c : v;
}

interface CloudLayer { base: number | null; amount: string | null; }

// api.weather.gov returns visibility in meters and cloud bases in meters;
// convert to statute miles and feet AGL for the FAA math.
const M_PER_SM = 1609.344;
const M_PER_FT = 0.3048;

function ceilingFromClouds(clouds: CloudLayer[] | null | undefined): number {
  if (!clouds) return Infinity;
  let ceiling = Infinity;
  for (const layer of clouds) {
    const amt = layer.amount?.toUpperCase();
    if (amt !== "BKN" && amt !== "OVC" && amt !== "VV") continue;
    const baseFt = (layer.base ?? Infinity) / M_PER_FT;
    if (baseFt < ceiling) ceiling = baseFt;
  }
  return ceiling;
}

interface NwsObservationProps {
  visibility?: { value: number | null };
  cloudLayers?: CloudLayer[];
}

interface NwsObservation {
  properties?: NwsObservationProps;
}

async function fetchStation(st: Station): Promise<void> {
  const url = `https://api.weather.gov/stations/${encodeURIComponent(st.icao)}/observations/latest`;
  const resp = await fetch(url, {
    headers: { Accept: "application/geo+json" },
  });
  if (!resp.ok) throw new Error(`weather ${st.icao}: HTTP ${resp.status}`);
  const doc = (await resp.json()) as NwsObservation;
  const props = doc.properties ?? {};
  const visMeters = props.visibility?.value;
  const visibilitySm =
    visMeters == null ? 10 : Math.min(10, Math.round(visMeters / M_PER_SM));
  const ceilingFt = ceilingFromClouds(props.cloudLayers);
  st.visibilitySm = visibilitySm;
  st.ceilingFtAgl = ceilingFt;
  st.category = deriveCategory(ceilingFt, visibilitySm);
  st.fetchedAtMs = Date.now();
}

/** Fire off one HTTP call per station in parallel. Individual failures
 *  leave that station's category as Unknown but don't reject the whole
 *  update. Populates the module-level cache. */
export async function updateAll(): Promise<void> {
  await Promise.all(
    STATIONS.map(async (st) => {
      try {
        await fetchStation(st);
      } catch (err) {
        console.warn(`weather ${st.icao}:`, err);
      }
    })
  );
  lastFleetUpdateMs = Date.now();
}

/** Fire an update only if data is missing or older than kTtlMs. */
export async function refreshIfStale(): Promise<void> {
  const ttlMs = 5 * 60 * 1000;
  const now = Date.now();
  if (lastFleetUpdateMs === 0 || now - lastFleetUpdateMs > ttlMs) {
    await updateAll();
  }
}
