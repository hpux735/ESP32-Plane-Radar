# Deploying the Plane Radar web preview

The site lives under [`web/`](.) and deploys as a static site with one
serverless function that proxies live ADS-B data. Free forever at this
scale, no credit card required.

**Live at:** currently deploying via **Netlify** (was Cloudflare Workers
Builds during initial exploration; migrated because Netlify's build
UX + auto-deploy for static-with-functions was cleaner for a
non-technical maintainer).

## What you need

- A GitHub account with the repo pushed (you have it — [benyaffe/ESP32-Plane-Radar](https://github.com/benyaffe/ESP32-Plane-Radar)).
- A [free Netlify account](https://app.netlify.com/signup).
- (Optional, for a custom domain) Access to your GoDaddy DNS management.

## Step 1 — connect the repo to Netlify

1. Sign in at [app.netlify.com](https://app.netlify.com/).
2. **Add new site** → **Import an existing project** → **Deploy with GitHub**.
3. Authorize Netlify to read your GitHub repos (pick "Only select
   repositories" and check `benyaffe/ESP32-Plane-Radar`).
4. Fill in the build settings:

    | Field                  | Value                                    |
    | ---------------------- | ---------------------------------------- |
    | Branch                 | `main`                                   |
    | Base directory         | `web`                                    |
    | Build command          | `npm install && npm run build`           |
    | Publish directory      | `web/dist`                               |
    | Functions directory    | `netlify/functions` (auto-detected)      |

5. **Add environment variable** (optional but recommended):
    - `NODE_VERSION = 20`
6. Click **Deploy site**.

First build takes ~2 minutes. When it finishes, Netlify gives you a
URL like `https://plane-radar-xyz.netlify.app`. Open it — the radar
should load with live Bay Area traffic.

## Step 2 — (optional) custom domain `radar.benyaffe.com`

Once the `netlify.app` URL is live:

1. In the Netlify project → **Domain management** → **Add a domain**.
2. Enter `radar.benyaffe.com`. Netlify shows the CNAME target
   (usually `<sitename>.netlify.app`).
3. In another tab, log into GoDaddy → **My Products** → **Domains** →
   `benyaffe.com` → **DNS**.
4. Add record:
   - **Type**: `CNAME`
   - **Name**: `radar`
   - **Value**: the Netlify target from step 2
   - **TTL**: default (1 hour) is fine
5. Back in Netlify, click **Verify DNS configuration** (or wait a
   few minutes). Netlify provisions SSL automatically via Let's
   Encrypt.

`https://radar.benyaffe.com` is live.

## Updating the site

Just push commits to the deploying branch (`main`). Netlify rebuilds
automatically. Build logs live under the **Deploys** tab.

## Local testing

```bash
cd web
npm install
npm run dev
```

Vite serves at `http://localhost:5173`. A small dev-only middleware
forwards `/api/adsb` to `opendata.adsb.fi` so aircraft data works
locally without deploying anything.

## What the deploy actually runs

- **Static assets** (`web/dist/*`): HTML, JS, CSS, baked geographic
  JSON files. Served straight off Netlify's edge CDN.
- **One Netlify Function** (`web/netlify/functions/adsb.ts`, migrated
  from the Cloudflare Worker at `web/worker.ts`): proxies
  `/api/adsb?lat=X&lon=Y&nm=Z` to airplanes.live (primary) and
  opendata.adsb.fi (fallback). Adds CORS headers.
- **Live METAR** (`api.weather.gov`): called direct from the
  browser (NWS sends `Access-Control-Allow-Origin: *`, no proxy
  needed).

## Troubleshooting

- **Build fails on `npm install`** — check the Node version in
  **Site settings** → **Environment variables**. Set `NODE_VERSION = 20`.
- **Aircraft don't appear** — open browser devtools → Network → visit
  `/api/adsb?lat=37.75&lon=-122.41&nm=25`. If it 404s or 502s, check
  the Function's logs in Netlify.
  The specific case `{"error": "upstream 403"}` means the ADS-B source
  is blocking cloud IP ranges (opendata.adsb.fi does this); the
  Function already falls through a list — check that airplanes.live
  is first.
- **Weather view says "no data"** — CORS on `api.weather.gov` was
  working when this was written. If it stops, that's on the NWS end.

## Notes on the older Cloudflare setup

If you had a working Cloudflare Workers Builds deploy from earlier
iterations, `web/worker.ts` and `web/wrangler.jsonc` are still in the
tree — they'd still deploy correctly under Cloudflare Workers if you
ever want to switch back. The Netlify function at `web/functions/adsb.js`
does the same job in the Netlify runtime.
