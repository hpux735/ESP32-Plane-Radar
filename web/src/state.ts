// Global application state — center + range + view + layer toggles.
// Kept as a module-scoped store rather than a full state library
// because the preview has narrow needs.

import { RANGE_PRESETS } from "./theme";

export type LayerId = "coast" | "land" | "roads" | "runways" | "tags";
export type ViewMode = "radar" | "weather";

export interface FocusPoint {
  label: string;
  lat: number;
  lon: number;
  defaultRangeIdx: number;
  isHome: boolean;
}

export interface HomeLocation {
  lat: number;
  lon: number;
}

export interface MetarConfig {
  centerLat: number;
  centerLon: number;
  radiusNm: number;
}

// Bay Area focus ring — baked default. Copied into state.focusRing at boot
// (or overridden from localStorage). First entry is the default center:
// Sutro Tower (well-known SF broadcast landmark), NOT a private residence.
export const DEFAULT_FOCUS_RING: FocusPoint[] = [
  { label: "Sutro", lat: 37.7552, lon: -122.4528, defaultRangeIdx: 1, isHome: true  },
  { label: "SFO",   lat: 37.6188, lon: -122.3750, defaultRangeIdx: 1, isHome: false },
  { label: "OAK",   lat: 37.7213, lon: -122.2214, defaultRangeIdx: 1, isHome: false },
  { label: "SJC",   lat: 37.3639, lon: -121.9289, defaultRangeIdx: 1, isHome: false },
  { label: "HWD",   lat: 37.6591, lon: -122.1214, defaultRangeIdx: 0, isHome: false },
  { label: "SQL",   lat: 37.5119, lon: -122.2495, defaultRangeIdx: 0, isHome: false },
  { label: "PAO",   lat: 37.4611, lon: -122.1150, defaultRangeIdx: 0, isHome: false },
  { label: "HAF",   lat: 37.5136, lon: -122.5006, defaultRangeIdx: 0, isHome: false },
];

// Kept as a re-export so any older imports still resolve. New code should
// prefer state.focusRing (which reflects the user's edits).
export const FOCUS_RING = DEFAULT_FOCUS_RING;

export const DEFAULT_HOME: HomeLocation = {
  lat: DEFAULT_FOCUS_RING[0].lat,
  lon: DEFAULT_FOCUS_RING[0].lon,
};

// Defaults chosen to match the firmware's config::kDefaultMetar* so the
// initial map looks identical on both platforms.
export const DEFAULT_METAR: MetarConfig = {
  centerLat: 37.55,
  centerLon: -122.30,
  radiusNm: 45,
};

export interface AppState {
  home: HomeLocation;
  metar: MetarConfig;
  focusRing: FocusPoint[];
  centerLat: number;
  centerLon: number;
  centerLabel: string;     // "Home" | "SFO" | etc.
  focusIdx: number;        // index into state.focusRing; -1 = custom
  rangeIdx: number;        // index into RANGE_PRESETS
  view: ViewMode;
  layers: Record<LayerId, boolean>;
}

const LS_KEYS = {
  home: "planeradar.home",
  metar: "planeradar.metar",
  focus: "planeradar.focusRing",
};

function loadJson<T>(key: string, fallback: T, validate: (v: unknown) => v is T): T {
  try {
    const raw = window.localStorage.getItem(key);
    if (!raw) return fallback;
    const parsed: unknown = JSON.parse(raw);
    return validate(parsed) ? parsed : fallback;
  } catch {
    return fallback;
  }
}

function isHome(v: unknown): v is HomeLocation {
  return typeof v === "object" && v !== null &&
         typeof (v as HomeLocation).lat === "number" &&
         typeof (v as HomeLocation).lon === "number" &&
         Number.isFinite((v as HomeLocation).lat) &&
         Number.isFinite((v as HomeLocation).lon);
}

function isMetar(v: unknown): v is MetarConfig {
  if (typeof v !== "object" || v === null) return false;
  const m = v as MetarConfig;
  return Number.isFinite(m.centerLat) && Number.isFinite(m.centerLon) &&
         Number.isFinite(m.radiusNm) && m.radiusNm > 0;
}

function isFocusPoint(v: unknown): v is FocusPoint {
  if (typeof v !== "object" || v === null) return false;
  const p = v as FocusPoint;
  return typeof p.label === "string" &&
         Number.isFinite(p.lat) && Number.isFinite(p.lon) &&
         typeof p.defaultRangeIdx === "number" &&
         typeof p.isHome === "boolean";
}

function isFocusRing(v: unknown): v is FocusPoint[] {
  return Array.isArray(v) && v.length >= 1 && v.every(isFocusPoint);
}

const loadedHome = loadJson(LS_KEYS.home, DEFAULT_HOME, isHome);
const loadedMetar = loadJson(LS_KEYS.metar, DEFAULT_METAR, isMetar);
const loadedFocus = loadJson(LS_KEYS.focus, DEFAULT_FOCUS_RING, isFocusRing);

// If the persisted ring's first entry isn't a home slot, prepend one so
// the "Home" cycle position always exists. isHome=true entries have their
// live lat/lon overwritten from state.home at render time.
if (!loadedFocus[0]?.isHome) {
  loadedFocus.unshift({
    label: "Home", lat: loadedHome.lat, lon: loadedHome.lon,
    defaultRangeIdx: 1, isHome: true,
  });
}

// The isHome slot always tracks the current home location.
loadedFocus[0].lat = loadedHome.lat;
loadedFocus[0].lon = loadedHome.lon;

// Default: focus[0] = Home, radar view. Roads default OFF — the user
// found them noisy under traffic.
export const state: AppState = {
  home: loadedHome,
  metar: loadedMetar,
  focusRing: loadedFocus,
  centerLat: loadedFocus[0].lat,
  centerLon: loadedFocus[0].lon,
  centerLabel: loadedFocus[0].label,
  focusIdx: 0,
  rangeIdx: loadedFocus[0].defaultRangeIdx,
  view: "radar",
  layers: {
    coast: true,
    land: true,
    roads: false,          // opt-in per user preference
    runways: true,
    tags: true,
  },
};

type Listener = () => void;
const listeners: Listener[] = [];

export function subscribe(fn: Listener): () => void {
  listeners.push(fn);
  return () => {
    const idx = listeners.indexOf(fn);
    if (idx >= 0) listeners.splice(idx, 1);
  };
}

export function notify(): void {
  for (const fn of listeners) fn();
}

export function cycleRange(): void {
  state.rangeIdx = (state.rangeIdx + 1) % RANGE_PRESETS.length;
  notify();
}

export function cycleFocus(): void {
  const ring = state.focusRing;
  if (ring.length === 0) return;
  state.focusIdx = (state.focusIdx + 1) % ring.length;
  const fp = ring[state.focusIdx];
  state.centerLat = fp.lat;
  state.centerLon = fp.lon;
  state.centerLabel = fp.label;
  state.rangeIdx = fp.defaultRangeIdx;
  notify();
}

export function setCenter(lat: number, lon: number, label: string): void {
  state.centerLat = lat;
  state.centerLon = lon;
  state.centerLabel = label;
  state.focusIdx = -1;   // typeahead picks aren't part of the ring
  notify();
}

export function setView(v: ViewMode): void {
  state.view = v;
  notify();
}

export function toggleView(): void {
  state.view = state.view === "radar" ? "weather" : "radar";
  notify();
}

export function toggleLayer(id: LayerId): boolean {
  state.layers[id] = !state.layers[id];
  notify();
  return state.layers[id];
}

export function currentOuterKm(): number {
  return RANGE_PRESETS[state.rangeIdx].outerKm;
}

export function currentRangeLabel(): string {
  return `${RANGE_PRESETS[state.rangeIdx].nm}nm`;
}

// Settings mutators — each one persists to localStorage and notifies.
// Home coordinates also update the isHome slot in the focus ring so the
// tap-cycle "Home" position stays in sync.
export function saveHome(home: HomeLocation): void {
  state.home = home;
  const homeSlot = state.focusRing.find(fp => fp.isHome);
  if (homeSlot) {
    homeSlot.lat = home.lat;
    homeSlot.lon = home.lon;
    if (state.focusIdx >= 0 && state.focusRing[state.focusIdx]?.isHome) {
      state.centerLat = home.lat;
      state.centerLon = home.lon;
    }
  }
  window.localStorage.setItem(LS_KEYS.home, JSON.stringify(home));
  notify();
}

export function saveMetar(metar: MetarConfig): void {
  state.metar = metar;
  window.localStorage.setItem(LS_KEYS.metar, JSON.stringify(metar));
  notify();
}

export function saveFocusRing(ring: FocusPoint[]): void {
  // Guard: keep at least one home slot at index 0.
  const cleaned = ring.slice();
  if (!cleaned[0]?.isHome) {
    cleaned.unshift({
      label: "Home", lat: state.home.lat, lon: state.home.lon,
      defaultRangeIdx: 1, isHome: true,
    });
  }
  cleaned[0].lat = state.home.lat;
  cleaned[0].lon = state.home.lon;
  state.focusRing = cleaned;
  if (state.focusIdx >= cleaned.length) state.focusIdx = 0;
  window.localStorage.setItem(LS_KEYS.focus, JSON.stringify(cleaned));
  notify();
}

export function resetAllSettings(): void {
  window.localStorage.removeItem(LS_KEYS.home);
  window.localStorage.removeItem(LS_KEYS.metar);
  window.localStorage.removeItem(LS_KEYS.focus);
  // Reload is simplest — settings touch many derived fields.
  window.location.reload();
}
