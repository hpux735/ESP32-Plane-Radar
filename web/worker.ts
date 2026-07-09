// Cloudflare Worker for the Plane Radar web preview.
//
// One entry point serves two things:
//   /api/adsb?lat=…&lon=…&nm=…   → proxied ADS-B data from opendata.adsb.fi
//   everything else               → static Vite build in ./dist (via ASSETS)
//
// This replaces the older Pages Function at functions/api/adsb.ts — modern
// Cloudflare deployments prefer a single Worker with a static-assets
// binding over the split Pages/Functions layout.

export interface Env {
  ASSETS: Fetcher;
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    if (url.pathname === "/api/adsb") {
      return handleAdsb(url);
    }
    // Everything else = static asset (HTML, JS, CSS, JSON, etc.).
    return env.ASSETS.fetch(request);
  },
} satisfies ExportedHandler<Env>;

async function handleAdsb(url: URL): Promise<Response> {
  const lat = parseFloat(url.searchParams.get("lat") ?? "");
  const lon = parseFloat(url.searchParams.get("lon") ?? "");
  const nm = parseFloat(url.searchParams.get("nm") ?? "25");
  if (!isFinite(lat) || !isFinite(lon) || !isFinite(nm)) {
    return json({ error: "lat, lon, nm are required numeric query params" }, 400);
  }
  if (nm <= 0 || nm > 250) {
    return json({ error: "nm out of range" }, 400);
  }
  // Both endpoints return the same schema (opendata network peers).
  // adsb.fi blocks Cloudflare Worker IPs (403); airplanes.live doesn't.
  // Fall back to adsb.fi only if the primary fails so we still route to
  // both when useful.
  const urls = [
    `https://api.airplanes.live/v2/point/${lat.toFixed(4)}/${lon.toFixed(4)}/${nm.toFixed(1)}`,
    `https://opendata.adsb.fi/api/v3/lat/${lat.toFixed(4)}/lon/${lon.toFixed(4)}/dist/${nm.toFixed(1)}`,
  ];
  const fetchOpts: RequestInit = {
    headers: {
      "User-Agent": "plane-radar-web (github.com/benyaffe/ESP32-Plane-Radar)",
      Accept: "application/json",
    },
    // Edge cache upstream responses briefly so multiple browsers hitting
    // the same center don't hammer the source.
    cf: { cacheTtl: 5, cacheEverything: true } as RequestInitCfProperties,
  };
  let resp: Response | null = null;
  let lastStatus = 0;
  for (const upstream of urls) {
    try {
      const r = await fetch(upstream, fetchOpts);
      if (r.ok) { resp = r; break; }
      lastStatus = r.status;
    } catch {
      lastStatus = 599;
    }
  }
  if (!resp) {
    return json({ error: `upstream ${lastStatus}` }, 502);
  }
  const body = await resp.text();
  return new Response(body, {
    status: 200,
    headers: {
      "Content-Type": "application/json",
      "Access-Control-Allow-Origin": "*",
      "Cache-Control": "public, max-age=3",
    },
  });
}

function json(payload: unknown, status = 200): Response {
  return new Response(JSON.stringify(payload), {
    status,
    headers: {
      "Content-Type": "application/json",
      "Access-Control-Allow-Origin": "*",
    },
  });
}
