// The advanced view's protocol (module 21d). Native side: source/ui/web/AdvancedLink.h.
//
//   relays        one JUCE control relay per parameter, named by its id (bridge/relays.ts)
//   advancedInfo  native -> page  what the engine runs with, which can differ from what the
//                                 parameters ask for (parseInfo)
//   showView      native -> page  the header's gear was pressed (parseView)
//
// RELAYS is the page's own copy of the plugin's list: contract/advanced.json, written by the
// plugin's test from the real parameter specs, is checked against it, so neither side can
// change the list alone. Strict parsing for the same reason as protocol/state.ts.

export const INFO_EVENT = 'advancedInfo'
export const VIEW_EVENT = 'showView'

export type RelayKind = 'slider' | 'toggle' | 'comboBox'

interface RelayBase {
  readonly id: string
  readonly kind: RelayKind
  /** The parameter's name in the host. */
  readonly name: string
  readonly min: number
  readonly max: number
  readonly default: number
  readonly unit: string
}

export interface SliderSpec extends RelayBase {
  readonly kind: 'slider'
  readonly taper: 'lin' | 'log'
}

export interface ToggleSpec extends RelayBase {
  readonly kind: 'toggle'
}

export interface ComboSpec extends RelayBase {
  readonly kind: 'comboBox'
  readonly choices: readonly string[]
}

export type RelaySpec = SliderSpec | ToggleSpec | ComboSpec

const hzSlider = (id: string, name: string, min: number, max: number, def: number): SliderSpec => ({
  id,
  kind: 'slider',
  name,
  min,
  max,
  default: def,
  unit: 'Hz',
  taper: 'log',
})

const combo = (id: string, name: string, choices: readonly string[], def: number): ComboSpec => ({
  id,
  kind: 'comboBox',
  name,
  min: 0,
  max: choices.length - 1,
  default: def,
  unit: '',
  choices,
})

/** In the plugin's order (kAdvancedRelays). */
export const RELAYS: readonly RelaySpec[] = [
  hzSlider('xover.f1', 'Crossover 1', 40, 500, 120),
  hzSlider('xover.f2', 'Crossover 2', 200, 5000, 1000),
  hzSlider('xover.f3', 'Crossover 3', 1000, 16000, 5000),
  combo('sat.os', 'Saturate Oversampling', ['1x', '2x', '4x'], 1),
  combo('ceiling.dither', 'Dither', ['Off', 'TPDF', 'TPDF Shaped'], 0),
  combo('ceiling.bits', 'Dither Bits', ['16', '24'], 1),
  combo('mb.scSource', 'MB Sidechain', ['Internal', 'External'], 0),
  { id: 'mb.scHpOn', kind: 'toggle', name: 'MB Sidechain HP On', min: 0, max: 1, default: 0, unit: '' },
  hzSlider('mb.scHpFreq', 'MB Sidechain HP Freq', 20, 500, 80),
]

export type Factor = 1 | 2 | 4

type Points = readonly [number, number, number]

export interface AdvancedInfo {
  /** The band split that runs: LR4 (TRACK, MIX) or linear-phase FIR (MASTER). null: not prepared. */
  readonly topology: 'iir' | 'fir' | null
  /** 0 while not prepared. */
  readonly sampleRate: number
  /** The points in use, after the engine sorted, spaced and capped them. null: not prepared. */
  readonly crossovers: Points | null
  /** The points as set, read with `crossovers`: they differ when the engine changed them.
   *  The page's sliders run ahead of this during a drag, so this is what to compare with. */
  readonly crossoversSet: Points | null
  readonly oversampling: { readonly requested: Factor; readonly active: Factor | null }
  /** Channels of the host's sidechain input: 0 when it connected none. */
  readonly sidechain: { readonly channels: 0 | 1 | 2 }
}

type Fields = Record<string, unknown>

const isObject = (v: unknown): v is Fields => typeof v === 'object' && v !== null && !Array.isArray(v)
const isFiniteNumber = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v)
const isFactor = (v: unknown): v is Factor => v === 1 || v === 2 || v === 4

/** @returns three finite points (a fresh array), null for null, undefined for anything else */
const parsePoints = (v: unknown): Points | null | undefined => {
  if (v === null) return null
  if (!Array.isArray(v) || v.length !== 3) return undefined
  const [a, b, c] = v as unknown[]
  return isFiniteNumber(a) && isFiniteNumber(b) && isFiniteNumber(c) ? [a, b, c] : undefined
}

/**
 * @param payload an "advancedInfo" event payload
 * @returns the info, or null if any field is off
 */
export function parseInfo(payload: unknown): AdvancedInfo | null {
  if (!isObject(payload)) return null
  const { topology, sampleRate, crossovers, crossoversSet, oversampling, sidechain } = payload
  if (topology !== 'iir' && topology !== 'fir' && topology !== null) return null
  if (!isFiniteNumber(sampleRate) || sampleRate < 0) return null

  // Both or neither: they are read together, and one without the other cannot be compared.
  const inUse = parsePoints(crossovers)
  const asSet = parsePoints(crossoversSet)
  if (inUse === undefined || asSet === undefined || (inUse === null) !== (asSet === null)) return null

  if (!isObject(oversampling) || !isFactor(oversampling.requested)) return null
  const { requested, active } = oversampling
  if (active !== null && !isFactor(active)) return null

  if (!isObject(sidechain)) return null
  const { channels } = sidechain
  if (channels !== 0 && channels !== 1 && channels !== 2) return null

  return { topology, sampleRate, crossovers: inUse, crossoversSet: asSet, oversampling: { requested, active }, sidechain: { channels } }
}

/**
 * @param payload a "showView" event payload
 * @returns the view to open, or null for one this page does not have
 */
export function parseView(payload: unknown): 'advanced' | null {
  return isObject(payload) && payload.view === 'advanced' ? 'advanced' : null
}
