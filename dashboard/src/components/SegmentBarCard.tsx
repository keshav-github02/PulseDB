import {
  Bar,
  BarChart,
  CartesianGrid,
  LabelList,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts'
import type { PlottableMetricKey, Segment } from '../api'
import { colors, tooltipStyles } from '../theme'

interface SegmentBarCardProps {
  title: string
  segments: Segment[] | undefined
  metricKey: PlottableMetricKey
}

/// A single-measure bar chart across categories (players or devices).
/// One measure -> a single hue (magnitude, not identity); bars are sorted
/// descending and directly labelled since the category set is small.
export function SegmentBarCard({ title, segments, metricKey }: SegmentBarCardProps) {
  const data = (segments ?? [])
    .map((segment) => ({ name: segment.name, value: segment.metrics[metricKey] }))
    .sort((a, b) => b.value - a.value)

  return (
    <div className="card">
      <div className="card-title">{title}</div>
      <div className="chart">
        {data.length === 0 ? (
          <div className="placeholder">Waiting for data…</div>
        ) : (
          <ResponsiveContainer width="100%" height="100%">
            <BarChart data={data} margin={{ top: 16, right: 12, bottom: 4, left: -8 }}>
              <CartesianGrid stroke={colors.grid} vertical={false} />
              <XAxis
                dataKey="name"
                tick={{ fill: colors.textSecondary, fontSize: 12 }}
                tickLine={false}
                axisLine={{ stroke: colors.baseline }}
                interval={0}
              />
              <YAxis
                tick={{ fill: colors.muted, fontSize: 12 }}
                tickLine={false}
                axisLine={false}
                width={44}
                allowDecimals={false}
              />
              <Tooltip {...tooltipStyles} />
              <Bar dataKey="value" name={title} fill={colors.series1} radius={[4, 4, 0, 0]} isAnimationActive={false}>
                <LabelList dataKey="value" position="top" fill={colors.textSecondary} fontSize={12} />
              </Bar>
            </BarChart>
          </ResponsiveContainer>
        )}
      </div>
    </div>
  )
}
