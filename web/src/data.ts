// Fetch + cache the baked JSON payloads emitted by
// scripts/build_web_data.py.

export type LonLat = [number, number];

export interface LandData {
  vertices: LonLat[];
  triangles: [number, number, number][];
}

export interface RoadLine {
  type: string;         // "Major Highway" | "Secondary Highway"
  points: LonLat[];
}

export interface Runway {
  le: string;
  he: string;
  lat1: number; lon1: number;
  lat2: number; lon2: number;
}

export interface Airport {
  name: string;
  city: string;
  lat: number;
  lon: number;
  tier: number;         // 3=large, 2=medium, 1=small
  runways: Runway[];
}

// Compact typeahead index: [icao, iata, city, name, lat, lon]
export type AirportIndexRow = [string, string, string, string, number, number];

export interface MapData {
  // High-detail Bay Area layers (200 km around home) — richer coastline
  // + minor islands + secondary highways. Used when the current center
  // falls inside BAY_BBOX; otherwise the CONUS coarse layers take over.
  coastline: LonLat[][];
  land: LandData;
  roads: RoadLine[];
  // CONUS-wide base layers (10 m Natural Earth, simplified harder) so
  // ANY US airport the user picks in the typeahead gets a legible map.
  // Airports table covers every US airport with scheduled service.
  coastlineConus: LonLat[][];
  landConus: LandData;
  roadsConus: RoadLine[];
  airports: Record<string, Airport>;
  airportIndex: AirportIndexRow[];
}

/** Rectangle inside which the high-detail Bay Area layers are valid. */
export const BAY_BBOX = {
  minLat: 35.96, maxLat: 39.56,
  minLon: -124.69, maxLon: -120.13,
};

export function isInBay(lat: number, lon: number): boolean {
  return lat >= BAY_BBOX.minLat && lat <= BAY_BBOX.maxLat &&
         lon >= BAY_BBOX.minLon && lon <= BAY_BBOX.maxLon;
}

/** Pick the appropriate coastline/land/roads triple for the current
 *  center. High-detail Bay Area set inside BAY_BBOX, else the coarser
 *  CONUS base (10 m Natural Earth simplified for the whole US). */
export function selectMap(data: MapData, lat: number, lon: number): {
  coastline: LonLat[][];
  land: LandData;
  roads: RoadLine[];
} {
  if (isInBay(lat, lon)) {
    return { coastline: data.coastline, land: data.land, roads: data.roads };
  }
  return {
    coastline: data.coastlineConus,
    land: data.landConus,
    roads: data.roadsConus,
  };
}

// Naive cache: fetch once per URL. Replace with a per-region cache when
// dynamic CONUS lands.
const cache = new Map<string, Promise<unknown>>();

async function fetchJSON<T>(url: string): Promise<T> {
  const existing = cache.get(url);
  if (existing) return existing as Promise<T>;
  const p = fetch(url).then(async (r) => {
    if (!r.ok) throw new Error(`fetch ${url}: HTTP ${r.status}`);
    return r.json() as Promise<T>;
  });
  cache.set(url, p);
  return p;
}

export async function loadMapData(basePath = "data"): Promise<MapData> {
  const [
    coastline, land, roads,
    coastlineConus, landConus, roadsConus,
    airports, airportIndex,
  ] = await Promise.all([
    fetchJSON<LonLat[][]>(`${basePath}/coastline.json`),
    fetchJSON<LandData>(`${basePath}/land.json`),
    fetchJSON<RoadLine[]>(`${basePath}/roads.json`),
    fetchJSON<LonLat[][]>(`${basePath}/coastline_conus.json`),
    fetchJSON<LandData>(`${basePath}/land_conus.json`),
    fetchJSON<RoadLine[]>(`${basePath}/roads_conus.json`),
    fetchJSON<Record<string, Airport>>(`${basePath}/airports.json`),
    fetchJSON<AirportIndexRow[]>(`${basePath}/airport_index.json`),
  ]);
  return {
    coastline, land, roads,
    coastlineConus, landConus, roadsConus,
    airports, airportIndex,
  };
}
