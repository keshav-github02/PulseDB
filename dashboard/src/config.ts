// Base URL of the PulseDB query API. Override at build/dev time with
// VITE_API_BASE (e.g. VITE_API_BASE=http://host:8081 npm run dev).
export const API_BASE: string =
  (import.meta.env.VITE_API_BASE as string | undefined) ?? 'http://localhost:8081'

// How often to poll the API, in milliseconds.
export const POLL_INTERVAL_MS = 2000

// How many recent minute buckets to request for the time-series charts.
export const LIVE_MINUTES = 15
