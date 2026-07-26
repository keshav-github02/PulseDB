# PulseDB Dashboard

A React + TypeScript + Recharts dashboard that polls the PulseDB metrics API
and renders live analytics: KPI tiles, per-minute time-series, and
player/device breakdowns.

## Prerequisites

- Node.js 18+ (developed on Node 24)
- The PulseDB server running with its query API (default `http://localhost:8081`)

## Run (development)

```powershell
cd dashboard
npm install          # first time only
npm run dev
```

Then open the printed URL (default <http://localhost:5173>).

Point it at a different API host with an env var:

```powershell
$env:VITE_API_BASE = "http://localhost:8081"; npm run dev
```

## Build (production)

```powershell
npm run build        # type-checks (tsc) then bundles to dist/
npm run preview      # serve the built dist/ locally
```

## What it shows

- **KPI tiles** — total views, total events, avg startup, rebuffers, errors,
  buffer ratio, avg bitrate, watch time (from `GET /metrics`)
- **Events / rebuffers per minute** — time-series area charts (from
  `GET /metrics/live`)
- **Views by player / device** — bar charts (from `GET /metrics/player` and
  `GET /metrics/device`)

The header shows a live/disconnected indicator; if the API is unreachable a
banner explains how to start the server. Data refreshes every 2 seconds.
