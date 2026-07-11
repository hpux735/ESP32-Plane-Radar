import { beforeEach, describe, expect, it, vi } from "vitest";
import { mountSettings } from "./settings";
import { state } from "./state";
import type { AirportIndexRow } from "./data";

// Runs against happy-dom. See vitest.config.ts.
//
// These tests focus on the settings-dialog contract — dialog exists,
// values round-trip through form → state, typeahead resolves as
// expected, invalid input is rejected. Would have surfaced the "RGN
// resolves to Yangon" note in the plan as an actual documented
// behavior (the search is a lexical rank across the whole index; the
// user's typed prefix does NOT auto-favor their region).

// Global-covering airport index for the tests.
const INDEX: AirportIndexRow[] = [
  ["KSFO", "SFO", "San Francisco", "San Francisco Intl", 37.6188, -122.3750],
  ["KOAK", "OAK", "Oakland", "Oakland Intl", 37.7213, -122.2214],
  ["KRNO", "RNO", "Reno", "Reno-Tahoe Intl", 39.4990, -119.7681],
  ["VYYY", "RGN", "Yangon", "Yangon Intl", 16.9073, 96.1332],
  ["EGLL", "LHR", "London", "London Heathrow", 51.4706, -0.4619],
];

beforeEach(() => {
  document.body.innerHTML = "";
  state.home = { lat: 37.7552, lon: -122.4528 };
  state.metar = { centerLat: 37.661, centerLon: -122.160, radiusNm: 28 };
  state.focusRing = [
    { label: "Home", lat: 37.7552, lon: -122.4528, defaultRangeIdx: 1, isHome: true },
    { label: "SFO",  lat: 37.6188, lon: -122.3750, defaultRangeIdx: 1, isHome: false },
  ];
});

function openSettings() {
  mountSettings(INDEX);
  const openBtn = document.querySelector<HTMLButtonElement>("button.settings-open")!;
  openBtn.click();
  const dialog = document.querySelector<HTMLDialogElement>("dialog.settings-dialog")!;
  const form = dialog.querySelector<HTMLFormElement>(".settings-form")!;
  return { dialog, form };
}

describe("settings dialog — mount + open", () => {
  it("injects the gear button and dialog into the body", () => {
    mountSettings(INDEX);
    expect(document.querySelector("button.settings-open")).not.toBeNull();
    expect(document.querySelector("dialog.settings-dialog")).not.toBeNull();
  });

  it("populates form fields from state when opened", () => {
    const { form } = openSettings();
    expect((form.elements.namedItem("home_lat") as HTMLInputElement).value).toBe("37.7552");
    expect((form.elements.namedItem("home_lon") as HTMLInputElement).value).toBe("-122.4528");
    expect((form.elements.namedItem("metar_rad") as HTMLInputElement).value).toBe("28");
  });

  it("renders one row per focus point (including the home row)", () => {
    const { form } = openSettings();
    const rows = form.querySelectorAll(".focus-table tbody tr");
    expect(rows.length).toBe(state.focusRing.length);
  });
});

describe("airport typeahead", () => {
  it("resolves 'SFO' to KSFO (lexical match, not geographic)", () => {
    openSettings();
    const homeSearch = document.querySelector<HTMLInputElement>(
      ".airport-search[data-target='home']")!;
    homeSearch.value = "SFO";
    homeSearch.dispatchEvent(new Event("input"));
    const first = document.querySelector<HTMLLIElement>(
      ".airport-search[data-target='home'] + .airport-suggest li");
    expect(first?.textContent).toContain("SFO");
    expect(first?.textContent).toContain("San Francisco");
  });

  it("documents that 'RGN' resolves to Yangon (Myanmar), NOT Reno", () => {
    // Behavior lock — the search is a global lexical rank. A user typing
    // "RGN" gets Yangon because that's the actual IATA for RGN. If you
    // want Reno, type "RNO" or "KRNO". Plan intentionally called this
    // out; test guards against a silent behavior flip.
    openSettings();
    const homeSearch = document.querySelector<HTMLInputElement>(
      ".airport-search[data-target='home']")!;
    homeSearch.value = "RGN";
    homeSearch.dispatchEvent(new Event("input"));
    const first = document.querySelector<HTMLLIElement>(
      ".airport-search[data-target='home'] + .airport-suggest li");
    expect(first?.textContent).toContain("Yangon");
    expect(first?.textContent?.toLowerCase()).not.toContain("reno");
  });

  it("does not surface a suggestion for an unmatched query", () => {
    openSettings();
    const homeSearch = document.querySelector<HTMLInputElement>(
      ".airport-search[data-target='home']")!;
    homeSearch.value = "ZZZZ";
    homeSearch.dispatchEvent(new Event("input"));
    const suggest = document.querySelector<HTMLUListElement>(
      ".airport-search[data-target='home'] + .airport-suggest")!;
    expect(suggest.hidden).toBe(true);
  });
});

describe("form submit — validation + persistence", () => {
  it("saves valid input to state.home and state.metar", () => {
    const { form } = openSettings();
    (form.elements.namedItem("home_lat") as HTMLInputElement).value = "39.499";
    (form.elements.namedItem("home_lon") as HTMLInputElement).value = "-119.7681";
    (form.elements.namedItem("metar_lat") as HTMLInputElement).value = "39.5";
    (form.elements.namedItem("metar_lon") as HTMLInputElement).value = "-119.8";
    (form.elements.namedItem("metar_rad") as HTMLInputElement).value = "25";
    form.dispatchEvent(new Event("submit", { cancelable: true }));
    expect(state.home).toEqual({ lat: 39.499, lon: -119.7681 });
    expect(state.metar.centerLat).toBe(39.5);
    expect(state.metar.radiusNm).toBe(25);
  });

  it("rejects a non-finite home_lat with an alert and leaves state unchanged", () => {
    const alertSpy = vi.fn();
    vi.stubGlobal("alert", alertSpy);
    const { form } = openSettings();
    const beforeHome = { ...state.home };
    (form.elements.namedItem("home_lat") as HTMLInputElement).value = "";
    form.dispatchEvent(new Event("submit", { cancelable: true }));
    expect(alertSpy).toHaveBeenCalled();
    expect(state.home).toEqual(beforeHome);
    vi.unstubAllGlobals();
  });

  it("rejects a non-positive radius with an alert", () => {
    const alertSpy = vi.fn();
    vi.stubGlobal("alert", alertSpy);
    const { form } = openSettings();
    (form.elements.namedItem("metar_rad") as HTMLInputElement).value = "0";
    form.dispatchEvent(new Event("submit", { cancelable: true }));
    expect(alertSpy).toHaveBeenCalled();
    vi.unstubAllGlobals();
  });
});

describe("focus airport table", () => {
  it("adds a new row when the '+ Add airport' button is clicked", () => {
    const { form } = openSettings();
    const addBtn = form.querySelector<HTMLButtonElement>(".focus-add")!;
    const before = form.querySelectorAll(".focus-table tbody tr").length;
    addBtn.click();
    const after = form.querySelectorAll(".focus-table tbody tr").length;
    expect(after).toBe(before + 1);
  });

  it("removes a non-home row when its × button is clicked", () => {
    const { form } = openSettings();
    const rows = form.querySelectorAll<HTMLTableRowElement>(".focus-table tbody tr");
    const removable = rows[1];  // idx 1 = SFO, has a remove button
    const removeBtn = removable.querySelector<HTMLButtonElement>(".fp-remove")!;
    removeBtn.click();
    const after = form.querySelectorAll(".focus-table tbody tr");
    expect(after.length).toBe(rows.length - 1);
  });

  it("marks the home row read-only (no × button, lat/lon readonly)", () => {
    const { form } = openSettings();
    const homeRow = form.querySelector<HTMLTableRowElement>(".focus-table tbody tr")!;
    expect(homeRow.querySelector(".fp-remove")).toBeNull();
    expect(homeRow.querySelector<HTMLInputElement>(".fp-lat")?.readOnly).toBe(true);
  });
});
