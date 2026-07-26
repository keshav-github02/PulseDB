import { API_BASE } from './config'

// --- Response shapes, matching the PulseDB query API JSON ------------------

export interface Metrics {
  total_events: number
  total_views: number
  startup_samples: number
  startup_avg_ms: number
  /// Stalls that *began* in this population.
  buffer_count: number
  /// Stalls that *finished* here, i.e. the denominator of buffer_avg_ms.
  buffer_samples: number
  buffer_avg_ms: number
  /// Only present on aggregate populations (totals, per-player, per-device);
  /// omitted from per-minute points, where rebuffers-per-view is not meaningful.
  buffer_ratio_per_view?: number
  error_count: number
  watch_time_ms: number
  watch_time_min: number
  bitrate_avg_kbps: number
}

/// Metric keys present on every population, so they are safe to chart directly.
/// Excludes buffer_ratio_per_view, which per-minute points omit.
export type PlottableMetricKey = Exclude<keyof Metrics, 'buffer_ratio_per_view'>

export interface Overall {
  totals: Metrics
  minutes_tracked: number
}

export interface MinutePoint {
  minute: string // e.g. "2026-07-18T14:03"
  metrics: Metrics
}

export interface Live {
  minutes: MinutePoint[]
}

export interface Segment {
  name: string
  metrics: Metrics
}

export interface Players {
  players: Segment[]
}

export interface Devices {
  devices: Segment[]
}

export interface Status {
  uptime_sec: number
  workers: number
  queue_depth: number
  events_per_sec: number
  active_sessions: number
  total_events: number
  error_count: number
  error_rate: number // fraction in [0, 1]
  cpu_percent: number
  memory_mb: number
}

// --- Fetchers --------------------------------------------------------------

async function getJson<T>(path: string): Promise<T> {
  const res = await fetch(`${API_BASE}${path}`)
  if (!res.ok) {
    throw new Error(`GET ${path} failed: ${res.status}`)
  }
  return (await res.json()) as T
}

export const fetchOverall = () => getJson<Overall>('/metrics')
export const fetchLive = (minutes: number) =>
  getJson<Live>(`/metrics/live?minutes=${minutes}`)
export const fetchPlayers = () => getJson<Players>('/metrics/player')
export const fetchDevices = () => getJson<Devices>('/metrics/device')
export const fetchStatus = () => getJson<Status>('/status')
