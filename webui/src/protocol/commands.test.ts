// Commands the page sends. Contracts tested here:
//   - the builders produce exactly the objects in contract/commands.json. The plugin's real
//     parser is tested against that same file (tests/assistant/test_panel_contract.cpp), so
//     a builder cannot drift from what the plugin accepts without one side failing
//   - the file lists every call tested here and nothing else
//   - a depth outside 0..1 is clamped; one that is not a number reaches the plugin as null,
//     which its parser refuses (bad input changes nothing)
import { describe, expect, it } from 'vitest'

import contract from '../../contract/commands.json'
import { accept, analyse, applyAll, cancel, captureReference, clearTarget, depth, reject, type Command } from './commands'

const built: Record<string, Command> = {
  'analyse(30)': analyse(30),
  'analyse(60)': analyse(60),
  'captureReference(30)': captureReference(30),
  'captureReference(60)': captureReference(60),
  'cancel()': cancel(),
  'accept(7)': accept(7),
  'reject(7)': reject(7),
  'depth(7, 0.35)': depth(7, 0.35),
  'depth(7, 0)': depth(7, 0),
  'depth(7, 1)': depth(7, 1),
  'depth(7, 1.7)': depth(7, 1.7),
  'depth(7, -0.2)': depth(7, -0.2),
  'applyAll()': applyAll(),
  'clearTarget()': clearTarget(),
}

describe('commands', () => {
  it('covers the same calls as the contract file', () => {
    expect(Object.keys(built).sort()).toEqual(contract.map((entry) => entry.call).sort())
  })

  it.each(contract)('$call builds the command the plugin is tested against', ({ call, command }) => {
    expect(built[call]).toEqual(command)
  })

  it('covers every command type the plugin has', () => {
    const types = new Set(contract.map((entry) => entry.command.type))
    expect([...types].sort()).toEqual(['accept', 'analyse', 'applyAll', 'cancel', 'captureReference', 'clearTarget', 'depth', 'reject'])
  })

  it('sends a depth that is not a number as null, which the plugin refuses', () => {
    expect(JSON.parse(JSON.stringify(depth(7, Number.NaN)))).toEqual({ type: 'depth', id: 7, depth: null })
  })
})
