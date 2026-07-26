import { fetchDevices, fetchLive, fetchOverall, fetchPlayers, fetchStatus } from './api'
import { KpiCard } from './components/KpiCard'
import { SegmentBarCard } from './components/SegmentBarCard'
import { TimeSeriesCard } from './components/TimeSeriesCard'
import { API_BASE, LIVE_MINUTES, POLL_INTERVAL_MS } from './config'
import { fmt1, fmt2, fmtInt } from './format'
import { usePolling } from './usePolling'

export default function App() {
  const overall = usePolling(fetchOverall, POLL_INTERVAL_MS)
  const live = usePolling(() => fetchLive(LIVE_MINUTES), POLL_INTERVAL_MS)
  const players = usePolling(fetchPlayers, POLL_INTERVAL_MS)
  const devices = usePolling(fetchDevices, POLL_INTERVAL_MS)
  const status = usePolling(fetchStatus, POLL_INTERVAL_MS)

  const totals = overall.data?.totals
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

      <section className="kpis">
        <KpiCard label="Total Views" value={totals ? fmtInt(totals.total_views) : '—'} />
        <KpiCard label="Total Events" value={totals ? fmtInt(totals.total_events) : '—'} />
        <KpiCard label="Avg Startup" value={totals ? fmt1(totals.startup_avg_ms) : '—'} unit="ms" />
        <KpiCard label="Rebuffers" value={totals ? fmtInt(totals.buffer_count) : '—'} />
        <KpiCard label="Errors" value={totals ? fmtInt(totals.error_count) : '—'} />
        <KpiCard label="Buffer Ratio" value={totals ? fmt2(totals.buffer_ratio_per_view ?? 0) : '—'} unit="/view" />
        <KpiCard label="Avg Bitrate" value={totals ? fmtInt(totals.bitrate_avg_kbps) : '—'} unit="kbps" />
        <KpiCard label="Watch Time" value={totals ? fmt1(totals.watch_time_min) : '—'} unit="min" />
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
        <TimeSeriesCard title="Events per minute" minutes={live.data?.minutes} metricKey="total_events" />
        <TimeSeriesCard title="Rebuffers per minute" minutes={live.data?.minutes} metricKey="buffer_count" />
        <SegmentBarCard title="Views by player" segments={players.data?.players} metricKey="total_views" />
        <SegmentBarCard title="Views by device" segments={devices.data?.devices} metricKey="total_views" />
      </section>

      <footer className="app-footer">
        Polling <code>{API_BASE}</code> every {POLL_INTERVAL_MS / 1000}s
      </footer>
    </div>
  )
}
