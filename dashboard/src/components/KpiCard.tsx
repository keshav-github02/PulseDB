interface KpiCardProps {
  label: string
  value: string
  unit?: string
}

/// A single headline metric. Not a chart -- a stat tile, per the form
/// heuristic (a single number is best shown as a number).
export function KpiCard({ label, value, unit }: KpiCardProps) {
  return (
    <div className="kpi-card">
      <div className="kpi-value">
        {value}
        {unit ? <span className="kpi-unit"> {unit}</span> : null}
      </div>
      <div className="kpi-label">{label}</div>
    </div>
  )
}
