// The page's side of the JUCE WebView bridge. Contracts tested here:
//   - outside the plugin (no window.__JUCE__.backend, or one missing a function the page
//     calls) there is no bridge
//   - states arrive on the "assistantState" event; stopping removes the JUCE listener
//   - the page asks for the state with "assistantReady" and sends one command object per
//     "assistantCommand" event, exactly as built
import { describe, expect, it, vi } from 'vitest'

import { analyse, depth } from '../protocol/commands'
import { createFakeBackend } from '../test/fakeBackend'
import { connect } from './juce'

describe('connect', () => {
  it('finds no bridge outside the plugin', () => {
    expect(connect({})).toBeNull()
    expect(connect({ __JUCE__: {} })).toBeNull()
    expect(connect({ __JUCE__: { backend: null } })).toBeNull()
    expect(connect({ __JUCE__: { backend: 'backend' } })).toBeNull()
  })

  it.each(['addEventListener', 'removeEventListener', 'emitEvent'] as const)('finds no bridge when the backend has no %s', (missing) => {
    const { backend } = createFakeBackend()
    expect(connect({ __JUCE__: { backend: { ...backend, [missing]: undefined } } })).toBeNull()
  })

  it('finds the bridge inside the plugin', () => {
    expect(connect({ __JUCE__: { backend: createFakeBackend().backend } })).not.toBeNull()
  })

  it('looks at the page it runs in by default', () => {
    expect(connect()).toBeNull() // the test page is a plain browser page
  })
})

describe('bridge', () => {
  const setUp = () => {
    const fake = createFakeBackend()
    const bridge = connect({ __JUCE__: { backend: fake.backend } })
    if (bridge === null) throw new Error('no bridge')
    return { fake, bridge }
  }

  it('hands every state the plugin sends to the listener', () => {
    const { fake, bridge } = setUp()
    const listener = vi.fn()
    bridge.onState(listener)
    fake.deliver('assistantState', { revision: 1 })
    fake.deliver('assistantState', { revision: 2 })
    fake.deliver('somethingElse', { revision: 3 })
    expect(listener.mock.calls).toEqual([[{ revision: 1 }], [{ revision: 2 }]])
  })

  it('stops listening when asked, and takes its listener off the backend', () => {
    const { fake, bridge } = setUp()
    const listener = vi.fn()
    const stop = bridge.onState(listener)
    expect(fake.listenerCount('assistantState')).toBe(1)
    stop()
    expect(fake.listenerCount('assistantState')).toBe(0)
    fake.deliver('assistantState', { revision: 1 })
    expect(listener).not.toHaveBeenCalled()
  })

  it('asks for the state with assistantReady', () => {
    const { fake, bridge } = setUp()
    bridge.requestState()
    expect(fake.sent).toEqual([{ eventId: 'assistantReady', payload: null }])
  })

  it('sends each command as one assistantCommand event', () => {
    const { fake, bridge } = setUp()
    bridge.send(analyse(30))
    bridge.send(depth(4, 0.35))
    expect(fake.sent).toEqual([
      { eventId: 'assistantCommand', payload: { type: 'analyse', seconds: 30 } },
      { eventId: 'assistantCommand', payload: { type: 'depth', id: 4, depth: 0.35 } },
    ])
  })
})

describe('bridge: other events (module 21d)', () => {
  it('delivers any plugin event to its listeners until stopped', () => {
    const fake = createFakeBackend()
    const bridge = connect({ __JUCE__: { backend: fake.backend } })
    if (bridge === null) throw new Error('no bridge')
    const listener = vi.fn()
    const stop = bridge.on('advancedInfo', listener)
    fake.deliver('advancedInfo', { a: 1 })
    fake.deliver('showView', { view: 'advanced' })
    expect(listener).toHaveBeenCalledTimes(1)
    expect(listener).toHaveBeenCalledWith({ a: 1 })
    stop()
    expect(fake.listenerCount('advancedInfo')).toBe(0)
  })
})
