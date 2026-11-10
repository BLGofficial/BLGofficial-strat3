// parseState: the page's reading of what the plugin sends (source/assistant/Protocol.h,
// stateToVar). Contracts tested here:
//   - every recorded plugin state is read, with every field kept exactly as sent
//   - the list of recorded states is the one the page's tests know about
//   - a payload that is off in any field is refused whole (null), never half-read
//   - unknown extra fields are ignored; the result never shares objects with the payload
import { describe, expect, it } from 'vitest'

import { FIXTURE_NAMES, raw, recordedNames, state } from '../test/fixtures'
import { parseState } from './state'

describe('parseState: recorded plugin states', () => {
  it('knows every state the plugin test records', () => {
    expect(recordedNames).toEqual([...FIXTURE_NAMES])
  })

  it.each(FIXTURE_NAMES)('reads "%s" and keeps every field as sent', (name) => {
    expect(parseState(raw(name))).toEqual(raw(name))
  })

  it('reads the fresh session', () => {
    const s = state('idle')
    expect(s.revision).toBe(0)
    expect(s.capture).toEqual({ kind: 'none', progress: 0, seconds: 0, paused: false })
    expect(s.result).toBe('none')
    expect(s.target).toEqual({ captured: false, references: 0, maxReferences: 16, levelDb: [], bandHz: [] })
    expect(s.moves).toEqual([])
  })

  it('reads a captured target as one level per band', () => {
    const { target } = state('reference-added')
    expect(target.captured).toBe(true)
    expect(target.references).toBe(1)
    expect(target.levelDb).toHaveLength(61)
    expect(target.bandHz).toHaveLength(61)
    expect(target.bandHz[34]).toBeCloseTo(1000, 6)
  })

  it('reads moves with their status, depth, lock and parameter changes', () => {
    const { moves } = state('moves-decided')
    expect(moves.map((m) => [m.id, m.layer, m.status, m.locked])).toEqual([
      [1, 'eq', 'accepted', false],
      [2, 'eq', 'accepted', false],
      [3, 'eq', 'pending', false],
      [4, 'low', 'rejected', false],
      [5, 'limit', 'accepted', false],
    ])
    expect(moves[1]?.depth).toBeCloseTo(0.6, 6)
    expect(moves[0]?.changes.find((c) => c.paramId === 'eq.b1.gain')).toMatchObject({ from: 0, scaled: true })
    expect(state('moves-history').moves.filter((m) => m.locked).map((m) => m.id)).toEqual([1, 2, 4, 5])
  })
})

describe('parseState: refusals', () => {
  it.each([null, undefined, 'state', 42, true, []])('refuses %j, which is not a state object', (payload) => {
    expect(parseState(payload)).toBeNull()
  })

  // Each entry damages one field of a state the page otherwise reads.
  const damage: [string, (s: ReturnType<typeof raw>) => void][] = [
    ['no revision', (s) => delete s.revision],
    ['a negative revision', (s) => (s.revision = -1)],
    ['a fractional revision', (s) => (s.revision = 1.5)],
    ['a revision as text', (s) => (s.revision = '3606')],
    ['no capture', (s) => delete s.capture],
    ['an unknown capture kind', (s) => (s.capture.kind = 'record')],
    ['progress above 1', (s) => (s.capture.progress = 1.2)],
    ['progress below 0', (s) => (s.capture.progress = -0.1)],
    ['progress as text', (s) => (s.capture.progress = '0.5')],
    ['progress null (a NaN on the native side)', (s) => (s.capture.progress = null)],
    ['a negative capture length', (s) => (s.capture.seconds = -1)],
    ['paused as text', (s) => (s.capture.paused = 'no')],
    ['an unknown result', (s) => (s.result = 'done')],
    ['no target', (s) => delete s.target],
    ['captured as a number', (s) => (s.target.captured = 1)],
    ['a negative reference count', (s) => (s.target.references = -1)],
    ['a fractional reference count', (s) => (s.target.references = 2.5)],
    ['a zero reference limit', (s) => (s.target.maxReferences = 0)],
    ['more references than the limit', (s) => (s.target.references = 17)],
    ['levels that are not a list', (s) => (s.target.levelDb = 'flat')],
    ['fewer levels than bands', (s) => s.target.levelDb.pop()],
    ['a level that is not a number', (s) => (s.target.levelDb[3] = null)],
    ['a band at 0 Hz', (s) => (s.target.bandHz[0] = 0)],
    ['a band frequency as text', (s) => (s.target.bandHz[5] = '45')],
    ['a captured target with no bands', (s) => ((s.target.levelDb = []), (s.target.bandHz = []))],
    ['bands without a captured target', (s) => (s.target.captured = false)],
    ['moves that are not a list', (s) => (s.moves = {})],
    ['a move that is not an object', (s) => (s.moves[0] = 'eq')],
    ['a move that is null', (s) => (s.moves[0] = null)],
    ['a move id of 0', (s) => (s.moves[0].id = 0)],
    ['a fractional move id', (s) => (s.moves[0].id = 1.5)],
    ['two moves with one id', (s) => (s.moves[1].id = s.moves[0].id)],
    ['a layer that is not text', (s) => (s.moves[0].layer = 3)],
    ['an unknown move status', (s) => (s.moves[0].status = 'done')],
    ['a depth above 1', (s) => (s.moves[0].depth = 1.01)],
    ['a depth below 0', (s) => (s.moves[0].depth = -0.01)],
    ['locked as text', (s) => (s.moves[0].locked = 'yes')],
    ['a reason that is not text', (s) => (s.moves[0].reason = 5)],
    ['a metric that is not text', (s) => (s.moves[0].metric = null)],
    ['a measured value that is not a number', (s) => (s.moves[0].metricBefore = null)],
    ['a target value as text', (s) => (s.moves[0].metricTarget = '0')],
    ['changes that are not a list', (s) => (s.moves[0].changes = null)],
    ['a change that is not an object', (s) => (s.moves[0].changes[0] = 7)],
    ['a change that is null', (s) => (s.moves[0].changes[0] = null)],
    ['a parameter id that is not text', (s) => (s.moves[0].changes[0].paramId = 7)],
    ['a from value as text', (s) => (s.moves[0].changes[0].from = '0')],
    ['a to value that is not a number', (s) => (s.moves[0].changes[0].to = null)],
    ['scaled as a number', (s) => (s.moves[0].changes[0].scaled = 0)],
  ]

  it.each(damage)('refuses a state with %s', (_what, apply) => {
    const payload = raw('moves-decided')
    apply(payload)
    expect(parseState(payload)).toBeNull()
  })

  it('reads the undamaged state those refusals start from', () => {
    expect(parseState(raw('moves-decided'))).not.toBeNull()
  })

  it('refuses a zero reference limit even when no reference is in', () => {
    const payload = raw('idle')
    payload.target.maxReferences = 0
    expect(parseState(payload)).toBeNull()
  })

  it('reads a limit of a single reference', () => {
    const payload = raw('reference-added')
    payload.target.maxReferences = 1
    expect(parseState(payload)?.target).toMatchObject({ references: 1, maxReferences: 1 })
  })

  it('refuses a number JSON cannot carry, should one ever arrive', () => {
    const payload = raw('moves-decided')
    payload.moves[0].metricBefore = Number.NaN
    expect(parseState(payload)).toBeNull()
    payload.moves[0].metricBefore = Number.POSITIVE_INFINITY
    expect(parseState(payload)).toBeNull()
  })
})

describe('parseState: what it hands back', () => {
  it('ignores fields it does not know', () => {
    const payload = raw('moves-decided')
    payload.build = 'next'
    payload.capture.eta = 12
    payload.target.name = 'club'
    payload.moves[0].confidence = 0.9
    payload.moves[0].changes[0].unit = 'dB'
    expect(parseState(payload)).toEqual(raw('moves-decided'))
  })

  it('shares nothing with the payload', () => {
    const payload = raw('moves-decided')
    const parsed = parseState(payload)
    payload.moves[0].changes[0].to = 99
    payload.moves.length = 0
    payload.target.levelDb[0] = 99
    payload.capture.kind = 'analyse'
    expect(parsed).toEqual(raw('moves-decided'))
  })
})
