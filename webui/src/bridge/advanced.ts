// The advanced view's store (module 21d): what the plugin last said the engine runs with,
// and the header gear's request to open the view. The parameter values themselves are not
// here: they live in the relays (bridge/relays.ts), one per parameter.
import { INFO_EVENT, parseInfo, parseView, VIEW_EVENT, type AdvancedInfo } from '../protocol/advanced'
import type { Bridge } from './juce'

export type InfoLink =
  | { readonly status: 'waiting' } // nothing yet, or not inside the plugin
  | { readonly status: 'invalid' } // the plugin sent something this page cannot read
  | { readonly status: 'live'; readonly info: AdvancedInfo }

export interface AdvancedStore {
  /** @returns the function that unsubscribes */
  subscribe(onChange: () => void): () => void
  /** The same object until the info changes (React compares by identity). */
  getSnapshot(): InfoLink
  /**
   * Calls `listener` each time the plugin asks to open the advanced view (the header gear).
   * @returns the function that stops it
   */
  onShowView(listener: () => void): () => void
  /** Stops listening to the plugin. */
  dispose(): void
}

const WAITING: InfoLink = { status: 'waiting' }
const INVALID: InfoLink = { status: 'invalid' }

/**
 * @param bridge the plugin link, or null outside the plugin
 * @returns a store that follows the plugin until disposed
 */
export function createAdvancedStore(bridge: Bridge | null): AdvancedStore {
  if (bridge === null) {
    return { subscribe: () => () => {}, getSnapshot: () => WAITING, onShowView: () => () => {}, dispose: () => {} }
  }

  let link: InfoLink = WAITING
  // The plugin sends the info again on every state change and on every show; most of those
  // carry the same info, which must not re-render the view.
  let linkText = ''
  const subscribers = new Set<() => void>()
  const viewListeners = new Set<() => void>()

  const set = (next: InfoLink) => {
    const text = JSON.stringify(next)
    if (text === linkText) return
    link = next
    linkText = text
    for (const onChange of [...subscribers]) onChange()
  }

  const stopInfo = bridge.on(INFO_EVENT, (payload) => {
    const info = parseInfo(payload)
    set(info === null ? INVALID : { status: 'live', info })
  })
  const stopView = bridge.on(VIEW_EVENT, (payload) => {
    if (parseView(payload) === null) return
    for (const listener of [...viewListeners]) listener()
  })

  return {
    subscribe(onChange) {
      subscribers.add(onChange)
      return () => void subscribers.delete(onChange)
    },
    getSnapshot: () => link,
    onShowView(listener) {
      viewListeners.add(listener)
      return () => void viewListeners.delete(listener)
    },
    dispose() {
      stopInfo()
      stopView()
    },
  }
}
