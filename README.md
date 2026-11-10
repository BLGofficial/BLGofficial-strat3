# STRAT3

A mastering plugin: one chain of processing layers, each with its own depth, a live view of
what every layer changes, delivery metering, and an assistant that proposes moves from what
it hears and explains each one.

VST3, AU, CLAP and Standalone, built with JUCE 8 and C++20. The main editor is native JUCE;
the assistant and target panels are React pages in a WebView (`webui/`).

**Status: in development (version 0.1.0).** Not yet tested in a DAW, with pluginval or with
clap-validator. See [Status](#status).

## The chain

```
IN TRIM -> EQ -> LOW -> MB -> WIDTH -> SAT -> LIMIT -> OUT TRIM -> monitor -> CEILING
```

- **EQ**: 8 bands, static or dynamic, stereo or mid / side. Linear phase (FIR) in MASTER
  mode, IIR in TRACK and MIX.
- **LOW**: below a set crossover only: punch / sustain shaping of the low band, and mono bass.
- **MB**: multiband dynamics, downward and upward compression per band.
- **WIDTH**: stereo width per band, and a stereoizer that leaves the mono fold-down unchanged.
- **SAT**: saturation per band, oversampled 1x / 2x / 4x.
- **LIMIT**: look-ahead limiter, three styles (Clean, Dense, Clip).
- **CEILING**: true-peak safety limiter and dither. Always last, also on the monitor signals.

The six depth layers (EQ to LIMIT) can be reordered. Neighbouring band layers (MB, WIDTH,
SAT) share one band split. Each layer blends between its input and its output with its own
depth control. Three latency modes (TRACK, MIX, MASTER). Latency is fixed per mode, so the host's delay compensation does not move
when a layer is switched on or off.

Monitoring: delta per layer or for the whole chain (hear only what a layer adds), and a
gain match that applies to the monitor only and is never printed.

Metering: integrated / short-term / momentary loudness (ITU-R BS.1770), true peak,
correlation, PLR, input and output peaks with clip latches, and gain reduction.

## The assistant

The assistant listens to a capture of 30 or 60 s of the plugin's output and proposes moves:
tonal EQ (static or a dynamic dip), mono bass, limiter level for the chosen delivery target
(-14, -11, -9 or -8 LUFS). Each proposal says why, what was measured and what it aims for.
You accept, reject or scale it; nothing changes until you do. Moves decided before the latest
analysis stay as read-only history.

The target is your own: play reference tracks through the plugin and capture them (up to
16). Tonal moves need a target; the loudness and bass moves do not.

## Building

Requirements:
- CMake 3.25 or newer and a C++20 compiler. Only gcc 13 on Linux has been built and tested so
  far; macOS (universal binary, macOS 13 or newer) and Windows are set up in CMake but not
  built yet.
- Node 22.22 or newer with npm, for the WebView panels.
- On Linux, the JUCE dependencies, and WebKitGTK 4.1 when building with the WebView panels
  (Debian / Ubuntu: `libwebkit2gtk-4.1-dev`).

Dependencies (JUCE 8.0.17, clap-juce-extensions, Catch2 3.16.1) are fetched by CMake at
configure time, at pinned versions.

Build the panels first. The plugin embeds them, so it never loads its UI from the network:

```sh
cd webui
npm ci
npm run build
cd ..
```

Then the plugin and the tests:

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

CMake options:

| Option | Default | Effect |
| --- | --- | --- |
| `STRAT3_BUILD_PLUGIN` | `ON` | Build VST3, AU, CLAP and Standalone |
| `STRAT3_BUILD_TESTS` | `ON` | Build the C++ test suite (`strat3_tests`) |
| `STRAT3_WITH_WEBUI` | `ON` | Embed the WebView panels. `OFF` builds a native-only plugin and needs neither Node nor WebKitGTK |

Builds land in `build/STRAT3_artefacts/`. The plugin is not copied to the system plugin
folders. AU builds on macOS only.

## Testing

```sh
ctest --test-dir build                 # C++: DSP, engine, processor, assistant, state
./build/strat3_tests "[contract]"      # only the C++ side of the panel contract
cd webui && npm test                   # the panels (Vitest)
```

With `STRAT3_BUILD_PLUGIN`, `STRAT3_BUILD_TESTS` and `STRAT3_WITH_WEBUI` all on, ctest also
runs `webview_check`: the real editor in a window with the system WebView, clicking through
the panels. When `xvfb-run` is installed (package `xvfb`) ctest runs it under that, so no
display is needed; it is skipped when no WebView can be started.

The C++ side and the panels share one contract in `webui/contract/`: protocol states
recorded from the real C++ code, and every command the panels send. Both test suites read
it, so neither side can change the protocol alone. How to re-record it after a protocol
change: [webui/README.md](webui/README.md).

`tests/golden/param_ids_v1.txt` holds every parameter id. Hosts save sessions by these ids,
so the test fails if one is renamed or removed.

## Repository layout

```
source/
  dsp/          the layers, crossovers, EQ (IIR and FIR), oversampler, depth blend
  engine/       the chain: order, latency, delta, gain match
  analysis/     loudness, true peak, correlation, the per-band impact analysis
  params/       parameter specs and layout, saved-state migration
  processor/    the AudioProcessor: parameters to engine, state, analysis thread
  assistant/    capture session, proposal rules, the JSON protocol for the panels
  ui/           native editor (ui/native/), its headless layout logic (ui/model/), and the
                WebView host for the panels (ui/web/)
tests/          Catch2 tests, mirroring source/; tests/webview/ runs the panels in a real WebView
webui/          React panels for the WebView, and the shared contract
cmake/          CPM (dependency fetching)
```

## Status

Done and tested on Linux (x86_64, gcc 13): the DSP layers, the engine, the processor,
metering, the native editor, the assistant session and protocol, and the assistant and target
panels in the plugin window (WebKitGTK). Below 1100 px wide the panels open as a drawer from
the header's ASSISTANT button. Without a working WebView the column says why and the rest of
the plugin works as usual.

Not done yet:
- The advanced panel.
- STEMS (stem-aware processing).
- Importing references from audio files.
- CPU readout per layer.

Not verified yet: any DAW, macOS (WKWebView), Windows (WebView2), the WebView inside a
plugin host on Linux (it was run in a standalone window), pluginval, clap-validator.

## Licence

No licence has been chosen yet. Until one is, all rights are reserved.
