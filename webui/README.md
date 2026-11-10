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
- `contract/advanced.json`: the advanced panel's parameters (id, kind, range, unit, taper or
  choices, from the real parameter specs) and the `advancedInfo` payload in four situations,
  from real processors. Written by `tests/ui/test_advanced_link.cpp`.

After a protocol change: run the C++ tests, copy `<build>/panel-contract/states/*.json` over
`contract/states/` (and `<build>/panel-contract/advanced.json` over `contract/advanced.json`), then run `npm test` and fix what the page no longer reads. A new state
also needs its name in `src/test/fixtures.ts`; the page's tests fail until it is listed.

## The bridge

`src/bridge/juce.ts` talks to `window.__JUCE__.backend`, the object JUCE injects into a
WebView with native integration on. Three events:

| Event | Direction | Payload |
| --- | --- | --- |
| `assistantState` | plugin to page | the whole session (`src/protocol/state.ts`) |
| `assistantReady` | page to plugin | none: "send the state now" |
| `assistantCommand` | page to plugin | one command (`src/protocol/commands.ts`) |

| `advancedInfo` | plugin to page | what the engine runs with (`src/protocol/advanced.ts`), with every state and whenever it changes |
| `showView` | plugin to page | `{ "view": "advanced" }`: the header's gear was pressed |

The page asks with `assistantReady` on load, once a second until the first state arrives,
and whenever it becomes visible again: the plugin only delivers events to a visible WebView.

The advanced panel's controls are plugin parameters bound through JUCE's control relays
(`src/bridge/relays.ts`): one event per parameter, `__juce__slider<id>`, `__juce__toggle<id>`
or `__juce__comboBox<id>`, as JUCE 8's own page helper sends them. The page does not bundle
that helper (it ships in the JUCE checkout, not on npm). A slider drag is one host gesture.
