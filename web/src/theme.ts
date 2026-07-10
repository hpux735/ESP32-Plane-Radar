// Colors and geometric constants for the radar view. Mirrored from
// firmware include/ui/radar_theme.h so the web visuals track hardware.

export const SIZE = 240;
export const CENTER_X = 120;
export const CENTER_Y = 120;
export const GRID_OUTER_RADIUS = 107;
export const PHYSICAL_PANEL_RADIUS = 120;

export const RANGE_PRESETS = [
  { nm: 5, outerKm: 9.26 },
  { nm: 10, outerKm: 18.52 },
  { nm: 15, outerKm: 27.78 },
  { nm: 25, outerKm: 46.3 },
];

export const KM_PER_DEG = 111.0;

// RGB CSS colors — sampled from actual SDL emulator pixels so the web
// render matches what the desk toy shows *on screen* (not the C++
// constants, which get post-processed by the GC9A01 BGR panel swap).
// See the samples in the commit that added this file — each RGB below
// was picked as the modal color of that class in a real emulator snap.
export const COLORS = {
  background:      "rgb(0, 8, 24)",         // water (dominant emulator bg)
  grid:            "rgb(16, 101, 33)",      // dim green rings
  label:           "rgb(255, 255, 255)",    // white text
  aircraft:        "rgb(0, 0, 255)",        // BLUE icon (kAircraftR=255 through BGR swap)
  trackVector:     "rgb(255, 0, 255)",      // magenta speed line
  tagType:         "rgb(255, 203, 0)",      // yellow-orange (type code)
  tagAltitude:     "rgb(90, 203, 255)",     // light blue (altitude in hundreds)
  runway:          "rgb(57, 150, 173)",     // teal runway line
  runwayLabel:     "rgb(107, 211, 231)",    // light cyan airport label
  land:            "rgb(8, 20, 33)",        // subtle land tint (barely above water)
  coastline:       "rgb(44, 70, 68)",       // subtle teal border
  emergency:       "rgb(255, 0, 0)",        // red (pure — no BGR swap for emergency)
} as const;

// Track vector length constants — mirror firmware:
//   speedLineLengthPx = gs_kt · (km/kt/hr · 60s/3600) · 107px / 13.3km · 1.5/5
// which yields a 60-second-ahead vector at the "reference" 13.3 km outer,
// scaled 30% down for readability. Same formula, same numbers.
export const TRACK_HORIZON_SEC = 60.0;
export const TRACK_REF_OUTER_KM = 13.3;
export const TRACK_LENGTH_SCALE = 1.5 / 5.0;
export const TRACK_MIN_PX = 2;

// Weather-view category colors — match src/ui/weather_map.cpp.
export const WX_COLORS = {
  VFR:  "rgb(40, 200, 60)",
  MVFR: "rgb(70, 130, 255)",
  IFR:  "rgb(240, 70, 70)",
  LIFR: "rgb(220, 70, 200)",
  Unknown: "rgb(120, 120, 120)",
} as const;
