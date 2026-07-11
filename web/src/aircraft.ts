// Aircraft fetch + parse. Calls /api/adsb (proxied to
// airplanes.live / opendata.adsb.fi by the Netlify Function at
// web/netlify/functions/adsb.mjs) and normalizes the response into a
// shape aligned with the firmware's services::adsb::Aircraft struct.

export interface Aircraft {
  hex: string;
  callsign: string;    // trimmed "flight" > registration > hex
  type: string;        // "B738", "A320", etc; empty if unknown
  lat: number;
  lon: number;
  altFt: number | null;   // barometric altitude (null = on ground / unknown)
  gsKnots: number;        // ground speed
  trackDeg: number;       // direction of travel
  noseDeg: number;        // nose heading (falls back to track)
  vsFpm: number;          // vertical rate; 0 if unknown
  squawk: number;         // transponder code (0 = unknown)
}

interface AdsbRawAircraft {
  hex?: string;
  flight?: string;
  r?: string;
  t?: string;
  lat?: number;
  lon?: number;
  alt_baro?: number | "ground";
  alt_geom?: number;
  gs?: number;
  tas?: number;
  ias?: number;
  track?: number;
  true_heading?: number;
  mag_heading?: number;
  baro_rate?: number;
  geom_rate?: number;
  squawk?: string;
}

interface AdsbResponse {
  ac?: AdsbRawAircraft[];        // opendata.adsb.fi field name
  aircraft?: AdsbRawAircraft[];  // fallback for other ADS-B sources
}

function pickCallsign(raw: AdsbRawAircraft): string {
  const flight = (raw.flight ?? "").trim();
  if (flight) return flight;
  const reg = (raw.r ?? "").trim();
  if (reg) return reg;
  return (raw.hex ?? "").toUpperCase();
}

function pickAltitude(raw: AdsbRawAircraft): number | null {
  if (raw.alt_baro === "ground") return null;
  if (typeof raw.alt_baro === "number") return raw.alt_baro;
  if (typeof raw.alt_geom === "number") return raw.alt_geom;
  return null;
}

function pickGroundSpeed(raw: AdsbRawAircraft): number {
  return raw.gs ?? raw.tas ?? raw.ias ?? 0;
}

function pickTrack(raw: AdsbRawAircraft): number {
  return raw.track ?? raw.true_heading ?? raw.mag_heading ?? 0;
}

function pickNose(raw: AdsbRawAircraft): number {
  return raw.true_heading ?? raw.mag_heading ?? raw.track ?? 0;
}

function pickVerticalRate(raw: AdsbRawAircraft): number {
  return raw.baro_rate ?? raw.geom_rate ?? 0;
}

function pickSquawk(raw: AdsbRawAircraft): number {
  const s = raw.squawk;
  if (!s) return 0;
  const n = parseInt(s, 10);
  return isFinite(n) ? n : 0;
}

function normalize(raw: AdsbRawAircraft): Aircraft | null {
  if (typeof raw.lat !== "number" || typeof raw.lon !== "number") return null;
  return {
    hex: raw.hex ?? "",
    callsign: pickCallsign(raw),
    type: (raw.t ?? "").trim(),
    lat: raw.lat,
    lon: raw.lon,
    altFt: pickAltitude(raw),
    gsKnots: pickGroundSpeed(raw),
    trackDeg: pickTrack(raw),
    noseDeg: pickNose(raw),
    vsFpm: pickVerticalRate(raw),
    squawk: pickSquawk(raw),
  };
}

// Cache the last successful payload so a transient fetch failure doesn't
// blank the display.
let s_aircraft: Aircraft[] = [];
let s_lastUpdateMs = 0;
let s_lastError: string | null = null;
let s_fetchCount = 0;

export function aircraft(): readonly Aircraft[] {
  return s_aircraft;
}

export function lastUpdateMs(): number {
  return s_lastUpdateMs;
}

export function fetchCount(): number {
  return s_fetchCount;
}

export function lastError(): string | null {
  return s_lastError;
}

export async function fetchAircraft(
  centerLat: number,
  centerLon: number,
  nm: number,
): Promise<void> {
  // cache: 'no-store' so the browser doesn't reuse a stale copy after
  // the Worker's short edge-cache window. Aircraft are moving; the API
  // is our single source of freshness.
  const url =
    `api/adsb?lat=${centerLat.toFixed(4)}` +
    `&lon=${centerLon.toFixed(4)}&nm=${nm.toFixed(1)}`;
  // Abort after 5 s so a stalled upstream doesn't freeze the poll loop.
  // The polling caller uses an "in-flight" flag; if we don't abort,
  // one slow request can block every subsequent 3 s tick.
  const controller = new AbortController();
  const timer = window.setTimeout(() => controller.abort(), 5000);
  try {
    const resp = await fetch(url, {
      cache: "no-store",
      signal: controller.signal,
    });
    if (!resp.ok) {
      s_lastError = `HTTP ${resp.status}`;
      return;
    }
    const doc = (await resp.json()) as AdsbResponse;
    const list: Aircraft[] = [];
    for (const raw of doc.ac ?? doc.aircraft ?? []) {
      const a = normalize(raw);
      if (a) list.push(a);
    }
    s_aircraft = list;
    s_lastUpdateMs = Date.now();
    s_lastError = null;
    s_fetchCount += 1;
  } catch (err) {
    s_lastError = err instanceof Error ? err.message : String(err);
  } finally {
    clearTimeout(timer);
  }
}
