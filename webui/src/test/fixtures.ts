// The plugin's own output. Every file under contract/states/ was written by the real
// stateToVar in the C++ contract test (tests/assistant/test_panel_contract.cpp), which also
// fails if a file here stops matching what the plugin sends. Nothing is made up for the page.
import { parseState, type SessionState } from '../protocol/state'

export const FIXTURE_NAMES = [
  'analyse-paused',
  'analyse-running',
  'cancelled',
  'idle',
  'moves-applied',
  'moves-decided',
  'moves-history',
  'moves-no-target',
  'moves-pending',
  'no-moves',
  'reference-added',
  'reference-running',
  'references-full',
  'silent',
] as const

export type FixtureName = (typeof FIXTURE_NAMES)[number]

const files = import.meta.glob<unknown>('../../contract/states/*.json', { eager: true, import: 'default' })

/** The names of the files on disk, sorted. */
export const recordedNames: readonly string[] = Object.keys(files)
  .map((path) => path.replace(/^.*\/([^/]+)\.json$/, '$1'))
  .sort()

/** A fresh copy of the JSON exactly as the page receives it. */
// `any` on purpose: the tests reach into the raw payload and damage it field by field.
export function raw(name: FixtureName): any {
  const json = files[`../../contract/states/${name}.json`]
  if (json === undefined) throw new Error(`no recorded state "${name}"`)
  return JSON.parse(JSON.stringify(json))
}

/** The recorded state as the page's own parser reads it. */
export function state(name: FixtureName): SessionState {
  const parsed = parseState(raw(name))
  if (parsed === null) throw new Error(`the page cannot read the recorded state "${name}"`)
  return parsed
}
