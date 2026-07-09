// Cloudflare Pages Function — proxy for opendata.adsb.fi.
//
// The browser can't call adsb.fi directly (no CORS headers). This
// function runs at the edge, calls adsb.fi server-side, and returns
// the same JSON with permissive CORS so the browser can consume it.
//
// URL:   /api/adsb?lat=37.75&lon=-122.41&nm=25
// Upstream: https://opendata.adsb.fi/api/v3/lat/{lat}/lon/{lon}/dist/{nm}

interface Env {
  // No env vars needed today; Pages Functions get this signature by
  // convention so custom bindings can be added later.
}

export const onRequestGet: PagesFunction<Env> = async (context) => {
  const url = new URL(context.request.url);
  const lat = parseFloat(url.searchParams.get("lat") ?? "");
  const lon = parseFloat(url.searchParams.get("lon") ?? "");
  const nm = parseFloat(url.searchParams.get("nm") ?? "25");

  if (!isFinite(lat) || !isFinite(lon) || !isFinite(nm)) {
    return json({ error: "lat, lon, nm are required numeric query params" }, 400);
  }
  if (nm <= 0 || nm > 250) {
    return json({ error: "nm out of range" }, 400);
  }

  const upstream =
    `https://opendata.adsb.fi/api/v3/lat/${lat.toFixed(4)}/` +
    `lon/${lon.toFixed(4)}/dist/${nm.toFixed(1)}`;

  const resp = await fetch(upstream, {
    // adsb.fi is friendly to plain GETs; a UA header is polite.
    headers: { "User-Agent": "plane-radar-web (github.com/benyaffe/ESP32-Plane-Radar)" },
    cf: {
      // Cache upstream responses at the edge for 5 s so multiple
      // browsers hitting the same center don't hammer the source.
      cacheTtl: 5,
      cacheEverything: true,
    },
  });

  if (!resp.ok) {
    return json({ error: `upstream ${resp.status}` }, 502);
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
};

function json(payload: unknown, status = 200): Response {
  return new Response(JSON.stringify(payload), {
    status,
    headers: {
      "Content-Type": "application/json",
      "Access-Control-Allow-Origin": "*",
    },
  });
}
