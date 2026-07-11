import { defineConfig } from "vite";
import type { IncomingMessage, ServerResponse } from "http";

// Static site — build outputs to web/dist/, which Netlify serves.
// The real /api/adsb + /api/metar Netlify Functions live in
// web/netlify/functions/ and take over in production.
//
// The dev-only middleware below emulates those two proxies by fetching
// their upstreams server-side (bypasses the CORS wall). Only active
// under `vite dev`; the production build has no server component.
export default defineConfig({
  base: "./",
  build: {
    outDir: "dist",
    emptyOutDir: true,
    target: "es2022",
  },
  server: {
    port: 5173,
    strictPort: false,
  },
  plugins: [
    {
      name: "dev-adsb-proxy",
      configureServer(server) {
        server.middlewares.use(async (req: IncomingMessage, res: ServerResponse, next) => {
          if (!req.url?.startsWith("/api/adsb")) return next();
          const url = new URL(req.url, "http://localhost");
          const lat = parseFloat(url.searchParams.get("lat") ?? "");
          const lon = parseFloat(url.searchParams.get("lon") ?? "");
          const nm = parseFloat(url.searchParams.get("nm") ?? "25");
          if (!isFinite(lat) || !isFinite(lon) || !isFinite(nm)) {
            res.statusCode = 400;
            res.setHeader("content-type", "application/json");
            res.end(JSON.stringify({ error: "bad params" }));
            return;
          }
          const upstream =
            `https://opendata.adsb.fi/api/v3/lat/${lat.toFixed(4)}/` +
            `lon/${lon.toFixed(4)}/dist/${nm.toFixed(1)}`;
          try {
            const upResp = await fetch(upstream, {
              headers: { "User-Agent": "plane-radar-web-dev" },
            });
            const body = await upResp.text();
            res.statusCode = upResp.status;
            res.setHeader("content-type", "application/json");
            res.setHeader("access-control-allow-origin", "*");
            res.end(body);
          } catch (err) {
            res.statusCode = 502;
            res.setHeader("content-type", "application/json");
            res.end(JSON.stringify({ error: String(err) }));
          }
        });
      },
    },
    {
      // Dev-only echo of netlify/functions/metar.mjs — vite dev doesn't
      // run Netlify Functions, so /api/metar 404s locally without this.
      name: "dev-metar-proxy",
      configureServer(server) {
        server.middlewares.use(async (req: IncomingMessage, res: ServerResponse, next) => {
          if (!req.url?.startsWith("/api/metar")) return next();
          const url = new URL(req.url, "http://localhost");
          const bbox = (url.searchParams.get("bbox") ?? "").trim();
          const parts = bbox.split(",");
          if (parts.length !== 4 || !parts.every((s) => isFinite(parseFloat(s)))) {
            res.statusCode = 400;
            res.setHeader("content-type", "application/json");
            res.end(JSON.stringify({ error: "bbox=lat_min,lon_min,lat_max,lon_max required" }));
            return;
          }
          const upstream =
            `https://aviationweather.gov/api/data/metar?bbox=${bbox}&format=json`;
          try {
            const upResp = await fetch(upstream, {
              headers: { "User-Agent": "plane-radar-web-dev" },
            });
            const body = await upResp.text();
            res.statusCode = upResp.status;
            res.setHeader("content-type", "application/json");
            res.setHeader("access-control-allow-origin", "*");
            res.end(body);
          } catch (err) {
            res.statusCode = 502;
            res.setHeader("content-type", "application/json");
            res.end(JSON.stringify({ error: String(err) }));
          }
        });
      },
    },
  ],
});
