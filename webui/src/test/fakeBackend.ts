// Test double for the object JUCE injects as window.__JUCE__.backend, written from JUCE
// 8.0.17 (modules/juce_gui_extra/misc/juce_WebBrowserComponent.cpp, class Backend):
//   addEventListener    returns the removal token [eventId, listenerId]
//   emitEvent           posts JSON text { eventId, payload } to the native side
//   native -> page      arrives as JSON text and is parsed before the listeners run
// So everything crossing this double goes through JSON, as it does in the plugin: a NaN
// becomes null and an undefined field disappears.
import type { JuceBackend } from '../bridge/juce'

export interface Sent {
  readonly eventId: string
  readonly payload: unknown
}

export interface FakeBackend {
  readonly backend: JuceBackend
  /** Everything the page emitted, as the native side would parse it. */
  readonly sent: Sent[]
  /** Payloads of the commands the page sent. */
  commands(): unknown[]
  /** How often the page asked for the state. */
  readyCount(): number
  /** The native side emitting an event to the page. */
  deliver(eventId: string, payload: unknown): void
  listenerCount(eventId: string): number
}

export function createFakeBackend(): FakeBackend {
  const listeners = new Map<string, Map<number, (payload: unknown) => void>>()
  const sent: Sent[] = []
  let nextId = 0

  const backend: JuceBackend = {
    addEventListener(eventId, fn) {
      const forEvent = listeners.get(eventId) ?? new Map<number, (payload: unknown) => void>()
      listeners.set(eventId, forEvent)
      const id = nextId++
      forEvent.set(id, fn)
      return [eventId, id]
    },
    removeEventListener(token) {
      const [eventId, id] = token as [string, number]
      listeners.get(eventId)?.delete(id)
    },
    emitEvent(eventId, payload) {
      sent.push(JSON.parse(JSON.stringify({ eventId, payload })) as Sent)
    },
  }

  return {
    backend,
    sent,
    commands: () => sent.filter((e) => e.eventId === 'assistantCommand').map((e) => e.payload),
    readyCount: () => sent.filter((e) => e.eventId === 'assistantReady').length,
    deliver(eventId, payload) {
      const parsed: unknown = JSON.parse(JSON.stringify(payload))
      for (const fn of [...(listeners.get(eventId)?.values() ?? [])]) fn(parsed)
    },
    listenerCount: (eventId) => listeners.get(eventId)?.size ?? 0,
  }
}
