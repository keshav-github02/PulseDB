import { useEffect, useRef, useState } from 'react'

export interface PollResult<T> {
  data: T | null
  error: string | null
}

/// Poll @p fetcher every @p intervalMs, exposing the latest data or the
/// most recent error. Fetches once immediately on mount.
export function usePolling<T>(fetcher: () => Promise<T>, intervalMs: number): PollResult<T> {
  const [data, setData] = useState<T | null>(null)
  const [error, setError] = useState<string | null>(null)

  // Keep the latest fetcher without restarting the interval each render.
  const fetcherRef = useRef(fetcher)
  fetcherRef.current = fetcher

  useEffect(() => {
    let active = true

    const tick = async () => {
      try {
        const next = await fetcherRef.current()
        if (active) {
          setData(next)
          setError(null)
        }
      } catch (e) {
        if (active) {
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
  }, [intervalMs])

  return { data, error }
}
