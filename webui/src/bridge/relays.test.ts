// The page's side of JUCE's control relays (module 21d). Written from JUCE 8.0.17:
// juce_WebControlRelays.cpp (native) and native/javascript/index.js (JUCE's page helper,
// which this page does not bundle). Contracts tested here:
//   - outside the plugin there are no relays
//   - every relay asks for its value once when the page connects, on JUCE's event name
//   - a value from the plugin is shown; the plugin's "propertiesChanged" changes nothing
//     (the page has the ranges from the contract); a value that is off is ignored
//   - setting a value sends it the way JUCE's helper does: a slider's in its own unit (kept
//     in range), a combo box's as 0..1 over its choices (read back to the nearest index), a
//     toggle's as true or false; the page shows what it set at once
//   - a slider drag is bracketed by sliderDragStarted / sliderDragEnded (the host's gesture)
//   - a relay the plugin did not create is reported, not silently dead
//   - dispose stops listening
import { describe, expect, it, vi } from 'vitest'

import { RELAYS } from '../protocol/advanced'
import { createFakeBackend, initialisationData } from '../test/fakeBackend'
import { connectRelays } from './relays'

const setUp = (omit: readonly string[] = []) => {
  const fake = createFakeBackend()
  const relays = connectRelays({ __JUCE__: { backend: fake.backend, initialisationData: initialisationData(omit) } })
  if (relays === null) throw new Error('no relays')
  const sentOn = (eventId: string) => fake.sent.filter((e) => e.eventId === eventId).map((e) => e.payload)
  return { fake, relays, sentOn }
}

describe('relays: connecting', () => {
  it('has none outside the plugin', () => {
    expect(connectRelays({})).toBeNull()
    expect(connectRelays({ __JUCE__: { backend: {} } })).toBeNull()
  })

  it('asks for every relay once, on JUCE event names, in contract order', () => {
    const { fake } = setUp()
    const prefix = { slider: '__juce__slider', toggle: '__juce__toggle', comboBox: '__juce__comboBox' } as const
    expect(fake.sent).toEqual(RELAYS.map((r) => ({ eventId: prefix[r.kind] + r.id, payload: { eventType: 'requestInitialUpdate' } })))
  })

  it('reports a relay the plugin did not create, and neither asks it nor listens to it', () => {
    expect(setUp().relays.missing).toEqual([])
    const { fake, relays } = setUp(['xover.f3', 'mb.scHpOn'])
    expect(relays.missing).toEqual(['xover.f3', 'mb.scHpOn'])
    expect(fake.sent.map((e) => e.eventId)).not.toContain('__juce__sliderxover.f3')
    expect(fake.listenerCount('__juce__sliderxover.f3')).toBe(0)
    expect(fake.listenerCount('__juce__togglemb.scHpOn')).toBe(0)
  })

  it('stops listening when disposed', () => {
    const { fake, relays } = setUp()
    expect(fake.listenerCount('__juce__sliderxover.f1')).toBe(1)
    relays.dispose()
    expect(fake.listenerCount('__juce__sliderxover.f1')).toBe(0)
    expect(fake.listenerCount('__juce__comboBoxsat.os')).toBe(0)
  })
})

describe('relays: slider', () => {
  it('shows the value the plugin sends, in its own unit', () => {
    const { fake, relays } = setUp()
    const f1 = relays.slider('xover.f1')
    const onChange = vi.fn()
    f1.subscribe(onChange)
    expect(f1.getSnapshot()).toBeNull()
    fake.deliver('__juce__sliderxover.f1', { eventType: 'valueChanged', value: 250 })
    expect(f1.getSnapshot()).toBe(250)
    expect(onChange).toHaveBeenCalledTimes(1)
    fake.deliver('__juce__sliderxover.f1', { eventType: 'valueChanged', value: 250 }) // the plugin's echo
    expect(onChange).toHaveBeenCalledTimes(1)

    fake.deliver('__juce__sliderxover.f1', { eventType: 'propertiesChanged', start: 40, end: 500, skew: 1, name: 'Crossover 1' })
    expect(f1.getSnapshot()).toBe(250)
    expect(onChange).toHaveBeenCalledTimes(1)

    fake.deliver('__juce__sliderxover.f1', { eventType: 'sliderDragStarted', value: 300 }) // not a value event
    for (const value of ['300', null, true]) fake.deliver('__juce__sliderxover.f1', { eventType: 'valueChanged', value })
    fake.deliver('__juce__sliderxover.f1', 'valueChanged')
    expect(f1.getSnapshot()).toBe(250)
    expect(onChange).toHaveBeenCalledTimes(1)
  })

  it('sends a drag as start, values, end, and shows each value at once', () => {
    const { relays, sentOn } = setUp()
    const f2 = relays.slider('xover.f2')
    f2.dragStart()
    f2.set(1200)
    expect(f2.getSnapshot()).toBe(1200)
    f2.set(1300)
    f2.dragEnd()
    expect(sentOn('__juce__sliderxover.f2')).toEqual([
      { eventType: 'requestInitialUpdate' },
      { eventType: 'sliderDragStarted' },
      { eventType: 'valueChanged', value: 1200 },
      { eventType: 'valueChanged', value: 1300 },
      { eventType: 'sliderDragEnded' },
    ])
  })

  it('keeps a value inside the parameter range, as the plugin would', () => {
    const { relays, sentOn } = setUp()
    const f1 = relays.slider('xover.f1') // 40..500 Hz
    f1.set(600)
    expect(f1.getSnapshot()).toBe(500)
    f1.set(10)
    expect(f1.getSnapshot()).toBe(40)
    expect(sentOn('__juce__sliderxover.f1').slice(1)).toEqual([
      { eventType: 'valueChanged', value: 500 },
      { eventType: 'valueChanged', value: 40 },
    ])
  })

  it('does not send a value that is not a number', () => {
    const { relays, sentOn } = setUp()
    relays.slider('xover.f2').set(Number.NaN)
    expect(sentOn('__juce__sliderxover.f2')).toHaveLength(1)
  })
})

describe('relays: combo box', () => {
  it('reads 0..1 from the plugin as a choice index', () => {
    const { fake, relays } = setUp()
    const os = relays.combo('sat.os') // 1x, 2x, 4x
    fake.deliver('__juce__comboBoxsat.os', { eventType: 'valueChanged', value: 0.5 })
    expect(os.getSnapshot()).toBe(1)
    fake.deliver('__juce__comboBoxsat.os', { eventType: 'valueChanged', value: 1 })
    expect(os.getSnapshot()).toBe(2)
    fake.deliver('__juce__comboBoxsat.os', { eventType: 'valueChanged', value: 0 })
    expect(os.getSnapshot()).toBe(0)
    for (const value of [1.5, -0.1, '1']) fake.deliver('__juce__comboBoxsat.os', { eventType: 'valueChanged', value })
    expect(os.getSnapshot()).toBe(0)
  })

  it('reads a choice through float noise to the nearest index', () => {
    // The plugin's value is a float32 through a var: 1/2 of a 3-way choice can arrive a hair low.
    const { fake, relays } = setUp()
    const os = relays.combo('sat.os')
    fake.deliver('__juce__comboBoxsat.os', { eventType: 'valueChanged', value: 0.49999997 })
    expect(os.getSnapshot()).toBe(1)
    fake.deliver('__juce__comboBoxsat.os', { eventType: 'valueChanged', value: 0.99999994 })
    expect(os.getSnapshot()).toBe(2)
  })

  it('sends a choice as 0..1 over its choices', () => {
    const { relays, sentOn } = setUp()
    relays.combo('sat.os').set(2)
    relays.combo('sat.os').set(1)
    relays.combo('ceiling.bits').set(1) // 16, 24
    expect(sentOn('__juce__comboBoxsat.os').slice(1)).toEqual([
      { eventType: 'valueChanged', value: 1 },
      { eventType: 'valueChanged', value: 0.5 },
    ])
    expect(sentOn('__juce__comboBoxceiling.bits').slice(1)).toEqual([{ eventType: 'valueChanged', value: 1 }])
    expect(relays.combo('sat.os').getSnapshot()).toBe(1)
  })

  it('does not send a choice it does not have', () => {
    const { relays, sentOn } = setUp()
    relays.combo('sat.os').set(3)
    relays.combo('sat.os').set(-1)
    relays.combo('sat.os').set(0.5)
    expect(sentOn('__juce__comboBoxsat.os')).toHaveLength(1)
  })
})

describe('relays: toggle', () => {
  it('reads and sends true or false', () => {
    const { fake, relays, sentOn } = setUp()
    const hp = relays.toggle('mb.scHpOn')
    fake.deliver('__juce__togglemb.scHpOn', { eventType: 'valueChanged', value: true })
    expect(hp.getSnapshot()).toBe(true)
    fake.deliver('__juce__togglemb.scHpOn', { eventType: 'valueChanged', value: 0 })
    expect(hp.getSnapshot()).toBe(false)
    fake.deliver('__juce__togglemb.scHpOn', { eventType: 'valueChanged', value: 1 })
    expect(hp.getSnapshot()).toBe(true)
    fake.deliver('__juce__togglemb.scHpOn', { eventType: 'valueChanged', value: 2 })
    expect(hp.getSnapshot()).toBe(true)
    hp.set(false)
    expect(hp.getSnapshot()).toBe(false)
    expect(sentOn('__juce__togglemb.scHpOn').slice(1)).toEqual([{ eventType: 'valueChanged', value: false }])
  })
})

describe('relays: lookup', () => {
  it('has each relay under its own kind only', () => {
    const { relays } = setUp()
    expect(() => relays.slider('sat.os')).toThrow()
    expect(() => relays.combo('xover.f1')).toThrow()
    expect(() => relays.toggle('nope')).toThrow()
  })
})
