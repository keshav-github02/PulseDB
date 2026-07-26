import {
  Area,
  AreaChart,
  CartesianGrid,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts'
import type { MinutePoint, PlottableMetricKey } from '../api'
import { minuteLabel } from '../format'
import { colors, tooltipStyles } from '../theme'

interface TimeSeriesCardProps {
  title: string
  minutes: MinutePoint[] | undefined
  metricKey: PlottableMetricKey
  color?: string
}

/// A single-series area chart of one metric over the recent minute buckets.
/// One series, so no legend is needed -- the title names the metric.
export function TimeSeriesCard({ title, minutes, metricKey, color = colors.series1 }: TimeSeriesCardProps) {
  const data = (minutes ?? []).map((point) => ({
    label: minuteLabel(point.minute),
    value: point.metrics[metricKey],
  }))
  const gradientId = `grad-${String(metricKey)}`

  return (
    <div className="card">
      <div className="card-title">{title}</div>
      <div className="chart">
        {data.length === 0 ? (
          <div className="placeholder">Waiting for data…</div>
        ) : (
          <ResponsiveContainer width="100%" height="100%">
            <AreaChart data={data} margin={{ top: 8, right: 12, bottom: 4, left: -8 }}>
              <defs>
                <linearGradient id={gradientId} x1="0" y1="0" x2="0" y2="1">
                  <stop offset="0%" stopColor={color} stopOpacity={0.35} />
                  <stop offset="100%" stopColor={color} stopOpacity={0.02} />
                </linearGradient>
              </defs>
              <CartesianGrid stroke={colors.grid} vertical={false} />
              <XAxis
                dataKey="label"
                tick={{ fill: colors.muted, fontSize: 12 }}
                tickLine={false}
                axisLine={{ stroke: colors.baseline }}
                minTickGap={24}
              />
              <YAxis
                tick={{ fill: colors.muted, fontSize: 12 }}
                tickLine={false}
                axisLine={false}
                width={44}
                allowDecimals={false}
              />
              <Tooltip {...tooltipStyles} />
              <Area
                type="monotone"
                dataKey="value"
                name={title}
                stroke={color}
                strokeWidth={2}
                fill={`url(#${gradientId})`}
                dot={false}
                activeDot={{ r: 4, strokeWidth: 0 }}
                isAnimationActive={false}
              />
            </AreaChart>
          </ResponsiveContainer>
        )}
      </div>
    </div>
  )
}
