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

// RGB CSS colors — mirrored exactly from include/ui/radar_theme.h so
// the browser render matches the firmware/SDL panel pixel for pixel.
// Convention here follows the firmware: BACKGROUND is the deeper dark,
// LAND is a slightly lighter tint on top. Reads as "dim navy sea with
// slightly-lighter landmass" — same as hardware.
export const COLORS = {
  background:      "rgb(4, 10, 28)",        // kBg — deep navy
  grid:            "rgb(16, 100, 32)",
  label:           "rgb(255, 255, 255)",
  aircraft:        "rgb(255, 0, 0)",        // kAircraft — red icon
  trackVector:     "rgb(255, 0, 255)",      // kTrack — magenta speed line
  tagType:         "rgb(255, 200, 0)",      // kTagType — yellow-orange
  tagAltitude:     "rgb(90, 200, 255)",     // kTagAlt — light blue
  runway:          "rgb(56, 150, 170)",
  runwayLabel:     "rgb(110, 210, 230)",
  land:            "rgb(12, 20, 36)",       // kLand — subtle land tint
  coastline:       "rgb(44, 70, 68)",       // subtle teal border
  road:            "rgb(110, 110, 130)",
  emergency:       "rgb(255, 0, 0)",
  centerDot:       "rgb(255, 255, 255)",
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
