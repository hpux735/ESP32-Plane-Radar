# Deploying the Plane Radar web preview

The site lives under [`web/`](.) and deploys as a Cloudflare Worker with
a static-assets binding — one Worker serves both the HTML/JS/CSS/JSON
bundle AND the `/api/adsb` proxy that fetches live ADS-B data. Free at
this scale, forever.

## What you need

- A GitHub account with the repo pushed (you have it — [benyaffe/ESP32-Plane-Radar](https://github.com/benyaffe/ESP32-Plane-Radar) on branch `sdl-emulator`).
- A [free Cloudflare account](https://dash.cloudflare.com/sign-up). No credit card required.
- (Optional, for a custom domain like `radar.benyaffe.com`) Access to your GoDaddy DNS management.

## Step 1 — connect the repo to Cloudflare

1. Sign in at [dash.cloudflare.com](https://dash.cloudflare.com/).
2. Left sidebar: **Compute (Workers)** (or **Workers & Pages** in older
   accounts) → **Create** → **Import a repository**.
3. Authorize the Cloudflare GitHub app. On the GitHub screen, pick
   **"Only select repositories"** and check
   **`benyaffe/ESP32-Plane-Radar`**. Click **Install & Authorize**.
4. Cloudflare shows the "Set up your application" screen with the repo
   already selected. Fill in exactly:

    | Field                                | Value                                    |
    | ------------------------------------ | ---------------------------------------- |
    | Project name                         | `plane-radar` (becomes part of the URL)  |
    | Build command                        | `cd web && npm install && npm run build` |
    | Deploy command                       | `cd web && npx wrangler deploy`          |
    | Builds for non-production branches   | leave checked (default)                  |
    | Non-production branch deploy command | `cd web && npx wrangler versions upload` |
    | Path                                 | *(leave blank — repo root)*              |

5. Click **Deploy**.

The first build takes ~2 minutes. Watch the live log — when it finishes,
Cloudflare gives you a URL like `https://plane-radar.your-subdomain.workers.dev`.
Open it — you should see the radar with live Bay Area traffic.

## Step 2 — pick the deploying branch

Cloudflare defaults to whatever GitHub branch is the repo's default
(usually `main`). If your changes are on `sdl-emulator`, either:

- **Set `sdl-emulator` as the production branch** — in the project's
  **Settings** → **Builds & deployments** → change **Production branch**
  to `sdl-emulator`, then push a commit (or click **Retry deployment**).
- **Or merge `sdl-emulator` into `main`** on GitHub — the site will
  auto-redeploy from `main`.

Either works.

## Step 3 — (optional) custom domain `radar.benyaffe.com`

Once the `workers.dev` URL is live:

1. In the Cloudflare project → **Settings** → **Domains & Routes** →
   **Add** → **Custom domain**.
2. Enter `radar.benyaffe.com`. Cloudflare gives you either a CNAME target
   or DNS records to add.
3. In another tab, log into GoDaddy → **My Products** → **Domains** →
   `benyaffe.com` → **DNS**.
4. Add the record Cloudflare showed (usually a CNAME:
   `radar` → `plane-radar.workers.dev` or similar).
5. Back in Cloudflare, click **Verify** (or wait a few minutes).
   Cloudflare provisions SSL automatically.

`https://radar.benyaffe.com` is live.

## Updating the site

Push commits to the deploying branch. Cloudflare rebuilds automatically.
Build logs live under the project's **Deployments** tab.

## Local testing

```bash
cd web
npm install
npm run dev
```

Vite serves at `http://localhost:5173`. A small dev-only middleware
forwards `/api/adsb` to `opendata.adsb.fi` so aircraft data works
without deploying anything.

To test the Worker itself locally (Vite dev doesn't run it):

```bash
cd web
npm run build
npx wrangler dev
```

Serves at `http://localhost:8787`.

## Troubleshooting

- **Build fails on `npm install`** — check that Node version is 18+. In
  project **Settings** → **Variables and Secrets** → add
  `NODE_VERSION=20`.
- **Deploy step fails with "You must set an account ID"** — the
  Cloudflare Builds environment provides `CLOUDFLARE_ACCOUNT_ID` and
  `CLOUDFLARE_API_TOKEN` automatically. If you see this error, the
  build ran outside Cloudflare (e.g. locally without `wrangler login`).
- **Aircraft don't appear** — open browser devtools → Network → visit
  `/api/adsb?lat=37.75&lon=-122.41&nm=25`. If it 404s or 502s, check
  the Worker's logs in the Cloudflare dashboard. The specific case
  `{"error": "upstream 403"}` means the ADS-B source is blocking
  Cloudflare Worker IP ranges (opendata.adsb.fi does this) — the
  Worker already falls back through a list; check that airplanes.live
  is still first, and try adding another peer if both start blocking.
- **Weather view says "no data"** — CORS on `api.weather.gov` works as
  of this writing. If it stops, that's on the NWS end.
