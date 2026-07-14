// Global application state — center + range + view + layer toggles.
// Kept as a module-scoped store rather than a full state library
// because the preview has narrow needs.

import { RANGE_PRESETS } from "./theme";

export type LayerId = "coast" | "land" | "runways" | "tags";
export type ViewMode = "radar" | "weather" | "cockpit";

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
// (or overridden from localStorage). The first entry always represents
// "Home" — the label is fixed and it reads its lat/lon from state.home
// at render time. The default home coord is Sutro Tower (a public SF
// broadcast landmark), never a private residence.
// Two focus airports plus the synthetic Home slot. The whole app now
// navigates a 5-screen ring (3 radars + weather + cockpit) via
// single/double tap, so the size of this list caps how many radar
// screens are in the ring.
export const DEFAULT_FOCUS_RING: FocusPoint[] = [
  { label: "Home",  lat: 37.7552, lon: -122.4528, defaultRangeIdx: 1, isHome: true  },
  { label: "SFO",   lat: 37.6188, lon: -122.3750, defaultRangeIdx: 1, isHome: false },
  { label: "OAK",   lat: 37.7213, lon: -122.2214, defaultRangeIdx: 1, isHome: false },
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
  centerLat: 37.661,
  centerLon: -122.160,
  radiusNm: 28,
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
  layers: "planeradar.layers",
  session: "planeradar.session",  // ephemeral-ish: focusIdx, rangeIdx
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

const DEFAULT_LAYERS: Record<LayerId, boolean> = {
  coast: true,
  land: true,
  runways: true,
  tags: true,
};

function isLayers(v: unknown): v is Record<LayerId, boolean> {
  if (typeof v !== "object" || v === null) return false;
  const l = v as Record<string, unknown>;
  return typeof l.coast === "boolean" && typeof l.land === "boolean" &&
         typeof l.runways === "boolean" && typeof l.tags === "boolean";
}

const loadedHome = loadJson(LS_KEYS.home, DEFAULT_HOME, isHome);
const loadedMetar = loadJson(LS_KEYS.metar, DEFAULT_METAR, isMetar);
const loadedFocus = loadJson(LS_KEYS.focus, DEFAULT_FOCUS_RING, isFocusRing);
const loadedLayers = loadJson(LS_KEYS.layers, DEFAULT_LAYERS, isLayers);

// If the persisted ring's first entry isn't a home slot, prepend one so
// the "Home" cycle position always exists. isHome=true entries have their
// label + lat/lon overwritten from state.home at render time.
if (!loadedFocus[0]?.isHome) {
  loadedFocus.unshift({
    label: "Home", lat: loadedHome.lat, lon: loadedHome.lon,
    defaultRangeIdx: 1, isHome: true,
  });
}

// The isHome slot always tracks the current home location and is always
// labeled "Home" (never a place name like the old "Sutro").
loadedFocus[0].label = "Home";
loadedFocus[0].lat = loadedHome.lat;
loadedFocus[0].lon = loadedHome.lon;

// Always land on Home (slot 0) at page load. The session file still records
// the last-visited focusIdx / rangeIdx via persistSession() for debugging,
// but we don't restore it — matches the firmware's boot behavior in
// services::focus::init(). Returning to whatever was on-screen before
// power-off is more surprising than starting fresh at the pilot's Home.
// View mode always starts on "radar" — coming back to the site is more
// useful with the map than a stale weather / cockpit view.
const restoredFocusIdx = 0;
const restoredFocus = loadedFocus[restoredFocusIdx];
const restoredRangeIdx = restoredFocus.defaultRangeIdx;

export const state: AppState = {
  home: loadedHome,
  metar: loadedMetar,
  focusRing: loadedFocus,
  centerLat: restoredFocus.lat,
  centerLon: restoredFocus.lon,
  centerLabel: restoredFocus.label,
  focusIdx: restoredFocusIdx,
  rangeIdx: restoredRangeIdx,
  view: "radar",
  layers: loadedLayers,
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

function persistSession(): void {
  window.localStorage.setItem(LS_KEYS.session, JSON.stringify({
    focusIdx: state.focusIdx,
    rangeIdx: state.rangeIdx,
  }));
}

export function cycleRange(): void {
  state.rangeIdx = (state.rangeIdx + 1) % RANGE_PRESETS.length;
  persistSession();
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
  persistSession();
  notify();
}

export function setCenter(lat: number, lon: number, label: string): void {
  state.centerLat = lat;
  state.centerLon = lon;
  state.centerLabel = label;
  state.focusIdx = -1;   // typeahead picks aren't part of the ring
  persistSession();
  notify();
}

// Session-only override setters — mutate state in place but do NOT
// persist to localStorage. Used by the shareable-URL feature: someone
// visits ?view=cockpit&home=KSFO and the app applies the override for
// this tab session without touching the cookie. Their saved home
// returns the next time they open the site without params. If they
// open Settings and click Save, `saveHome`/`saveMetar` takes over and
// the session override becomes persisted.
export function setSessionHome(home: HomeLocation): void {
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
  notify();
}

export function setSessionMetar(metar: MetarConfig): void {
  state.metar = metar;
  notify();
}

export function setView(v: ViewMode): void {
  state.view = v;
  notify();
}

// Atomic view + focus change: land on the target focus and view in one
// mutation, notify once. Called from the tap-cycle handler so cycling
// from cockpit back to a radar focus doesn't fire two notifies with
// mismatched (view, center) mid-transition (which would race the
// tile/aircraft refetch subscribers).
export function setViewAndFocus(v: ViewMode, focusIdx: number): void {
  state.view = v;
  const ring = state.focusRing;
  if (focusIdx >= 0 && focusIdx < ring.length) {
    const fp = ring[focusIdx];
    state.focusIdx = focusIdx;
    state.centerLat = fp.lat;
    state.centerLon = fp.lon;
    state.centerLabel = fp.label;
    state.rangeIdx = fp.defaultRangeIdx;
  }
  persistSession();
  notify();
}

export function toggleView(): void {
  // Cycle: radar → weather → cockpit → radar. Kept for keyboard tests /
  // future toggle wiring; the actual tap dispatcher lives in main.ts.
  if      (state.view === "radar")   state.view = "weather";
  else if (state.view === "weather") state.view = "cockpit";
  else                               state.view = "radar";
  notify();
}

export function toggleLayer(id: LayerId): boolean {
  state.layers[id] = !state.layers[id];
  window.localStorage.setItem(LS_KEYS.layers, JSON.stringify(state.layers));
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
    // Only re-center immediately if the user is currently looking at the
    // Home slot. Otherwise the fresh coords sit on the ring and take
    // effect the next time the tap-cycle lands on Home.
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

// Safety net that mirrors config::kMaxFocusAirports on the firmware side
// and MAX_FOCUS_AIRPORTS in settings.ts. The settings UI already prevents
// the user from adding a 7th airport; if a stale payload / hand-edited
// localStorage slips past the UI cap, truncate rather than persist it.
const MAX_FOCUS_AIRPORTS = 6;

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
  if (cleaned.length > 1 + MAX_FOCUS_AIRPORTS) {
    cleaned.length = 1 + MAX_FOCUS_AIRPORTS;
  }
  state.focusRing = cleaned;
  if (state.focusIdx >= cleaned.length) state.focusIdx = 0;
  window.localStorage.setItem(LS_KEYS.focus, JSON.stringify(cleaned));
  notify();
}

export function resetAllSettings(): void {
  for (const key of Object.values(LS_KEYS)) {
    window.localStorage.removeItem(key);
  }
  // Reload is simplest — settings touch many derived fields.
  window.location.reload();
}
