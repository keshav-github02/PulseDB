const nf = new Intl.NumberFormat('en-US')

/// Whole number with thousands separators.
export const fmtInt = (n: number): string => nf.format(Math.round(n))

/// One decimal place.
export const fmt1 = (n: number): string => n.toFixed(1)

/// Two decimal places.
export const fmt2 = (n: number): string => n.toFixed(2)

/// The "HH:MM" portion of an ISO-ish "YYYY-MM-DDTHH:MM" minute label.
export const minuteLabel = (iso: string): string => iso.slice(11, 16)
