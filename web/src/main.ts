// Plane Radar — web preview entry point.

import "./style.css";
import { loadMapData, type MapData } from "./data";
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

let mapData: MapData | null = null;
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
    if (!mapData) {
      drawLoadingState(bctx, "loading map…");
    } else if (state.view === "weather") {
      drawWeatherView(bctx, mapData);
    } else if (state.view === "cockpit") {
      drawCockpitView(bctx);
    } else {
      renderFrame(bctx, mapData);
    }
    // Single blit — the visible canvas never shows a partial frame.
    visible.clearRect(0, 0, 240, 240);
    visible.drawImage(buf, 0, 0);
  });
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
  // Make sure the station list matches the current METAR center before
  // firing the fetch. rebuildStations() is idempotent; if the config
  // hasn't moved since last entry, this is a no-op.
  if (mapData) {
    rebuildStations(mapData.airportIndex, state.metar.centerLat,
                    state.metar.centerLon, state.metar.radiusNm);
  }
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
  requestFrame();
});

// If the user edits the METAR center/radius in settings, rebuild the
// station pool against the new box and clear the cache so the next
// weather-view entry refetches. Cheap enough to run on every state
// change (rebuild is O(airport_index size) ≈ 800 ops).
let lastMetarKey = "";
subscribe(() => {
  if (!mapData) return;
  const key = `${state.metar.centerLat}:${state.metar.centerLon}:${state.metar.radiusNm}`;
  if (key === lastMetarKey) return;
  lastMetarKey = key;
  rebuildStations(mapData.airportIndex, state.metar.centerLat,
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
    mapData = await loadMapData("data");
  } catch (err) {
    console.error(err);
    const canvas2 = document.getElementById("radar") as HTMLCanvasElement | null;
    const ctx = canvas2?.getContext("2d");
    if (ctx) drawLoadingState(ctx, "map load failed");
    return;
  }

  mountSettings(mapData.airportIndex);
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
