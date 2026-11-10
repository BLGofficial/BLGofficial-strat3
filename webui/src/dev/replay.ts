// DEV SERVER ONLY. A stand-in for the plugin so the panels can be looked at in a browser
// before the native host exists (module 21c): `npm run dev`, then open /?state=moves-pending.
// It replays one recorded plugin state from contract/states/ and logs commands instead of
// acting on them: there is no session behind it, so nothing on screen changes on a click.
// main.tsx loads this file only under import.meta.env.DEV; it is not in the built bundle.
import type { Bridge } from '../bridge/juce'

const recorded = import.meta.glob<unknown>('../../contract/states/*.json', { eager: true, import: 'default' })

/**
 * @param search the page's query string, e.g. "?state=moves-pending"
 * @returns a bridge replaying that recorded state, or null if the name is missing or unknown
 */
export function replayBridge(search: string): Bridge | null {
  const name = new URLSearchParams(search).get('state')
  const state = name === null ? undefined : recorded[`../../contract/states/${name}.json`]
  if (state === undefined) return null

  const listeners = new Set<(payload: unknown) => void>()
  return {
    onState(listener) {
      listeners.add(listener)
      return () => void listeners.delete(listener)
    },
    // No advanced info in a recording: main.tsx has no relays here, so that view is not shown.
    on: () => () => {},
    requestState: () => listeners.forEach((listener) => listener(state)),
    send: (command) => console.info('[replay] command not sent, no plugin:', command),
  }
}
