// reuse: keeps the previous object wherever the new state says the same thing, so a state
// that only moved the capture progress does not re-render every move row. Contracts:
//   - equal content gives back the previous object itself
//   - a changed part is taken from the new value; unchanged siblings keep their identity
//   - the result always equals the new value
import { describe, expect, it } from 'vitest'

import { state } from '../test/fixtures'
import { reuse } from './reuse'

describe('reuse', () => {
  it('gives back the previous state when the new one says the same', () => {
    const previous = state('moves-decided')
    expect(reuse(previous, state('moves-decided'))).toBe(previous)
  })

  it('keeps what did not change when only the capture moved on', () => {
    const previous = state('analyse-running')
    const next = { ...state('analyse-running'), revision: previous.revision + 1, capture: { ...previous.capture, progress: 0.5 } }
    const merged = reuse(previous, next)
    expect(merged).toEqual(next)
    expect(merged).not.toBe(previous)
    expect(merged.target).toBe(previous.target)
    expect(merged.moves).toBe(previous.moves)
    expect(merged.capture).not.toBe(previous.capture)
  })

  it('keeps the untouched moves when one move changes', () => {
    const previous = state('moves-pending')
    const next = state('moves-decided')
    const merged = reuse(previous, next)
    expect(merged).toEqual(next)
    expect(merged.moves).not.toBe(previous.moves)
    expect(merged.moves[2]).toBe(previous.moves[2]) // still pending at full depth
    expect(merged.moves[0]).not.toBe(previous.moves[0]) // accepted since
    expect(merged.moves[0]?.changes).toBe(previous.moves[0]?.changes)
    expect(merged.target).toBe(previous.target)
  })

  it('takes the new list when its length changed', () => {
    const previous = state('moves-decided')
    const next = state('moves-history')
    const merged = reuse(previous, next)
    expect(merged).toEqual(next)
    expect(merged.moves).toHaveLength(next.moves.length)
  })

  it('compares plain values by value', () => {
    expect(reuse(1, 1)).toBe(1)
    expect(reuse('a', 'b')).toBe('b')
    expect(reuse<unknown>([1, 2], [1, 2, 3])).toEqual([1, 2, 3])
    expect(reuse<unknown>({ a: 1 }, { a: 1, b: 2 })).toEqual({ a: 1, b: 2 })
    expect(reuse<unknown>({ a: 1, b: 2 }, { a: 1 })).toEqual({ a: 1 })
    expect(reuse<unknown>([1], { 0: 1 })).toEqual({ 0: 1 })
    expect(reuse<unknown>(null, { a: 1 })).toEqual({ a: 1 })
    expect(reuse<unknown>({ a: 1 }, null)).toBeNull()
  })
})
