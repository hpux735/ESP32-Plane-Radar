// Plane Radar — web preview entry point.

import "./style.css";
import { loadIndexData, type IndexData } from "./data";
import { renderFrame } from "./renderer";
import {
  state,
  subscribe,
  cycleRange,
  cycleFocus,
  setView,
} from "./state";
import { makeTapDiscriminator, type Tap } from "./input";
import { mountSettings } from "./settings";
import { drawWeatherView } from "./weatherView";
import { drawCockpitView } from "./cockpitView";
import { refreshIfStale, rebuildStations, invalidate as invalidateMetar } from "./weather";
import { refreshIfStale as refreshOutdoorTemp } from "./outdoorTemp";
import { fetchAircraft } from "./aircraft";
import { RANGE_PRESETS } from "./theme";
import { loadTilesForView } from "./tileFetch";
import type { Tile } from "./tile";

const KM_PER_NM = 1.852;

let indexData: IndexData | null = null;
// Currently-loaded tile set for whichever view is up. Reassigned when
// the center of the active view crosses into a different tile family;
// individual tile fetches are memoized inside tileFetch so pans across
// already-seen tiles never hit the network twice.
let tiles: Tile[] = [];
let tilesKey = "";
// While in weather or cockpit mode, we repaint once/second — the weather
// view's "n min ago" freshness label ticks up and the cockpit view's
// second-sweep animates. Cleared when returning to radar.
let nonRadarTicker: number | null = null;

function drawLoadingState(ctx: CanvasRenderingContext2D, msg: string): void {
  ctx.fillStyle = "rgb(12, 20, 36)";
  ctx.fillRect(0, 0, 240, 240);
  ctx.fillStyle = "rgb(122, 134, 173)";
  ctx.font = "10px system-ui, sans-serif";
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillText(msg, 120, 120);
}

// Double-buffered draw: composite the whole frame into a 240×240
// offscreen canvas, then blit to the visible canvas in a single
// drawImage() call. Prevents any partial-frame flash you'd see if
// each layer painted straight onto the visible canvas.
let offscreen: HTMLCanvasElement | null = null;
function getOffscreen(): HTMLCanvasElement {
  if (!offscreen) {
    offscreen = document.createElement("canvas");
    offscreen.width = 240;
    offscreen.height = 240;
  }
  return offscreen;
}

let frameQueued = false;
function requestFrame(): void {
  if (frameQueued) return;
  frameQueued = true;
  requestAnimationFrame(() => {
    frameQueued = false;
    const canvas = document.getElementById("radar") as HTMLCanvasElement | null;
    if (!canvas) return;
    const visible = canvas.getContext("2d");
    if (!visible) return;
    const buf = getOffscreen();
    const bctx = buf.getContext("2d");
    if (!bctx) return;
    if (!indexData) {
      drawLoadingState(bctx, "loading map…");
    } else if (state.view === "weather") {
      drawWeatherView(bctx, tiles);
    } else if (state.view === "cockpit") {
      drawCockpitView(bctx);
    } else {
      renderFrame(bctx, tiles);
    }
    // Single blit — the visible canvas never shows a partial frame.
    visible.clearRect(0, 0, 240, 240);
    visible.drawImage(buf, 0, 0);
  });
}

// The current view's (lat, lon, radius) — determines which tiles we need.
function currentViewGeometry(): { lat: number; lon: number; radiusKm: number } {
  if (state.view === "weather") {
    return {
      lat: state.metar.centerLat,
      lon: state.metar.centerLon,
      radiusKm: state.metar.radiusNm * KM_PER_NM,
    };
  }
  // Radar view: fetch the outer-ring radius with a 10% safety margin so
  // features near the edge don't pop in a beat late when a pan crosses
  // into a fresh tile.
  return {
    lat: state.centerLat,
    lon: state.centerLon,
    radiusKm: RANGE_PRESETS[state.rangeIdx].outerKm * 1.1,
  };
}

// Refetch tiles when the active view's center/radius has moved into a
// combination we haven't loaded yet. Tile fetches are memoized inside
// tileFetch, so this is only a network round-trip when the tile set
// actually changes.
let tilesFetchInFlight: Promise<void> | null = null;
async function ensureTiles(): Promise<void> {
  const g = currentViewGeometry();
  // Cheap change detector: same key → same tile list → nothing to do.
  const key = `${state.view}:${g.lat.toFixed(3)}:${g.lon.toFixed(3)}:${g.radiusKm.toFixed(1)}`;
  if (key === tilesKey && tiles.length > 0) return;
  if (tilesFetchInFlight) return tilesFetchInFlight;
  tilesFetchInFlight = (async () => {
    try {
      const next = await loadTilesForView(g.lat, g.lon, g.radiusKm);
      tiles = next;
      tilesKey = key;
      requestFrame();
    } catch (err) {
      console.error("tile fetch failed", err);
    } finally {
      tilesFetchInFlight = null;
    }
  })();
  return tilesFetchInFlight;
}

// Central gesture handler — one place so canvas taps, hint buttons, and
// the space key all route through the same logic. Mirrors the firmware's
// three-position dispatcher: Radar → Weather → Cockpit → Radar. Any
// single/double tap from a non-radar view returns to radar.
function handleGesture(tap: Tap): void {
  if (state.view === "weather") {
    if (tap === "triple") enterCockpit();
    else                  setView("radar");
    return;
  }
  if (state.view === "cockpit") {
    setView("radar");
    return;
  }
  if (tap === "single") cycleRange();
  else if (tap === "double") cycleFocus();
  else if (tap === "triple") enterWeather();
}

function startNonRadarTicker(): void {
  if (nonRadarTicker !== null) clearInterval(nonRadarTicker);
  nonRadarTicker = window.setInterval(() => requestFrame(), 1000);
}

function enterWeather(): void {
  setView("weather");
  if (indexData) {
    rebuildStations(indexData.airportIndex, state.metar.centerLat,
                    state.metar.centerLon, state.metar.radiusNm);
  }
  void ensureTiles();
  refreshIfStale().then(() => requestFrame()).catch(() => { /* no-op */ });
  startNonRadarTicker();
}

function enterCockpit(): void {
  setView("cockpit");
  refreshOutdoorTemp().then(() => requestFrame()).catch(() => { /* no-op */ });
  startNonRadarTicker();
}

// Clear the ticker whenever we leave a non-radar view. Subscribed to
// state changes so it works regardless of what caused the exit.
subscribe(() => {
  if (state.view === "radar" && nonRadarTicker !== null) {
    clearInterval(nonRadarTicker);
    nonRadarTicker = null;
  }
  void ensureTiles();
  requestFrame();
});

// If the user edits the METAR center/radius in settings, rebuild the
// station pool against the new box and clear the cache so the next
// weather-view entry refetches. Cheap enough to run on every state
// change (rebuild is O(airport_index size) ≈ 800 ops).
let lastMetarKey = "";
subscribe(() => {
  if (!indexData) return;
  const key = `${state.metar.centerLat}:${state.metar.centerLon}:${state.metar.radiusNm}`;
  if (key === lastMetarKey) return;
  lastMetarKey = key;
  rebuildStations(indexData.airportIndex, state.metar.centerLat,
                  state.metar.centerLon, state.metar.radiusNm);
  invalidateMetar();
  // If we're currently on the weather view, kick a fresh fetch now.
  if (state.view === "weather") {
    refreshIfStale().then(() => requestFrame()).catch(() => { /* no-op */ });
  }
});

// Aircraft fetch loop. Fires immediately on center/range change plus
// every 3 s while the radar view is active. Skipped in non-radar modes.
let aircraftFetchInFlight = false;
let modeFlipTimer: number | null = null;
async function pollAircraft(): Promise<void> {
  if (state.view !== "radar") return;
  if (aircraftFetchInFlight) return;
  aircraftFetchInFlight = true;
  try {
    const nm = RANGE_PRESETS[state.rangeIdx].nm * 1.1;
    await fetchAircraft(state.centerLat, state.centerLon, nm);
    requestFrame();
    if (modeFlipTimer !== null) clearTimeout(modeFlipTimer);
    modeFlipTimer = window.setTimeout(() => requestFrame(), 1500);
  } finally {
    aircraftFetchInFlight = false;
  }
}
subscribe(() => { void pollAircraft(); });
setInterval(() => { void pollAircraft(); }, 3000);

function mountCanvasGestures(canvas: HTMLCanvasElement): void {
  const disc = makeTapDiscriminator(handleGesture);
  canvas.addEventListener("click", () => disc.tap());
  canvas.style.setProperty("-webkit-tap-highlight-color", "transparent");
}

function mountHintButtons(): void {
  const buttons = document.querySelectorAll<HTMLButtonElement>("button.hint");
  for (const btn of buttons) {
    btn.addEventListener("click", () => {
      const g = btn.dataset.gesture;
      if (g === "single" || g === "double" || g === "triple") {
        handleGesture(g);
      }
    });
  }
}

function mountKeyboardShortcuts(): void {
  const disc = makeTapDiscriminator(handleGesture);
  window.addEventListener("keydown", (e) => {
    if (e.target instanceof HTMLInputElement) return;
    if (e.key === " " || e.code === "Space") {
      e.preventDefault();
      disc.tap();
    }
  });
}

async function init(): Promise<void> {
  requestFrame();  // "loading map…"

  const canvas = document.getElementById("radar") as HTMLCanvasElement | null;
  if (canvas) mountCanvasGestures(canvas);
  mountHintButtons();
  mountKeyboardShortcuts();

  try {
    indexData = await loadIndexData("data");
  } catch (err) {
    console.error(err);
    const canvas2 = document.getElementById("radar") as HTMLCanvasElement | null;
    const ctx = canvas2?.getContext("2d");
    if (ctx) drawLoadingState(ctx, "map load failed");
    return;
  }

  mountSettings(indexData.airportIndex);
  await ensureTiles();
  requestFrame();

  // URL hooks — kept for direct-link testing / shareable views.
  //   ?view=weather | cockpit    boot straight into that view
  const qs = new URLSearchParams(location.search);
  if (qs.get("view") === "weather") enterWeather();
  else if (qs.get("view") === "cockpit") enterCockpit();
}

if (document.readyState === "loading") {
  document.addEventListener("DOMContentLoaded", init);
} else {
  init();
}
