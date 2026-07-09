# Deploying the Plane Radar web preview

The web preview lives under [`web/`](.) and deploys as a static site
with one small [Cloudflare Pages Function](functions/api/adsb.ts) that
proxies aircraft data. Everything below is free forever at this scale.

## What you need

- A GitHub account with this repo forked (already true — [benyaffe/ESP32-Plane-Radar](https://github.com/benyaffe/ESP32-Plane-Radar)).
- A [free Cloudflare account](https://dash.cloudflare.com/sign-up). No credit card required for what we're using.
- (Optional, for `radar.benyaffe.com`) Access to your GoDaddy DNS management to add one CNAME record.

## Step 1 — connect the repo to Cloudflare Pages

1. Sign in at [dash.cloudflare.com](https://dash.cloudflare.com/).
2. In the left sidebar: **Workers & Pages** → **Create** → **Pages** tab → **Connect to Git**.
3. Authorize Cloudflare to read your GitHub — Cloudflare will only see repos you allow.
4. Pick **`benyaffe/ESP32-Plane-Radar`**. Click **Begin setup**.
5. Fill in the build config (this is the only Cloudflare screen where the defaults are wrong for us — because our site lives in a `web/` subdirectory, not the repo root):

    | Field                  | Value                              |
    | ---------------------- | ---------------------------------- |
    | Project name           | `plane-radar` (or anything you like) |
    | Production branch      | `sdl-emulator` (or `main` after you merge) |
    | Framework preset       | **None**                           |
    | Build command          | `cd web && npm install && npm run build` |
    | Build output directory | `web/dist`                         |
    | Root directory         | *(leave blank — the build command already `cd`s in)* |

6. Click **Save and Deploy**. First build takes ~2 minutes.
7. When it finishes, Cloudflare gives you a URL like `plane-radar-xxx.pages.dev`. Open it — you should see the radar with live Bay Area traffic.

Every git push to the production branch auto-deploys after this. No commands to run.

## Step 2 — (optional) point `radar.benyaffe.com` at it

Once the `pages.dev` URL works:

1. In the Cloudflare Pages project → **Custom domains** → **Set up a custom domain**.
2. Enter `radar.benyaffe.com`. Cloudflare will show you a CNAME target string (e.g. `plane-radar-xxx.pages.dev`).
3. In another tab, log into GoDaddy → **My Products** → **Domains** → `benyaffe.com` → **DNS**.
4. Add a new record:
   - **Type**: `CNAME`
   - **Name**: `radar`
   - **Value**: the target string from step 2
   - **TTL**: default (1 hour) is fine
5. Save. Go back to the Cloudflare tab and click **Verify** (or wait a few minutes). Cloudflare provisions the SSL cert automatically.

`https://radar.benyaffe.com` is live.

## What if I want to update the site?

Just push commits to the production branch. Cloudflare rebuilds
automatically. You'll get a build log in the Cloudflare dashboard if
anything breaks.

## What if I want to test locally first?

```
cd web
npm install
npm run dev
```

Vite serves at `http://localhost:5173`. A small dev-only middleware
forwards `/api/adsb` to `opendata.adsb.fi` so aircraft data works
without deploying. Everything else runs the same as production.

## What if adsb.fi goes down?

The Cloudflare Pages Function returns a 502 in that case; the client
keeps the last successful frame on screen. Wait it out.

## Troubleshooting

- **Cloudflare build fails on `npm install`** — check that Node version
  is set to 18+ in the Pages env variables (**Settings** → **Environment
  variables** → add `NODE_VERSION=20`).
- **Aircraft don't appear** — open the browser devtools Network tab and
  load `/api/adsb?lat=37.75&lon=-122.41&nm=25`. If it 404s, the Pages
  Function didn't deploy (check the build log for the `functions/` dir).
- **Weather view says "no data"** — CORS on `api.weather.gov` was working
  when we tested. If it stops, that's on the NWS end. Refresh, wait a
  bit.
