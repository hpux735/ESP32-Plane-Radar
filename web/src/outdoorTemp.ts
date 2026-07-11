// Cockpit-view weather source: live Open-Meteo reading at the home
// point. Mirrors src/services/outdoor_temp.cpp — same endpoint, same
// units, same 15 min refresh cadence. CORS is enabled on the Open-Meteo
// public API, so the browser calls it directly.

import { state } from "./state";

export interface WxReading {
  tempF: number;
  windKts: number;
  windDegFrom: number;
  baroInHg: number;
  valid: boolean;
  // Open-Meteo's `utc_offset_seconds` at the home lat/lon for the moment
  // of the fetch — DST-aware. Populated when the URL includes
  // `&timezone=auto`, which buildUrl below now does. Used by the cockpit
  // view to render home-local HH:MM without a client-side lat→tz table.
  utcOffsetSec: number;
}

const HPA_PER_INHG = 33.8639;
const REFRESH_MS = 15 * 60 * 1000;

let cache: WxReading = {
  tempF: NaN,
  windKts: NaN,
  windDegFrom: NaN,
  baroInHg: NaN,
  valid: false,
  utcOffsetSec: 0,
};
let lastFetchMs = 0;
let inFlight: Promise<void> | null = null;

export function cachedReading(): WxReading {
  return cache;
}

function buildUrl(lat: number, lon: number): string {
  const params = new URLSearchParams({
    latitude: lat.toFixed(6),
    longitude: lon.toFixed(6),
    current: "temperature_2m,wind_speed_10m,wind_direction_10m,pressure_msl",
    temperature_unit: "fahrenheit",
    wind_speed_unit: "kn",
    timezone: "auto",
    forecast_days: "1",
  });
  return `https://api.open-meteo.com/v1/forecast?${params.toString()}`;
}

async function doFetch(): Promise<void> {
  const url = buildUrl(state.home.lat, state.home.lon);
  const resp = await fetch(url, { cache: "no-store" });
  if (!resp.ok) throw new Error(`open-meteo HTTP ${resp.status}`);
  const doc = (await resp.json()) as {
    current?: {
      temperature_2m?: unknown;
      wind_speed_10m?: unknown;
      wind_direction_10m?: unknown;
      pressure_msl?: unknown;
    };
    utc_offset_seconds?: unknown;
  };
  const cur = doc.current;
  if (cur == null || typeof cur.temperature_2m !== "number") {
    throw new Error("open-meteo: missing temperature_2m");
  }
  const pressureHpa = typeof cur.pressure_msl === "number" ? cur.pressure_msl : NaN;
  // Preserve the prior offset if the response omits it (older schema);
  // stale offset beats a snap back to UTC.
  const nextOffset = typeof doc.utc_offset_seconds === "number"
    ? doc.utc_offset_seconds
    : cache.utcOffsetSec;
  cache = {
    tempF: cur.temperature_2m,
    windKts: typeof cur.wind_speed_10m === "number" ? cur.wind_speed_10m : NaN,
    windDegFrom: typeof cur.wind_direction_10m === "number" ? cur.wind_direction_10m : NaN,
    baroInHg: isFinite(pressureHpa) ? pressureHpa / HPA_PER_INHG : NaN,
    valid: true,
    utcOffsetSec: nextOffset,
  };
  lastFetchMs = Date.now();
}

export function refreshIfStale(): Promise<void> {
  if (inFlight) return inFlight;
  const age = Date.now() - lastFetchMs;
  if (cache.valid && age < REFRESH_MS) return Promise.resolve();
  inFlight = doFetch()
    .catch((err) => { console.warn("outdoor_temp fetch failed:", err); })
    .finally(() => { inFlight = null; });
  return inFlight;
}

// Drop the TTL cache. Called from the state.home subscriber so a home
// move triggers a fresh fetch on the next refreshIfStale() instead of
// serving the old location's reading for up to 15 minutes.
export function invalidate(): void {
  cache = {
    tempF: NaN,
    windKts: NaN,
    windDegFrom: NaN,
    baroInHg: NaN,
    valid: false,
    utcOffsetSec: 0,
  };
  lastFetchMs = 0;
}
