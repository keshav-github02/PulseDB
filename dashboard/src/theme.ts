// Dark-theme colors taken from the validated data-viz reference palette
// (dark-surface steps). Series slots are assigned in fixed categorical
// order; single-measure charts use a single hue (magnitude, not identity).
export const colors = {
  surface: '#1a1a19',
  page: '#0d0d0d',
  textPrimary: '#ffffff',
  textSecondary: '#c3c2b7',
  muted: '#898781',
  grid: '#2c2c2a',
  baseline: '#383835',
  border: 'rgba(255,255,255,0.10)',
  series1: '#3987e5', // blue  (categorical slot 1, dark)
  series2: '#008300', // green (categorical slot 2)
} as const

// Shared Recharts tooltip styling for the dark surface.
export const tooltipStyles = {
  contentStyle: {
    background: colors.surface,
    border: `1px solid ${colors.border}`,
    borderRadius: 8,
    color: colors.textPrimary,
  },
  labelStyle: { color: colors.textSecondary },
  itemStyle: { color: colors.textPrimary },
  cursor: { fill: 'rgba(255,255,255,0.05)', stroke: colors.baseline },
} as const
