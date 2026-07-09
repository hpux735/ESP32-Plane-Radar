// Plane Radar — web preview entry point.

import "./style.css";
import { loadMapData, type MapData } from "./data";
import { renderFrame } from "./renderer";
import {
  state,
  subscribe,
  cycleRange,
  cycleFocus,
  toggleLayer,
  setView,
  setCenter,
  type LayerId,
} from "./state";
import { makeTapDiscriminator, type Tap } from "./input";
import { mountTypeahead } from "./airports";
import { drawWeatherView } from "./weatherView";
import { refreshIfStale } from "./weather";
import { fetchAircraft } from "./aircraft";
import { RANGE_PRESETS } from "./theme";

interface LayerDef {
  id: LayerId;
  label: string;
}

const LAYERS: LayerDef[] = [
  { id: "coast", label: "Coast" },
  { id: "land", label: "Land" },
  { id: "roads", label: "Roads" },
  { id: "runways", label: "Runways" },
  { id: "tags", label: "Tags" },
];

let mapData: MapData | null = null;
// While in weather mode, we repaint once/second so the "n min ago" age
// indicator updates. Cleared when leaving weather.
let weatherTicker: number | null = null;

function drawLoadingState(ctx: CanvasRenderingContext2D, msg: string): void {
  ctx.fillStyle = "rgb(12, 20, 36)";
  ctx.fillRect(0, 0, 240, 240);
  ctx.fillStyle = "rgb(122, 134, 173)";
  ctx.font = "10px system-ui, sans-serif";
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillText(msg, 120, 120);
}

let frameQueued = false;
function requestFrame(): void {
  if (frameQueued) return;
  frameQueued = true;
  requestAnimationFrame(() => {
    frameQueued = false;
    const canvas = document.getElementById("radar") as HTMLCanvasElement | null;
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    if (!mapData) {
      drawLoadingState(ctx, "loading map…");
      return;
    }
    if (state.view === "weather") {
      drawWeatherView(ctx, mapData);
    } else {
      renderFrame(ctx, mapData);
    }
  });
}

function mountLayerToggles(root: HTMLElement): void {
  for (const l of LAYERS) {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.textContent = l.label;
    btn.setAttribute("aria-pressed", String(state.layers[l.id]));
    btn.addEventListener("click", () => {
      const on = toggleLayer(l.id);
      btn.setAttribute("aria-pressed", String(on));
    });
    root.appendChild(btn);
  }
}

// Central gesture handler — one place so canvas taps, hint buttons, and
// the space key all route through the same logic.
function handleGesture(tap: Tap): void {
  if (state.view === "weather") {
    // In weather mode, any tap returns to radar. Triple re-enters
    // weather (and forces a refresh).
    if (tap === "triple") {
      enterWeather();
    } else {
      setView("radar");
    }
    return;
  }
  if (tap === "single") cycleRange();
  else if (tap === "double") cycleFocus();
  else if (tap === "triple") enterWeather();
}

function enterWeather(): void {
  setView("weather");
  // Kick a background fetch; the render will update whenever it lands.
  refreshIfStale().then(() => requestFrame()).catch(() => { /* no-op */ });
  // Repaint every second so the freshness label ticks up.
  if (weatherTicker !== null) clearInterval(weatherTicker);
  weatherTicker = window.setInterval(() => requestFrame(), 1000);
}

// Clear the weather ticker whenever we leave that view. Subscribed to
// state changes so it works regardless of what caused the exit.
subscribe(() => {
  if (state.view !== "weather" && weatherTicker !== null) {
    clearInterval(weatherTicker);
    weatherTicker = null;
  }
  requestFrame();
});

// Aircraft fetch loop. Fires immediately on center/range change plus
// every 3 s while the radar view is active. Skipped in weather mode.
let aircraftFetchInFlight = false;
async function pollAircraft(): Promise<void> {
  if (state.view !== "radar") return;
  if (aircraftFetchInFlight) return;
  aircraftFetchInFlight = true;
  try {
    // Fetch a slightly wider radius than the visible ring so a plane
    // arriving from beyond the edge appears smoothly.
    const nm = RANGE_PRESETS[state.rangeIdx].nm * 1.1;
    await fetchAircraft(state.centerLat, state.centerLon, nm);
    requestFrame();
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
    if (e.target instanceof HTMLInputElement) return;  // don't hijack typing
    if (e.key === " " || e.code === "Space") {
      e.preventDefault();
      disc.tap();
    } else if (["1", "2", "3", "4", "5"].includes(e.key)) {
      const idx = Number(e.key) - 1;
      const id = LAYERS[idx]?.id;
      if (id) {
        toggleLayer(id);
        const btns = document.querySelectorAll("#layer-toggles button");
        const btn = btns[idx] as HTMLButtonElement | undefined;
        if (btn) btn.setAttribute("aria-pressed", String(state.layers[id]));
      }
    }
  });
}

async function init(): Promise<void> {
  requestFrame();  // "loading map…"

  const canvas = document.getElementById("radar") as HTMLCanvasElement | null;
  const layerRoot = document.getElementById("layer-toggles");
  const search = document.getElementById("airport-search") as HTMLInputElement | null;
  const results = document.getElementById("airport-results");

  if (canvas) mountCanvasGestures(canvas);
  if (layerRoot) mountLayerToggles(layerRoot);
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

  requestFrame();

  if (search && results && mapData) {
    mountTypeahead({
      input: search,
      results,
      index: mapData.airportIndex,
      onSelected: () => requestFrame(),
    });
  }

  // URL hooks (mostly for testing + shareable links):
  //   ?view=weather              boot straight into the weather view
  //   ?apt=KJFK                  center on this ICAO from the airport table
  //   ?lat=40.6&lon=-73.7        raw center override
  const qs = new URLSearchParams(location.search);
  const apt = qs.get("apt");
  if (apt && mapData?.airports[apt]) {
    const a = mapData.airports[apt];
    setCenter(a.lat, a.lon, apt);
  } else {
    const lat = parseFloat(qs.get("lat") ?? "");
    const lon = parseFloat(qs.get("lon") ?? "");
    if (isFinite(lat) && isFinite(lon)) {
      setCenter(lat, lon, "?");
    }
  }
  if (qs.get("view") === "weather") enterWeather();
}

if (document.readyState === "loading") {
  document.addEventListener("DOMContentLoaded", init);
} else {
  init();
}
