// Settings overlay — lets the user edit the home location, the METAR
// map's center + radius, and the focus airport ring. Persists via the
// mutators in state.ts (localStorage). Rendered as a modal <dialog> so
// it takes over the screen on mobile without disrupting the radar view
// underneath.

import {
  state,
  saveHome,
  saveMetar,
  saveFocusRing,
  toggleLayer,
  resetAllSettings,
  type FocusPoint,
  type LayerId,
} from "./state";
import { search as airportSearch } from "./airports";
import type { AirportIndexRow } from "./data";
import { lookup as geocodeLookup, type GeocodeHit } from "./geocode";

interface LayerDef { id: LayerId; label: string; }
const LAYERS: LayerDef[] = [
  { id: "coast",   label: "Coast" },
  { id: "land",    label: "Land" },
  { id: "runways", label: "Runways" },
  { id: "tags",    label: "Tags" },
];

const RANGE_LABELS = ["5nm", "10nm", "15nm", "25nm"];

// Must match config::kMaxFocusAirports in include/config.h and MAX_AIRPORTS
// in the LAN portal customization JS. Home occupies slot 0 of the rows
// table, so a full ring is MAX_FOCUS_AIRPORTS + 1 = 7 rows.
const MAX_FOCUS_AIRPORTS = 6;

function inject(): { dialog: HTMLDialogElement; openBtn: HTMLButtonElement } {
  const openBtn = document.createElement("button");
  openBtn.type = "button";
  openBtn.className = "settings-open";
  openBtn.innerHTML = "<span class=\"gear\">⚙</span> <span>Settings</span>";
  openBtn.title = "Configure home, METAR map, focus airports, and layers";

  const dialog = document.createElement("dialog");
  dialog.className = "settings-dialog";
  dialog.innerHTML = `
    <form method="dialog" class="settings-form">
      <header>
        <h2>Settings</h2>
        <button type="button" class="settings-close" aria-label="Close">&times;</button>
      </header>

      <section>
        <h3>Home</h3>
        <p class="hint">Radar center + outdoor-temperature reference.</p>
        <div class="airport-search-row">
          <label>Center on airport
            <input type="text" class="airport-search" data-target="home" placeholder="type SFO / palo alto…" autocomplete="off">
            <ul class="airport-suggest" role="listbox" hidden></ul>
          </label>
        </div>
        <div class="row">
          <label>Latitude<input type="number" step="0.000001" name="home_lat" required></label>
          <label>Longitude<input type="number" step="0.000001" name="home_lon" required></label>
        </div>
      </section>

      <section>
        <h3>METAR flight-rules map</h3>
        <p class="hint">Center + radius for the airport dots view.</p>
        <div class="airport-search-row">
          <label>Center on airport
            <input type="text" class="airport-search" data-target="metar" placeholder="type SFO / palo alto…" autocomplete="off">
            <ul class="airport-suggest" role="listbox" hidden></ul>
          </label>
        </div>
        <div class="row">
          <label>Center lat<input type="number" step="0.000001" name="metar_lat" required></label>
          <label>Center lon<input type="number" step="0.000001" name="metar_lon" required></label>
          <label>Radius (nm)<input type="number" step="0.1" min="1" name="metar_rad" required></label>
        </div>
      </section>

      <section>
        <h3>Focus airports</h3>
        <p class="hint">Cycled by double-tap. Type an airport code in the Label
          field to autofill lat/lon and default range. First row is Home.</p>
        <table class="focus-table">
          <thead>
            <tr><th>Label</th><th>Lat</th><th>Lon</th><th>Default Range</th><th></th></tr>
          </thead>
          <tbody></tbody>
        </table>
        <button type="button" class="focus-add">+ Add airport</button>
      </section>

      <section>
        <h3>Map layers</h3>
        <p class="hint">What's drawn behind the aircraft on the radar view.</p>
        <div class="layer-toggles"></div>
      </section>

      <footer>
        <button type="button" class="settings-reset">Reset all to defaults</button>
        <div>
          <button type="button" class="settings-cancel">Cancel</button>
          <button type="submit" class="settings-save">Save</button>
        </div>
      </footer>
    </form>
  `;

  document.body.appendChild(openBtn);
  document.body.appendChild(dialog);
  return { dialog, openBtn };
}

function escape(s: string): string {
  return s.replace(/[&<>"']/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c] as string));
}

function renderFocusRows(tbody: HTMLTableSectionElement, ring: FocusPoint[]): void {
  tbody.innerHTML = "";
  ring.forEach((fp, i) => {
    const tr = document.createElement("tr");
    tr.dataset.idx = String(i);
    const labelCell = fp.isHome
      ? `<input class="fp-label" type="text" value="Home" readonly>`
      : `<div class="fp-label-wrap">
           <input class="fp-label" type="text" maxlength="14" value="${escape(fp.label)}" placeholder="ICAO / city">
           <ul class="airport-suggest" role="listbox" hidden></ul>
         </div>`;
    tr.innerHTML = `
      <td>${labelCell}</td>
      <td><input class="fp-lat" type="number" step="0.000001" value="${fp.lat}" ${fp.isHome ? "readonly" : ""}></td>
      <td><input class="fp-lon" type="number" step="0.000001" value="${fp.lon}" ${fp.isHome ? "readonly" : ""}></td>
      <td>
        <select class="fp-range">
          ${RANGE_LABELS.map((lbl, idx) =>
            `<option value="${idx}"${idx === fp.defaultRangeIdx ? " selected" : ""}>${lbl}</option>`
          ).join("")}
        </select>
      </td>
      <td>${fp.isHome ? "" : '<button type="button" class="fp-remove" aria-label="Remove">&times;</button>'}</td>
    `;
    tbody.appendChild(tr);
  });
}

function readFocusRows(tbody: HTMLTableSectionElement): FocusPoint[] {
  const out: FocusPoint[] = [];
  for (const tr of Array.from(tbody.querySelectorAll<HTMLTableRowElement>("tr"))) {
    const labelInput = tr.querySelector(".fp-label") as HTMLInputElement | null;
    const latInput   = tr.querySelector(".fp-lat")   as HTMLInputElement | null;
    const lonInput   = tr.querySelector(".fp-lon")   as HTMLInputElement | null;
    const rangeSel   = tr.querySelector(".fp-range") as HTMLSelectElement | null;
    const label = (labelInput?.value ?? "").trim();
    const lat = parseFloat(latInput?.value ?? "");
    const lon = parseFloat(lonInput?.value ?? "");
    const rangeIdx = parseInt(rangeSel?.value ?? "1", 10);
    const isHome = tr.dataset.idx === "0";
    if (!label || !isFinite(lat) || !isFinite(lon)) continue;
    out.push({
      label,
      lat,
      lon,
      defaultRangeIdx: isFinite(rangeIdx) ? rangeIdx : 1,
      isHome,
    });
  }
  return out;
}

function renderLayerToggles(root: HTMLElement): void {
  root.innerHTML = "";
  for (const l of LAYERS) {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "layer-toggle";
    btn.textContent = l.label;
    btn.setAttribute("aria-pressed", String(state.layers[l.id]));
    btn.addEventListener("click", (e) => {
      e.preventDefault();
      const on = toggleLayer(l.id);
      btn.setAttribute("aria-pressed", String(on));
    });
    root.appendChild(btn);
  }
}

function populate(form: HTMLFormElement): void {
  (form.elements.namedItem("home_lat") as HTMLInputElement).value = String(state.home.lat);
  (form.elements.namedItem("home_lon") as HTMLInputElement).value = String(state.home.lon);
  (form.elements.namedItem("metar_lat") as HTMLInputElement).value = String(state.metar.centerLat);
  (form.elements.namedItem("metar_lon") as HTMLInputElement).value = String(state.metar.centerLon);
  (form.elements.namedItem("metar_rad") as HTMLInputElement).value = String(state.metar.radiusNm);
  const tbody = form.querySelector<HTMLTableSectionElement>(".focus-table tbody")!;
  renderFocusRows(tbody, state.focusRing);
  const layerRoot = form.querySelector<HTMLElement>(".layer-toggles")!;
  renderLayerToggles(layerRoot);
  form.querySelectorAll<HTMLInputElement>(".airport-search").forEach((el) => {
    el.value = "";
  });
}

// --- Airport typeahead ---------------------------------------------------
// Shared across the Home + METAR "Center on airport" boxes and each
// focus row's Label field. Attaches to any input, looks up matches via
// airports.ts, and calls `onPick` with the chosen airport row.

// Optional hook — when supplied, an address-typed pick writes lat/lon
// directly (bypassing the airport `onPick` path). If omitted, address
// suggestions are hidden for this input (e.g., focus-row Label inputs
// that don't need address support).
interface AirportSelectionHandler {
  input: HTMLInputElement;
  suggestList: HTMLUListElement;
  index: AirportIndexRow[];
  onPick: (row: AirportIndexRow) => void;
  onAddressPick?: (hit: GeocodeHit) => void;
}

const GEOCODE_MIN_CHARS = 4;
const GEOCODE_DEBOUNCE_MS = 300;

function mountAirportTypeahead(opts: AirportSelectionHandler): void {
  const { input, suggestList, index, onPick, onAddressPick } = opts;
  let currentAirports: AirportIndexRow[] = [];
  let currentAddresses: GeocodeHit[] = [];
  let debounceTimer: number | null = null;
  let inFlight: AbortController | null = null;
  // Track the query the last geocode results correspond to so a stale
  // response can't overwrite fresh airport-only results after the user
  // shortened the query below GEOCODE_MIN_CHARS.
  let geocodeQuery = "";

  function render() {
    suggestList.innerHTML = "";
    for (const row of currentAirports) {
      const [icao, iata, city, name] = row;
      const li = document.createElement("li");
      li.setAttribute("role", "option");
      li.innerHTML =
        `<span class="icao">${escape(icao)}${iata ? " · " + escape(iata) : ""}</span>` +
        `${city ? escape(city) + " — " : ""}${escape(name)}`;
      li.addEventListener("mousedown", (e) => {
        e.preventDefault();
        pickAirport(row);
      });
      suggestList.appendChild(li);
    }
    if (onAddressPick) {
      for (const hit of currentAddresses) {
        const li = document.createElement("li");
        li.setAttribute("role", "option");
        li.innerHTML =
          `<span class="icao">📍</span>${escape(hit.displayName)}`;
        li.addEventListener("mousedown", (e) => {
          e.preventDefault();
          pickAddress(hit);
        });
        suggestList.appendChild(li);
      }
    }
    suggestList.hidden =
      currentAirports.length === 0 && currentAddresses.length === 0;
  }

  function pickAirport(row: AirportIndexRow) {
    onPick(row);
    suggestList.hidden = true;
  }

  function pickAddress(hit: GeocodeHit) {
    if (onAddressPick) onAddressPick(hit);
    suggestList.hidden = true;
  }

  input.addEventListener("input", () => {
    const q = input.value.trim();
    currentAirports = airportSearch(index, q);
    // Airport results render immediately every keystroke. Address
    // results trail behind on debounce; blank them now so stale hits
    // don't mix with the fresh airport pass.
    if (q.length < GEOCODE_MIN_CHARS || q !== geocodeQuery) {
      currentAddresses = [];
    }
    render();

    if (!onAddressPick) return;
    if (debounceTimer !== null) window.clearTimeout(debounceTimer);
    if (inFlight) { inFlight.abort(); inFlight = null; }
    if (q.length < GEOCODE_MIN_CHARS) return;
    debounceTimer = window.setTimeout(() => {
      const ctrl = new AbortController();
      inFlight = ctrl;
      const queryAtRequest = q;
      geocodeLookup(queryAtRequest, ctrl.signal)
        .then((hits) => {
          // Ignore if the user has typed since the request fired.
          if (input.value.trim() !== queryAtRequest) return;
          geocodeQuery = queryAtRequest;
          currentAddresses = hits;
          render();
        })
        .catch(() => { /* aborted or transient — silent */ });
    }, GEOCODE_DEBOUNCE_MS);
  });
  input.addEventListener("focus", () => {
    if (currentAirports.length > 0 || currentAddresses.length > 0) {
      suggestList.hidden = false;
    }
  });
  input.addEventListener("blur", () => {
    setTimeout(() => (suggestList.hidden = true), 120);
  });
  input.addEventListener("keydown", (e) => {
    if (e.key === "Enter") {
      if (currentAirports[0]) {
        e.preventDefault();
        pickAirport(currentAirports[0]);
      } else if (currentAddresses[0]) {
        e.preventDefault();
        pickAddress(currentAddresses[0]);
      }
    } else if (e.key === "Escape") {
      suggestList.hidden = true;
      input.blur();
    }
  });
}

// Default range preset per airport: 3nm bucket 0 (5nm) for small GA
// fields, 1 (10nm) for anything with an IATA (proxy for scheduled
// airline service). Best-effort — AirportIndexRow doesn't carry tier.
function inferDefaultRange(row: AirportIndexRow): number {
  const [, iata] = row;
  return iata ? 1 : 0;
}

export function mountSettings(airportIndex: AirportIndexRow[]): void {
  const { dialog, openBtn } = inject();
  const form = dialog.querySelector<HTMLFormElement>(".settings-form")!;
  const tbody = form.querySelector<HTMLTableSectionElement>(".focus-table tbody")!;

  // Mirror the current Home lat/lon inputs into the focus-ring Home row
  // (readonly Lat/Lon cells at data-idx="0"). Called on every keystroke
  // in the Home number inputs and on typeahead pick so the focus table
  // reflects the draft value before Save.
  function syncFocusHomeRow(): void {
    const homeLat = (form.elements.namedItem("home_lat") as HTMLInputElement)?.value;
    const homeLon = (form.elements.namedItem("home_lon") as HTMLInputElement)?.value;
    const tr = tbody.querySelector<HTMLTableRowElement>('tr[data-idx="0"]');
    if (!tr) return;
    const latIn = tr.querySelector<HTMLInputElement>(".fp-lat");
    const lonIn = tr.querySelector<HTMLInputElement>(".fp-lon");
    if (latIn && homeLat !== undefined) latIn.value = homeLat;
    if (lonIn && homeLon !== undefined) lonIn.value = homeLon;
  }
  (form.elements.namedItem("home_lat") as HTMLInputElement)
    ?.addEventListener("input", syncFocusHomeRow);
  (form.elements.namedItem("home_lon") as HTMLInputElement)
    ?.addEventListener("input", syncFocusHomeRow);

  // Home + METAR "Center on airport" boxes. Both accept ICAO/IATA (via
  // airport typeahead) AND street addresses (via geocode).
  //
  // Round lat/lon to 6 decimal places (~11 cm precision) so writes match
  // the number inputs' step=0.000001 constraint — HTML5 form validation
  // otherwise rejects Nominatim's 7-decimal responses with "Please enter
  // a valid value".
  const fmtCoord = (n: number) => n.toFixed(6);
  for (const input of Array.from(form.querySelectorAll<HTMLInputElement>(".airport-search"))) {
    const target = input.dataset.target;
    const suggest = input.nextElementSibling as HTMLUListElement;
    mountAirportTypeahead({
      input,
      suggestList: suggest,
      index: airportIndex,
      onPick(row) {
        const [icao, , , , lat, lon] = row;
        if (target === "home") {
          (form.elements.namedItem("home_lat") as HTMLInputElement).value = fmtCoord(lat);
          (form.elements.namedItem("home_lon") as HTMLInputElement).value = fmtCoord(lon);
          syncFocusHomeRow();
        } else if (target === "metar") {
          (form.elements.namedItem("metar_lat") as HTMLInputElement).value = fmtCoord(lat);
          (form.elements.namedItem("metar_lon") as HTMLInputElement).value = fmtCoord(lon);
        }
        input.value = icao;
      },
      onAddressPick(hit) {
        if (target === "home") {
          (form.elements.namedItem("home_lat") as HTMLInputElement).value = fmtCoord(hit.lat);
          (form.elements.namedItem("home_lon") as HTMLInputElement).value = fmtCoord(hit.lon);
          syncFocusHomeRow();
        } else if (target === "metar") {
          (form.elements.namedItem("metar_lat") as HTMLInputElement).value = fmtCoord(hit.lat);
          (form.elements.namedItem("metar_lon") as HTMLInputElement).value = fmtCoord(hit.lon);
        }
        // Nominatim formats house-numbered addresses as
        // "2125, Bryant Street, Inner Mission, San Francisco, …" —
        // splitting on the first comma alone would leave just "2125"
        // in the input. Keep the first ~2 parts so it reads as a
        // recognizable street address without swallowing the whole
        // administrative tail.
        const parts = hit.displayName.split(",").map((s) => s.trim()).filter(Boolean);
        input.value = parts.slice(0, 2).join(" ") || hit.displayName;
      },
    });
  }

  // Focus row label typeahead. Called after every row render.
  function wireRowTypeahead(tr: HTMLTableRowElement): void {
    const wrap = tr.querySelector<HTMLDivElement>(".fp-label-wrap");
    if (!wrap) return;   // Home row (readonly) has no wrap.
    const labelInput = wrap.querySelector<HTMLInputElement>(".fp-label")!;
    const suggest = wrap.querySelector<HTMLUListElement>(".airport-suggest")!;
    const latInput = tr.querySelector<HTMLInputElement>(".fp-lat")!;
    const lonInput = tr.querySelector<HTMLInputElement>(".fp-lon")!;
    const rangeSel = tr.querySelector(".fp-range") as unknown as HTMLSelectElement;
    mountAirportTypeahead({
      input: labelInput,
      suggestList: suggest,
      index: airportIndex,
      onPick(row) {
        const [icao, , , , lat, lon] = row;
        labelInput.value = icao;
        latInput.value = String(lat);
        lonInput.value = String(lon);
        rangeSel.value = String(inferDefaultRange(row));
      },
    });
  }
  function rewireAllRows() {
    for (const tr of Array.from(tbody.querySelectorAll<HTMLTableRowElement>("tr"))) {
      wireRowTypeahead(tr);
    }
  }

  openBtn.addEventListener("click", () => {
    populate(form);
    rewireAllRows();
    updateFocusCap();
    dialog.showModal();
  });

  dialog.querySelector(".settings-close")!.addEventListener("click", () => dialog.close());
  dialog.querySelector(".settings-cancel")!.addEventListener("click", () => dialog.close());

  const focusAddBtn = dialog.querySelector<HTMLButtonElement>(".focus-add")!;
  const focusCapHint = document.createElement("p");
  focusCapHint.className = "hint";
  focusCapHint.style.marginTop = "4px";
  focusCapHint.style.display = "none";
  focusCapHint.textContent = `Max ${MAX_FOCUS_AIRPORTS} focus airports — remove one to add another.`;
  focusAddBtn.insertAdjacentElement("afterend", focusCapHint);

  function updateFocusCap(): void {
    const airportCount = readFocusRows(tbody).filter(r => !r.isHome).length;
    const atCap = airportCount >= MAX_FOCUS_AIRPORTS;
    focusAddBtn.disabled = atCap;
    focusCapHint.style.display = atCap ? "block" : "none";
  }

  focusAddBtn.addEventListener("click", () => {
    const rows = readFocusRows(tbody);
    const airportCount = rows.filter(r => !r.isHome).length;
    if (airportCount >= MAX_FOCUS_AIRPORTS) {
      updateFocusCap();
      return;
    }
    rows.push({
      label: "",
      lat: state.home.lat,
      lon: state.home.lon,
      defaultRangeIdx: 1,
      isHome: false,
    });
    renderFocusRows(tbody, rows);
    rewireAllRows();
    updateFocusCap();
  });

  tbody.addEventListener("click", (e) => {
    const target = e.target as HTMLElement;
    if (!target.classList.contains("fp-remove")) return;
    const tr = target.closest("tr");
    if (tr) {
      tr.remove();
      updateFocusCap();
    }
  });

  dialog.querySelector(".settings-reset")!.addEventListener("click", () => {
    if (confirm("Reset home, METAR map, focus airports, and map layers to defaults?")) {
      resetAllSettings();
    }
  });

  form.addEventListener("submit", (e) => {
    e.preventDefault();
    const homeLat = parseFloat((form.elements.namedItem("home_lat") as HTMLInputElement).value);
    const homeLon = parseFloat((form.elements.namedItem("home_lon") as HTMLInputElement).value);
    const metarLat = parseFloat((form.elements.namedItem("metar_lat") as HTMLInputElement).value);
    const metarLon = parseFloat((form.elements.namedItem("metar_lon") as HTMLInputElement).value);
    const metarRad = parseFloat((form.elements.namedItem("metar_rad") as HTMLInputElement).value);
    if (!isFinite(homeLat) || !isFinite(homeLon) || !isFinite(metarLat) ||
        !isFinite(metarLon) || !isFinite(metarRad) || metarRad <= 0) {
      alert("All fields must be finite numbers; radius must be > 0.");
      return;
    }
    saveHome({ lat: homeLat, lon: homeLon });
    saveMetar({ centerLat: metarLat, centerLon: metarLon, radiusNm: metarRad });
    saveFocusRing(readFocusRows(tbody));
    dialog.close();
  });
}
