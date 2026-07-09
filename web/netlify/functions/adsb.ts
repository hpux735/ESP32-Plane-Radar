// Netlify Function replacing the Cloudflare Worker at /api/adsb —
// proxies ADS-B data so the browser bypasses CORS and IP blocks.
// Two peers tried in order (airplanes.live primary, adsb.fi fallback),
// 4 s timeout each so a dead source can't stall the client's 3 s poll.

export default async (req: Request): Promise<Response> => {
  const url = new URL(req.url);
  const lat = parseFloat(url.searchParams.get("lat") ?? "");
  const lon = parseFloat(url.searchParams.get("lon") ?? "");
  const nm = parseFloat(url.searchParams.get("nm") ?? "25");
  if (!isFinite(lat) || !isFinite(lon) || !isFinite(nm)) {
    return json({ error: "lat, lon, nm are required numeric query params" }, 400);
  }
  if (nm <= 0 || nm > 250) {
    return json({ error: "nm out of range" }, 400);
  }
  const upstreams = [
    `https://api.airplanes.live/v2/point/${lat.toFixed(4)}/${lon.toFixed(4)}/${nm.toFixed(1)}`,
    `https://opendata.adsb.fi/api/v3/lat/${lat.toFixed(4)}/lon/${lon.toFixed(4)}/dist/${nm.toFixed(1)}`,
  ];
  let resp: Response | null = null;
  let lastStatus = 0;
  for (const upstream of upstreams) {
    const ctrl = new AbortController();
    const t = setTimeout(() => ctrl.abort(), 4000);
    try {
      const r = await fetch(upstream, {
        headers: {
          "User-Agent": "plane-radar-web (github.com/benyaffe/ESP32-Plane-Radar)",
          Accept: "application/json",
        },
        signal: ctrl.signal,
      });
      if (r.ok) { resp = r; break; }
      lastStatus = r.status;
    } catch {
      lastStatus = 599;
    } finally {
      clearTimeout(t);
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
      "Cache-Control": "no-store",
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

export const config = { path: "/api/adsb" };
