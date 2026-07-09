// Cockpit view — TypeScript mirror of src/ui/cockpit_screen.cpp.
// Renders a Garmin-style clock face with rim tick marks, seven-segment
// HH:MM, sweeping second bar, OAT, and Garmin/PFD-style wind + baro
// blocks. Data comes from services::outdoor_temp on the device; here in
// the web preview we use plausible SF Bay Area placeholder values until
// a real weather fetch is wired up.

import { CENTER_X, CENTER_Y, SIZE, PHYSICAL_PANEL_RADIUS } from "./theme";
import { state } from "./state";

const BG = "rgb(6, 12, 26)";
const WHITE = "rgb(230, 232, 235)";
const GRAY = "rgb(96, 96, 104)";
const GREEN = "rgb(80, 220, 80)";
const FRAME = "rgb(60, 90, 60)";
const TEMP = "rgb(180, 200, 230)";
const AMBER = "rgb(255, 190, 40)";

// Placeholder weather — swapped for a real Open-Meteo fetch once the
// web side gains a fetcher. Uses the same defaults the firmware seeds
// on native so the two look consistent.
interface WxReading {
  tempF: number;
  windKts: number;
  windDegFrom: number;
  baroInHg: number;
  valid: boolean;
}
function currentWx(): WxReading {
  return {
    tempF: 61,
    windKts: 12,
    windDegFrom: 280,
    baroInHg: 29.92,
    valid: true,
  };
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

function drawTime(ctx: CanvasRenderingContext2D, hour: number, min: number): void {
  const cx = hour >= 10 && hour <= 19 ? 116 : 120;
  const cy = 108;
  const text = `${hour.toString().padStart(2, "0")}:${min.toString().padStart(2, "0")}`;
  ctx.fillStyle = WHITE;
  // Canvas doesn't have LovyanGFX's seven-segment Font7 — use a bold
  // monospace at similar size so the layout matches.
  ctx.font = "bold 40px ui-monospace, SFMono-Regular, Menlo, monospace";
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillText(text, cx, cy);
}

function drawUnsyncedPlaceholder(ctx: CanvasRenderingContext2D): void {
  ctx.fillStyle = AMBER;
  ctx.font = "bold 40px ui-monospace, SFMono-Regular, Menlo, monospace";
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillText("--:--", 120, 108);
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
  drawLabelValue(ctx, "OAT", oat, 148, TEMP);
  // Web preview has no BME280 — CABIN/RH lines omitted, matching the
  // firmware behavior when no sensor is attached.
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

  const wx = currentWx();
  drawWindIndicator(ctx, wx);

  // Local system clock — on the device SNTP populates this; in the
  // browser we just use the user's wall clock.
  const now = new Date();
  const hour = now.getHours();
  const min = now.getMinutes();
  const sec = now.getSeconds();

  // JS Date is always synced; no equivalent of the firmware's
  // "waiting for SNTP" placeholder is needed. Kept the function around
  // so future feature parity (e.g. offline preview) can trigger it.
  void drawUnsyncedPlaceholder;

  drawTime(ctx, hour, min);
  drawSensorBlock(ctx, wx);
  drawSecondSweep(ctx, sec);
  drawBaroIndicator(ctx, wx);

  applyBezelMask(ctx);
  void state;  // reserved for future per-view state
}
