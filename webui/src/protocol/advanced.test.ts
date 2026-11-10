// The advanced view's protocol (module 21d): the page's list of relays and its reading of the
// plugin's "advancedInfo" and "showView" events. contract/advanced.json is written by the
// plugin's own test (tests/ui/test_advanced_link.cpp) from the real parameter specs and real
// processors. Contracts tested here:
//   - the page binds exactly the relays the plugin creates, with the same kind, range, unit,
//     taper and choices
//   - every recorded info payload is read, with every field kept as sent
//   - a payload that is off in any field is refused whole (null)
//   - "showView" opens only a view the page has
import { describe, expect, it } from 'vitest'

import contract from '../../contract/advanced.json'
import { parseInfo, parseView, RELAYS } from './advanced'

const INFO_NAMES = ['default', 'master-96k', 'sidechain-connected', 'not-prepared'] as const

const info = (name: (typeof INFO_NAMES)[number]): any => JSON.parse(JSON.stringify(contract.info[name]))

describe('advanced protocol: relays', () => {
  it('binds the relays the plugin creates, in its order, with its ranges', () => {
    expect(RELAYS).toEqual(contract.relays)
  })

  it('knows every recorded info scenario', () => {
    expect(Object.keys(contract.info).sort()).toEqual([...INFO_NAMES].sort())
  })
})

describe('advanced protocol: parseInfo', () => {
  it.each(INFO_NAMES)('reads "%s" and keeps every field as sent', (name) => {
    expect(parseInfo(info(name))).toEqual(info(name))
  })

  it('reads what runs: topology, the points in use, the capped oversampling', () => {
    const master = parseInfo(info('master-96k'))
    expect(master?.topology).toBe('fir')
    expect(master?.sampleRate).toBe(96000)
    expect(master?.crossovers?.[0]).toBeCloseTo(300, 3)
    expect(master?.crossoversSet?.[0]).toBeCloseTo(450, 3)
    expect(master?.oversampling).toEqual({ requested: 4, active: 2 })
    expect(parseInfo(info('sidechain-connected'))?.sidechain.channels).toBe(2)
    const idle = parseInfo(info('not-prepared'))
    expect(idle?.topology).toBeNull()
    expect(idle?.crossovers).toBeNull()
    expect(idle?.crossoversSet).toBeNull()
    expect(idle?.oversampling.active).toBeNull()
  })

  it('ignores unknown extra fields and never shares objects with the payload', () => {
    const payload = info('default')
    payload.extra = 1
    const parsed = parseInfo(payload)
    expect(parsed).toEqual(info('default'))
    expect(parsed?.crossovers).not.toBe(payload.crossovers)
    expect(parsed?.crossoversSet).not.toBe(payload.crossoversSet)
    expect(parsed?.oversampling).not.toBe(payload.oversampling)
  })

  const damage: ReadonlyArray<[string, (p: any) => void]> = [
    ['not an object', () => {}],
    ['an unknown topology', (p) => (p.topology = 'lr8')],
    ['a missing sample rate', (p) => delete p.sampleRate],
    ['a negative sample rate', (p) => (p.sampleRate = -1)],
    ['two crossover points', (p) => p.crossovers.pop()],
    ['a crossover point that is text', (p) => (p.crossovers[1] = '1000')],
    ['a crossover point that is not finite', (p) => (p.crossovers[1] = null)],
    ['two set points', (p) => p.crossoversSet.pop()],
    ['a set point that is text', (p) => (p.crossoversSet[0] = '120')],
    ['no set points next to points in use', (p) => (p.crossoversSet = null)],
    ['set points next to no points in use', (p) => (p.crossovers = null)],
    ['an oversampling factor of 3', (p) => (p.oversampling.requested = 3)],
    ['an active factor of 8', (p) => (p.oversampling.active = 8)],
    ['no oversampling', (p) => delete p.oversampling],
    ['three sidechain channels', (p) => (p.sidechain.channels = 3)],
    ['no sidechain', (p) => delete p.sidechain],
  ]

  it.each(damage)('refuses %s', (what, change) => {
    const payload = what === 'not an object' ? 'advanced' : info('default')
    change(payload)
    expect(parseInfo(payload)).toBeNull()
  })

  it('refuses null and arrays', () => {
    expect(parseInfo(null)).toBeNull()
    expect(parseInfo([])).toBeNull()
  })
})

describe('advanced protocol: parseView', () => {
  it('opens the advanced view, and nothing else', () => {
    expect(parseView({ view: 'advanced' })).toBe('advanced')
    expect(parseView({ view: 'settings' })).toBeNull()
    expect(parseView({})).toBeNull()
    expect(parseView('advanced')).toBeNull()
    expect(parseView(null)).toBeNull()
  })
})
