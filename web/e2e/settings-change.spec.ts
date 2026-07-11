import { test, expect, type Page } from "@playwright/test";

// The exact bug this suite was built to prevent:
//
// User boots on default (Sutro Tower home). Everything renders.
// Opens settings, types RNO/SEA/etc into the home box, saves. The 10nm
// radar goes blank because two tile fetches race and the older one
// wins. This test locks the whole flow: open, change home, save, and
// wait for the canvas to be visibly non-blank.

const OCEAN_BLUE = { r: 0, g: 8, b: 24 };     // COLORS.background from theme.ts
const NEAR_BLACK_TOLERANCE = 4;

async function samplePixel(page: Page, x: number, y: number) {
  return await page.evaluate(
    ([xx, yy]) => {
      const canvas = document.getElementById("radar") as HTMLCanvasElement | null;
      if (!canvas) throw new Error("radar canvas not found");
      const ctx = canvas.getContext("2d");
      if (!ctx) throw new Error("no 2d context");
      const d = ctx.getImageData(xx, yy, 1, 1).data;
      return { r: d[0], g: d[1], b: d[2], a: d[3] };
    },
    [x, y] as const,
  );
}

// A "non-blank" canvas is one where at least one sample pixel deviates
// from the water/background color by more than the tolerance. The radar
// grid, coastline, land, etc. all show up as non-background pixels.
async function canvasHasContent(page: Page): Promise<boolean> {
  const samples = await page.evaluate(() => {
    const canvas = document.getElementById("radar") as HTMLCanvasElement | null;
    if (!canvas) return null;
    const ctx = canvas.getContext("2d");
    if (!ctx) return null;
    const grid = [30, 60, 90, 120, 150, 180, 210];
    const out: Array<{ r: number; g: number; b: number }> = [];
    for (const x of grid) for (const y of grid) {
      const d = ctx.getImageData(x, y, 1, 1).data;
      out.push({ r: d[0], g: d[1], b: d[2] });
    }
    return out;
  });
  if (!samples) return false;
  return samples.some(s =>
    Math.abs(s.r - OCEAN_BLUE.r) > NEAR_BLACK_TOLERANCE ||
    Math.abs(s.g - OCEAN_BLUE.g) > NEAR_BLACK_TOLERANCE ||
    Math.abs(s.b - OCEAN_BLUE.b) > NEAR_BLACK_TOLERANCE,
  );
}

async function openSettings(page: Page) {
  await page.locator("button.settings-open").click();
  await expect(page.locator("dialog.settings-dialog")).toBeVisible();
}

async function saveSettings(page: Page) {
  await page.locator("button.settings-save").click();
  // Wait for the dialog to close before continuing.
  await expect(page.locator("dialog.settings-dialog")).toBeHidden();
}

test.beforeEach(async ({ page }) => {
  // Fresh state at test start: clear localStorage on the FIRST load
  // only. Doing it via addInitScript would fire on every navigation
  // — including the reload the persistence test needs — so we use a
  // one-shot init script that removes itself after running.
  await page.addInitScript(() => {
    if (!sessionStorage.getItem("_e2e_cleared")) {
      try { window.localStorage.clear(); } catch { /* not available */ }
      sessionStorage.setItem("_e2e_cleared", "1");
    }
  });
  await page.goto("/");
  // Wait for the initial map bootstrap to render at least one non-blank
  // pixel (proves the canvas is fully wired up before we start poking).
  await expect.poll(async () => canvasHasContent(page), { timeout: 10_000 }).toBe(true);
});

test("default (Sutro) view renders content on boot", async ({ page }) => {
  expect(await canvasHasContent(page)).toBe(true);
});

test("changing home to Reno keeps the 10nm radar non-blank", async ({ page }) => {
  await openSettings(page);
  // Fill home lat/lon directly (bypasses the typeahead race). Reno's
  // KRNO coordinates.
  await page.locator("input[name='home_lat']").fill("39.4990");
  await page.locator("input[name='home_lon']").fill("-119.7681");
  // Also move METAR so the weather view aligns.
  await page.locator("input[name='metar_lat']").fill("39.4990");
  await page.locator("input[name='metar_lon']").fill("-119.7681");
  await page.locator("input[name='metar_rad']").fill("30");
  await saveSettings(page);
  // Give tile fetches time to complete for the new location.
  await expect.poll(async () => canvasHasContent(page), { timeout: 10_000 }).toBe(true);
});

test("cycling views (radar → weather → cockpit → radar) never lands on a blank", async ({ page }) => {
  // Double-tap = advance ring. Use the hint button (data-gesture="double").
  const doubleBtn = page.locator("button.hint[data-gesture='double']");
  for (let i = 0; i < 6; i++) {   // cycle far enough to touch every view slot
    await doubleBtn.click();
    // Give whichever view we landed on a moment to paint.
    await page.waitForTimeout(500);
    expect(await canvasHasContent(page)).toBe(true);
  }
});

test("cycling ranges (single-tap) keeps map non-blank at every step", async ({ page }) => {
  const singleBtn = page.locator("button.hint[data-gesture='single']");
  for (let i = 0; i < 4; i++) {   // 4 range presets — walk them all
    await singleBtn.click();
    await page.waitForTimeout(400);
    expect(await canvasHasContent(page)).toBe(true);
  }
});

test("settings persist across page reload", async ({ page }) => {
  await openSettings(page);
  await page.locator("input[name='home_lat']").fill("39.4990");
  await page.locator("input[name='home_lon']").fill("-119.7681");
  await saveSettings(page);
  await page.reload();
  await expect.poll(async () => canvasHasContent(page), { timeout: 10_000 }).toBe(true);
  // Re-open the dialog and confirm the values stuck.
  await openSettings(page);
  await expect(page.locator("input[name='home_lat']")).toHaveValue("39.499");
  await expect(page.locator("input[name='home_lon']")).toHaveValue("-119.7681");
});
