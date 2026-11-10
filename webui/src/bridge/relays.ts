// The page's side of JUCE's control relays (module 21d): one live value per advanced
// parameter, bound to the plugin's WebSliderRelay / WebToggleButtonRelay / WebComboBoxRelay
// of the same name (source/ui/web/WebPanelHost.cpp).
//
// Written from JUCE 8.0.17 (juce_WebControlRelays.cpp, juce_ParameterAttachments.cpp and
// native/javascript/index.js). The page does not bundle JUCE's helper (it lives in the JUCE
// checkout, not in npm), so this file speaks the same events itself:
//   "__juce__slider" + id    a float parameter, value in its own unit
//   "__juce__toggle" + id    a bool parameter, value true or false
//   "__juce__comboBox" + id  a choice parameter, value 0..1 over its choices
// each with eventType requestInitialUpdate / valueChanged (both ways) / propertiesChanged
// (native -> page) and, for sliders, sliderDragStarted / sliderDragEnded (the host gesture).
// The ranges come from protocol/advanced.ts, which a plugin test checks against the real
// parameters, so propertiesChanged is not needed and is ignored.
import { RELAYS, type RelayKind, type RelaySpec } from '../protocol/advanced'
import { isBackend, type JuceBackend } from './juce'

/** One parameter as the page sees it. */
export interface RelayValue<T> {
  /** null until the plugin has sent the value. */
  getSnapshot(): T | null
  /** @returns the function that unsubscribes */
  subscribe(onChange: () => void): () => void
  /** Sends a value to the plugin and shows it at once. */
  set(value: T): void
}

export interface SliderRelay extends RelayValue<number> {
  /** Opens the host gesture: call before the first value of a drag. */
  dragStart(): void
  /** Closes it. */
  dragEnd(): void
}

export interface Relays {
  /** Ids the plugin did not create (in RELAYS order): a plugin from another build. */
  readonly missing: readonly string[]
  /** @throws if `id` is not a slider in RELAYS */
  slider(id: string): SliderRelay
  /** A choice as its index. @throws if `id` is not a combo box in RELAYS */
  combo(id: string): RelayValue<number>
  /** @throws if `id` is not a toggle in RELAYS */
  toggle(id: string): RelayValue<boolean>
  /** Stops listening to the plugin. */
  dispose(): void
}

/** The page's global object, as far as the relays look into it. */
export interface RelayHost {
  readonly __JUCE__?: { readonly backend?: unknown; readonly initialisationData?: unknown } | undefined
}

const PREFIX: Readonly<Record<RelayKind, string>> = { slider: '__juce__slider', toggle: '__juce__toggle', comboBox: '__juce__comboBox' }
const LIST: Readonly<Record<RelayKind, string>> = { slider: '__juce__sliders', toggle: '__juce__toggles', comboBox: '__juce__comboBoxes' }

/** The relay names JUCE listed for one kind; anything that is not a list of names is none. */
const listed = (data: unknown, kind: RelayKind): readonly string[] => {
  if (typeof data !== 'object' || data === null) return []
  const list = (data as Record<string, unknown>)[LIST[kind]]
  return Array.isArray(list) ? list.filter((n): n is string => typeof n === 'string') : []
}

interface Bound {
  readonly spec: RelaySpec
  readonly value: RelayValue<number | boolean> & { dragStart(): void; dragEnd(): void }
  readonly stop: () => void
}

/**
 * Reads one plugin value from a "valueChanged" event, in the page's terms.
 * @returns the value, or undefined for anything JUCE would not send
 */
const decode = (spec: RelaySpec, value: unknown): number | boolean | undefined => {
  switch (spec.kind) {
    case 'slider':
      return typeof value === 'number' && Number.isFinite(value) ? value : undefined
    case 'toggle':
      // JUCE sends a bool; 0 and 1 are the same thing through a var.
      return typeof value === 'boolean' ? value : value === 0 || value === 1 ? value === 1 : undefined
    case 'comboBox': {
      if (typeof value !== 'number' || !(value >= 0 && value <= 1)) return undefined
      return Math.round(value * (spec.choices.length - 1)) // as JUCE's getChoiceIndex
    }
  }
}

/**
 * Puts a page value into the form JUCE's attachment takes.
 * @returns the payload value, or undefined for a value the parameter cannot hold
 */
const encode = (spec: RelaySpec, value: number | boolean): number | boolean | undefined => {
  switch (spec.kind) {
    case 'slider':
      // The attachment would clamp too; clamping here keeps what the page shows in range.
      return typeof value === 'number' && Number.isFinite(value) ? Math.min(spec.max, Math.max(spec.min, value)) : undefined
    case 'toggle':
      return typeof value === 'boolean' ? value : undefined
    case 'comboBox': {
      const n = spec.choices.length
      if (typeof value !== 'number' || !Number.isInteger(value) || value < 0 || value >= n) return undefined
      return n > 1 ? value / (n - 1) : 0 // as JUCE's setChoiceIndex
    }
  }
}

const bind = (backend: JuceBackend, spec: RelaySpec, present: boolean): Bound => {
  const eventId = PREFIX[spec.kind] + spec.id
  const subscribers = new Set<() => void>()
  let current: number | boolean | null = null

  const show = (next: number | boolean) => {
    if (next === current) return
    current = next
    for (const onChange of [...subscribers]) onChange()
  }

  // A relay the plugin did not create has no listener on the native side: nothing is sent
  // to it and nothing comes from it (the page shows a mismatch notice instead).
  let stop = () => {}
  if (present) {
    const token = backend.addEventListener(eventId, (payload) => {
      if (typeof payload !== 'object' || payload === null) return
      const { eventType, value } = payload as Record<string, unknown>
      if (eventType !== 'valueChanged') return
      const decoded = decode(spec, value)
      if (decoded !== undefined) show(decoded)
    })
    stop = () => backend.removeEventListener(token)
    backend.emitEvent(eventId, { eventType: 'requestInitialUpdate' })
  }

  return {
    spec,
    stop,
    value: {
      getSnapshot: () => current,
      subscribe(onChange) {
        subscribers.add(onChange)
        return () => void subscribers.delete(onChange)
      },
      set(value) {
        const encoded = encode(spec, value)
        if (encoded === undefined) return
        // The plugin echoes the value back. A slow echo of an earlier step of a fast drag can
        // show for a moment; the last one always wins, as the plugin handles them in order.
        show(spec.kind === 'slider' ? (encoded as number) : value)
        backend.emitEvent(eventId, { eventType: 'valueChanged', value: encoded })
      },
      dragStart: () => backend.emitEvent(eventId, { eventType: 'sliderDragStarted' }),
      dragEnd: () => backend.emitEvent(eventId, { eventType: 'sliderDragEnded' }),
    },
  }
}

/**
 * Binds every relay in RELAYS and asks the plugin for each value once.
 * @param host the page's global object (the window)
 * @returns the relays, or null outside the plugin
 */
export function connectRelays(host: RelayHost = window as RelayHost): Relays | null {
  const backend = host.__JUCE__?.backend
  if (!isBackend(backend)) return null
  const data = host.__JUCE__?.initialisationData

  const bound = new Map<string, Bound>()
  const missing: string[] = []
  for (const spec of RELAYS) {
    const present = listed(data, spec.kind).includes(spec.id)
    if (!present) missing.push(spec.id)
    bound.set(spec.id, bind(backend, spec, present))
  }

  const lookup = (id: string, kind: RelayKind) => {
    const b = bound.get(id)
    if (b === undefined || b.spec.kind !== kind) throw new Error(`no ${kind} relay "${id}"`)
    return b.value
  }

  return {
    missing,
    slider: (id) => lookup(id, 'slider') as SliderRelay,
    combo: (id) => lookup(id, 'comboBox') as RelayValue<number>,
    toggle: (id) => lookup(id, 'toggle') as RelayValue<boolean>,
    dispose: () => bound.forEach((b) => b.stop()),
  }
}
