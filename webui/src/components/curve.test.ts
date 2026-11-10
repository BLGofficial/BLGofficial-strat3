// Geometry of the target curve (pure, no DOM). Contracts:
//   - x is logarithmic: 20 Hz at the left edge, 20 kHz at the right, 632 Hz in the middle
//   - y is linear in dB: 0 dB on the centre line, +range at the top, -range at the bottom
//   - anything outside the box is clamped to its edge, never drawn outside
//   - the range is the smallest of 12 / 18 / 24 dB that holds the curve between 40 Hz and
//     16 kHz (the roll-off under 40 Hz must not flatten the part that matters)
//   - the path has one point per band, in band order
import { describe, expect, it } from 'vitest'

import { state } from '../test/fixtures'
import { curvePath, curvePoints, rangeDb } from './curve'

const box = { width: 200, height: 100 }

describe('curvePoints', () => {
  it('places frequencies on a log axis from 20 Hz to 20 kHz', () => {
    const points = curvePoints([0, 0, 0], [20, Math.sqrt(20 * 20000), 20000], { ...box, rangeDb: 12 })
    expect(points.map((p) => p.x)).toEqual([0, expect.closeTo(100, 6), 200])
  })

  it('places levels linearly around the centre line', () => {
    const points = curvePoints([0, 12, -12, 6], [100, 100, 100, 100], { ...box, rangeDb: 12 })
    expect(points.map((p) => p.y)).toEqual([50, 0, 100, 25])
  })

  it('clamps to the box', () => {
    const points = curvePoints([40, -40], [10, 40000], { ...box, rangeDb: 12 })
    expect(points).toEqual([
      { x: 0, y: 0 },
      { x: 200, y: 100 },
    ])
  })

  it('gives one point per band of a recorded target', () => {
    const { target } = state('reference-added')
    const points = curvePoints(target.levelDb, target.bandHz, { ...box, rangeDb: rangeDb(target.levelDb, target.bandHz) })
    expect(points).toHaveLength(61)
    for (const p of points) {
      expect(p.x).toBeGreaterThanOrEqual(0)
      expect(p.x).toBeLessThanOrEqual(200)
      expect(p.y).toBeGreaterThanOrEqual(0)
      expect(p.y).toBeLessThanOrEqual(100)
    }
    expect(points.map((p) => p.x)).toEqual([...points.map((p) => p.x)].sort((a, b) => a - b))
  })
})

describe('rangeDb', () => {
  it.each([
    [[0, 3, -5], 12],
    [[0, 12, 0], 12],
    [[0, 12.1, 0], 18],
    [[0, -17, 0], 18],
    [[0, 19, 0], 24],
    [[0, 60, 0], 24],
  ])('holds %j in +-%s dB', (levels, range) => {
    expect(rangeDb(levels, [100, 1000, 10000])).toBe(range)
  })

  it('ignores the roll-off outside 40 Hz to 16 kHz', () => {
    expect(rangeDb([-30, 2, -30], [20, 1000, 20000])).toBe(12)
  })

  it('counts the bands at 40 Hz and 16 kHz themselves, and nothing beyond them', () => {
    expect(rangeDb([13, 0], [40, 1000])).toBe(18)
    expect(rangeDb([0, 13], [1000, 16000])).toBe(18)
    expect(rangeDb([13, 0, 13], [39.9, 1000, 16000.1])).toBe(12)
  })

  it('is 12 dB for no curve at all', () => {
    expect(rangeDb([], [])).toBe(12)
  })
})

describe('curvePath', () => {
  it('draws the points in order with one decimal', () => {
    expect(curvePath([{ x: 0, y: 50 }, { x: 100.04, y: 25.26 }, { x: 200, y: 0 }])).toBe('M0 50L100 25.3L200 0')
  })

  it('is empty for no points', () => {
    expect(curvePath([])).toBe('')
  })
})
