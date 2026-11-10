// The advanced view's store (module 21d): what the plugin last said about the engine, and
// the header gear's request to open the view. Contracts tested here:
//   - outside the plugin nothing arrives and nothing breaks
//   - an info payload is read (protocol/advanced.ts); an unreadable one is shown as such,
//     not as the last good one
//   - the same info again keeps the same object (React compares by identity)
//   - "showView" reaches every listener; one that is off reaches none
//   - dispose stops listening
import { describe, expect, it, vi } from 'vitest'

import contract from '../../contract/advanced.json'
import { createFakeBackend } from '../test/fakeBackend'
import { createAdvancedStore } from './advanced'
import { connect } from './juce'

const setUp = () => {
  const fake = createFakeBackend()
  const store = createAdvancedStore(connect({ __JUCE__: { backend: fake.backend } }))
  return { fake, store }
}

describe('advanced store', () => {
  it('waits outside the plugin, and a gear listener is harmless', () => {
    const store = createAdvancedStore(null)
    expect(store.getSnapshot()).toEqual({ status: 'waiting' })
    const stop = store.onShowView(() => {})
    stop()
    store.dispose()
  })

  it('reads the info the plugin sends, and keeps the object while it does not change', () => {
    const { fake, store } = setUp()
    const onChange = vi.fn()
    store.subscribe(onChange)
    expect(store.getSnapshot()).toEqual({ status: 'waiting' })

    fake.deliver('advancedInfo', contract.info['master-96k'])
    const first = store.getSnapshot()
    expect(first).toEqual({ status: 'live', info: contract.info['master-96k'] })
    fake.deliver('advancedInfo', contract.info['master-96k'])
    expect(store.getSnapshot()).toBe(first)
    expect(onChange).toHaveBeenCalledTimes(1)

    fake.deliver('advancedInfo', contract.info.default)
    expect(store.getSnapshot()).toEqual({ status: 'live', info: contract.info.default })
    expect(onChange).toHaveBeenCalledTimes(2)
  })

  it('says so when the info cannot be read', () => {
    const { fake, store } = setUp()
    fake.deliver('advancedInfo', contract.info.default)
    fake.deliver('advancedInfo', { ...contract.info.default, topology: 'lr8' })
    expect(store.getSnapshot()).toEqual({ status: 'invalid' })
  })

  it('passes the gear on to every listener', () => {
    const { fake, store } = setUp()
    const a = vi.fn()
    const b = vi.fn()
    store.onShowView(a)
    const stopB = store.onShowView(b)
    fake.deliver('showView', { view: 'advanced' })
    stopB()
    fake.deliver('showView', { view: 'advanced' })
    fake.deliver('showView', { view: 'mixer' })
    expect(a).toHaveBeenCalledTimes(2)
    expect(b).toHaveBeenCalledTimes(1)
  })

  it('stops listening when disposed', () => {
    const { fake, store } = setUp()
    store.dispose()
    expect(fake.listenerCount('advancedInfo')).toBe(0)
    expect(fake.listenerCount('showView')).toBe(0)
  })
})
