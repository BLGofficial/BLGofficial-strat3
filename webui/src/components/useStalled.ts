import { useEffect, useState } from 'react'

/** How long the progress may stand still before the capture counts as stalled. */
export const STALL_MS = 2000

/**
 * True once `progress` has not changed for STALL_MS while `active`.
 * The plugin counts a capture in audio that arrived, not in time, so a stopped transport
 * leaves the bar standing with no error anywhere. The page can only infer that from the
 * progress not moving.
 */
export function useStalled(active: boolean, progress: number): boolean {
  // The progress value the stall was seen at: any other value means it moved since.
  const [stalledAt, setStalledAt] = useState<number | null>(null)

  useEffect(() => {
    if (!active) {
      setStalledAt(null) // a later capture must not inherit this one's stall
      return
    }
    const timer = setTimeout(() => setStalledAt(progress), STALL_MS)
    return () => clearTimeout(timer)
  }, [active, progress])

  return active && stalledAt === progress
}
