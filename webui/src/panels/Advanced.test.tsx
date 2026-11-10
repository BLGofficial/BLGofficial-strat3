// The advanced view (module 21d): Phase 4's advanced slide-over, as the column's third view.
// Its controls are real plugin parameters bound through JUCE's relays; the info lines say
// what the engine runs with. Contracts tested here:
//   - four sections: crossover, oversampling, dither, sidechain
//   - the crossover topology follows what runs (LR4 in TRACK and MIX, FIR in MASTER), and
//     the points in use are shown when the engine had to sort, space or cap them, decided
//     from the info alone
//   - a slider moves on a log scale between the parameter's limits; a drag (pointer or keys)
//     is one gesture: start, values, end
//   - a choice is sent as its index; oversampling says when the rate cap runs less than
//     asked; dither says it applies at 16 bit only; the sidechain says when the host has
//     connected no input to listen to
//   - a control is disabled until the plugin has sent its value; the high-pass frequency
//     while the high-pass is off
//   - a plugin without these relays, or an info the page cannot read, gets a plain notice
//     and no controls
import { act, fireEvent, render, screen, within } from '@testing-library/react'
import { describe, expect, it } from 'vitest'

import contract from '../../contract/advanced.json'
import { createAdvancedStore } from '../bridge/advanced'
import { connect } from '../bridge/juce'
import { connectRelays } from '../bridge/relays'
import { createFakeBackend, initialisationData } from '../test/fakeBackend'
import { Advanced } from './Advanced'

type InfoName = keyof typeof contract.info

interface Values {
  readonly f1?: number
  readonly f2?: number
  readonly f3?: number
  readonly os?: number // 0..1
  readonly dither?: number
  readonly bits?: number
  readonly source?: number
  readonly hpOn?: boolean
  readonly hpHz?: number
}

// The parameter defaults (contract/advanced.json), as the plugin sends them on request.
const DEFAULTS: Required<Values> = { f1: 120, f2: 1000, f3: 5000, os: 0.5, dither: 0, bits: 1, source: 0, hpOn: false, hpHz: 80 }

const setUp = (info: InfoName | null = 'default', values: Values | null = {}, omit: readonly string[] = []) => {
  const fake = createFakeBackend()
  const relays = connectRelays({ __JUCE__: { backend: fake.backend, initialisationData: initialisationData(omit) } })
  const store = createAdvancedStore(connect({ __JUCE__: { backend: fake.backend } }))
  if (relays === null) throw new Error('no relays')
  const send = (relay: string, value: unknown) => act(() => fake.deliver(relay, { eventType: 'valueChanged', value }))
  const setValues = (v: Values) => {
    const all = { ...DEFAULTS, ...v }
    send('__juce__sliderxover.f1', all.f1)
    send('__juce__sliderxover.f2', all.f2)
    send('__juce__sliderxover.f3', all.f3)
    send('__juce__comboBoxsat.os', all.os)
    send('__juce__comboBoxceiling.dither', all.dither)
    send('__juce__comboBoxceiling.bits', all.bits)
    send('__juce__comboBoxmb.scSource', all.source)
    send('__juce__togglemb.scHpOn', all.hpOn)
    send('__juce__slidermb.scHpFreq', all.hpHz)
  }
  const setInfo = (name: InfoName) => act(() => fake.deliver('advancedInfo', contract.info[name]))
  const view = render(<Advanced info={store} relays={relays} />)
  if (info !== null) setInfo(info)
  if (values !== null) setValues(values)
  fake.sent.length = 0 // the initial requests: tested in bridge/relays.test.ts
  return { fake, view, setValues, setInfo, sentOn: (eventId: string) => fake.sent.filter((e) => e.eventId === eventId).map((e) => e.payload) }
}

const section = (name: string) => screen.getByRole('region', { name })
const slider = (name: string) => screen.getByRole('slider', { name }) as HTMLInputElement
const radio = (group: string, name: string) => within(screen.getByRole('radiogroup', { name: group })).getByRole('radio', { name })

describe('Advanced: layout', () => {
  it('has the four sections', () => {
    setUp()
    for (const name of ['Crossover', 'Oversampling', 'Dither', 'Sidechain']) expect(section(name)).toBeTruthy()
  })
})

describe('Advanced: crossover', () => {
  it('names the topology that runs', () => {
    const { setInfo } = setUp('default')
    expect(within(section('Crossover')).getByText('Linkwitz-Riley (IIR), as in TRACK and MIX')).toBeTruthy()
    setInfo('master-96k')
    expect(within(section('Crossover')).getByText('Linear phase (FIR), as in MASTER')).toBeTruthy()
    setInfo('not-prepared')
    expect(within(section('Crossover')).getByText('Not running: the host has not started the plugin.')).toBeTruthy()
  })

  it('shows each point with its value', () => {
    setUp('default', { f1: 120, f2: 1000, f3: 5000 })
    expect(slider('Crossover 1').getAttribute('aria-valuetext')).toBe('120 Hz')
    expect(slider('Crossover 2').getAttribute('aria-valuetext')).toBe('1.00 kHz')
    expect(slider('Crossover 3').getAttribute('aria-valuetext')).toBe('5.00 kHz')
    expect(within(section('Crossover')).getByText('1.00 kHz')).toBeTruthy()
    expect(within(section('Crossover')).queryByText(/In use/)).toBeNull() // as set
  })

  it('shows the points in use when the engine sorted or spaced them', () => {
    setUp('master-96k', { f1: 450, f2: 300, f3: 5000 })
    expect(within(section('Crossover')).getByText('In use: 300 Hz, 450 Hz, 5.00 kHz')).toBeTruthy()
    expect(within(section('Crossover')).getByText('The engine sorts the points, keeps them at least 1.25x apart and under 0.45x the sample rate.')).toBeTruthy()
  })

  // The info is a tick behind the hand: comparing it with the sliders would flicker the line
  // on every drag. It carries the points as set, from the same moment, so it decides alone.
  it('keeps the line while the hand is ahead of the info', () => {
    const { setValues } = setUp('master-96k', { f1: 450, f2: 300 })
    setValues({ f1: 200, f2: 1000 })
    expect(within(section('Crossover')).getByText('In use: 300 Hz, 450 Hz, 5.00 kHz')).toBeTruthy()
  })

  it('shows no line while the info says the points run as set, whatever the sliders say', () => {
    setUp('default', { f1: 450, f2: 300 })
    expect(within(section('Crossover')).queryByText(/In use/)).toBeNull()
  })

  it('places a point on a log scale between its limits', () => {
    setUp('default', { f1: 40 * Math.sqrt(500 / 40) }) // the geometric middle of 40..500 Hz
    expect(Number(slider('Crossover 1').value)).toBe(500)
  })

  it('sends a pointer drag as one gesture, on a log scale', () => {
    const { sentOn } = setUp()
    const f1 = slider('Crossover 1')
    fireEvent.pointerDown(f1)
    fireEvent.change(f1, { target: { value: '500' } })
    fireEvent.change(f1, { target: { value: '1000' } })
    fireEvent.pointerUp(f1)
    const sent = sentOn('__juce__sliderxover.f1') as any[]
    expect(sent.map((e) => e.eventType)).toEqual(['sliderDragStarted', 'valueChanged', 'valueChanged', 'sliderDragEnded'])
    expect(sent[1].value).toBeCloseTo(141.42, 2)
    expect(sent[2].value).toBeCloseTo(500, 6)
  })

  it('sends a key change as one gesture too, ended when the key comes up', () => {
    const { sentOn } = setUp()
    const f3 = slider('Crossover 3')
    fireEvent.change(f3, { target: { value: '0' } })
    fireEvent.change(f3, { target: { value: '1' } })
    fireEvent.keyUp(f3, { key: 'ArrowRight' })
    fireEvent.blur(f3)
    const sent = sentOn('__juce__sliderxover.f3') as any[]
    expect(sent.map((e) => e.eventType)).toEqual(['sliderDragStarted', 'valueChanged', 'valueChanged', 'sliderDragEnded'])
    expect(sent[1].value).toBeCloseTo(1000, 6)
  })

  it('ends a key gesture when the key comes up, before any blur', () => {
    const { sentOn } = setUp()
    const f1 = slider('Crossover 1')
    fireEvent.change(f1, { target: { value: '10' } })
    fireEvent.keyUp(f1, { key: 'ArrowRight' })
    expect((sentOn('__juce__sliderxover.f1') as any[]).map((e) => e.eventType)).toEqual(['sliderDragStarted', 'valueChanged', 'sliderDragEnded'])
  })

  it('ends a gesture when the slider loses focus without a key coming up', () => {
    const { sentOn } = setUp()
    const f1 = slider('Crossover 1')
    fireEvent.change(f1, { target: { value: '10' } })
    fireEvent.blur(f1)
    expect((sentOn('__juce__sliderxover.f1') as any[]).map((e) => e.eventType)).toEqual(['sliderDragStarted', 'valueChanged', 'sliderDragEnded'])
  })

  it('ends a gesture when the view goes away mid-drag (a tab switch)', () => {
    const { sentOn, view } = setUp()
    const f2 = slider('Crossover 2')
    fireEvent.pointerDown(f2)
    fireEvent.change(f2, { target: { value: '10' } })
    view.unmount()
    expect((sentOn('__juce__sliderxover.f2') as any[]).map((e) => e.eventType)).toEqual(['sliderDragStarted', 'valueChanged', 'sliderDragEnded'])
  })

  it('pins a value outside the range to the end of the travel', () => {
    setUp('default', { f1: 600 }) // over 500 Hz: not from this build, but drawn sanely
    expect(slider('Crossover 1').style.getPropertyValue('--fill')).toBe('100%')
  })

  it('is disabled until the plugin sent the value', () => {
    setUp('default', null)
    expect(slider('Crossover 1').disabled).toBe(true)
  })
})

describe('Advanced: oversampling', () => {
  it('shows and sends the factor', () => {
    const { sentOn } = setUp('default', { os: 0.5 })
    expect(radio('Oversampling', '2x').getAttribute('aria-checked')).toBe('true')
    expect(radio('Oversampling', '4x').getAttribute('aria-checked')).toBe('false')
    fireEvent.click(radio('Oversampling', '4x'))
    expect(sentOn('__juce__comboBoxsat.os')).toEqual([{ eventType: 'valueChanged', value: 1 }])
    expect(radio('Oversampling', '4x').getAttribute('aria-checked')).toBe('true')
  })

  it('moves and chooses with the arrow keys, wrapping at the ends, as a radio group does', () => {
    const { sentOn } = setUp('default', { os: 0.5 }) // 2x
    const group = screen.getByRole('radiogroup', { name: 'Oversampling' })
    fireEvent.keyDown(group, { key: 'ArrowRight' })
    expect(radio('Oversampling', '4x').getAttribute('aria-checked')).toBe('true')
    fireEvent.keyDown(group, { key: 'ArrowRight' })
    expect(radio('Oversampling', '1x').getAttribute('aria-checked')).toBe('true')
    fireEvent.keyDown(group, { key: 'ArrowLeft' })
    fireEvent.keyDown(group, { key: 'Enter' }) // not an arrow: nothing
    expect(sentOn('__juce__comboBoxsat.os')).toEqual([
      { eventType: 'valueChanged', value: 1 },
      { eventType: 'valueChanged', value: 0 },
      { eventType: 'valueChanged', value: 1 },
    ])
    expect(document.activeElement).toBe(radio('Oversampling', '4x'))
  })

  it('says when the rate cap runs less than asked', () => {
    const { setInfo } = setUp('master-96k', { os: 1 })
    expect(within(section('Oversampling')).getByText('Runs at 2x: 4x is over the limit at 96 kHz.')).toBeTruthy()
    setInfo('default')
    expect(within(section('Oversampling')).queryByText(/Runs at/)).toBeNull()
  })
})

describe('Advanced: dither', () => {
  it('shows and sends the type and the bits', () => {
    const { sentOn } = setUp('default', { dither: 0, bits: 1 })
    expect(radio('Dither', 'Off').getAttribute('aria-checked')).toBe('true')
    expect(radio('Dither bits', '24').getAttribute('aria-checked')).toBe('true')
    fireEvent.click(radio('Dither', 'TPDF Shaped'))
    fireEvent.click(radio('Dither bits', '16'))
    expect(sentOn('__juce__comboBoxceiling.dither')).toEqual([{ eventType: 'valueChanged', value: 1 }])
    expect(sentOn('__juce__comboBoxceiling.bits')).toEqual([{ eventType: 'valueChanged', value: 0 }])
  })

  it('says dither applies at 16 bit only, when it would do nothing', () => {
    const { setValues } = setUp('default', { dither: 0.5, bits: 1 })
    const note = 'Dither applies at 16 bit only: at 24 bit it is skipped.'
    expect(within(section('Dither')).getByText(note)).toBeTruthy()
    setValues({ dither: 0.5, bits: 0 })
    expect(within(section('Dither')).queryByText(note)).toBeNull()
    setValues({ dither: 0, bits: 1 })
    expect(within(section('Dither')).queryByText(note)).toBeNull()
  })
})

describe('Advanced: sidechain', () => {
  it('says what the host connected', () => {
    const { setInfo } = setUp('default')
    expect(within(section('Sidechain')).getByText('Host input: not connected')).toBeTruthy()
    setInfo('sidechain-connected')
    expect(within(section('Sidechain')).getByText('Host input: stereo')).toBeTruthy()
  })

  it('warns when MULTIBAND is set to listen to an input the host has not connected', () => {
    const { setInfo } = setUp('default', { source: 1 })
    const warning = 'No sidechain input is connected, so MULTIBAND listens to the mix.'
    expect(within(section('Sidechain')).getByText(warning)).toBeTruthy()
    setInfo('sidechain-connected')
    expect(within(section('Sidechain')).queryByText(warning)).toBeNull()
  })

  it('sends the source and the high-pass; the frequency only works with the high-pass on', () => {
    const { sentOn, setValues } = setUp('default', { source: 0, hpOn: false })
    expect(slider('High-pass frequency').disabled).toBe(true)
    fireEvent.click(radio('Sidechain source', 'External'))
    const hp = screen.getByRole('switch', { name: 'High-pass' })
    expect(hp.getAttribute('aria-checked')).toBe('false')
    fireEvent.click(hp)
    expect(sentOn('__juce__comboBoxmb.scSource')).toEqual([{ eventType: 'valueChanged', value: 1 }])
    expect(sentOn('__juce__togglemb.scHpOn')).toEqual([{ eventType: 'valueChanged', value: true }])
    expect(slider('High-pass frequency').disabled).toBe(false)
    setValues({ hpOn: true, hpHz: 80 })
    expect(slider('High-pass frequency').getAttribute('aria-valuetext')).toBe('80 Hz')
    fireEvent.click(hp)
    expect(sentOn('__juce__togglemb.scHpOn')).toEqual([
      { eventType: 'valueChanged', value: true },
      { eventType: 'valueChanged', value: false },
    ])
  })
})

describe('Advanced: mismatches', () => {
  it('shows no controls when the plugin lacks a relay', () => {
    setUp('default', null, ['xover.f2'])
    expect(screen.getByText('The panel and the plugin do not match. Reinstall STRAT3.')).toBeTruthy()
    expect(screen.queryAllByRole('slider')).toEqual([])
    expect(screen.queryAllByRole('radio')).toEqual([])
  })

  it('shows no controls when the info cannot be read', () => {
    const { fake } = setUp('default')
    act(() => fake.deliver('advancedInfo', { topology: 'lr8' }))
    expect(screen.getByText('The panel and the plugin do not match. Reinstall STRAT3.')).toBeTruthy()
    expect(screen.queryAllByRole('slider')).toEqual([])
  })
})
