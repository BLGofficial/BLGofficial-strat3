// The page's side of the JUCE WebView bridge.
//
// JUCE injects window.__JUCE__.backend into every page of a WebBrowserComponent with native
// integration on (JUCE 8.0.17, juce_WebBrowserComponent.cpp, class Backend). This file uses
// only that documented object and three events of our own:
//   assistantState    native -> page   the whole session (protocol/state.ts)
//   assistantReady    page -> native   "send me the state now"
//   assistantCommand  page -> native   one command object (protocol/commands.ts)
// and, through on(), any other plugin event (the advanced view's: protocol/advanced.ts).
//
// Phase 5 named withNativeFunction for page -> native. That is the same event channel plus
// a promise layer that lives in JUCE's own JS helper, which sits inside the JUCE checkout
// and is not available when Vite builds this page. Plain events need nothing from it, and
// the panels do not need a return value: every accepted command comes back as a new state.
// Own choice [INFERRED]; the native end (module 21c) registers the matching listeners.
import type { Command } from '../protocol/commands'

export const STATE_EVENT = 'assistantState'
export const READY_EVENT = 'assistantReady'
export const COMMAND_EVENT = 'assistantCommand'

/** The part of JUCE's page-side backend object this page uses. */
export interface JuceBackend {
  /** @returns the token removeEventListener takes */
  addEventListener(eventId: string, listener: (payload: unknown) => void): unknown
  removeEventListener(token: unknown): void
  emitEvent(eventId: string, payload: unknown): void
}

export interface Bridge {
  /**
   * Calls `listener` with every state payload the plugin sends (unparsed).
   * @returns the function that stops it
   */
  onState(listener: (payload: unknown) => void): () => void
  /**
   * Calls `listener` with every payload of one plugin event (unparsed).
   * @returns the function that stops it
   */
  on(eventId: string, listener: (payload: unknown) => void): () => void
  /** Asks the plugin to send the state now. */
  requestState(): void
  send(command: Command): void
}

/** The page's global object, as far as the bridge looks into it. */
export interface Host {
  readonly __JUCE__?: { readonly backend?: unknown } | undefined
}

/** @returns whether `v` has the three methods of JUCE's backend object */
export const isBackend = (v: unknown): v is JuceBackend => {
  if (typeof v !== 'object' || v === null) return false
  const b = v as Record<string, unknown>
  return typeof b.addEventListener === 'function' && typeof b.removeEventListener === 'function' && typeof b.emitEvent === 'function'
}

/**
 * Finds the plugin this page is running in.
 * @param host the page's global object (the window)
 * @returns the bridge, or null in a plain browser or a WebView without native integration
 */
export function connect(host: Host = window as Host): Bridge | null {
  const backend = host.__JUCE__?.backend
  if (!isBackend(backend)) return null
  const on = (eventId: string, listener: (payload: unknown) => void) => {
    const token = backend.addEventListener(eventId, listener)
    return () => backend.removeEventListener(token)
  }
  return {
    onState: (listener) => on(STATE_EVENT, listener),
    on,
    requestState: () => backend.emitEvent(READY_EVENT, null),
    send: (command) => backend.emitEvent(COMMAND_EVENT, command),
  }
}
