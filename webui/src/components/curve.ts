// Geometry of the target curve: band levels to points in a box. Pure, so it is tested
// without a DOM. Axes match the native impact strip (UiModel.h): 20 Hz to 20 kHz, log.

export interface Point {
  readonly x: number
  readonly y: number
}

export interface CurveBox {
  readonly width: number
  readonly height: number
  /** The level at the top edge; the bottom edge is its negative. */
  readonly rangeDb: number
}

export const AXIS_LOW_HZ = 20
export const AXIS_HIGH_HZ = 20000

// A master rolls off steeply under 40 Hz and above 16 kHz. Scaling the axis to hold those
// ends would flatten the range in between, which is the part the tonal rule works on.
const FIT_LOW_HZ = 40
const FIT_HIGH_HZ = 16000
const RANGES_DB = [12, 18, 24] as const

const clamp = (v: number, lo: number, hi: number): number => Math.min(hi, Math.max(lo, v))

/** Where a frequency sits on the axis, 0 (20 Hz) to 1 (20 kHz), clamped. */
export const axisPosition = (hz: number): number => clamp(Math.log(hz / AXIS_LOW_HZ) / Math.log(AXIS_HIGH_HZ / AXIS_LOW_HZ), 0, 1)

/** The smallest of 12 / 18 / 24 dB that holds every level between 40 Hz and 16 kHz. */
export function rangeDb(levelDb: readonly number[], bandHz: readonly number[]): number {
  const peak = levelDb.reduce((max, level, i) => {
    const hz = bandHz[i] ?? 0
    return hz >= FIT_LOW_HZ && hz <= FIT_HIGH_HZ ? Math.max(max, Math.abs(level)) : max
  }, 0)
  return RANGES_DB.find((range) => peak <= range) ?? 24
}

/** One point per band, in band order, clamped to the box. */
export function curvePoints(levelDb: readonly number[], bandHz: readonly number[], box: CurveBox): Point[] {
  return levelDb.map((level, i) => ({
    x: axisPosition(bandHz[i] ?? AXIS_LOW_HZ) * box.width,
    y: clamp(0.5 - level / (2 * box.rangeDb), 0, 1) * box.height,
  }))
}

/** The SVG path through the points, one decimal each; empty for no points. */
export function curvePath(points: readonly Point[]): string {
  const at = (p: Point) => `${Number(p.x.toFixed(1))} ${Number(p.y.toFixed(1))}`
  return points.map((p, i) => `${i === 0 ? 'M' : 'L'}${at(p)}`).join('')
}
