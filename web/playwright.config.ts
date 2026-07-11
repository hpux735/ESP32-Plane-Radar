import { defineConfig, devices } from "@playwright/test";

// End-to-end suite. Boots `vite dev` on demand, then drives the app
// through a real Chromium so async-fetch bugs (like the one this repo
// just fixed) get caught before deploy. Run via `npm run test:e2e`.
//
// The bug this suite was built to prevent: change home/METAR to a new
// airport in the settings dialog → the 10nm radar goes blank because
// two overlapping tile fetches race and the loser overwrites the
// winner. That whole class of races is invisible to vitest — the DOM
// doesn't blank, only the pixels do.

export default defineConfig({
  testDir: "./e2e",
  timeout: 30_000,
  expect: { timeout: 5000 },
  fullyParallel: false,     // one canvas at a time keeps the assertions predictable
  reporter: process.env.CI ? [["dot"], ["html", { open: "never" }]] : "list",
  use: {
    baseURL: "http://localhost:5173",
    trace: "retain-on-failure",
    video: "retain-on-failure",
  },
  webServer: {
    command: "npm run dev -- --port 5173 --strictPort",
    port: 5173,
    reuseExistingServer: !process.env.CI,
    timeout: 30_000,
  },
  projects: [
    { name: "chromium", use: { ...devices["Desktop Chrome"] } },
  ],
});
