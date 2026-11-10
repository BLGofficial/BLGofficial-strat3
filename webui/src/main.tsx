import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'

import { App, type AdvancedLink } from './App'
import { createAdvancedStore } from './bridge/advanced'
import { connect, type Bridge } from './bridge/juce'
import { connectRelays } from './bridge/relays'
import { createSessionStore } from './bridge/session'
import './index.css'

function start(bridge: Bridge | null) {
  const root = document.getElementById('root')
  if (root === null) throw new Error('index.html has no #root')
  // The advanced store listens before the session store asks for the state: the plugin sends
  // the info with it. The relays ask for their own values.
  const relays = bridge === null ? null : connectRelays()
  const advanced: AdvancedLink | undefined = relays === null ? undefined : { store: createAdvancedStore(bridge), relays }
  createRoot(root).render(
    <StrictMode>
      <App store={createSessionStore(bridge)} advanced={advanced} />
    </StrictMode>,
  )
}

const bridge = connect()
if (import.meta.env.DEV && bridge === null) {
  // Outside the plugin, on the dev server only: replay a recorded state (see dev/replay.ts).
  void import('./dev/replay').then(({ replayBridge }) => start(replayBridge(window.location.search)))
} else {
  start(bridge)
}
