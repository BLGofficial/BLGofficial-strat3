// The assistant panel: analyse, then decide on each proposal. Contracts:
//   - Analyse 30 s / 60 s send the command; both are off while any capture runs
//   - every adjustable move is listed with the plugin's own reason, what was measured, and
//     each parameter it changes
//   - a pending move offers Accept and Reject; an accepted one Undo (the plugin's reject,
//     which restores the values from analysis time); a rejected one Accept
//   - the depth slider exists only where depth does something: an adjustable, not rejected
//     move with at least one scaled change. It shows the plugin's depth, follows the hand
//     at once, sends at most one command per 50 ms (a held-down arrow key included) and
//     always ends on the latest value, and falls back to the plugin's depth if the plugin
//     never confirms
//   - Apply all accepts everything pending and is off when nothing is
//   - moves decided before the latest analysis are history: newest first, no controls
//   - without a target the panel says tonal moves need one
import { act, fireEvent, render, screen, within } from '@testing-library/react'
import { describe, expect, it, vi } from 'vitest'

import { accept, analyse, applyAll, depth, reject } from '../protocol/commands'
import type { SessionState } from '../protocol/state'
import { state } from '../test/fixtures'
import { Assistant } from './Assistant'

const show = (s: SessionState) => {
  const send = vi.fn()
  const view = render(<Assistant state={s} send={send} />)
  return { send, ...view }
}

const proposals = () => within(screen.getByRole('list', { name: 'Proposals' })).getAllByRole('listitem')
const move = (id: number) => screen.getByRole('listitem', { name: `Move ${id}` })
const slider = (id: number) => screen.getByRole<HTMLInputElement>('slider', { name: `Depth of move ${id}` })
const button = (name: string) => screen.getByRole<HTMLButtonElement>('button', { name })

// What a browser does with a range input: one "input" event per step while the thumb moves,
// then a single "change" event when it is let go.
const drag = (element: HTMLInputElement, percent: number) => fireEvent.input(element, { target: { value: String(percent) } })
const letGo = (element: HTMLInputElement) => fireEvent.change(element)

describe('Assistant: analysing', () => {
  it('sends the analysis the user asked for', () => {
    const { send } = show(state('idle'))
    fireEvent.click(button('Analyse 30 s'))
    fireEvent.click(button('Analyse 60 s'))
    expect(send.mock.calls).toEqual([[analyse(30)], [analyse(60)]])
  })

  it.each(['analyse-running', 'analyse-paused', 'reference-running'] as const)('cannot start another capture during "%s"', (name) => {
    const { send } = show(state(name))
    expect(button('Analyse 30 s').disabled).toBe(true)
    expect(button('Analyse 60 s').disabled).toBe(true)
    fireEvent.click(button('Analyse 30 s'))
    expect(send).not.toHaveBeenCalled()
  })

  it('says tonal moves need a target when there is none', () => {
    show(state('moves-no-target'))
    expect(screen.getByText('No target. Tonal moves need one: capture a reference in Target.')).toBeTruthy()
  })

  it('does not say so once a target exists', () => {
    show(state('moves-pending'))
    expect(screen.queryByText(/No target/)).toBeNull()
  })

  it('says what to do when there is nothing to decide', () => {
    show(state('idle'))
    expect(screen.getByText('No proposals. Play the mix and analyse.')).toBeTruthy()
    expect(screen.queryByRole('list', { name: 'Proposals' })).toBeNull()
    expect(screen.queryByRole('list', { name: 'History' })).toBeNull()
  })
})

describe('Assistant: proposals', () => {
  it('lists every adjustable move in the plugin order, with the plugin reason', () => {
    const s = state('moves-pending')
    show(s)
    const items = proposals()
    expect(items.map((li) => li.getAttribute('aria-label'))).toEqual(['Move 1', 'Move 2', 'Move 3', 'Move 4', 'Move 5'])
    s.moves.forEach((m, i) => expect(within(items[i] as HTMLElement).getByText(m.reason)).toBeTruthy())
    expect(screen.getByText('5 pending')).toBeTruthy()
  })

  it('shows the layer, what was measured and where the move aims', () => {
    show(state('moves-pending'))
    expect(move(1).textContent).toContain('EQ')
    expect(move(1).textContent).toContain('deviation from target (dB)')
    expect(move(1).textContent).toContain('4.3 → 0.0')
    expect(move(5).textContent).toContain('LIMIT')
    expect(move(5).textContent).toContain('integrated LUFS')
    expect(move(5).textContent).toContain('-17.2 → -14.0')
  })

  it('lists each parameter a move changes, from and to', () => {
    const s = state('moves-pending')
    show(s)
    const first = s.moves[0]
    if (first === undefined) throw new Error('no move')
    expect(within(move(1)).getByText(`${first.changes.length} changes`)).toBeTruthy()
    const rows = within(move(1)).getAllByRole('row')
    expect(rows.map((r) => r.textContent)).toEqual(['eq.on0 → 1', 'eq.b1.on0 → 1', 'eq.b1.freq30 → 250', 'eq.b1.q0.707 → 1.707', 'eq.b1.gain0 → -3.035× depth'])
  })

  it('offers Accept and Reject on a pending move', () => {
    const { send } = show(state('moves-pending'))
    fireEvent.click(button('Accept move 1'))
    fireEvent.click(button('Reject move 3'))
    expect(send.mock.calls).toEqual([[accept(1)], [reject(3)]])
    expect(screen.queryByRole('button', { name: 'Undo move 1' })).toBeNull()
  })

  it('offers Undo on an accepted move and Accept on a rejected one', () => {
    const { send } = show(state('moves-decided'))
    expect(within(move(1)).getByText('Accepted')).toBeTruthy()
    expect(within(move(4)).getByText('Rejected')).toBeTruthy()
    expect(within(move(3)).getByText('Pending')).toBeTruthy()
    expect(within(move(1)).getAllByRole('button').map((b) => b.getAttribute('aria-label'))).toEqual(['Undo move 1'])
    expect(within(move(4)).getAllByRole('button').map((b) => b.getAttribute('aria-label'))).toEqual(['Accept move 4'])
    expect(within(move(3)).getAllByRole('button').map((b) => b.getAttribute('aria-label'))).toEqual(['Accept move 3', 'Reject move 3'])
    fireEvent.click(button('Undo move 1'))
    fireEvent.click(button('Accept move 4'))
    expect(send.mock.calls).toEqual([[reject(1)], [accept(4)]])
    expect(screen.getByText('1 pending')).toBeTruthy()
  })

  it('accepts everything pending with Apply all', () => {
    const { send } = show(state('moves-decided'))
    fireEvent.click(button('Apply all'))
    expect(send.mock.calls).toEqual([[applyAll()]])
  })

  it('has nothing to apply when no move is pending', () => {
    const decided = state('moves-decided')
    const allDecided = { ...decided, moves: decided.moves.filter((m) => m.status !== 'pending') }
    const { send } = show(allDecided)
    expect(button('Apply all').disabled).toBe(true)
    fireEvent.click(button('Apply all'))
    expect(send).not.toHaveBeenCalled()
    expect(screen.getByText('0 pending')).toBeTruthy()
  })
})

describe('Assistant: depth', () => {
  it('shows the depth the plugin holds', () => {
    show(state('moves-decided'))
    expect(slider(1).value).toBe('100')
    expect(slider(2).value).toBe('60')
    expect(within(move(2)).getByText('60 %')).toBeTruthy()
    expect(slider(3).value).toBe('100') // pending: the depth it will be accepted at
  })

  it('has no slider on a move made only of switches', () => {
    show(state('moves-no-target'))
    expect(screen.queryByRole('slider', { name: 'Depth of move 1' })).toBeNull() // mono bass: on or off
    expect(slider(2).value).toBe('100') // the limiter threshold scales
  })

  it('has no slider on a rejected move', () => {
    const decided = state('moves-decided')
    show({ ...decided, moves: decided.moves.map((m) => (m.id === 2 ? { ...m, status: 'rejected' as const } : m)) })
    expect(screen.queryByRole('slider', { name: 'Depth of move 2' })).toBeNull()
    expect(slider(1)).toBeTruthy()
  })

  it('follows the hand at once and sends the depth', () => {
    const { send } = show(state('moves-decided'))
    drag(slider(2), 35)
    expect(send.mock.calls).toEqual([[depth(2, 0.35)]])
    expect(slider(2).value).toBe('35')
    expect(within(move(2)).getByText('35 %')).toBeTruthy()
  })

  it('sends at most one command per 50 ms, ending on the latest value', () => {
    vi.useFakeTimers()
    const { send } = show(state('moves-decided'))
    drag(slider(2), 35)
    drag(slider(2), 30)
    drag(slider(2), 20)
    expect(send.mock.calls).toEqual([[depth(2, 0.35)]])
    act(() => void vi.advanceTimersByTime(49))
    expect(send).toHaveBeenCalledTimes(1)
    act(() => void vi.advanceTimersByTime(1))
    expect(send.mock.calls).toEqual([[depth(2, 0.35)], [depth(2, 0.2)]])
    act(() => void vi.advanceTimersByTime(500))
    expect(send).toHaveBeenCalledTimes(2)
  })

  it('still sends the last value after the slider is let go, on the same 50 ms beat', () => {
    vi.useFakeTimers()
    const { send } = show(state('moves-decided'))
    drag(slider(2), 35)
    drag(slider(2), 10)
    letGo(slider(2))
    expect(send.mock.calls).toEqual([[depth(2, 0.35)]])
    act(() => void vi.advanceTimersByTime(50))
    expect(send.mock.calls).toEqual([[depth(2, 0.35)], [depth(2, 0.1)]])
    act(() => void vi.advanceTimersByTime(500))
    expect(send).toHaveBeenCalledTimes(2)
  })

  it('holds a held-down arrow key to the same rate', () => {
    vi.useFakeTimers()
    const { send } = show(state('moves-decided'))
    // Key repeat: every repeat is a step and a release.
    for (const percent of [61, 62, 63]) {
      drag(slider(2), percent)
      letGo(slider(2))
    }
    expect(send.mock.calls).toEqual([[depth(2, 0.61)]])
    act(() => void vi.advanceTimersByTime(50))
    expect(send.mock.calls).toEqual([[depth(2, 0.61)], [depth(2, 0.63)]])
  })

  it('treats focus leaving the slider as letting go', () => {
    vi.useFakeTimers()
    const { send } = show(state('moves-decided'))
    drag(slider(2), 35)
    drag(slider(2), 10)
    fireEvent.blur(slider(2))
    act(() => void vi.advanceTimersByTime(50))
    expect(send.mock.calls).toEqual([[depth(2, 0.35)], [depth(2, 0.1)]])
    expect(slider(2).value).toBe('10')
    act(() => void vi.advanceTimersByTime(550))
    expect(slider(2).value).toBe('60') // let go 600 ms ago and never confirmed
  })

  it('sends nothing more when the slider is let go with nothing waiting', () => {
    vi.useFakeTimers()
    const { send } = show(state('moves-decided'))
    drag(slider(2), 35)
    letGo(slider(2))
    act(() => void vi.advanceTimersByTime(500))
    expect(send.mock.calls).toEqual([[depth(2, 0.35)]])
  })

  it('follows the plugin when the hand is off', () => {
    const decided = state('moves-decided')
    const { rerender } = show(decided)
    rerender(<Assistant state={{ ...decided, moves: decided.moves.map((m) => (m.id === 2 ? { ...m, depth: 0.8 } : m)) }} send={vi.fn()} />)
    expect(slider(2).value).toBe('80')
    expect(within(move(2)).getByText('80 %')).toBeTruthy()
  })

  it('marks the slider of an accepted move as live, and of a pending one as not', () => {
    show(state('moves-decided'))
    expect(slider(1).dataset.live).toBe('true')
    expect(slider(3).dataset.live).toBe('false')
  })

  it('sends through the current link when the state changes under a drag', () => {
    vi.useFakeTimers()
    const decided = state('moves-decided')
    const send = vi.fn()
    const { rerender } = render(<Assistant state={decided} send={send} />)
    drag(slider(2), 35)
    drag(slider(2), 30)
    const later = vi.fn()
    rerender(<Assistant state={{ ...decided, revision: decided.revision + 1 }} send={later} />)
    act(() => void vi.advanceTimersByTime(50))
    expect(send.mock.calls).toEqual([[depth(2, 0.35)]])
    expect(later.mock.calls).toEqual([[depth(2, 0.3)]])
  })

  it('sends the last value when the panel goes away mid-drag', () => {
    vi.useFakeTimers()
    const { send, unmount } = show(state('moves-decided'))
    drag(slider(2), 35)
    drag(slider(2), 10)
    unmount()
    expect(send.mock.calls).toEqual([[depth(2, 0.35)], [depth(2, 0.1)]])
  })

  it('holds the released value until the plugin confirms it', () => {
    vi.useFakeTimers()
    const decided = state('moves-decided')
    const { rerender } = show(decided)
    drag(slider(2), 35)
    letGo(slider(2))
    act(() => void vi.advanceTimersByTime(100))
    expect(slider(2).value).toBe('35')

    const confirmed = { ...decided, moves: decided.moves.map((m) => (m.id === 2 ? { ...m, depth: 0.35 } : m)) }
    rerender(<Assistant state={confirmed} send={vi.fn()} />)
    expect(slider(2).value).toBe('35')
    act(() => void vi.advanceTimersByTime(2000))
    expect(slider(2).value).toBe('35')
  })

  it('goes back to the plugin depth if the plugin never confirms', () => {
    vi.useFakeTimers()
    show(state('moves-decided'))
    drag(slider(2), 35)
    letGo(slider(2))
    act(() => void vi.advanceTimersByTime(599))
    expect(slider(2).value).toBe('35')
    act(() => void vi.advanceTimersByTime(1))
    expect(slider(2).value).toBe('60')
  })

  it('does not fall back while the hand is still on the slider', () => {
    vi.useFakeTimers()
    show(state('moves-decided'))
    drag(slider(2), 35)
    act(() => void vi.advanceTimersByTime(5000))
    expect(slider(2).value).toBe('35')
  })

  it('stores the depth of a pending move too', () => {
    const { send } = show(state('moves-pending'))
    drag(slider(3), 50)
    expect(send.mock.calls).toEqual([[depth(3, 0.5)]])
  })
})

describe('Assistant: history', () => {
  it('keeps moves decided before the latest analysis apart, newest first, without controls', () => {
    show(state('moves-history'))
    expect(proposals().map((li) => li.getAttribute('aria-label'))).toEqual(['Move 6', 'Move 7', 'Move 8'])
    const history = screen.getByRole('list', { name: 'History' })
    expect(within(history).getAllByRole('listitem').map((li) => li.getAttribute('aria-label'))).toEqual(['Move 5', 'Move 4', 'Move 2', 'Move 1'])
    expect(within(history).queryAllByRole('button')).toEqual([])
    expect(within(history).queryAllByRole('slider')).toEqual([])
    expect(within(move(2)).getByText('Accepted at 60 %')).toBeTruthy()
    expect(within(move(1)).getByText('Accepted at 100 %')).toBeTruthy()
    expect(within(move(4)).getByText('Rejected')).toBeTruthy()
    expect(screen.getByText('3 pending')).toBeTruthy()
  })

  it('shows no history before a second analysis', () => {
    show(state('moves-decided'))
    expect(screen.queryByRole('list', { name: 'History' })).toBeNull()
  })
})
