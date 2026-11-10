// The page shell: says plainly when there is no usable link to the plugin, otherwise shows
// the two panels as tabs with the capture strip between the tabs and the panel.
import { useEffect, useRef, useState, useSyncExternalStore, type KeyboardEvent } from 'react'

import type { Link, SessionStore } from './bridge/session'
import { CaptureStrip, type PanelProps } from './components/CaptureStrip'
import { Assistant } from './panels/Assistant'
import { Targets } from './panels/Targets'

const TABS = [
  { id: 'assistant', label: 'Assistant' },
  { id: 'target', label: 'Target' },
] as const

type PanelId = (typeof TABS)[number]['id']

const NOTICES: Readonly<Record<Exclude<Link['status'], 'live'>, { title: string; text: string }>> = {
  unavailable: { title: 'Not connected', text: 'This panel runs inside the STRAT3 plugin window.' },
  waiting: { title: 'Waiting for the engine', text: 'The plugin has not sent its state yet.' },
  invalid: { title: 'Cannot read the engine state', text: 'The panel and the plugin do not match. Reinstall STRAT3.' },
}

/** The panel the address asks for: the native side can open the page on "#target". */
const panelFromAddress = (): PanelId => (window.location.hash === '#target' ? 'target' : 'assistant')

function Panels({ state, send }: PanelProps) {
  const [panel, setPanel] = useState<PanelId>(panelFromAddress)
  const tabs = useRef(new Map<PanelId, HTMLButtonElement>())

  const onKeyDown = (event: KeyboardEvent) => {
    const step = event.key === 'ArrowRight' ? 1 : event.key === 'ArrowLeft' ? -1 : 0
    if (step === 0) return
    const index = TABS.findIndex((t) => t.id === panel)
    const next = TABS[(index + step + TABS.length) % TABS.length]
    if (next === undefined) return
    event.preventDefault()
    setPanel(next.id)
    tabs.current.get(next.id)?.focus()
  }

  return (
    <div className="flex h-full flex-col">
      <div role="tablist" aria-label="Panels" className="flex border-b border-rule">
        {TABS.map((t) => (
          <button
            key={t.id}
            ref={(el) => void (el === null ? tabs.current.delete(t.id) : tabs.current.set(t.id, el))}
            type="button"
            role="tab"
            id={`tab-${t.id}`}
            aria-selected={t.id === panel}
            aria-controls={`panel-${t.id}`}
            // Only the selected tab is a tab stop; the arrow keys move between tabs.
            tabIndex={t.id === panel ? 0 : -1}
            className={`label h-8 flex-1 border-b-2 ${t.id === panel ? 'border-signal text-text' : 'border-transparent'}`}
            onClick={() => setPanel(t.id)}
            onKeyDown={onKeyDown}
          >
            {t.label}
          </button>
        ))}
      </div>
      <CaptureStrip state={state} send={send} />
      <div role="tabpanel" id={`panel-${panel}`} aria-labelledby={`tab-${panel}`} className="scroll min-h-0 flex-1 overflow-y-auto">
        {panel === 'assistant' ? <Assistant state={state} send={send} /> : <Targets state={state} send={send} />}
      </div>
    </div>
  )
}

/**
 * The page.
 * @param store the session store this page follows (created once, in main.tsx)
 */
export function App({ store }: { readonly store: SessionStore }) {
  const link = useSyncExternalStore(store.subscribe, store.getSnapshot)

  // The plugin drops state events while the panel is hidden, so what is on screen can be
  // stale when it comes back.
  useEffect(() => {
    const onVisibility = () => {
      if (document.visibilityState === 'visible') store.refresh()
    }
    document.addEventListener('visibilitychange', onVisibility)
    return () => document.removeEventListener('visibilitychange', onVisibility)
  }, [store])

  if (link.status !== 'live') {
    const notice = NOTICES[link.status]
    return (
      <div className="px-3 py-4">
        <h1 className="label text-text">{notice.title}</h1>
        <p className="mt-2 text-muted">{notice.text}</p>
      </div>
    )
  }
  return <Panels state={link.state} send={store.send} />
}
