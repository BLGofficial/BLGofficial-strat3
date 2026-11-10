// The capture strip: what the running capture is doing, or how the last one ended. It sits
// above both panels because the plugin runs one capture at a time. Contracts:
//   - a running capture shows its kind, whole seconds done, a progress bar and Cancel
//   - a capture held back by a delta listen says so
//   - a capture whose progress has not moved for 2 s says no audio is arriving (the plugin
//     counts audio, not time, so a stopped transport stalls it); it stops saying so as soon
//     as progress moves, and never says it while paused
//   - with no capture running it shows how the last one ended, or nothing
import { act, fireEvent, render, screen } from '@testing-library/react'
import { describe, expect, it, vi } from 'vitest'

import { cancel } from '../protocol/commands'
import type { SessionState } from '../protocol/state'
import { state, type FixtureName } from '../test/fixtures'
import { CaptureStrip } from './CaptureStrip'

const show = (s: SessionState) => {
  const send = vi.fn()
  const view = render(<CaptureStrip state={s} send={send} />)
  return { send, ...view }
}

describe('CaptureStrip: a running capture', () => {
  it('shows an analysis with its seconds, progress and Cancel', () => {
    const { send } = show(state('analyse-running'))
    expect(screen.getByText('Analysing')).toBeTruthy()
    expect(screen.getByText('12 / 30 s')).toBeTruthy()
    expect(screen.getByRole('progressbar', { name: 'Capture progress' }).getAttribute('aria-valuenow')).toBe('41')
    fireEvent.click(screen.getByRole('button', { name: 'Cancel' }))
    expect(send.mock.calls).toEqual([[cancel()]])
  })

  it('shows a reference capture as such', () => {
    show(state('reference-running'))
    expect(screen.getByText('Capturing reference')).toBeTruthy()
    expect(screen.getByText('42 / 60 s')).toBeTruthy()
    expect(screen.getByRole('progressbar').getAttribute('aria-valuenow')).toBe('70')
  })

  it('says when a delta listen is holding the capture back', () => {
    show(state('analyse-paused'))
    expect(screen.getByRole('status').textContent).toBe('Paused while delta is on.')
  })

  it('says nothing extra while the capture is moving', () => {
    show(state('analyse-running'))
    expect(screen.getByRole('status').textContent).toBe('')
  })

  it('says no audio is arriving once progress has stood still for 2 s', () => {
    vi.useFakeTimers()
    const running = state('analyse-running')
    const { rerender } = show(running)
    act(() => void vi.advanceTimersByTime(1900))
    expect(screen.getByRole('status').textContent).toBe('')
    act(() => void vi.advanceTimersByTime(100))
    expect(screen.getByRole('status').textContent).toBe('No audio arriving. Start playback.')

    rerender(<CaptureStrip state={{ ...running, capture: { ...running.capture, progress: 0.42 } }} send={vi.fn()} />)
    expect(screen.getByRole('status').textContent).toBe('')
  })

  it('keeps the stall clock running across states that do not move the progress', () => {
    vi.useFakeTimers()
    const running = state('analyse-running')
    const { rerender } = show(running)
    act(() => void vi.advanceTimersByTime(1500))
    rerender(<CaptureStrip state={{ ...running, revision: running.revision + 1 }} send={vi.fn()} />)
    act(() => void vi.advanceTimersByTime(500))
    expect(screen.getByRole('status').textContent).toBe('No audio arriving. Start playback.')
  })

  it('does not call a paused capture stalled', () => {
    vi.useFakeTimers()
    show(state('analyse-paused'))
    act(() => void vi.advanceTimersByTime(5000))
    expect(screen.getByRole('status').textContent).toBe('Paused while delta is on.')
  })
})

describe('CaptureStrip: no capture running', () => {
  const results: [FixtureName, string][] = [
    ['idle', ''],
    ['moves-pending', 'Analysis done.'],
    ['no-moves', 'Analysis done. Nothing to change.'],
    ['reference-added', 'Reference added.'],
    ['silent', 'No signal in the capture. Nothing changed.'],
    ['cancelled', 'Capture cancelled.'],
  ]

  it.each(results)('after "%s" it says "%s"', (name, text) => {
    show(state(name))
    expect(screen.getByRole('status').textContent).toBe(text)
    expect(screen.queryByRole('progressbar')).toBeNull()
    expect(screen.queryByRole('button', { name: 'Cancel' })).toBeNull()
  })

  it('never calls an idle session stalled', () => {
    vi.useFakeTimers()
    show(state('idle'))
    act(() => void vi.advanceTimersByTime(5000))
    expect(screen.getByRole('status').textContent).toBe('')
  })
})
