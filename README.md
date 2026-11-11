# STRAT3

A mastering plugin: one chain of processing layers, each with its own depth, a live view of
what every layer changes, delivery metering, and an assistant that proposes moves from what
it hears and explains each one.

VST3, AU, CLAP and Standalone, built with JUCE 8 and C++20. The main editor is native JUCE;
the assistant, target and advanced panels are React pages in a WebView (`webui/`).

**Status: in development (version 0.1.0).** Not yet tested in a DAW, with pluginval or with
clap-validator. See [Status](#status).

## The chain

```
IN TRIM -> STEMS -> EQ -> LOW -> MB -> WIDTH -> SAT -> LIMIT -> OUT TRIM -> monitor -> CEILING
```

- **STEMS**: gain and tilt per stem (vocals, bass, drums, other), from masks an analysis pass
  made beforehand. MIX and MASTER only. See [STEMS](#stems).

- **EQ**: 8 bands, static or dynamic, stereo or mid / side. Linear phase (FIR) in MASTER
  mode, IIR in TRACK and MIX.
- **LOW**: below a set crossover only: punch / sustain shaping of the low band, and mono bass.
- **MB**: multiband dynamics, downward and upward compression per band.
- **WIDTH**: stereo width per band, and a stereoizer that leaves the mono fold-down unchanged.
- **SAT**: saturation per band, oversampled 1x / 2x / 4x.
- **LIMIT**: look-ahead limiter, three styles (Clean, Dense, Clip).
- **CEILING**: true-peak safety limiter and dither. Always last, also on the monitor signals.

STEMS always comes first. The six depth layers after it (EQ to LIMIT) can be reordered.
Neighbouring band layers (MB, WIDTH, SAT) share one band split. Each layer blends between its input and its output with its own
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
analysis stay as read-only history. Dragging a move's depth is one undo step in the host.

The target is your own: play reference tracks through the plugin and capture them (up to
16). Tonal moves need a target; the loudness and bass moves do not.

## STEMS

STEMS needs an analysis of the song first: arm it (the panel's Analyse button, not built yet),
play the song (or the part you want), stop.
The pass follows the host's timeline: it starts when playback starts and ends when playback
stops, jumps or loops. The separator needs no trained model: it splits by DSP (a percussive /
harmonic split, a bass band, and the centre of the mid band), so its "vocals" are any
sustained, centred mid-band sound. The result is looked up by timeline position, so seeks and
loops find the right place. If the mix changes after the analysis, STEMS notices within about
30 ms and steps aside until the audio matches again.

The analysis is saved as a compressed file in a per-user folder (macOS
`~/Library/Application Support/STRAT3/Stems`, Windows `%LOCALAPPDATA%\STRAT3\Stems`, Linux
`~/.local/share/STRAT3/Stems`): up to about 46 MB per 4 minutes at 48 kHz before
compression. The project keeps only its name. A project opened on a machine without the file plays without STEMS until it is
analysed again. One pass holds up to about 11 minutes at 48 kHz. Files are not deleted
automatically.

STEMS adds 1024 samples (21 ms at 48 kHz) of latency in MIX and MASTER, on or off, so the
host's delay compensation never moves. TRACK has no STEMS.

## Advanced

The gear in the header opens the advanced panel: the three crossover points, SATURATE's
oversampling, dither type and bits, and MULTIBAND's sidechain source and high-pass. They are
ordinary plugin parameters, so the host automates and undoes them as usual. The panel also
says what the engine runs with when that differs from the settings: the crossover type
(Linkwitz-Riley in TRACK and MIX, linear phase in MASTER), the points after the engine sorts
and spaces them, oversampling capped at high sample rates, and an external sidechain the host
has not connected.

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
the panels and moving parameters from the advanced panel. When `xvfb-run` is installed (package `xvfb`) ctest runs it under that, so no
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
metering, the native editor, the assistant session and protocol, and the assistant, target and
advanced panels in the plugin window (WebKitGTK). Below 1100 px wide the panels open as a
drawer from the header's ASSISTANT button (or the gear, for the advanced panel). Without a working WebView the column says why and the rest of
the plugin works as usual.

Not done yet:
- The STEMS panel: the STEMS row with its four stems, the Analyse button and its progress.
  STEMS itself (the layer, the analysis pass, the saved analysis) is in and tested, but has
  no controls in the editor yet; its parameters can be automated from the host.
- Importing references from audio files.
- CPU readout per layer.

Not verified yet: any DAW, macOS (WKWebView), Windows (WebView2), the WebView inside a
plugin host on Linux (it was run in a standalone window), pluginval, clap-validator.

## Licence

No licence has been chosen yet. Until one is, all rights are reserved.
