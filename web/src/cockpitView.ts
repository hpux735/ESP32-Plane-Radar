// Cockpit view — TypeScript mirror of src/ui/cockpit_screen.cpp.
// Renders a Garmin-style clock face with rim tick marks, seven-segment
// HH:MM, sweeping second bar, OAT, and Garmin/PFD-style wind + baro
// blocks. Data comes from services::outdoor_temp on the device; here in
// the web preview we use plausible SF Bay Area placeholder values until
// a real weather fetch is wired up.

import tzlookup from "tz-lookup";
import { CENTER_X, CENTER_Y, SIZE, PHYSICAL_PANEL_RADIUS } from "./theme";
import { state } from "./state";
import { cachedReading, type WxReading } from "./outdoorTemp";
import { lastUpdateMs } from "./weather";
import { formatFreshness } from "./weatherView";
import { bearingDeg, compass8 } from "./projection";
import { nearestIapAirport } from "./airports";
import type { IndexData } from "./data";

const BG = "rgb(6, 12, 26)";
const WHITE = "rgb(230, 232, 235)";
const GRAY = "rgb(96, 96, 104)";
const GREEN = "rgb(80, 220, 80)";
const FRAME = "rgb(60, 90, 60)";
const TEMP = "rgb(180, 200, 230)";
const AMBER = "rgb(255, 190, 40)";

// The cockpit view uses the airport index (for nearest-IAP lookup) that
// main.ts loads on boot. Injected via setIndexData(), read via the
// module-scoped `indexData` variable so the view function stays a pure
// (ctx) → void signature.
let indexData: IndexData | null = null;
export function setIndexData(d: IndexData | null): void {
  indexData = d;
}

function drawRadialLine(
  ctx: CanvasRenderingContext2D,
  angleRad: number,
  inner: number,
  outer: number,
  color: string,
  width = 1,
): void {
  const x0 = CENTER_X + Math.cos(angleRad) * inner;
  const y0 = CENTER_Y + Math.sin(angleRad) * inner;
  const x1 = CENTER_X + Math.cos(angleRad) * outer;
  const y1 = CENTER_Y + Math.sin(angleRad) * outer;
  ctx.strokeStyle = color;
  ctx.lineWidth = width;
  ctx.beginPath();
  ctx.moveTo(x0, y0);
  ctx.lineTo(x1, y1);
  ctx.stroke();
}

function drawTicks(ctx: CanvasRenderingContext2D): void {
  for (let i = 0; i < 60; i++) {
    const angle = ((i * 6 - 90) * Math.PI) / 180;
    if (i % 5 === 0) drawRadialLine(ctx, angle, 92, 108, WHITE);
    else             drawRadialLine(ctx, angle, 92, 100, GRAY);
  }
}

function drawSecondSweep(ctx: CanvasRenderingContext2D, sec: number): void {
  const angle = ((sec * 6 - 90) * Math.PI) / 180;
  // Five slightly-fanned lines so the sweep reads as a solid bar.
  for (const d of [-0.020, -0.010, 0.0, 0.010, 0.020]) {
    drawRadialLine(ctx, angle + d, 88, 108, WHITE);
  }
}

// Return the IANA time zone for the current home location, falling back
// to the browser's default zone if lookup throws (out-of-range coords).
function homeTimeZone(): string {
  try {
    return tzlookup(state.home.lat, state.home.lon);
  } catch {
    return Intl.DateTimeFormat().resolvedOptions().timeZone;
  }
}

// HH:MM in the given IANA zone using 24-hour clock. Returns "--:--" on
// any Intl failure so the display degrades gracefully.
function formatInZone(now: Date, timeZone: string): string {
  try {
    return new Intl.DateTimeFormat("en-US", {
      timeZone,
      hour: "2-digit",
      minute: "2-digit",
      hourCycle: "h23",
    }).format(now);
  } catch {
    return "--:--";
  }
}

function drawTime(ctx: CanvasRenderingContext2D, hhmm: string): void {
  // Big 40 px bold mono for HH:MM, then a small "L" (same size + face
  // as the Zulu marker below) tucked just to the right of the last
  // digit's right edge. Center the HH:MM at CENTER_X — the L sits
  // outside that centering so the numeric digits stay symmetric.
  ctx.fillStyle = WHITE;
  ctx.font = "bold 40px ui-monospace, SFMono-Regular, Menlo, monospace";
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillText(hhmm, CENTER_X, CENTER_Y);

  const timeHalfW = ctx.measureText(hhmm).width / 2;
  const lX = CENTER_X + timeHalfW + 3;
  ctx.font = "10px system-ui, sans-serif";
  ctx.textAlign = "left";
  ctx.textBaseline = "middle";
  // Same TEMP color as the Zulu below so the two markers read as a
  // matched pair. Baseline nudged down 1 px so the small "L" visually
  // sits at the bottom of the digits' baseline.
  ctx.fillStyle = TEMP;
  ctx.fillText("L", lX, CENTER_Y + 8);
}

function drawZulu(ctx: CanvasRenderingContext2D, now: Date): void {
  const hh = now.getUTCHours().toString().padStart(2, "0");
  const mm = now.getUTCMinutes().toString().padStart(2, "0");
  ctx.fillStyle = TEMP;
  ctx.font = "10px system-ui, sans-serif";
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  ctx.fillText(`${hh}:${mm} Z`, CENTER_X, 142);
}

function drawUnsyncedPlaceholder(ctx: CanvasRenderingContext2D): void {
  ctx.fillStyle = AMBER;
  ctx.font = "bold 40px ui-monospace, SFMono-Regular, Menlo, monospace";
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillText("--:--", CENTER_X, CENTER_Y);
  ctx.font = "10px system-ui, sans-serif";
  ctx.fillText("SYNC", CENTER_X, 148);
}

function drawLabelValue(
  ctx: CanvasRenderingContext2D,
  label: string,
  value: string,
  y: number,
  color: string,
): void {
  ctx.fillStyle = color;
  ctx.font = "10px system-ui, sans-serif";
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  ctx.fillText(`${label}  ${value}`, CENTER_X, y);
}

// Filled triangle arrow at (cx, cy), pointing along `angleRad`.
function drawArrow(
  ctx: CanvasRenderingContext2D,
  cx: number,
  cy: number,
  angleRad: number,
  len: number,
  halfW: number,
  color: string,
): void {
  const cs = Math.cos(angleRad);
  const sn = Math.sin(angleRad);
  const tx = cx + cs * len;
  const ty = cy + sn * len;
  const bx = cx - cs * (len * 0.15);
  const by = cy - sn * (len * 0.15);
  const px = -sn * halfW;
  const py = cs * halfW;
  ctx.fillStyle = color;
  ctx.beginPath();
  ctx.moveTo(tx, ty);
  ctx.lineTo(bx + px, by + py);
  ctx.lineTo(bx - px, by - py);
  ctx.closePath();
  ctx.fill();
}

function drawWindIndicator(ctx: CanvasRenderingContext2D, wx: WxReading): void {
  const blockCy = 52;
  const arrowCx = CENTER_X - 30;
  if (!wx.valid) {
    ctx.fillStyle = GREEN;
    ctx.font = "10px system-ui, sans-serif";
    ctx.textAlign = "left";
    ctx.textBaseline = "middle";
    ctx.fillText("WND --", CENTER_X - 18, blockCy);
    return;
  }
  // "Wind from 270°" → blowing to 90° (east). Screen angle: +y is DOWN,
  // so compass heading 0 = up = -90° in screen radians.
  const goingToDeg = wx.windDegFrom + 180;
  const angleRad = ((goingToDeg - 90) * Math.PI) / 180;
  drawArrow(ctx, arrowCx, blockCy, angleRad, 10, 4, GREEN);
  ctx.fillStyle = GREEN;
  ctx.font = "10px system-ui, sans-serif";
  ctx.textAlign = "left";
  ctx.textBaseline = "middle";
  const dir = Math.round(wx.windDegFrom) % 360;
  ctx.fillText(`${dir.toString().padStart(3, "0")}/${Math.round(wx.windKts)}kt`,
               arrowCx + 14, blockCy);
}

function drawFreshness(ctx: CanvasRenderingContext2D): void {
  ctx.fillStyle = GRAY;
  ctx.font = "10px system-ui, sans-serif";
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  ctx.fillText(formatFreshness(lastUpdateMs()), CENTER_X, 30);
}

function drawBaroIndicator(ctx: CanvasRenderingContext2D, wx: WxReading): void {
  const blockCy = 205;
  const halfW = 36;
  const halfH = 8;
  ctx.strokeStyle = FRAME;
  ctx.lineWidth = 1;
  ctx.strokeRect(CENTER_X - halfW, blockCy - halfH, halfW * 2, halfH * 2);
  ctx.fillStyle = GREEN;
  ctx.font = "10px ui-monospace, SFMono-Regular, Menlo, monospace";
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  const val = wx.valid ? `${wx.baroInHg.toFixed(2)} IN` : "--.-- IN";
  ctx.fillText(val, CENTER_X, blockCy);
}

function drawSensorBlock(ctx: CanvasRenderingContext2D, wx: WxReading): void {
  const oat = wx.valid ? `${Math.round(wx.tempF)}F` : "--F";
  drawLabelValue(ctx, "OAT", oat, 155, TEMP);
  // Web preview has no BME280 — CABIN/RH lines omitted, matching the
  // firmware behavior when no sensor is attached. Slot at y=167-179
  // reserved for a future cabin/RH readout so the layout stays stable
  // when it lands.
}

// "1.2 nm NE of KSFO" — where the home planning point sits relative to
// the nearest IAP-capable airport. If home is within 0.1 nm of that
// airport, show just "KSFO". Returns null if the airport index isn't
// loaded yet or the scan finds no IAP row (should not happen once
// airport_index.json is present).
function referencePositionLabel(): string | null {
  if (!indexData) return null;
  const nearest = nearestIapAirport(
    indexData.airportIndex, state.home.lat, state.home.lon);
  if (!nearest) return null;
  if (nearest.distanceNm <= 0.1) return nearest.icao;
  // Bearing FROM the airport TO home reads naturally as "home is X of
  // airport" — e.g. bearing 350° → home is N of the airport.
  const brg = bearingDeg(
    nearest.lat, nearest.lon, state.home.lat, state.home.lon);
  return `${nearest.distanceNm.toFixed(1)} nm ${compass8(brg)} of ${nearest.icao}`;
}

function drawReferencePosition(ctx: CanvasRenderingContext2D): void {
  const label = referencePositionLabel();
  if (!label) return;
  ctx.fillStyle = GRAY;
  ctx.font = "9px system-ui, sans-serif";
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  ctx.fillText(label, CENTER_X, 182);
}

function applyBezelMask(ctx: CanvasRenderingContext2D): void {
  // Same round-panel mask the radar + weather views use so the corners
  // match the physical GC9A01's bezel.
  ctx.fillStyle = BG;
  ctx.beginPath();
  ctx.rect(0, 0, SIZE, SIZE);
  ctx.arc(CENTER_X, CENTER_Y, PHYSICAL_PANEL_RADIUS, 0, Math.PI * 2, true);
  ctx.fill("evenodd");
}

export function drawCockpitView(ctx: CanvasRenderingContext2D): void {
  ctx.fillStyle = BG;
  ctx.fillRect(0, 0, SIZE, SIZE);

  drawTicks(ctx);

  const wx = cachedReading();
  drawFreshness(ctx);
  drawWindIndicator(ctx, wx);

  // Local clock in the home planning-point's IANA zone. JS Date is
  // always synced; no equivalent of the firmware's "waiting for SNTP"
  // placeholder is needed — the function is kept for future offline
  // preview parity.
  const now = new Date();
  const hhmm = formatInZone(now, homeTimeZone());
  const sec = now.getSeconds();
  void drawUnsyncedPlaceholder;

  drawTime(ctx, hhmm);
  drawZulu(ctx, now);
  drawSensorBlock(ctx, wx);
  drawReferencePosition(ctx);
  drawSecondSweep(ctx, sec);
  drawBaroIndicator(ctx, wx);

  applyBezelMask(ctx);
}
