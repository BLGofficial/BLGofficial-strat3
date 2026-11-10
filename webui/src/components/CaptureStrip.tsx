// What the running capture is doing, or how the last one ended. Shown above both panels:
// the plugin runs one capture at a time, whichever panel started it.
import { elapsed } from '../format'
import { cancel, type Command } from '../protocol/commands'
import type { CaptureResult, SessionState } from '../protocol/state'
import { useStalled } from './useStalled'

export interface PanelProps {
  readonly state: SessionState
  readonly send: (command: Command) => void
}

const RESULT_TEXT: Readonly<Record<CaptureResult, string>> = {
  none: '',
  moves: 'Analysis done.',
  noMoves: 'Analysis done. Nothing to change.',
  referenceAdded: 'Reference added.',
  silent: 'No signal in the capture. Nothing changed.',
  cancelled: 'Capture cancelled.',
}

/** The capture status strip. */
export function CaptureStrip({ state, send }: PanelProps) {
  const { capture, result } = state
  const running = capture.kind !== 'none'
  const stalled = useStalled(running && !capture.paused, capture.progress)

  const note = !running ? RESULT_TEXT[result] : capture.paused ? 'Paused while delta is on.' : stalled ? 'No audio arriving. Start playback.' : ''
  const warning = running && note !== ''

  return (
    <section aria-label="Capture" className={running || note !== '' ? 'border-b border-rule px-3 py-2' : ''}>
      {running && (
        <div>
          <div className="flex items-baseline justify-between">
            <span className="label text-text">{capture.kind === 'analyse' ? 'Analysing' : 'Capturing reference'}</span>
            <span className="num">{elapsed(capture.progress, capture.seconds)}</span>
          </div>
          <div className="mt-2 flex items-center">
            <div
              role="progressbar"
              aria-label="Capture progress"
              aria-valuemin={0}
              aria-valuemax={100}
              aria-valuenow={Math.round(capture.progress * 100)}
              className="mr-2 h-1 flex-1 bg-rule"
            >
              <div className={capture.paused || stalled ? 'h-full bg-muted' : 'h-full bg-signal'} style={{ width: `${capture.progress * 100}%` }} />
            </div>
            <button type="button" className="btn" onClick={() => send(cancel())}>
              Cancel
            </button>
          </div>
        </div>
      )}
      {/* Always in the page, even when empty: a status region only announces changes made
          while it exists. */}
      <p role="status" className={`label ${warning ? 'mt-2 text-signal' : ''}`}>
        {note}
      </p>
    </section>
  )
}
