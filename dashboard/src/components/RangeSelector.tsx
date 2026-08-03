import type { TimeRange } from '../config'
import { TIME_RANGES } from '../config'

interface RangeSelectorProps {
  value: TimeRange
  onChange: (range: TimeRange) => void
}

/// Window picker for the time-series charts and the windowed KPI tiles.
///
/// Radio buttons rather than a <select>: there are four mutually exclusive
/// options and they all fit, so making the current one visible without a click
/// is worth the space. Keyboard and screen-reader behaviour comes free from the
/// native control, which a div-based button group would have to reimplement.
export function RangeSelector({ value, onChange }: RangeSelectorProps) {
  return (
    <fieldset className="range-selector">
      <legend className="sr-only">Time range</legend>
      {TIME_RANGES.map((range) => (
        <label
          key={range.label}
          className={`range-option ${range.label === value.label ? 'active' : ''}`}
        >
          <input
            type="radio"
            name="time-range"
            value={range.label}
            checked={range.label === value.label}
            onChange={() => onChange(range)}
          />
          {range.label}
        </label>
      ))}
    </fieldset>
  )
}
