// The target curve as drawn. Contracts:
//   - the curve is the plugin's bands, placed by curve.ts in the drawing's own box
//   - the frequency ticks stand at 100 Hz, 1 kHz and 10 kHz on the same log axis as the
//     curve, over the full height, and each label sits under its tick
//   - the 0 dB line runs across the middle
//   - the scale says the range the curve was fitted to
import { render, screen, within } from '@testing-library/react'
import { describe, expect, it } from 'vitest'

import { state } from '../test/fixtures'
import { axisPosition, curvePath, curvePoints, rangeDb } from './curve'
import { TargetCurve } from './TargetCurve'

const TICKS = [
  [100, '100'],
  [1000, '1k'],
  [10000, '10k'],
] as const

const draw = () => {
  const { levelDb, bandHz } = state('moves-pending').target // the average of two references
  const view = render(<TargetCurve levelDb={levelDb} bandHz={bandHz} />)
  const svg = screen.getByRole('img', { name: 'Target curve' })
  const [left, top, width, height] = (svg.getAttribute('viewBox') ?? '').split(' ').map(Number)
  const lines = [...svg.querySelectorAll('line')].map((line) => ({
    x1: Number(line.getAttribute('x1')),
    x2: Number(line.getAttribute('x2')),
    y1: Number(line.getAttribute('y1')),
    y2: Number(line.getAttribute('y2')),
  }))
  return { levelDb, bandHz, svg, left, top, width: width ?? Number.NaN, height: height ?? Number.NaN, lines, ...view }
}

describe('TargetCurve', () => {
  it('draws in a box that starts at the origin', () => {
    const { left, top, width, height } = draw()
    expect([left, top]).toEqual([0, 0])
    expect(width).toBeGreaterThan(0)
    expect(height).toBeGreaterThan(0)
  })

  it('draws the plugin bands as placed by the curve geometry, in that box', () => {
    const { levelDb, bandHz, svg, width, height } = draw()
    const expected = curvePath(curvePoints(levelDb, bandHz, { width, height, rangeDb: rangeDb(levelDb, bandHz) }))
    expect(expected).not.toContain('NaN')
    expect(svg.querySelector('path[data-curve]')?.getAttribute('d')).toBe(expected)
  })

  it('stands a full-height tick at 100 Hz, 1 kHz and 10 kHz on the curve axis', () => {
    const { lines, width, height } = draw()
    const upright = lines.filter((line) => line.x1 === line.x2)
    expect(upright).toHaveLength(TICKS.length)
    TICKS.forEach(([hz], i) => {
      expect(upright[i]?.x1).toBeCloseTo(axisPosition(hz) * width, 6)
      expect([upright[i]?.y1, upright[i]?.y2]).toEqual([0, height])
    })
  })

  it('runs the 0 dB line across the middle', () => {
    const { lines, width, height } = draw()
    const level = lines.filter((line) => line.y1 === line.y2)
    expect(level).toEqual([{ x1: 0, x2: width, y1: height / 2, y2: height / 2 }])
  })

  it('labels each tick under its line', () => {
    const { container } = draw()
    for (const [hz, text] of TICKS) {
      const label = within(container).getByText(text, { ignore: 'svg *' })
      expect(Number.parseFloat(label.style.left)).toBeCloseTo(axisPosition(hz) * 100, 6)
      expect(label.style.left.endsWith('%')).toBe(true)
    }
  })

  it('says the range the curve was fitted to', () => {
    const { levelDb, bandHz, container } = draw()
    const range = rangeDb(levelDb, bandHz)
    expect(within(container).getByText(`+${range}`)).toBeTruthy()
    expect(within(container).getByText(`-${range}`)).toBeTruthy()
  })
})
