import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'

import { App } from './App'
import { connect, type Bridge } from './bridge/juce'
import { createSessionStore } from './bridge/session'
import './index.css'

function start(bridge: Bridge | null) {
  const root = document.getElementById('root')
  if (root === null) throw new Error('index.html has no #root')
  createRoot(root).render(
    <StrictMode>
      <App store={createSessionStore(bridge)} />
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
