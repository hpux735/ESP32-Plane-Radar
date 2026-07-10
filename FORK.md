# Fork changelog

Personal fork of [`MatixYo/ESP32-Plane-Radar`](https://github.com/MatixYo/ESP32-Plane-Radar) — same firmware core, with a lot bolted on top so the desk toy runs on real hardware, in a Mac window, and in a browser.

The upstream firmware and 3D-printed case are unchanged; everything here is additive. Files touched are grouped by feature so each section can be reviewed independently.

---

## 1. Desktop emulator (`pio run -e native`)

The full radar renders in a Mac window backed by LovyanGFX's SDL panel. Iteration on any UI change is now sub-second — no flash cycle, no hardware.

- **New PlatformIO env** `[env:native]` in [`platformio.ini`](platformio.ini). Firmware env `[env:supermini]` is untouched.
- **Host shim tree** in [`host_shims/`](host_shims/) — Arduino/HTTPClient/WiFi/Preferences/`nvs_flash` stubs so firmware modules link cleanly against a desktop libc.
- **`src/host/host_main.cpp`** — desktop `setup()`/`loop()` that skips the WiFi setup portal and drives the panel from `emscripten_set_main_loop`-style poll.
- **`src/host/host_stubs.cpp`** — native implementations of `services::adsb::fetchUpdate` (via `popen(curl)`) and `services::location` with a compile-time center.
- **Screenshot loop** — every 200 ms the framebuffer is dumped to `/tmp/plane-radar-screenshot.ppm` for closed-loop visual testing (see [`scripts/dev/snap.sh`](scripts/dev/snap.sh)).
- **Bezel mask** — the SDL panel is square 240×240, but the physical GC9A01 is a disc of radius ~120; a final pass paints the corner region back to background so what you see on the Mac matches what you'd see through the bezel.

## 2. Coastline / land overlays

Baked geographic vector data drawn *under* the traffic so you can tell where the aircraft actually are.

- **`scripts/build_coastlines.py`** — clips [Natural Earth 1:10m coastline](https://www.naturalearthdata.com/) to a 200 km bbox around the center, Douglas–Peucker simplifies, emits `src/data/coastlines_data.cpp` as int32 micro-degrees.
- **`scripts/build_land.py`** — same pattern for `ne_10m_land` + `ne_10m_minor_islands`, then ear-clip triangulates so the runtime just plays back triangles (no runtime tessellation).
- **`include/ui/map_projection.hpp`** + **`src/ui/map_projection.cpp`** — shared `latLonToScreen` / `clipSegmentToDisc` / `segmentIntersectsDisc` primitives; earlier the projection was inline in `runway_overlay.cpp`.
- **`src/ui/coastline_overlay.cpp`** and **`src/ui/land_overlay.cpp`** — each is a small draw-only module; each reads its layer's enable flag before touching pixels.

## 3. Airport + runway overlays

Any hub with a large-airport ICAO gets rendered runways + label.

- **`scripts/build_large_airports.py`** — pulls OurAirports `airports.csv` + `runways.csv`, keeps `type == "large_airport"`, joins runway endpoints. Filters heliports.
- **`scripts/build_focus_airports.py`** — a separate table for the smaller GA fields (SQL/HAF/PAO/HWD) so they render *only when they're the current focus*, keeping the general view uncluttered.
- **`src/ui/runway_overlay.cpp`** — templated draw routines so the same code renders either data set.

## 4. Aircraft data blocks (tags)

Callsign + altitude labels next to each icon, with the same style choices ATC uses on approach scopes.

- **`callsign` picking** — trims the ADS-B `flight` field first; if empty, falls back to registration (`r`), then hex.
- **2-line tag** — line 1 = callsign; line 2 alternates altitude (in hundreds of feet, aviation convention) and type code (e.g. `A320`). Toggle timed halfway between position updates so the flip is deliberate, not fighting the icon jump.
- **Trend triangle** — up/down arrow on the altitude when |vertical rate| ≥ 500 fpm.
- **Emergency squawk (7500/7600/7700)** — icon + tag render red, and an `EM` glyph pins to the tag's free corner opposite the alt/type block.
- **Two-tier collision** — labels register as HARD rectangles; icons + track vectors as SOFT. Aircraft tags first try to avoid all rects (strict), then fall back to avoiding just labels (relaxed). Result: labels never cover other labels but *may* cover a deprioritized aircraft's icon — which is what you want.
- **Tag budget per range** — `{5nm: 20, 10nm: 15, 15nm: 10, 25nm: 6}`. Aircraft ranked by clarity = `alt_ft + gs*20 + |vs|/5` (+ big bonus for emergency); top N get labels, everyone else keeps their icon but drops the callsign. Wide views stay legible.
- **On-ground filter** — ADS-B records reporting "ground" or (< 100 ft AND < 40 kt) are dropped entirely (no icon, no tag). KSFO alone has ~30 ground emitters; filtering keeps the view about flying traffic.

## 6. Nautical miles + range presets

Aviation convention throughout.

- **Range preset ring** — 5 / 10 / 15 / 25 nm (was 5 / 10 / 15 / 25 km).
- **Range label** — sits inside the outer ring on the E-of-N spoke by default, walks around symmetrically if it would collide with an airport label.
- **N/E/S/W cardinals removed** — north is always up on a radar; the cardinals were spending pixels on redundant info and crowding the bezel.

## 7. Focus points (double-tap to change center)

Cycle the radar's center through a small preset ring of Bay Area airports without touching the WiFi portal.

- **`src/services/focus_points.cpp`** — 8-entry ring: Sutro (public landmark default), SFO, OAK, SJC, HWD, SQL, PAO, HAF. Each entry carries its own preferred range preset so switching to a GA field auto-zooms to 5 nm.
- **`src/services/radar_location.h`** — new `setOverride()` / `clearOverride()` so `lat()`/`lon()` transparently redirect while a focus point is active. No code outside `services::location` needs to know.
- **Persistence** — current focus index saved to Preferences alongside the range preset.

## 8. Triple-tap weather view

A second view mode showing Bay Area airports as VFR/MVFR/IFR/LIFR colored dots.

- **`src/services/weather.cpp`** — bulk METAR fetch from `aviationweather.gov` (public, no key), local flight-category derivation (worst of ceiling and visibility per FAA rules).
- **`src/ui/weather_map.cpp`** — auto-fits the station bbox to the viewport, nudges overlapping dots apart, draws land + coastline underneath for context, single-blit via the shared frame sprite so the freshness updater doesn't strobe.
- **`BootTap`** state machine — extended from single/double to single/double/triple with a 400 ms quiet window (single-tap latency grew by ~150 ms; worth it for the third gesture).

## 9. Layer toggles

Every overlay individually on/off, persisted to Preferences.

- **`include/ui/layer_style.h`** + **`src/ui/layer_style.cpp`** — small registry: `enum class Layer { Coastline, Land, Roads, RunwaysLarge, RunwaysFocus, AircraftTags }`.
- **Keyboard bindings on native** — keys `1`–`7` flip the corresponding layer, `S` snaps a screenshot.

## 10. Web preview (`web/`)

Browser port so friends can try the interface without hardware. Same visual language, same interactions.

- **Vite + TypeScript** — no framework; ~7 KB of gzipped JS renders the whole thing.
- **`scripts/build_web_data.py`** — bakes the same Natural Earth + OurAirports sources into JSON. Ships high-detail Bay Area layers (10 m Natural Earth) plus a CONUS-wide 50 m base so *any* US airport picked in the typeahead gets some map context.
- **`web/functions/api/adsb.ts`** — Cloudflare Pages Function that proxies `opendata.adsb.fi` (no CORS on the upstream). Cached 5 s at the edge. In local dev, a small Vite middleware forwards the same path directly.
- **Weather** — talks straight to `api.weather.gov` from the browser (that endpoint *does* send CORS headers, so no proxy needed).
- **Typeahead** — 828-airport index, ranks exact/prefix ICAO+IATA above name matches.
- **Touch + click + keyboard** — one central `Tap` discriminator handles all three input modes; single/double/triple map to range/focus/weather just like on hardware. Layer toggles are clickable buttons on mobile, `1`–`5` keys on desktop.
- **Deploy** — one Cloudflare account, one Pages project, one CNAME. See [`web/DEPLOY.md`](web/DEPLOY.md) for the step-by-step.

## 11. Miscellaneous polish

- Land / water colors chosen so water reads as a subtle blue tint and land as near-black — matches how people describe what they expect to see.
- Bezel-radius label placement so nothing spills past the physical panel edge.
- Sprite double-buffering on both firmware and native — the weather view was strobing at 1 Hz before the sprite pass landed.
- All new files are additive; every change to an existing file that isn't `platformio.ini` is guarded by `#ifdef USE_NATIVE`.

---

**Default center:** the public code centers on **Sutro Tower** (37.7552 N, 122.4528 W) — the SF broadcast landmark — not any personal address. The real firmware still asks you for your own location via the WiFi setup portal on first boot.

---

## Three-environment parity rule

The stack has **three visual environments** that must stay lockstep:

1. **Hardware firmware** (ESP32-C3 + GC9A01) — `pio run -e supermini`
2. **SDL desktop emulator** — `pio run -e native`, same C++ as firmware
3. **Web preview** — `web/`, TypeScript port

Firmware and SDL share source (only host stubs differ under `USE_NATIVE`); the web is a separate TypeScript codebase and cannot autoshare. To keep them from drifting:

- **The SDL emulator is source of truth for what "looks right."** It's the daily-driver during development and its pixels are what the firmware actually shows on a real panel (with the same BGR-swap chain applied).
- **Colors** — sample from an actual emulator PPM screenshot, not from the C++ constants. `include/ui/radar_theme.h` names colors by their *logical* aviation meaning (e.g. `kAircraftR=255` = "red"), but the panel's BGR order flips channels at display time. What ships on screen is `color565(kAircraftB, kAircraftG, kAircraftR)`, not the raw RGB. Web must mirror the *rendered* pixels, not the logical constants.
- **Placement math + timing** — every animation offset (e.g. tag alt/type flip lagging fetches by 1.5 s), every slot ring (16-slot tag placement), every clip-to-disc gate — implement in the emulator first, port to web second. Never the other way around.
- **Data** — geometric baked data (coastlines, land triangles, roads, airports) comes from the same source files (Natural Earth, OurAirports, TIGER, OSM) via `scripts/build_*.py`. Firmware bakes to `.cpp`; web bakes to `.json`. Same DP tolerances where practical.
- **Feature adds** — new radar behaviour lands in `src/ui/*.cpp` first, gets pixel-sampled from the SDL emulator, then ported to `web/src/*.ts`. Any web-only additions (weather map, airport typeahead, dynamic map data) don't need a firmware counterpart.

If a divergence bug lands (web looks different from emulator), the fix is *always* to make the web match the emulator, never the reverse.
