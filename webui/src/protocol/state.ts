// The session as the plugin sends it (source/assistant/Protocol.h, stateToVar): one whole
// object per change, never a partial update, so the page re-renders from it and has nothing
// that can drift out of sync.
//
// parseState is strict on purpose. The plugin and this page ship in one binary, so a payload
// that does not fit means a build mismatch or a bug, and showing half of it with working
// buttons would be worse than showing nothing. Unknown extra fields are ignored, so the
// plugin can add one without breaking an embedded page that does not use it yet.

export type CaptureKind = 'none' | 'analyse' | 'reference'
export type CaptureResult = 'none' | 'moves' | 'noMoves' | 'referenceAdded' | 'silent' | 'cancelled'
export type MoveStatus = 'pending' | 'accepted' | 'rejected'

export interface Capture {
  readonly kind: CaptureKind
  /** 0..1 of the capture length, counted in audio that arrived (not wall-clock time). */
  readonly progress: number
  /** Capture length in seconds; 0 when nothing is running. */
  readonly seconds: number
  /** The plugin is holding the capture back (today: while a delta listen is on). */
  readonly paused: boolean
}

export interface Target {
  readonly captured: boolean
  readonly references: number
  readonly maxReferences: number
  /** Relative level per band in dB; empty until a target is captured. */
  readonly levelDb: readonly number[]
  /** Centre frequency of each band, same length as levelDb. */
  readonly bandHz: readonly number[]
}

export interface Change {
  readonly paramId: string
  readonly from: number
  readonly to: number
  /** true: follows the depth slider; false: a switch or setting, applied at any depth above 0. */
  readonly scaled: boolean
}

export interface Move {
  readonly id: number
  readonly layer: string
  readonly status: MoveStatus
  /** 0..1. Applied while accepted, remembered while pending or rejected. */
  readonly depth: number
  /** Decided before the latest analysis: history, no longer adjustable. */
  readonly locked: boolean
  readonly reason: string
  readonly metric: string
  readonly metricBefore: number
  readonly metricTarget: number
  readonly changes: readonly Change[]
}

export interface SessionState {
  readonly revision: number
  readonly capture: Capture
  readonly result: CaptureResult
  readonly target: Target
  readonly moves: readonly Move[]
}

const CAPTURE_KINDS: readonly CaptureKind[] = ['none', 'analyse', 'reference']
const RESULTS: readonly CaptureResult[] = ['none', 'moves', 'noMoves', 'referenceAdded', 'silent', 'cancelled']
const STATUSES: readonly MoveStatus[] = ['pending', 'accepted', 'rejected']

type Fields = Readonly<Record<string, unknown>>

const isObject = (v: unknown): v is Fields => typeof v === 'object' && v !== null && !Array.isArray(v)
const isNumber = (v: unknown): v is number => Number.isFinite(v) // false for anything that is not a number
const isCount = (v: unknown): v is number => isNumber(v) && Number.isInteger(v) && v >= 0
const isUnit = (v: unknown): v is number => isNumber(v) && v >= 0 && v <= 1
const isBoolean = (v: unknown): v is boolean => typeof v === 'boolean'
const isString = (v: unknown): v is string => typeof v === 'string'
const oneOf = <T extends string>(allowed: readonly T[], v: unknown): v is T => isString(v) && (allowed as readonly string[]).includes(v)

/** Every element read, or null as soon as one is refused. */
function readAll<T>(list: unknown, read: (item: unknown) => T | null): T[] | null {
  if (!Array.isArray(list)) return null
  const out: T[] = []
  for (const item of list) {
    const value = read(item)
    if (value === null) return null
    out.push(value)
  }
  return out
}

function readCapture(v: unknown): Capture | null {
  if (!isObject(v)) return null
  const { kind, progress, seconds, paused } = v
  if (!oneOf(CAPTURE_KINDS, kind) || !isUnit(progress) || !isNumber(seconds) || seconds < 0 || !isBoolean(paused)) return null
  return { kind, progress, seconds, paused }
}

function readTarget(v: unknown): Target | null {
  if (!isObject(v)) return null
  const { captured, references, maxReferences } = v
  if (!isBoolean(captured) || !isCount(references) || !isCount(maxReferences) || maxReferences < 1 || references > maxReferences) return null
  const levelDb = readAll(v.levelDb, (x) => (isNumber(x) ? x : null))
  const bandHz = readAll(v.bandHz, (x) => (isNumber(x) && x > 0 ? x : null))
  if (levelDb === null || bandHz === null || levelDb.length !== bandHz.length) return null
  // The plugin sends the bands with a captured target and none without one.
  if (captured !== (levelDb.length > 0)) return null
  return { captured, references, maxReferences, levelDb, bandHz }
}

function readChange(v: unknown): Change | null {
  if (!isObject(v)) return null
  const { paramId, from, to, scaled } = v
  if (!isString(paramId) || !isNumber(from) || !isNumber(to) || !isBoolean(scaled)) return null
  return { paramId, from, to, scaled }
}

function readMove(v: unknown): Move | null {
  if (!isObject(v)) return null
  const { id, layer, status, depth, locked, reason, metric, metricBefore, metricTarget } = v
  if (!isCount(id) || id < 1 || !isString(layer) || !oneOf(STATUSES, status) || !isUnit(depth) || !isBoolean(locked)) return null
  if (!isString(reason) || !isString(metric) || !isNumber(metricBefore) || !isNumber(metricTarget)) return null
  const changes = readAll(v.changes, readChange)
  if (changes === null) return null
  return { id, layer, status, depth, locked, reason, metric, metricBefore, metricTarget, changes }
}

/**
 * Reads one state payload from the plugin.
 * @param value what arrived on the state event, already parsed from JSON
 * @returns the session, or null if anything about the payload is off
 */
export function parseState(value: unknown): SessionState | null {
  if (!isObject(value)) return null
  const { revision, result } = value
  if (!isCount(revision) || !oneOf(RESULTS, result)) return null
  const capture = readCapture(value.capture)
  const target = readTarget(value.target)
  const moves = readAll(value.moves, readMove)
  if (capture === null || target === null || moves === null) return null
  // The page addresses moves by id: two with one id would make a button act on the wrong one.
  if (new Set(moves.map((m) => m.id)).size !== moves.length) return null
  return { revision, capture, result, target, moves }
}
