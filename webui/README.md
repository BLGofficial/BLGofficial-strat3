# STRAT3 web panels

The assistant and target panels, shown in a WebView in the plugin's right column. React 18,
TypeScript, Tailwind, built by Vite into three files (`dist/index.html`, `dist/assets/app.js`,
`dist/assets/style.css`) that CMake embeds in the plugin. The page never loads anything from
the network.

## Commands

Node 22.22 or newer. Run in `webui/`:

| Command | Does |
| --- | --- |
| `npm ci` | installs the pinned versions from `package-lock.json` |
| `npm test` | runs the page's tests (Vitest, jsdom) |
| `npm run build` | type-checks, then writes `dist/` |
| `npm run dev` | dev server; open `/?state=moves-pending` to see a recorded plugin state (any file name from `contract/states/`, add `#target` for the target panel) |

Build `dist/` before configuring CMake with `STRAT3_WITH_WEBUI=ON`.

To change a dependency, use npm 11 or newer: `npm install` in npm 10.9 crashes while
resolving this tree (`Cannot read properties of null (reading 'edgesOut')`). `npm ci` is fine
in both.

## The contract with the plugin

`contract/` is read by both test suites, so neither side can change the protocol alone.

- `contract/states/*.json`: what the plugin sends in each situation the panels draw. Written
  by the real `stateToVar` from the scenarios in `tests/support/PanelScenarios.h`. The C++
  test `[contract]` fails when a file no longer matches; the page's tests render from them.
- `contract/commands.json`: every command the page builds. The page's tests check the
  builders against it; the C++ test feeds it to the real `parseCommand`.

After a protocol change: run the C++ tests, copy `<build>/panel-contract/states/*.json` over
`contract/states/`, then run `npm test` and fix what the page no longer reads.

## The bridge

`src/bridge/juce.ts` talks to `window.__JUCE__.backend`, the object JUCE injects into a
WebView with native integration on. Three events:

| Event | Direction | Payload |
| --- | --- | --- |
| `assistantState` | plugin to page | the whole session (`src/protocol/state.ts`) |
| `assistantReady` | page to plugin | none: "send the state now" |
| `assistantCommand` | page to plugin | one command (`src/protocol/commands.ts`) |

The page asks with `assistantReady` on load, once a second until the first state arrives,
and whenever it becomes visible again: the plugin only delivers events to a visible WebView.
