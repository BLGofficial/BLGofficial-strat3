// The assistant panel: capture the mix, then decide on each proposal. Every sentence about
// a move (reason, metric) is the plugin's own text; the page adds only labels and controls.
import { memo } from 'react'

import type { PanelProps } from '../components/CaptureStrip'
import { DepthSlider } from '../components/DepthSlider'
import { fixed1, layerName, percent, plain } from '../format'
import { accept, analyse, applyAll, reject, type Command } from '../protocol/commands'
import type { Move } from '../protocol/state'

const STATUS_LABEL = { pending: 'Pending', accepted: 'Accepted', rejected: 'Rejected' } as const

interface MoveCardProps {
  readonly move: Move
  readonly send: (command: Command) => void
}

/** One move. Memoised: the store keeps an unchanged move's object, so its card does not
 *  re-render while only the capture progress changes. */
const MoveCard = memo(function MoveCard({ move, send }: MoveCardProps) {
  const { id, status, locked, changes } = move
  const scales = changes.some((c) => c.scaled)
  // Depth does nothing to a rejected move, to history, or to a move made only of switches.
  const hasSlider = !locked && status !== 'rejected' && scales
  const statusText = locked && status === 'accepted' && scales ? `Accepted at ${percent(move.depth)}` : STATUS_LABEL[status]

  return (
    <li aria-label={`Move ${id}`} className={`border-b border-rule px-3 py-2 ${locked ? 'text-muted' : ''}`}>
      <div className="flex items-baseline justify-between">
        <span className="label text-text">{layerName(move.layer)}</span>
        <span className={`label ${status === 'accepted' && !locked ? 'text-signal' : ''}`}>{statusText}</span>
      </div>
      <p className="mt-1">{move.reason}</p>
      <p className="mt-1 flex items-baseline justify-between text-muted">
        <span>{move.metric}</span>
        <span className="num ml-2 whitespace-nowrap">
          {fixed1(move.metricBefore)} → {fixed1(move.metricTarget)}
        </span>
      </p>

      {hasSlider && <DepthSlider id={id} depth={move.depth} live={status === 'accepted'} send={send} />}

      {!locked && (
        <div className="mt-2 flex">
          {status !== 'accepted' && (
            <button type="button" className="btn btn-signal mr-2" aria-label={`Accept move ${id}`} onClick={() => send(accept(id))}>
              Accept
            </button>
          )}
          {status === 'pending' && (
            <button type="button" className="btn" aria-label={`Reject move ${id}`} onClick={() => send(reject(id))}>
              Reject
            </button>
          )}
          {status === 'accepted' && (
            // The plugin's reject on an accepted move: parameters go back to analysis time.
            <button type="button" className="btn" aria-label={`Undo move ${id}`} onClick={() => send(reject(id))}>
              Undo
            </button>
          )}
        </div>
      )}

      <details className="mt-2">
        <summary className="label">
          {changes.length} {changes.length === 1 ? 'change' : 'changes'}
        </summary>
        <table className="num mt-1 w-full text-muted">
          <tbody>
            {changes.map((c) => (
              <tr key={c.paramId}>
                <td className="pr-2">{c.paramId}</td>
                <td className="whitespace-nowrap text-right">
                  {plain(c.from)} → {plain(c.to)}
                </td>
                <td className="whitespace-nowrap pl-2 text-right">{c.scaled ? '× depth' : ''}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </details>
    </li>
  )
})

/** The assistant panel. */
export function Assistant({ state, send }: PanelProps) {
  const busy = state.capture.kind !== 'none'
  const current = state.moves.filter((m) => !m.locked)
  const history = state.moves.filter((m) => m.locked).reverse() // newest first
  const pending = current.filter((m) => m.status === 'pending').length

  return (
    <div>
      <div className="border-b border-rule px-3 py-2">
        <div className="flex">
          <button type="button" className="btn mr-2 flex-1" disabled={busy} onClick={() => send(analyse(30))}>
            Analyse 30 <span className="unit">s</span>
          </button>
          <button type="button" className="btn flex-1" disabled={busy} onClick={() => send(analyse(60))}>
            Analyse 60 <span className="unit">s</span>
          </button>
        </div>
        {!state.target.captured && <p className="mt-2 text-muted">No target. Tonal moves need one: capture a reference in Target.</p>}
      </div>

      <div className="flex items-center justify-between border-b border-rule px-3 py-2">
        <h2 className="label text-text">Proposals</h2>
        <span className="label ml-2 mr-auto">{pending} pending</span>
        <button type="button" className="btn" disabled={pending === 0} onClick={() => send(applyAll())}>
          Apply all
        </button>
      </div>

      {current.length === 0 ? (
        <p className="border-b border-rule px-3 py-2 text-muted">No proposals. Play the mix and analyse.</p>
      ) : (
        <ul aria-label="Proposals">
          {current.map((m) => (
            <MoveCard key={m.id} move={m} send={send} />
          ))}
        </ul>
      )}

      {history.length > 0 && (
        <>
          <h2 className="label border-b border-rule px-3 py-2">History</h2>
          <ul aria-label="History">
            {history.map((m) => (
              <MoveCard key={m.id} move={m} send={send} />
            ))}
          </ul>
        </>
      )}
    </div>
  )
}
