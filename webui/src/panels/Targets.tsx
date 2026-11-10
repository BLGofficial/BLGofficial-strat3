// The target panel: the user's own target, built from reference tracks played through the
// plugin. There is no built-in genre curve and (for now) no file import, so there are no
// controls for either.
import { useState } from 'react'

import type { PanelProps } from '../components/CaptureStrip'
import { TargetCurve } from '../components/TargetCurve'
import { captureReference, clearTarget } from '../protocol/commands'

/** The target panel. */
export function Targets({ state, send }: PanelProps) {
  const { target, capture } = state
  const full = target.references >= target.maxReferences
  const cannotCapture = capture.kind !== 'none' || full

  // Clearing throws away minutes of captured references, so it takes a second click. The
  // question is tied to the count it asked about: if that changes underneath (a capture
  // finishes, another clear lands), the question is no longer the one the user saw.
  // Dropped while rendering, not in an effect: React then renders again before anything is
  // painted, so the stale question is never on screen, not even for a frame.
  const [askedFor, setAskedFor] = useState<number | null>(null)
  if (askedFor !== null && askedFor !== target.references) setAskedFor(null)
  const asking = askedFor !== null

  return (
    <div>
      <div className="border-b border-rule px-3 py-2">
        <div className="flex items-baseline justify-between">
          <h2 className="label text-text">References</h2>
          <span className="num">
            {target.references} / {target.maxReferences}
          </span>
        </div>
        <p className="mt-1 text-muted">Play a reference track through STRAT3, then capture. The target is the average shape of all references.</p>
        <div className="mt-2 flex">
          <button type="button" className="btn mr-2 flex-1" disabled={cannotCapture} onClick={() => send(captureReference(30))}>
            Capture 30 <span className="unit">s</span>
          </button>
          <button type="button" className="btn flex-1" disabled={cannotCapture} onClick={() => send(captureReference(60))}>
            Capture 60 <span className="unit">s</span>
          </button>
        </div>
        {full && (
          <p className="mt-2 text-muted">
            {target.references} of {target.maxReferences} used. Clear the target to capture again.
          </p>
        )}
      </div>

      <div className="border-b border-rule px-3 py-2">
        <h2 className="label mb-2 text-text">Target</h2>
        {target.captured ? <TargetCurve levelDb={target.levelDb} bandHz={target.bandHz} /> : <p className="text-muted">No target yet.</p>}
      </div>

      <div className="px-3 py-2">
        {asking ? (
          <div className="flex items-center">
            <span className="mr-auto">
              Clear {target.references} {target.references === 1 ? 'reference' : 'references'}?
            </span>
            <button
              type="button"
              className="btn btn-signal mr-2"
              onClick={() => {
                setAskedFor(null)
                send(clearTarget())
              }}
            >
              Clear
            </button>
            <button type="button" className="btn" onClick={() => setAskedFor(null)}>
              Keep
            </button>
          </div>
        ) : (
          <button type="button" className="btn" disabled={target.references === 0} onClick={() => setAskedFor(target.references)}>
            Clear target
          </button>
        )}
      </div>
    </div>
  )
}
