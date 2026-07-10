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

interface LayerDef { id: LayerId; label: string; }
const LAYERS: LayerDef[] = [
  { id: "coast",   label: "Coast" },
  { id: "land",    label: "Land" },
  { id: "runways", label: "Runways" },
  { id: "tags",    label: "Tags" },
];

const RANGE_LABELS = ["5nm", "10nm", "15nm", "25nm"];

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

interface AirportSelectionHandler {
  input: HTMLInputElement;
  suggestList: HTMLUListElement;
  index: AirportIndexRow[];
  onPick: (row: AirportIndexRow) => void;
}

function mountAirportTypeahead(opts: AirportSelectionHandler): void {
  const { input, suggestList, index, onPick } = opts;
  let currentRows: AirportIndexRow[] = [];

  function render() {
    suggestList.innerHTML = "";
    for (const row of currentRows) {
      const [icao, iata, city, name] = row;
      const li = document.createElement("li");
      li.setAttribute("role", "option");
      li.innerHTML =
        `<span class="icao">${escape(icao)}${iata ? " · " + escape(iata) : ""}</span>` +
        `${city ? escape(city) + " — " : ""}${escape(name)}`;
      li.addEventListener("mousedown", (e) => {
        e.preventDefault();
        pick(row);
      });
      suggestList.appendChild(li);
    }
    suggestList.hidden = currentRows.length === 0;
  }

  function pick(row: AirportIndexRow) {
    onPick(row);
    suggestList.hidden = true;
  }

  input.addEventListener("input", () => {
    currentRows = airportSearch(index, input.value);
    render();
  });
  input.addEventListener("focus", () => {
    if (currentRows.length > 0) suggestList.hidden = false;
  });
  input.addEventListener("blur", () => {
    setTimeout(() => (suggestList.hidden = true), 120);
  });
  input.addEventListener("keydown", (e) => {
    if (e.key === "Enter" && currentRows[0]) {
      e.preventDefault();
      pick(currentRows[0]);
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

  // Home + METAR "Center on airport" boxes.
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
          (form.elements.namedItem("home_lat") as HTMLInputElement).value = String(lat);
          (form.elements.namedItem("home_lon") as HTMLInputElement).value = String(lon);
        } else if (target === "metar") {
          (form.elements.namedItem("metar_lat") as HTMLInputElement).value = String(lat);
          (form.elements.namedItem("metar_lon") as HTMLInputElement).value = String(lon);
        }
        input.value = icao;
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
    dialog.showModal();
  });

  dialog.querySelector(".settings-close")!.addEventListener("click", () => dialog.close());
  dialog.querySelector(".settings-cancel")!.addEventListener("click", () => dialog.close());

  dialog.querySelector(".focus-add")!.addEventListener("click", () => {
    const rows = readFocusRows(tbody);
    rows.push({
      label: "",
      lat: state.home.lat,
      lon: state.home.lon,
      defaultRangeIdx: 1,
      isHome: false,
    });
    renderFocusRows(tbody, rows);
    rewireAllRows();
  });

  tbody.addEventListener("click", (e) => {
    const target = e.target as HTMLElement;
    if (!target.classList.contains("fp-remove")) return;
    const tr = target.closest("tr");
    if (tr) tr.remove();
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
