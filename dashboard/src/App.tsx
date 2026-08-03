import { useMemo, useState } from 'react'
import { fetchDevices, fetchLive, fetchOverall, fetchPlayers, fetchStatus } from './api'
import { fillGaps, sumBuckets } from './aggregate'
import { KpiCard } from './components/KpiCard'
import { RangeSelector } from './components/RangeSelector'
import { SegmentBarCard } from './components/SegmentBarCard'
import { TimeSeriesCard } from './components/TimeSeriesCard'
import { API_BASE, DEFAULT_RANGE, POLL_INTERVAL_MS } from './config'
import { fmt1, fmt2, fmtInt } from './format'
import { usePolling } from './usePolling'

export default function App() {
  const [range, setRange] = useState(DEFAULT_RANGE)

  // Windowed: re-requested (and re-polled at the range's own cadence) whenever
  // the range changes. range.label is the reset key so the switch takes effect
  // immediately instead of at the next tick.
  const live = usePolling(() => fetchLive(range.minutes), range.pollMs, range.label)

  // Not windowed. /metrics is all-time, and the player/device breakdowns have no
  // time dimension at all -- segments are aggregated globally, not per minute --
  // so these keep the fixed cadence and are labelled "all time" below rather
  // than silently appearing to follow the selector.
  const overall = usePolling(fetchOverall, POLL_INTERVAL_MS)
  const players = usePolling(fetchPlayers, POLL_INTERVAL_MS)
  const devices = usePolling(fetchDevices, POLL_INTERVAL_MS)
  const status = usePolling(fetchStatus, POLL_INTERVAL_MS)

  // Buckets carry every mean's denominator, so the window's totals are exact
  // rather than an average of averages.
  const minutes = useMemo(() => fillGaps(live.data?.minutes ?? []), [live.data])
  const windowed = useMemo(() => sumBuckets(minutes), [minutes])

  const ops = status.data
  const connected = overall.data !== null && overall.error === null

  return (
    <div className="app">
      <header className="app-header">
        <div className="brand">
          <span className="brand-mark">◆</span>
          <h1>PulseDB</h1>
          <span className="brand-sub">Telemetry Analytics</span>
        </div>
        <div className={`status ${connected ? 'ok' : 'down'}`}>
          <span className="dot" />
          {connected ? 'Live' : 'Disconnected'}
          {overall.data ? (
            <span className="status-sub">· {overall.data.minutes_tracked} min tracked</span>
          ) : null}
        </div>
      </header>

      {overall.error ? (
        <div className="banner">
          Cannot reach the metrics API at <code>{API_BASE}</code>. Is the server running?
          <span className="banner-detail"> ({overall.error})</span>
        </div>
      ) : null}

      <div className="section-label">
        <span>Last {range.label}</span>
        <RangeSelector value={range} onChange={setRange} />
      </div>
      <section className="kpis">
        <KpiCard label="Views" value={live.data ? fmtInt(windowed.total_views) : '—'} />
        <KpiCard label="Events" value={live.data ? fmtInt(windowed.total_events) : '—'} />
        <KpiCard label="Avg Startup" value={live.data ? fmt1(windowed.startup_avg_ms) : '—'} unit="ms" />
        <KpiCard label="Rebuffers" value={live.data ? fmtInt(windowed.buffer_count) : '—'} />
        <KpiCard label="Errors" value={live.data ? fmtInt(windowed.error_count) : '—'} />
        <KpiCard label="Buffer Ratio" value={live.data ? fmt2(windowed.buffer_ratio_per_view ?? 0) : '—'} unit="/view" />
        <KpiCard label="Avg Bitrate" value={live.data ? fmtInt(windowed.bitrate_avg_kbps) : '—'} unit="kbps" />
        <KpiCard label="Watch Time" value={live.data ? fmt1(windowed.watch_time_min) : '—'} unit="min" />
      </section>

      <div className="section-label">Live operations</div>
      <section className="kpis">
        <KpiCard label="Events / sec" value={ops ? fmt1(ops.events_per_sec) : '—'} />
        <KpiCard label="Queue Length" value={ops ? fmtInt(ops.queue_depth) : '—'} />
        <KpiCard label="Active Sessions" value={ops ? fmtInt(ops.active_sessions) : '—'} />
        <KpiCard label="Error Rate" value={ops ? fmt2(ops.error_rate * 100) : '—'} unit="%" />
        <KpiCard label="CPU" value={ops ? fmt1(ops.cpu_percent) : '—'} unit="%" />
        <KpiCard label="Memory" value={ops ? fmt1(ops.memory_mb) : '—'} unit="MB" />
        <KpiCard label="Workers" value={ops ? fmtInt(ops.workers) : '—'} />
        <KpiCard label="Uptime" value={ops ? fmt1(ops.uptime_sec) : '—'} unit="s" />
      </section>

      <section className="charts">
        <TimeSeriesCard
          title={`Events per minute — last ${range.label}`}
          minutes={live.data ? minutes : undefined}
          metricKey="total_events"
        />
        <TimeSeriesCard
          title={`Rebuffers per minute — last ${range.label}`}
          minutes={live.data ? minutes : undefined}
          metricKey="buffer_count"
        />
      </section>

      {/* Segments carry no time dimension, so these cannot follow the selector.
          Saying so beats letting them look filtered when they are not. */}
      <div className="section-label">Breakdowns (all time)</div>
      <section className="charts">
        <SegmentBarCard title="Views by player" segments={players.data?.players} metricKey="total_views" />
        <SegmentBarCard title="Views by device" segments={devices.data?.devices} metricKey="total_views" />
      </section>

      <footer className="app-footer">
        Polling <code>{API_BASE}</code> — windowed panels every {range.pollMs / 1000}s, the
        rest every {POLL_INTERVAL_MS / 1000}s
      </footer>
    </div>
  )
}
