// The target as a curve: relative level per band, 20 Hz to 20 kHz. Drawn from the bands
// the plugin sends, so the page has no band layout of its own to keep in step.
import { axisHz } from '../format'
import { axisPosition, curvePath, curvePoints, rangeDb } from './curve'

const WIDTH = 276
const HEIGHT = 96
const TICKS_HZ = [100, 1000, 10000] as const

interface Props {
  readonly levelDb: readonly number[]
  readonly bandHz: readonly number[]
}

/** The target curve with a 0 dB centre line and three frequency ticks. */
export function TargetCurve({ levelDb, bandHz }: Props) {
  const range = rangeDb(levelDb, bandHz)
  const path = curvePath(curvePoints(levelDb, bandHz, { width: WIDTH, height: HEIGHT, rangeDb: range }))

  return (
    <div>
      <div className="flex">
        <div className="num mr-1 flex w-7 flex-col justify-between text-right text-muted" aria-hidden="true">
          <span>+{range}</span>
          <span>0</span>
          <span>-{range}</span>
        </div>
        <svg role="img" aria-label="Target curve" viewBox={`0 0 ${WIDTH} ${HEIGHT}`} preserveAspectRatio="none" className="h-24 flex-1 border border-rule">
          {TICKS_HZ.map((hz) => (
            <line key={hz} x1={axisPosition(hz) * WIDTH} x2={axisPosition(hz) * WIDTH} y1={0} y2={HEIGHT} className="stroke-rule" strokeWidth={1} vectorEffect="non-scaling-stroke" />
          ))}
          <line x1={0} x2={WIDTH} y1={HEIGHT / 2} y2={HEIGHT / 2} className="stroke-secondary" strokeWidth={1} vectorEffect="non-scaling-stroke" />
          <path data-curve d={path} fill="none" className="stroke-text" strokeWidth={1.5} vectorEffect="non-scaling-stroke" />
        </svg>
      </div>
      <div className="num relative ml-8 h-4 text-muted" aria-hidden="true">
        {TICKS_HZ.map((hz) => (
          <span key={hz} className="absolute -translate-x-1/2" style={{ left: `${axisPosition(hz) * 100}%` }}>
            {axisHz(hz)}
          </span>
        ))}
      </div>
      <p className="mt-1 text-muted">dB around the median, 20 Hz to 20 kHz</p>
    </div>
  )
}
