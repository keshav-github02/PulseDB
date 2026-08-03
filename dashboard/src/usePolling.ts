import { useEffect, useRef, useState } from 'react'

export interface PollResult<T> {
  data: T | null
  error: string | null
}

/// Poll @p fetcher every @p intervalMs, exposing the latest data or the
/// most recent error. Fetches once immediately on mount.
/// @param resetKey change this to restart polling immediately rather than at the
///        next tick. Switching the time range changes what `fetcher` asks for,
///        and without a restart the panel would keep showing the previous
///        window's data until the interval next elapsed -- up to ten seconds on
///        the widest range, which reads as the control having done nothing.
export function usePolling<T>(
  fetcher: () => Promise<T>,
  intervalMs: number,
  resetKey?: unknown,
): PollResult<T> {
  const [data, setData] = useState<T | null>(null)
  const [error, setError] = useState<string | null>(null)

  // Keep the latest fetcher without restarting the interval each render.
  //
  // Assigned in an effect rather than during render: mutating a ref while
  // rendering is a side effect, which StrictMode's deliberate double-invoke and
  // concurrent rendering are both entitled to break.
  const fetcherRef = useRef(fetcher)
  useEffect(() => {
    fetcherRef.current = fetcher
  }, [fetcher])

  useEffect(() => {
    let active = true
    // Sequence numbers of the newest request started and the newest response
    // already applied to state.
    let started = 0
    let applied = 0

    const tick = async () => {
      const seq = ++started
      try {
        const next = await fetcherRef.current()
        // The interval fires regardless of whether the previous request has
        // returned, so several can be in flight at once and they are not
        // guaranteed to resolve in order -- these endpoints traverse every
        // bucket under the store's mutex, so their latency varies with load.
        // Without this check a slower earlier response could land after a newer
        // one and overwrite it, making the dashboard jump backwards to stale
        // numbers with no indication anything was wrong.
        if (active && seq > applied) {
          applied = seq
          setData(next)
          setError(null)
        }
      } catch (e) {
        if (active && seq > applied) {
          applied = seq
          setError(e instanceof Error ? e.message : String(e))
        }
      }
    }

    void tick()
    const id = window.setInterval(() => void tick(), intervalMs)
    return () => {
      active = false
      window.clearInterval(id)
    }
  }, [intervalMs, resetKey])

  return { data, error }
}
