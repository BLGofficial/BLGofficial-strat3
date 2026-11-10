// The page shell. Contracts:
//   - outside the plugin, before the first state, and on an unreadable state it says so
//     plainly and shows no controls
//   - live: two tabs (Assistant, Target), Assistant first unless the address says #target;
//     arrow keys move between tabs; the capture strip stays on screen under both
//   - the plugin drops state events while the panel is hidden, so the page asks again
//     whenever it becomes visible
//   - commands from the panels reach the plugin
import { act, fireEvent, render, screen } from '@testing-library/react'
import { afterEach, describe, expect, it } from 'vitest'

import { App } from './App'
import { connect } from './bridge/juce'
import { createSessionStore } from './bridge/session'
import { createFakeBackend } from './test/fakeBackend'
import { raw, type FixtureName } from './test/fixtures'

const setUp = (first?: FixtureName) => {
  const fake = createFakeBackend()
  const store = createSessionStore(connect({ __JUCE__: { backend: fake.backend } }))
  if (first !== undefined) fake.deliver('assistantState', raw(first))
  const view = render(<App store={store} />)
  // A state arriving from the plugin is an update from outside React: act() makes the test
  // wait for the re-render, as a browser frame would.
  const deliver = (payload: unknown) => act(() => fake.deliver('assistantState', payload))
  return { fake, store, deliver, ...view }
}

const tab = (name: string) => screen.getByRole('tab', { name })

const setVisibility = (value: 'visible' | 'hidden') => {
  Object.defineProperty(document, 'visibilityState', { configurable: true, get: () => value })
  fireEvent(document, new Event('visibilitychange'))
}

afterEach(() => {
  window.location.hash = ''
  Reflect.deleteProperty(document, 'visibilityState')
})

describe('App: no usable link', () => {
  it('says it is not connected outside the plugin', () => {
    render(<App store={createSessionStore(null)} />)
    expect(screen.getByRole('heading', { name: 'Not connected' })).toBeTruthy()
    expect(screen.getByText('This panel runs inside the STRAT3 plugin window.')).toBeTruthy()
    expect(screen.queryByRole('tablist')).toBeNull()
    expect(screen.queryAllByRole('button')).toEqual([])
  })

  it('says it is waiting before the first state', () => {
    setUp()
    expect(screen.getByRole('heading', { name: 'Waiting for the engine' })).toBeTruthy()
    expect(screen.queryAllByRole('button')).toEqual([])
  })

  it('says it cannot read a state that is off, and shows no controls', () => {
    const { deliver } = setUp('moves-pending')
    const damaged = raw('moves-pending')
    damaged.result = 'done'
    fireEvent.click(tab('Target')) // wherever the user was
    deliver(damaged)
    expect(screen.getByRole('heading', { name: 'Cannot read the engine state' })).toBeTruthy()
    expect(screen.getByText('The panel and the plugin do not match. Reinstall STRAT3.')).toBeTruthy()
    expect(screen.queryAllByRole('button')).toEqual([])
  })
})

describe('App: live', () => {
  it('opens on the assistant', () => {
    setUp('moves-pending')
    expect(tab('Assistant').getAttribute('aria-selected')).toBe('true')
    expect(tab('Target').getAttribute('aria-selected')).toBe('false')
    expect(screen.getByRole('tabpanel', { name: 'Assistant' })).toBeTruthy()
    expect(screen.getByRole('button', { name: 'Analyse 30 s' })).toBeTruthy()
    expect(screen.queryByRole('button', { name: 'Capture 30 s' })).toBeNull()
  })

  it('opens on the target when the address asks for it', () => {
    window.location.hash = '#target'
    setUp('moves-pending')
    expect(tab('Target').getAttribute('aria-selected')).toBe('true')
    expect(screen.getByRole('tabpanel', { name: 'Target' })).toBeTruthy()
    expect(screen.getByRole('button', { name: 'Capture 30 s' })).toBeTruthy()
  })

  it('switches panels on click', () => {
    setUp('moves-pending')
    fireEvent.click(tab('Target'))
    expect(tab('Target').getAttribute('aria-selected')).toBe('true')
    expect(screen.getByRole('button', { name: 'Capture 30 s' })).toBeTruthy()
    expect(screen.queryByRole('button', { name: 'Analyse 30 s' })).toBeNull()
    fireEvent.click(tab('Assistant'))
    expect(screen.getByRole('button', { name: 'Analyse 30 s' })).toBeTruthy()
  })

  it('moves between tabs with the arrow keys, and only the selected tab is in the tab order', () => {
    setUp('moves-pending')
    expect(tab('Assistant').tabIndex).toBe(0)
    expect(tab('Target').tabIndex).toBe(-1)
    fireEvent.keyDown(tab('Assistant'), { key: 'ArrowRight' })
    expect(tab('Target').getAttribute('aria-selected')).toBe('true')
    expect(document.activeElement).toBe(tab('Target'))
    expect(tab('Target').tabIndex).toBe(0)
    fireEvent.keyDown(tab('Target'), { key: 'ArrowRight' }) // wraps
    expect(tab('Assistant').getAttribute('aria-selected')).toBe('true')
    fireEvent.keyDown(tab('Assistant'), { key: 'ArrowLeft' }) // wraps the other way
    expect(tab('Target').getAttribute('aria-selected')).toBe('true')
    fireEvent.keyDown(tab('Target'), { key: 'a' })
    expect(tab('Target').getAttribute('aria-selected')).toBe('true')
  })

  it('keeps the capture strip on screen under both tabs', () => {
    setUp('analyse-running')
    expect(screen.getByRole('progressbar', { name: 'Capture progress' })).toBeTruthy()
    fireEvent.click(tab('Target'))
    expect(screen.getByRole('progressbar', { name: 'Capture progress' })).toBeTruthy()
    expect(screen.getByText('Analysing')).toBeTruthy()
  })

  it('follows the plugin as states arrive', () => {
    const { deliver } = setUp('moves-pending')
    expect(screen.getByText('5 pending')).toBeTruthy()
    deliver(raw('moves-decided'))
    expect(screen.getByText('1 pending')).toBeTruthy()
  })

  it('sends the panels commands to the plugin', () => {
    const { fake } = setUp('moves-pending')
    fireEvent.click(screen.getByRole('button', { name: 'Accept move 2' }))
    fireEvent.click(tab('Target'))
    fireEvent.click(screen.getByRole('button', { name: 'Capture 60 s' }))
    expect(fake.commands()).toEqual([
      { type: 'accept', id: 2 },
      { type: 'captureReference', seconds: 60 },
    ])
  })
})

describe('App: visibility', () => {
  it('asks for the state again when the page becomes visible', () => {
    const { fake } = setUp('moves-pending')
    const before = fake.readyCount()
    setVisibility('hidden')
    expect(fake.readyCount()).toBe(before)
    setVisibility('visible')
    expect(fake.readyCount()).toBe(before + 1)
  })

  it('stops watching when it is taken down', () => {
    const { fake, unmount } = setUp('moves-pending')
    const before = fake.readyCount()
    unmount()
    setVisibility('visible')
    expect(fake.readyCount()).toBe(before)
  })
})
