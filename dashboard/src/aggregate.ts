import type { Metrics, MinutePoint } from './api'

/// Fold per-minute buckets into one total for the selected window.
///
/// Averages are re-weighted by their sample counts rather than averaged. A
/// minute holding one startup sample and a minute holding a thousand do not
/// contribute equally to the mean, so averaging the per-bucket averages would
/// silently over-weight quiet minutes. The API ships every mean alongside its
/// denominator precisely so this can be done correctly.
///
/// buffer_ratio_per_view is recomputed from the window's own totals rather than
/// carried through: it is rebuffers per view over a whole population, and the
/// per-minute points deliberately omit it because a view and its later stalls
/// fall in different minutes. Over a window wide enough to contain both, the
/// ratio is meaningful again.
export function sumBuckets(points: MinutePoint[]): Metrics {
  let totalEvents = 0
  let totalViews = 0
  let startupSamples = 0
  let startupWeighted = 0
  let bufferCount = 0
  let bufferSamples = 0
  let bufferWeighted = 0
  let errorCount = 0
  let watchTimeMs = 0
  let bitrateSamples = 0
  let bitrateWeighted = 0

  for (const point of points) {
    const m = point.metrics
    totalEvents += m.total_events
    totalViews += m.total_views
    startupSamples += m.startup_samples
    startupWeighted += m.startup_avg_ms * m.startup_samples
    bufferCount += m.buffer_count
    bufferSamples += m.buffer_samples
    bufferWeighted += m.buffer_avg_ms * m.buffer_samples
    errorCount += m.error_count
    watchTimeMs += m.watch_time_ms
    bitrateSamples += m.bitrate_samples
    bitrateWeighted += m.bitrate_avg_kbps * m.bitrate_samples
  }

  return {
    total_events: totalEvents,
    total_views: totalViews,
    startup_samples: startupSamples,
    startup_avg_ms: startupSamples ? startupWeighted / startupSamples : 0,
    buffer_count: bufferCount,
    buffer_samples: bufferSamples,
    buffer_avg_ms: bufferSamples ? bufferWeighted / bufferSamples : 0,
    buffer_ratio_per_view: totalViews ? bufferCount / totalViews : 0,
    error_count: errorCount,
    watch_time_ms: watchTimeMs,
    watch_time_min: watchTimeMs / 60_000,
    bitrate_samples: bitrateSamples,
    bitrate_avg_kbps: bitrateSamples ? bitrateWeighted / bitrateSamples : 0,
  }
}

/// The server labels a bucket "YYYY-MM-DDTHH:MM" in UTC, with no offset.
///
/// Parsed with an explicit "Z": JavaScript reads a date-time string without an
/// offset as *local* time, which would shift every point by the viewer's
/// timezone and, worse, make the gap arithmetic below wrong across a DST change.
const minuteToMs = (iso: string): number => Date.parse(`${iso}:00Z`)

const msToMinute = (ms: number): string => new Date(ms).toISOString().slice(0, 16)

const MINUTE_MS = 60_000

/// An all-zero bucket, for a minute in which nothing was recorded.
const emptyMetrics = (): Metrics => ({
  total_events: 0,
  total_views: 0,
  startup_samples: 0,
  startup_avg_ms: 0,
  buffer_count: 0,
  buffer_samples: 0,
  buffer_avg_ms: 0,
  error_count: 0,
  watch_time_ms: 0,
  watch_time_min: 0,
  bitrate_samples: 0,
  bitrate_avg_kbps: 0,
})

/// Insert zero-valued points for minutes that have no bucket.
///
/// A minute in which nothing arrived has no bucket at all, so consecutive points
/// could be hours apart while the chart drew a straight line between them: an
/// ingestion outage rendered as unbroken, healthy-looking traffic. That is the
/// single most misleading thing a monitoring chart can do, and selecting a 24h
/// window makes it far more likely than the old fixed 15-minute view did.
///
/// Zero is the honest value -- "no bucket" means "no events", not "no data" --
/// so the gap now reads as a drop to the floor.
///
/// @param limit hard cap on the returned length, so a pathological span (a
///        bucket restored from a week-old snapshot sitting next to a live one)
///        cannot expand into an array big enough to hang the page. When the fill
///        would exceed it the input is returned unchanged, which is the old
///        behaviour: wrong, but bounded, and preferable to freezing the tab.
export function fillGaps(points: MinutePoint[], limit = 2_000): MinutePoint[] {
  if (points.length < 2) {
    return points
  }

  const first = minuteToMs(points[0].minute)
  const last = minuteToMs(points[points.length - 1].minute)
  if (Number.isNaN(first) || Number.isNaN(last) || last < first) {
    return points
  }

  const span = Math.floor((last - first) / MINUTE_MS) + 1
  if (span <= points.length || span > limit) {
    return points
  }

  const known = new Map(points.map((p) => [p.minute, p]))
  const filled: MinutePoint[] = []
  for (let ms = first; ms <= last; ms += MINUTE_MS) {
    const minute = msToMinute(ms)
    filled.push(known.get(minute) ?? { minute, metrics: emptyMetrics() })
  }
  return filled
}
