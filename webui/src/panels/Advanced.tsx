// The advanced view (module 21d): Phase 4's advanced slide-over, as the column's third view.
// Every control is a real plugin parameter, bound through JUCE's relays (bridge/relays.ts),
// so the host sees it, automates it and undoes it like any other. The lines under the
// controls say what the engine runs with (bridge/advanced.ts), which can differ from what the
// parameters ask for.
import { useEffect, useMemo, useRef, useSyncExternalStore, type CSSProperties, type KeyboardEvent, type ReactNode } from 'react'

import type { AdvancedStore, InfoLink } from '../bridge/advanced'
import type { RelayValue, Relays, SliderRelay } from '../bridge/relays'
import { hz } from '../format'
import { RELAYS, type AdvancedInfo, type ComboSpec, type SliderSpec } from '../protocol/advanced'

interface Props {
  readonly info: AdvancedStore
  readonly relays: Relays
}

const MISMATCH = 'The panel and the plugin do not match. Reinstall STRAT3.'

const sliderSpec = (id: string): SliderSpec => {
  const spec = RELAYS.find((r) => r.id === id)
  if (spec?.kind !== 'slider') throw new Error(`no slider "${id}"`)
  return spec
}

const comboSpec = (id: string): ComboSpec => {
  const spec = RELAYS.find((r) => r.id === id)
  if (spec?.kind !== 'comboBox') throw new Error(`no combo box "${id}"`)
  return spec
}

const useRelay = <T,>(relay: RelayValue<T>): T | null => useSyncExternalStore(relay.subscribe, relay.getSnapshot)

/** Steps of a slider's travel: fine enough that a step is under 0.3 % of the range on a log scale. */
const STEPS = 1000

const toPosition = (spec: SliderSpec, value: number): number => {
  const t = spec.taper === 'log' ? Math.log(value / spec.min) / Math.log(spec.max / spec.min) : (value - spec.min) / (spec.max - spec.min)
  return Math.round(Math.min(1, Math.max(0, t)) * STEPS)
}

const fromPosition = (spec: SliderSpec, position: number): number => {
  const t = position / STEPS
  return spec.taper === 'log' ? spec.min * Math.pow(spec.max / spec.min, t) : spec.min + t * (spec.max - spec.min)
}

/**
 * One host gesture per drag: opened by the pointer or by the first step from the keys, closed
 * when the pointer is let go (anywhere: a drag can end outside the slider), the key comes up,
 * the slider loses focus or the view goes away. Without it every step is its own undo step.
 */
function useGesture(relay: SliderRelay) {
  const state = useRef({ open: false, relay, release: () => {} })
  state.current.relay = relay
  const gesture = useMemo(() => {
    const s = state.current
    const end = () => {
      if (!s.open) return
      s.open = false
      s.release()
      s.relay.dragEnd()
    }
    const start = (byPointer: boolean) => {
      if (s.open) return
      s.open = true
      s.relay.dragStart()
      if (!byPointer) return
      document.addEventListener('pointerup', end)
      document.addEventListener('pointercancel', end)
      s.release = () => {
        document.removeEventListener('pointerup', end)
        document.removeEventListener('pointercancel', end)
        s.release = () => {}
      }
    }
    return { start, end }
  }, [])
  // A tab switch mid-drag unmounts the slider: the host gesture must not stay open.
  useEffect(() => gesture.end, [gesture])
  return gesture
}

interface SliderProps {
  readonly relay: SliderRelay
  readonly spec: SliderSpec
  /** The accessible name; it contains `label`. */
  readonly name: string
  /** What is written next to the slider. */
  readonly label: string
  readonly off?: boolean
}

/** A parameter slider on the parameter's own taper, showing the value the plugin holds. */
function ParamSlider({ relay, spec, name, label, off = false }: SliderProps) {
  const value = useRelay(relay)
  const gesture = useGesture(relay)
  const disabled = value === null || off
  const position = value === null ? 0 : toPosition(spec, value)

  return (
    <div className="mt-2 flex items-center">
      <span className="label mr-2 w-10 shrink-0">{label}</span>
      <input
        type="range"
        className="depth min-w-0 flex-1"
        min={0}
        max={STEPS}
        step={1}
        value={position}
        disabled={disabled}
        aria-label={name}
        aria-valuetext={value === null ? undefined : hz(value)}
        style={{ '--fill': `${position / (STEPS / 100)}%` } as CSSProperties}
        onPointerDown={() => !disabled && gesture.start(true)}
        onChange={(e) => {
          gesture.start(false)
          relay.set(fromPosition(spec, Number(e.currentTarget.value)))
        }}
        onKeyUp={gesture.end}
        onBlur={gesture.end}
      />
      <span className={`num ml-2 w-16 shrink-0 text-right ${disabled ? 'text-secondary' : ''}`}>{value === null ? '' : hz(value)}</span>
    </div>
  )
}

interface ChoiceProps {
  readonly relay: RelayValue<number>
  readonly spec: ComboSpec
  readonly name: string
  /** What is written over the choices, if the section heading does not say it. */
  readonly label?: string
}

/** A choice parameter as a row of radio buttons; arrows move and choose, as in a radio group. */
function Choice({ relay, spec, name, label }: ChoiceProps) {
  const index = useRelay(relay)
  const buttons = useRef<(HTMLButtonElement | null)[]>([])
  const choose = (i: number) => {
    relay.set(i)
    buttons.current[i]?.focus()
  }
  const onKeyDown = (event: KeyboardEvent) => {
    const step = event.key === 'ArrowRight' || event.key === 'ArrowDown' ? 1 : event.key === 'ArrowLeft' || event.key === 'ArrowUp' ? -1 : 0
    if (step === 0 || index === null) return
    event.preventDefault()
    choose((index + step + spec.choices.length) % spec.choices.length)
  }

  // The label sits over the row, not beside it: three choices need the column's full width.
  return (
    <div className="mt-2">
      {label !== undefined && <span className="label mb-1 block">{label}</span>}
      <div role="radiogroup" aria-label={name} className="flex" onKeyDown={onKeyDown}>
        {spec.choices.map((choice, i) => (
          <button
            key={choice}
            ref={(el) => void (buttons.current[i] = el)}
            type="button"
            role="radio"
            aria-checked={index === i}
            // One tab stop for the group: the chosen button, or the first while unknown.
            tabIndex={index === i || (index === null && i === 0) ? 0 : -1}
            disabled={index === null}
            className={`btn min-w-0 flex-1 whitespace-nowrap px-1 ${i > 0 ? '-ml-px' : ''}`}
            onClick={() => choose(i)}
          >
            {choice}
          </button>
        ))}
      </div>
    </div>
  )
}

function Section({ id, title, children }: { readonly id: string; readonly title: string; readonly children: ReactNode }) {
  return (
    <section aria-labelledby={`advanced-${id}`} className="border-b border-rule px-3 py-2">
      <h2 id={`advanced-${id}`} className="label text-text">
        {title}
      </h2>
      {children}
    </section>
  )
}

const Note = ({ children, signal = false }: { readonly children: ReactNode; readonly signal?: boolean }) => (
  <p className={`mt-2 ${signal ? 'text-signal' : 'text-muted'}`}>{children}</p>
)

/** A sample rate as "96 kHz" or "44.1 kHz". */
const rate = (fs: number): string => `${Number((fs / 1000).toFixed(1))} kHz`

const TOPOLOGY: Readonly<Record<'iir' | 'fir', string>> = {
  iir: 'Linkwitz-Riley (IIR), as in TRACK and MIX',
  fir: 'Linear phase (FIR), as in MASTER',
}

const topologyText = (link: InfoLink): string => {
  if (link.status !== 'live') return 'Waiting for the engine.'
  return link.info.topology === null ? 'Not running: the host has not started the plugin.' : TOPOLOGY[link.info.topology]
}

/** The points in use when they read differently from the points as set, both from the info. */
const changedPoints = (info: AdvancedInfo | null): readonly number[] | null => {
  if (info === null || info.crossovers === null || info.crossoversSet === null) return null
  const set = info.crossoversSet
  return info.crossovers.some((x, i) => hz(x) !== hz(set[i] ?? Number.NaN)) ? info.crossovers : null
}

const CHANNELS = ['not connected', 'mono', 'stereo'] as const

function Controls({ info, relays }: { readonly info: InfoLink; readonly relays: Relays }) {
  const live = info.status === 'live' ? info.info : null
  const inUse = changedPoints(live)

  const dither = useRelay(relays.combo('ceiling.dither'))
  const bits = useRelay(relays.combo('ceiling.bits'))
  const source = useRelay(relays.combo('mb.scSource'))
  const hpOn = useRelay(relays.toggle('mb.scHpOn'))
  const hpRelay = relays.toggle('mb.scHpOn')

  const os = live?.oversampling
  const capped = os !== undefined && os.active !== null && os.active < os.requested

  return (
    <div>
      <Section id="crossover" title="Crossover">
        <p className="mt-1 text-muted">{topologyText(info)}</p>
        {(['xover.f1', 'xover.f2', 'xover.f3'] as const).map((id, i) => (
          <ParamSlider key={id} relay={relays.slider(id)} spec={sliderSpec(id)} name={`Crossover ${i + 1}`} label={String(i + 1)} />
        ))}
        {inUse !== null && (
          <>
            <p className="mt-2">In use: {inUse.map(hz).join(', ')}</p>
            <Note>The engine sorts the points, keeps them at least 1.25x apart and under 0.45x the sample rate.</Note>
          </>
        )}
      </Section>

      <Section id="oversampling" title="Oversampling">
        <p className="mt-1 text-muted">For SATURATE.</p>
        <Choice relay={relays.combo('sat.os')} spec={comboSpec('sat.os')} name="Oversampling" />
        {capped && live !== null && (
          <Note signal>
            Runs at {os.active}x: {os.requested}x is over the limit at {rate(live.sampleRate)}.
          </Note>
        )}
      </Section>

      <Section id="dither" title="Dither">
        <Choice relay={relays.combo('ceiling.dither')} spec={comboSpec('ceiling.dither')} name="Dither" label="Type" />
        <Choice relay={relays.combo('ceiling.bits')} spec={comboSpec('ceiling.bits')} name="Dither bits" label="Bits" />
        {dither !== null && dither > 0 && bits === 1 && <Note>Dither applies at 16 bit only: at 24 bit it is skipped.</Note>}
      </Section>

      <Section id="sidechain" title="Sidechain">
        {live !== null && <p className="mt-1 text-muted">Host input: {CHANNELS[live.sidechain.channels]}</p>}
        <Choice relay={relays.combo('mb.scSource')} spec={comboSpec('mb.scSource')} name="Sidechain source" label="Source" />
        {source === 1 && live !== null && live.sidechain.channels === 0 && (
          <Note signal>No sidechain input is connected, so MULTIBAND listens to the mix.</Note>
        )}
        <div className="mt-2 flex items-center">
          <button
            type="button"
            role="switch"
            aria-checked={hpOn === true}
            disabled={hpOn === null}
            className="btn"
            onClick={() => hpOn !== null && hpRelay.set(!hpOn)}
          >
            High-pass
          </button>
        </div>
        <ParamSlider relay={relays.slider('mb.scHpFreq')} spec={sliderSpec('mb.scHpFreq')} name="High-pass frequency" label="Freq" off={hpOn !== true} />
      </Section>
    </div>
  )
}

/**
 * The advanced view.
 * @param info what the plugin says the engine runs with
 * @param relays the plugin's parameters behind the controls
 */
export function Advanced({ info, relays }: Props) {
  const link = useSyncExternalStore(info.subscribe, info.getSnapshot)
  // Half a set of controls next to a plugin from another build would move the wrong things.
  if (relays.missing.length > 0 || link.status === 'invalid') {
    return (
      <div className="px-3 py-4">
        <h2 className="label text-text">Cannot show the advanced controls</h2>
        <p className="mt-2 text-muted">{MISMATCH}</p>
      </div>
    )
  }
  return <Controls info={link} relays={relays} />
}
