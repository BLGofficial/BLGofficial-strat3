// The session store: the one place the page keeps what the plugin last said. Contracts:
//   - no bridge: "unavailable", and sending does nothing
//   - with a bridge: "waiting" until the first state, and the state is asked for once
//   - a readable state makes the link "live"; subscribers hear about it once
//   - a state that says nothing new changes nothing and notifies nobody; a new state keeps
//     the parts that did not change
//   - an unreadable state makes the link "invalid" (stale controls must not stay live);
//     the next readable one recovers
//   - refresh asks again; send forwards; dispose takes the listener off the bridge
import { describe, expect, it, vi } from 'vitest'

import { accept } from '../protocol/commands'
import { createFakeBackend } from '../test/fakeBackend'
import { raw, state } from '../test/fixtures'
import { connect } from './juce'
import { createSessionStore } from './session'

const setUp = () => {
  const fake = createFakeBackend()
  const store = createSessionStore(connect({ __JUCE__: { backend: fake.backend } }))
  const heard = vi.fn()
  const unsubscribe = store.subscribe(heard)
  return { fake, store, heard, unsubscribe }
}

describe('session store without the plugin', () => {
  it('is unavailable and ignores commands', () => {
    const store = createSessionStore(null)
    expect(store.getSnapshot()).toEqual({ status: 'unavailable' })
    expect(() => {
      store.send(accept(1))
      store.refresh()
      store.dispose()
    }).not.toThrow()
    expect(store.getSnapshot()).toEqual({ status: 'unavailable' })
  })
})

describe('session store inside the plugin', () => {
  it('waits for the first state and asks for it once', () => {
    const { fake, store } = setUp()
    expect(store.getSnapshot()).toEqual({ status: 'waiting' })
    expect(fake.readyCount()).toBe(1)
  })

  it('goes live on the first readable state', () => {
    const { fake, store, heard } = setUp()
    fake.deliver('assistantState', raw('moves-pending'))
    expect(store.getSnapshot()).toEqual({ status: 'live', state: state('moves-pending') })
    expect(heard).toHaveBeenCalledTimes(1)
  })

  it('keeps its snapshot and stays quiet when a state says nothing new', () => {
    const { fake, store, heard } = setUp()
    fake.deliver('assistantState', raw('moves-pending'))
    const before = store.getSnapshot()
    fake.deliver('assistantState', raw('moves-pending'))
    expect(store.getSnapshot()).toBe(before)
    expect(heard).toHaveBeenCalledTimes(1)
  })

  it('keeps the unchanged parts when a new state arrives', () => {
    const { fake, store, heard } = setUp()
    fake.deliver('assistantState', raw('analyse-running'))
    const before = store.getSnapshot()
    const later = raw('analyse-running')
    later.revision += 1
    later.capture.progress = 0.5
    fake.deliver('assistantState', later)
    const after = store.getSnapshot()
    expect(heard).toHaveBeenCalledTimes(2)
    if (before.status !== 'live' || after.status !== 'live') throw new Error('not live')
    expect(after.state.capture.progress).toBe(0.5)
    expect(after.state.target).toBe(before.state.target)
  })

  it('turns invalid on a state it cannot read, and recovers on the next readable one', () => {
    const { fake, store, heard } = setUp()
    fake.deliver('assistantState', raw('moves-pending'))
    const damaged = raw('moves-pending')
    damaged.moves[0].status = 'done'
    fake.deliver('assistantState', damaged)
    expect(store.getSnapshot()).toEqual({ status: 'invalid' })
    expect(heard).toHaveBeenCalledTimes(2)
    fake.deliver('assistantState', damaged)
    expect(heard).toHaveBeenCalledTimes(2) // still invalid: nothing new to say
    fake.deliver('assistantState', raw('moves-decided'))
    expect(store.getSnapshot()).toEqual({ status: 'live', state: state('moves-decided') })
    expect(heard).toHaveBeenCalledTimes(3)
  })

  it('asks for the state again on refresh', () => {
    const { fake, store } = setUp()
    store.refresh()
    expect(fake.readyCount()).toBe(2)
  })

  it('forwards commands to the plugin', () => {
    const { fake, store } = setUp()
    store.send(accept(3))
    expect(fake.commands()).toEqual([{ type: 'accept', id: 3 }])
  })

  it('stops telling a subscriber that left', () => {
    const { fake, heard, unsubscribe } = setUp()
    unsubscribe()
    fake.deliver('assistantState', raw('moves-pending'))
    expect(heard).not.toHaveBeenCalled()
  })

  it('takes its listener off the bridge when disposed', () => {
    const { fake, store, heard } = setUp()
    expect(fake.listenerCount('assistantState')).toBe(1)
    store.dispose()
    expect(fake.listenerCount('assistantState')).toBe(0)
    fake.deliver('assistantState', raw('moves-pending'))
    expect(heard).not.toHaveBeenCalled()
    expect(store.getSnapshot()).toEqual({ status: 'waiting' })
  })
})

describe('session store: a reply that never came', () => {
  // The plugin only delivers events while the panel is visible, so the answer to the first
  // request can be dropped. A page stuck on "waiting" would look broken.
  it('asks again every second until the first state arrives, then stops', () => {
    vi.useFakeTimers()
    const { fake } = setUp()
    expect(fake.readyCount()).toBe(1)
    vi.advanceTimersByTime(999)
    expect(fake.readyCount()).toBe(1)
    vi.advanceTimersByTime(1)
    expect(fake.readyCount()).toBe(2)
    vi.advanceTimersByTime(2000)
    expect(fake.readyCount()).toBe(4)
    fake.deliver('assistantState', raw('idle'))
    vi.advanceTimersByTime(5000)
    expect(fake.readyCount()).toBe(4)
  })

  it('stops asking when disposed', () => {
    vi.useFakeTimers()
    const { fake, store } = setUp()
    store.dispose()
    vi.advanceTimersByTime(5000)
    expect(fake.readyCount()).toBe(1)
  })

  it('keeps asking while what arrives is unreadable', () => {
    vi.useFakeTimers()
    const { fake } = setUp()
    fake.deliver('assistantState', { revision: 'x' })
    vi.advanceTimersByTime(1000)
    expect(fake.readyCount()).toBe(2)
  })
})
