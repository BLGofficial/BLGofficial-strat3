// The target panel: build the user's own target from reference tracks. Contracts:
//   - Capture 30 s / 60 s send the command; both are off while any capture runs and when
//     the reference list is full (the plugin would refuse), and the panel says why
//   - it shows how many references are in, out of the plugin's limit
//   - the target curve is drawn from the plugin's bands, one point each, and only when a
//     target exists
//   - Clear target asks first (the references took minutes of playback to capture): nothing
//     is sent until the second click, and Keep backs out
import { fireEvent, render, screen } from '@testing-library/react'
import { describe, expect, it, vi } from 'vitest'

import { captureReference, clearTarget } from '../protocol/commands'
import type { SessionState } from '../protocol/state'
import { state } from '../test/fixtures'
import { Targets } from './Targets'

const show = (s: SessionState) => {
  const send = vi.fn()
  const view = render(<Targets state={s} send={send} />)
  return { send, ...view }
}

const button = (name: string) => screen.getByRole<HTMLButtonElement>('button', { name })

describe('Targets: capturing references', () => {
  it('sends the capture the user asked for', () => {
    const { send } = show(state('idle'))
    fireEvent.click(button('Capture 30 s'))
    fireEvent.click(button('Capture 60 s'))
    expect(send.mock.calls).toEqual([[captureReference(30)], [captureReference(60)]])
  })

  it.each(['reference-running', 'analyse-running', 'analyse-paused'] as const)('cannot start another capture during "%s"', (name) => {
    const { send } = show(state(name))
    expect(button('Capture 30 s').disabled).toBe(true)
    expect(button('Capture 60 s').disabled).toBe(true)
    fireEvent.click(button('Capture 60 s'))
    expect(send).not.toHaveBeenCalled()
  })

  it('cannot capture past the limit, and says how to go on', () => {
    const { send } = show(state('references-full'))
    expect(screen.getByText('16 / 16')).toBeTruthy()
    expect(button('Capture 30 s').disabled).toBe(true)
    expect(button('Capture 60 s').disabled).toBe(true)
    expect(screen.getByText('16 of 16 used. Clear the target to capture again.')).toBeTruthy()
    fireEvent.click(button('Capture 30 s'))
    expect(send).not.toHaveBeenCalled()
  })

  it('does not talk about the limit before it is reached', () => {
    show(state('reference-added'))
    expect(screen.getByText('1 / 16')).toBeTruthy()
    expect(screen.queryByText(/used\. Clear the target/)).toBeNull()
  })
})

describe('Targets: the curve', () => {
  it('draws one point per band of the captured target', () => {
    show(state('reference-added'))
    const path = screen.getByRole('img', { name: 'Target curve' }).querySelector('path[data-curve]')
    expect(path?.getAttribute('d')?.split('L')).toHaveLength(61)
  })

  it('draws nothing and says so when there is no target', () => {
    show(state('idle'))
    expect(screen.queryByRole('img', { name: 'Target curve' })).toBeNull()
    expect(screen.getByText('No target yet.')).toBeTruthy()
    expect(screen.getByText('0 / 16')).toBeTruthy()
  })
})

describe('Targets: clearing', () => {
  it('has nothing to clear without references', () => {
    const { send } = show(state('idle'))
    expect(button('Clear target').disabled).toBe(true)
    fireEvent.click(button('Clear target'))
    expect(send).not.toHaveBeenCalled()
    expect(screen.queryByRole('button', { name: 'Clear' })).toBeNull()
  })

  it('asks before clearing and sends nothing until confirmed', () => {
    const { send } = show(state('moves-pending'))
    fireEvent.click(button('Clear target'))
    expect(send).not.toHaveBeenCalled()
    expect(screen.getByText('Clear 2 references?')).toBeTruthy()
    fireEvent.click(button('Clear'))
    expect(send.mock.calls).toEqual([[clearTarget()]])
    expect(screen.queryByText('Clear 2 references?')).toBeNull()
    expect(button('Clear target')).toBeTruthy()
  })

  it('backs out with Keep', () => {
    const { send } = show(state('reference-added'))
    fireEvent.click(button('Clear target'))
    expect(screen.getByText('Clear 1 reference?')).toBeTruthy()
    fireEvent.click(button('Keep'))
    expect(send).not.toHaveBeenCalled()
    expect(screen.queryByText('Clear 1 reference?')).toBeNull()
    expect(button('Clear target').disabled).toBe(false)
  })

  it('drops the question if the references are gone meanwhile', () => {
    const { rerender } = show(state('reference-added'))
    fireEvent.click(button('Clear target'))
    rerender(<Targets state={state('idle')} send={vi.fn()} />)
    expect(screen.queryByRole('button', { name: 'Clear' })).toBeNull()
    expect(button('Clear target').disabled).toBe(true)
  })
})
