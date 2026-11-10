// The session store: the one place the page keeps what the plugin last said. React reads
// it through useSyncExternalStore; nothing else in the page holds session state, so a
// button can only ever act on what the plugin last confirmed.
import type { Command } from '../protocol/commands'
import { reuse } from '../protocol/reuse'
import { parseState, type SessionState } from '../protocol/state'
import type { Bridge } from './juce'

export type Link =
  | { readonly status: 'unavailable' } // not running inside the plugin
  | { readonly status: 'waiting' } // connected, no state yet
  | { readonly status: 'invalid' } // the plugin sent something this page cannot read
  | { readonly status: 'live'; readonly state: SessionState }

export interface SessionStore {
  /** @returns the function that unsubscribes */
  subscribe(onChange: () => void): () => void
  /** The same object until something changes (React compares by identity). */
  getSnapshot(): Link
  send(command: Command): void
  /** Asks the plugin for the state again. */
  refresh(): void
  /** Stops listening to the plugin. */
  dispose(): void
}

/** How often the page asks again while it has no readable state. */
export const RETRY_MS = 1000

const UNAVAILABLE: Link = { status: 'unavailable' }
const WAITING: Link = { status: 'waiting' }
const INVALID: Link = { status: 'invalid' }

/**
 * @param bridge the plugin link, or null outside the plugin
 * @returns a store that follows the plugin until disposed
 */
export function createSessionStore(bridge: Bridge | null): SessionStore {
  if (bridge === null) {
    return { subscribe: () => () => {}, getSnapshot: () => UNAVAILABLE, send: () => {}, refresh: () => {}, dispose: () => {} }
  }

  let link: Link = WAITING
  const subscribers = new Set<() => void>()
  let retry: ReturnType<typeof setInterval> | undefined

  const stopRetry = () => {
    clearInterval(retry)
    retry = undefined
  }

  const set = (next: Link) => {
    if (next === link) return
    link = next
    for (const onChange of [...subscribers]) onChange()
  }

  const stopListening = bridge.onState((payload) => {
    const parsed = parseState(payload)
    if (parsed === null) {
      // Stale controls must not stay live next to a plugin the page no longer understands.
      set(INVALID)
      return
    }
    stopRetry()
    if (link.status !== 'live') {
      set({ status: 'live', state: parsed })
      return
    }
    const state = reuse(link.state, parsed)
    if (state !== link.state) set({ status: 'live', state })
  })

  // The plugin delivers events only while the panel is visible, so the answer to a request
  // can be dropped. Keep asking until one arrives.
  bridge.requestState()
  retry = setInterval(() => bridge.requestState(), RETRY_MS)

  return {
    subscribe(onChange) {
      subscribers.add(onChange)
      return () => void subscribers.delete(onChange)
    },
    getSnapshot: () => link,
    send: (command) => bridge.send(command),
    refresh: () => bridge.requestState(),
    dispose() {
      stopRetry()
      stopListening()
    },
  }
}
