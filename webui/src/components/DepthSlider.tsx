// The depth slider of one move: how much of the proposal is applied, 0 to 100 %.
import { useEffect, useRef, useState, type CSSProperties } from 'react'

import { percent } from '../format'
import { depth as depthCommand, type Command } from '../protocol/commands'

/** At most one depth command per this many ms while dragging. Each one moves real plugin
 *  parameters inside host gestures; 20 a second still sounds continuous. */
export const SEND_INTERVAL_MS = 50

/** How long a released value is shown while waiting for the plugin to confirm it. Past
 *  that, the plugin refused it and the slider must show what the plugin really holds. */
export const CONFIRM_MS = 600

interface Props {
  readonly id: number
  /** The depth the plugin holds, 0..1. */
  readonly depth: number
  /** Accepted: the depth is audible now. Pending: it is the depth the move will be accepted at. */
  readonly live: boolean
  readonly send: (command: Command) => void
}

/** A range input that follows the hand at once and the plugin otherwise. */
export function DepthSlider({ id, depth, live, send }: Props) {
  // What the hand set, in percent, until the plugin confirms it or CONFIRM_MS passes.
  const [held, setHeld] = useState<number | null>(null)
  const shown = held ?? Math.round(depth * 100)

  // Timers outlive renders, so they read the current props through a ref.
  const latest = useRef({ id, send })
  latest.current = { id, send }
  const throttle = useRef<{ timer: ReturnType<typeof setTimeout> | undefined; queued: number | null }>({ timer: undefined, queued: null })
  const confirm = useRef<ReturnType<typeof setTimeout> | undefined>(undefined)

  const sendNow = (value: number) => latest.current.send(depthCommand(latest.current.id, value / 100))

  const tick = () => {
    const t = throttle.current
    if (t.queued === null) {
      t.timer = undefined
      return
    }
    sendNow(t.queued)
    t.queued = null
    t.timer = setTimeout(tick, SEND_INTERVAL_MS)
  }

  const change = (value: number) => {
    clearTimeout(confirm.current) // the hand is on the slider again
    setHeld(value)
    const t = throttle.current
    if (t.timer === undefined) {
      sendNow(value)
      t.timer = setTimeout(tick, SEND_INTERVAL_MS)
    } else {
      t.queued = value
    }
  }

  // Nothing is sent here: a value still waiting goes out on the next beat, so the rate cap
  // also holds for a held-down arrow key (each repeat is a full input-then-release).
  const release = () => {
    clearTimeout(confirm.current)
    confirm.current = setTimeout(() => setHeld(null), CONFIRM_MS)
  }

  // React's onChange is the DOM "input" event: it fires for every step of a drag. The end of
  // the drag is the DOM "change" event, which React does not expose separately, so it is
  // listened to directly. It fires for mouse, touch and keyboard alike, wherever the pointer
  // is let go (a pointerup handler would miss a release outside the slider).
  const input = useRef<HTMLInputElement>(null)
  const onRelease = useRef(release)
  onRelease.current = release
  useEffect(() => {
    const element = input.current
    const listener = () => onRelease.current()
    element?.addEventListener('change', listener)
    return () => {
      element?.removeEventListener('change', listener)
      clearTimeout(confirm.current)
      // The panel can go away mid-drag (tab switch), and its timers with it: a value still
      // waiting must reach the plugin now.
      const t = throttle.current
      clearTimeout(t.timer)
      if (t.queued !== null) sendNow(t.queued)
    }
    // Once per mount: everything used here is read through refs, so it never goes stale.
  }, [])

  return (
    <div className="mt-2 flex items-center">
      <span className="label mr-2">Depth</span>
      <input
        type="range"
        className="depth flex-1"
        min={0}
        max={100}
        step={1}
        value={shown}
        aria-label={`Depth of move ${id}`}
        data-live={live}
        style={{ '--fill': `${shown}%` } as CSSProperties}
        ref={input}
        onChange={(e) => change(Number(e.currentTarget.value))}
        onBlur={release}
      />
      <span className="num ml-2 w-12 text-right">{percent(shown / 100)}</span>
    </div>
  )
}
