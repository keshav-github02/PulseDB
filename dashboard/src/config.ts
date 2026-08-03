// Base URL of the PulseDB query API. Override at build/dev time with
// VITE_API_BASE (e.g. VITE_API_BASE=http://host:8081 npm run dev).
export const API_BASE: string =
  (import.meta.env.VITE_API_BASE as string | undefined) ?? 'http://localhost:8081'

// How often to poll the API, in milliseconds. Used for the panels that are not
// windowed (live operations, player/device breakdowns).
export const POLL_INTERVAL_MS = 2000

/// A selectable window over the per-minute buckets.
export interface TimeRange {
  /// Shown on the selector button.
  label: string
  /// Minute buckets to request. The server clamps this to
  /// max_response_minutes (1,440 by default), so 24h is the widest useful value.
  minutes: number
  /// Poll interval for the windowed panels at this width.
  ///
  /// Deliberately slower for wider windows. A 24h window is ~1,440 buckets --
  /// roughly 360 KB -- and every read merges across shards while briefly holding
  /// each shard's mutex. Polling that every 2s would make an idle dashboard tab
  /// a sustained ~180 KB/s load against the ingest path, which is not what a
  /// monitoring view should cost.
  pollMs: number
}

export const TIME_RANGES: readonly TimeRange[] = [
  { label: '15m', minutes: 15, pollMs: 2_000 },
  { label: '1h', minutes: 60, pollMs: 2_000 },
  { label: '6h', minutes: 360, pollMs: 5_000 },
  { label: '24h', minutes: 1_440, pollMs: 10_000 },
]

export const DEFAULT_RANGE = TIME_RANGES[0]
