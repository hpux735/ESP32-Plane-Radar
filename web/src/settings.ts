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
  resetAllSettings,
  type FocusPoint,
} from "./state";

function inject(): { dialog: HTMLDialogElement; openBtn: HTMLButtonElement } {
  const openBtn = document.createElement("button");
  openBtn.type = "button";
  openBtn.className = "settings-open";
  openBtn.textContent = "⚙ Settings";
  openBtn.title = "Configure home, METAR map center, and focus airports";

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
        <div class="row">
          <label>Latitude<input type="number" step="0.000001" name="home_lat" required></label>
          <label>Longitude<input type="number" step="0.000001" name="home_lon" required></label>
        </div>
      </section>

      <section>
        <h3>METAR flight-rules map</h3>
        <p class="hint">Center + radius for the airport dots view.</p>
        <div class="row">
          <label>Center lat<input type="number" step="0.000001" name="metar_lat" required></label>
          <label>Center lon<input type="number" step="0.000001" name="metar_lon" required></label>
          <label>Radius (nm)<input type="number" step="0.1" min="1" name="metar_rad" required></label>
        </div>
      </section>

      <section>
        <h3>Focus airports</h3>
        <p class="hint">Cycled by double-tap. The first row is Home (uses the location above).</p>
        <table class="focus-table">
          <thead>
            <tr><th>Label</th><th>Lat</th><th>Lon</th><th>Range</th><th></th></tr>
          </thead>
          <tbody></tbody>
        </table>
        <button type="button" class="focus-add">+ Add airport</button>
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

function renderFocusRows(tbody: HTMLTableSectionElement, ring: FocusPoint[]): void {
  tbody.innerHTML = "";
  ring.forEach((fp, i) => {
    const tr = document.createElement("tr");
    tr.dataset.idx = String(i);
    tr.innerHTML = `
      <td><input class="fp-label" type="text" maxlength="14" value="${escape(fp.label)}" ${fp.isHome ? "readonly" : ""}></td>
      <td><input class="fp-lat"   type="number" step="0.000001" value="${fp.lat}" ${fp.isHome ? "readonly" : ""}></td>
      <td><input class="fp-lon"   type="number" step="0.000001" value="${fp.lon}" ${fp.isHome ? "readonly" : ""}></td>
      <td>
        <select class="fp-range">
          ${["5nm", "10nm", "15nm", "25nm"].map((lbl, idx) =>
            `<option value="${idx}"${idx === fp.defaultRangeIdx ? " selected" : ""}>${lbl}</option>`
          ).join("")}
        </select>
      </td>
      <td>${fp.isHome ? "" : '<button type="button" class="fp-remove" aria-label="Remove">&times;</button>'}</td>
    `;
    tbody.appendChild(tr);
  });
}

function escape(s: string): string {
  return s.replace(/[&<>"']/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c] as string));
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

function populate(form: HTMLFormElement): void {
  (form.elements.namedItem("home_lat") as HTMLInputElement).value = String(state.home.lat);
  (form.elements.namedItem("home_lon") as HTMLInputElement).value = String(state.home.lon);
  (form.elements.namedItem("metar_lat") as HTMLInputElement).value = String(state.metar.centerLat);
  (form.elements.namedItem("metar_lon") as HTMLInputElement).value = String(state.metar.centerLon);
  (form.elements.namedItem("metar_rad") as HTMLInputElement).value = String(state.metar.radiusNm);
  const tbody = form.querySelector<HTMLTableSectionElement>(".focus-table tbody")!;
  renderFocusRows(tbody, state.focusRing);
}

export function mountSettings(): void {
  const { dialog, openBtn } = inject();
  const form = dialog.querySelector<HTMLFormElement>(".settings-form")!;
  const tbody = form.querySelector<HTMLTableSectionElement>(".focus-table tbody")!;

  openBtn.addEventListener("click", () => {
    populate(form);
    dialog.showModal();
  });

  dialog.querySelector(".settings-close")!.addEventListener("click", () => dialog.close());
  dialog.querySelector(".settings-cancel")!.addEventListener("click", () => dialog.close());

  dialog.querySelector(".focus-add")!.addEventListener("click", () => {
    const rows = readFocusRows(tbody);
    rows.push({
      label: "NEW",
      lat: state.home.lat,
      lon: state.home.lon,
      defaultRangeIdx: 1,
      isHome: false,
    });
    renderFocusRows(tbody, rows);
  });

  tbody.addEventListener("click", (e) => {
    const target = e.target as HTMLElement;
    if (!target.classList.contains("fp-remove")) return;
    const tr = target.closest("tr");
    if (tr) tr.remove();
  });

  dialog.querySelector(".settings-reset")!.addEventListener("click", () => {
    if (confirm("Reset home, METAR map, and focus airports to defaults?")) {
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
