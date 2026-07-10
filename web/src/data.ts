// The typeahead airport index shipped with the website. Everything else
// the renderer needs (coastlines, land, airports with runways) comes
// from tile fetches — see web/src/tile.ts + web/src/tileFetch.ts.

// Compact typeahead index: [icao, iata, city, name, lat, lon]
export type AirportIndexRow = [string, string, string, string, number, number];

export interface IndexData {
  airportIndex: AirportIndexRow[];
}

async function fetchJSON<T>(url: string): Promise<T> {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`fetch ${url}: HTTP ${r.status}`);
  return r.json() as Promise<T>;
}

export async function loadIndexData(basePath = "data"): Promise<IndexData> {
  const airportIndex = await fetchJSON<AirportIndexRow[]>(
    `${basePath}/airport_index.json`,
  );
  return { airportIndex };
}
