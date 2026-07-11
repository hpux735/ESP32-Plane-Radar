import { defineConfig } from "vitest/config";

// Vitest-specific config. Kept separate from vite.config.ts so the dev
// server's proxy middleware doesn't run during tests.
export default defineConfig({
  test: {
    // Netlify's function-bundler mirrors our .mjs test files into
    // .netlify/functions-serve/ after `netlify build` or a Deploy Preview.
    // Vitest's default glob would then run each test twice — once in the
    // real source dir and once in the bundled copy that has no test
    // context — so exclude bundler outputs up front.
    exclude: [
      "**/node_modules/**",
      "**/dist/**",
      "**/.netlify/**",
      // Playwright specs are driven by its own runner (`npm run test:e2e`),
      // not vitest — the two use incompatible fixture APIs.
      "**/e2e/**",
    ],
    environment: "happy-dom",
    setupFiles: ["./src/testSetup.ts"],
    coverage: {
      provider: "v8",
      reporter: ["text", "html", "lcov"],
      // Exclude test infra + generated assets from the coverage denominator.
      exclude: [
        "**/node_modules/**",
        "**/dist/**",
        "**/.netlify/**",
        "**/*.test.*",
        "**/testCanvas.ts",
        "**/testSetup.ts",
        "vite.config.ts",
        "vitest.config.ts",
      ],
      // Report from all first-party source, even files with no tests
      // yet, so the number doesn't lie about untested modules.
      all: true,
      include: ["src/**/*.ts", "netlify/functions/*.mjs"],
    },
  },
});
