# Trace-To-Techno: UI Design (SoundSynth plugin)

This document is the spec, current implementation status, and handoff
notes for the UI side of the Trace-To-Techno project. It describes the
two-canvas graph-editor architecture, the state model, preset import
semantics, the hard-earned gotchas, and the milestone plan.

For the TP/engine side, see [trace-processor-design.md](trace-processor-design.md).
For the conceptual background on modular synths, see
[background-on-synths.md](background-on-synths.md).

## Status

**Milestone 1 is complete.** The two-canvas editor is functional with:
- Preset picker (256 presets across drum / bass / lead / pad / fx /
  strings / organ).
- Rack canvas with trace source, instrument and master nodes, context
  menus, drag to move, Delete to remove, click+drag wiring between
  ports.
- Instrument editor canvas with virtual INPUT / real OUTPUT nodes,
  inline parameter editing for 8 core block types (full panels) and
  read-only display for the other 10 (generic fallback).
- Auto-layout on preset import + when switching instruments, powered
  by `NodeGraph`'s built-in `autoLayout` API with real DOM-measured
  dimensions.
- Single-stream audio Transport with a generation counter so rapid
  Test-button clicks never stack sounds on top of each other.

Milestones 2 and 3 have concrete plans below; the plumbing they need
(textproto round-trip via WASM, `NodeGraphApi`, `BlockDescriptor`
registry) is already in place.

## Big picture

The SoundSynth plugin page is **two node-graph canvases stacked
vertically**, with a left sidebar listing the trace's tracks and a
bottom transport bar:

```
┌────────────────────────────────────────────────────────────────┐
│  Sound Synth · 256 presets loaded                              │
├──────────────────┬─────────────────────────────────────────────┤
│                  │  RACK · [+ Instrument]  [+ Trace Source]    │
│                  │                                             │
│  Track Browser   │  Trace sources, instrument nodes, master.   │
│  (process tree)  │  Macro wiring only.                         │
│                  │                                             │
│  Click a track   ├─────────────────────────────────────────────┤
│  to add it as a  │  INSTRUMENT EDITOR · [+ Add Block] [▶ Test] │
│  rack trace      │                                             │
│  source.         │  Full internal patch of the selected        │
│                  │  instrument, with virtual INPUT/OUTPUT      │
│                  │  nodes and every SynthModule as its own     │
│                  │  node. Empty state when nothing selected.   │
├──────────────────┴─────────────────────────────────────────────┤
│  Transport: [▶ Render]  [▶ Play / ⏹ Stop]                      │
└────────────────────────────────────────────────────────────────┘
```

- **Top canvas (Rack)** — macro view. Each instrument is a single node
  regardless of internal complexity. Only trace sources, instrument
  heads, and the rack master are visible.
- **Bottom canvas (Instrument Editor)** — micro view. The internal
  patch of the currently-edited instrument, with every `SynthModule`
  rendered as its own node. Hidden (empty state) when nothing is
  selected.

Both canvases mutate the same underlying `SynthesizeAudioArgs` proto.
Logically it is ONE graph — the two canvases are just different
projections of the same flat module/wire list.

## State model

### Single source of truth

The entire UI state is a `protos.ISynthesizeAudioArgs` object. Every
reactive bit of the UI renders directly from it. UI-only state (node
positions, display names, mute/solo, the currently-edited-instrument
id) is stashed into the opaque `ui_state_json` string fields of:

- `SynthPatch.ui_state_json` — page-level state
- `SynthModule.ui_state_json` — per-module state

The JSON blobs are validated and typed by Zod schemas in
`patch_state.ts` (`PatchUiStateSchema`, `ModuleUiStateSchema`).

### Instrument grouping: ID prefixing

Since the proto is flat, we use a naming convention to group modules
into instruments:

- Each instrument has an `instrumentId` of the form `inst_<base36>`.
- Every module belonging to that instrument has an ID of the form
  `${instrumentId}__${localName}`.
- The **instrument root** is the module whose `ui_state_json.nodeKind`
  is `instrument_root`. In practice this is the preset's internal
  `master` mixer, renamed to `${instrumentId}__master` on import. Its
  `ui_state_json` stores `displayName`, `presetId`, `muted`, `soloed`,
  `level`, `x`/`y` (rack position), `outX`/`outY` (position of the
  OUTPUT node inside the instrument editor — **separate** from x/y to
  avoid crosstalk between the two canvases), `gateSource` and
  `freqSource` (rack-level module IDs bound to the instrument's
  virtual inputs).
- The rack-level wire from the instrument root to the rack master
  mixer is a real `SynthWire` entry.

### Virtual INPUT node

Inside an instrument, there is no proto module representing the
"input from the rack". Instead, wires that should receive the
external signal use **reserved virtual module IDs**:

- `__input__gate` — the gate/trigger signal from the rack
- `__input__freq` — the pitch CV from the rack

These IDs never appear on real modules. In the bottom canvas the UI
synthesizes a virtual INPUT node at the top-left with output ports
`gate` and `freq`; wires drawn from these ports are stored as real
`SynthWire` entries with `from_module: "__input__gate"` (or
`__input__freq`) and `from_port: "out"`. The gate-vs-freq distinction
is encoded in the virtual ID itself.

When a patch is actually sent to TP (either for rack rendering or for
the Test button), `buildRenderPatch()` / `buildTestPatch()` walk all
wires and rewrite virtual `from_module` IDs:

- **Rack render**: `__input__gate` → `<instrument.gateSource>`; the
  wire is dropped entirely if no source is bound. Same for
  `__input__freq`.
- **Test render**: `__input__gate` → the temporary `TestPatternSource`
  module ID (port `out`); `__input__freq` → the same test source
  (port `freq`).

TP never sees the virtual IDs.

### Instrument OUTPUT node

The instrument root (`${instrumentId}__master`) is a real Mixer
module. It IS the "output" of the instrument. In the bottom canvas we
render it with a distinctive style and label it "OUTPUT". In the top
canvas the same underlying module is rendered as the instrument node.

**The two canvases must use different position fields** for the same
module, otherwise dragging OUTPUT in the bottom canvas teleports the
instrument on the rack:

- `ui_state_json.x`, `.y` → rack position (instrument node)
- `ui_state_json.outX`, `.outY` → instrument editor position (OUTPUT
  node)

This separation is enforced in `buildOutputNode()` and the
`onNodeMove` handler in `instrument_canvas.ts`, and in the auto-layout
helper in `patch_state.ts`.

### Preset import semantics

When the user picks a preset:

1. Parse the preset's JSON patch, converting snake_case field names to
   camelCase via `snakeToCamelDeep()` (protobufjs expects camelCase).
2. Generate a fresh `instrumentId`.
3. **Strip the `TestPatternSource` module(s)** — they exist only to
   make the preset self-contained for standalone rendering during
   preset generation. Record the stripped module's local ID (usually
   `"arp"`).
4. Walk all wires and rewrite any `from_module` that referenced a
   stripped test source:
   - `from_port: out` → `from_module: "__input__gate"`, `from_port:
     out`
   - `from_port: freq` → `from_module: "__input__freq"`, `from_port:
     out`
5. Prefix every remaining module's ID with `${instrumentId}__`.
6. Rewrite all non-virtual wire endpoints to use the prefixed names.
7. Deep-clone each module via `toObject`/`fromObject` (NOT
   `SynthModule.create`, which does a shallow copy and can alias
   nested config).
8. Mark the former preset `master` mixer (now
   `${instrumentId}__master`) as the instrument root via
   `ui_state_json.nodeKind = "instrument_root"` with display name,
   preset id, default level, etc.
9. Append all new modules and wires to the `SynthPatch`.
10. Add a rack-level wire `${instrumentId}__master → master` (rack
    master).
11. Run `layoutInstrumentModules()` — a BFS-depth column layout that
    assigns initial positions. This is a rough pass; after the
    NodeGraph mounts the instrument canvas triggers the built-in
    `autoLayout` which measures actual DOM node sizes and lays them
    out properly.

After import, the instrument is fully functional for the Test button.
To hear it in the rack render, the user must wire a rack-level trace
source into its gate (and optionally freq) input.

### Test button semantics

When the user clicks Test on an instrument:

1. Build a new `SynthPatch` containing:
   - A fresh `TestPatternSource` (mode: ARPEGGIO, bpm: 128, bars: 4)
     — `buildTestPatch()` in `patch_state.ts`.
   - The instrument's modules, with virtual `__input__gate` /
     `__input__freq` references rewritten to point at this test
     source's `out` / `freq` ports.
   - A fresh rack `master` Mixer receiving the instrument root's
     output.
2. Send the patch to TP via `synthesizeAudio()` with a small time
   window (16 × 1/48 seconds of trace time, stretched by the engine's
   48× time dilation to 16 seconds of audio).
3. Decode the returned WAV and play it via WebAudio.

The test patch is built on the fly and never persisted.

## Playback architecture

The Transport component enforces a **single-stream invariant**: at
most one audio source plays at any time.

```
┌─ Transport state ─────────────────────────────┐
│ audioCtx: AudioContext | null                 │
│ sourceNode: AudioBufferSourceNode | null      │
│ playing: boolean                              │
│ lastAutoPlayedBuf: ArrayBuffer | null         │
│ playbackGeneration: number  // monotonic      │
└───────────────────────────────────────────────┘
```

Key invariants:

1. Every `startPlayback()` and `stopPlayback()` call increments
   `playbackGeneration`. An in-flight `decodeAudioData` await that
   resumes with `gen !== playbackGeneration` silently bails out. This
   prevents two concurrent Test clicks from both ending up starting a
   sound.
2. `stopPlayback()` nulls `onended` **before** calling `.stop()`, so
   stale onended callbacks don't clear newer state. It also wraps
   `.stop()` and `.disconnect()` in try/catch (OK if already stopped).
3. When `wavData` transitions from non-null to null (new render
   begins), the Transport view calls `stopPlayback()` so the previous
   sound dies before the new one arrives.
4. The Play/Stop button stays visible whenever `this.playing` is
   true, **even if `wavData` was cleared during a re-render**, so the
   user always has a way to kill the current sound.

## Block descriptor registry

All synth block types are described in a central registry
(`block_registry.ts`):

```typescript
interface BlockDescriptor {
  protoField: string;                      // e.g. "classic_osc"
  displayName: string;                     // e.g. "Classic Osc"
  description: string;
  category: 'source' | 'oscillator' | 'filter'
          | 'effect'  | 'modulator'  | 'utility';
  hue: number;                             // Node color, 0-360
  inputs:  Array<{name: string; kind: PortKind}>;
  outputs: Array<{name: string; kind: PortKind}>;
  createDefault: () => protos.ISynthModule;
  renderParams: (mod: protos.ISynthModule, onChange: () => void)
              => m.Children;
}
```

`PortKind` = `'audio' | 'cv' | 'gate' | 'freq'` — informational only
today, but can be used to color wires by signal type later.

Every block type in the proto has an entry. Milestone 1 ships **full
interactive panels** for 8 blocks:

- `TestPatternSource`, `ClassicOsc`, `Adsr`, `MoogLadder`, `Svf`,
  `Waveshaper`, `Vca`, `Mixer`

Every other block has a **generic read-only fallback panel** built
from proto field introspection, which Milestone 2 will replace with
proper sliders/dropdowns.

## Rack canvas (top)

Nodes:

- **Trace source** — one per `TraceSliceSource` at the rack level.
  Output port `out`. Inline controls for glob and signal type. Hue
  140 (green). Right-click 3-dot menu → "Delete trace source"; or
  click-select + Delete key.
- **Instrument** — one per `instrument_root` module. Input ports:
  `gate`, `freq`. Output port `out`. Body:
  - Preset category chip (colored by category)
  - Mute / Solo toggle buttons
  - Level slider
  - Signal-chain preview string (e.g. "ClassicOsc → Moog → VCA")
  - **Test** button (green)
  - **Edit** button (loads the instrument into the bottom canvas)
  Context menu: "Delete instrument"; or click-select + Delete key.
- **Master** — the rack output mixer. Input port `in`. Fixed
  position, cannot be removed.

Connections on the rack:

- Trace source `out` → Instrument `gate` / `freq` — stored in the
  instrument root's `ui_state_json.gateSource` / `freqSource`, NOT as
  a real `SynthWire`. The wire is synthesized by `buildRenderPatch`
  at render time.
- Instrument root `out` → Master `in` — stored as a real `SynthWire`.

Interactions:

- Drag a node to reposition (persists in `ui_state_json.x/y`).
- Drag from a trace source output to an instrument's gate/freq input
  to bind. Drag from instrument to master to wire to the rack mix.
- Click a node to select it (enables Delete key).
- Click an instrument's **Edit** button to open the instrument editor
  below. The bottom canvas replaces its empty-state placeholder with
  that instrument's internal patch.

## Instrument Editor canvas (bottom)

Only visible when an instrument is selected.

Nodes:

- **INPUT** (virtual) — fixed at top-left. Output ports `gate`,
  `freq`. Not backed by a proto module; wires drawn from it use the
  reserved `__input__gate` / `__input__freq` IDs.
- Every internal module of the instrument, rendered using its
  `BlockDescriptor`. Inline parameter panels for the 8 "full panel"
  blocks, generic fallback for the rest. Click a node to select,
  press Delete (or use the 3-dot menu) to remove it.
- **OUTPUT** — the instrument root Mixer, rendered with a distinct
  style. Input port `in`. Real proto module. Its position is stored
  in `ui_state_json.outX/outY` (NOT `x/y`, which is reserved for the
  rack position of the same module).

Interactions:

- Drag nodes (persists to the module's `ui_state_json`).
- Connect ports by dragging from output to input. Adds a
  `SynthWire`. Port names on each end are resolved via the
  `BlockDescriptor`.
- Context menu on a wire → delete.
- Toolbar: **+ Add Block** (palette of all 18 block types grouped by
  category) · **▶ Test** (ephemeral preview render) · **Close**.

Auto-layout: when the instrument first mounts, the bottom canvas
triggers `NodeGraphApi.autoLayout()` via an `onReady` callback, gated
by a `pendingAutoLayout` flag. This uses real DOM node dimensions so
the layout avoids overlaps. It's followed by `recenter()` to fit the
graph into the canvas viewport.

## Preset library

### Storage and build integration

The preset library is a single JSON file:
`test/data/music_synth_presets.json` (256 presets, generated by
`tools/trace_to_techno/gen_presets.py`).

Because the UI build walks `ui/src/assets/` but **skips symbolic
links**, the JSON file is *duplicated* to
`ui/src/assets/sound_synth/music_synth_presets.json`. The Python
generator writes both copies. There's a `{r: /ui\/src\/assets\/(sound_synth\/(.*)[.]json)/, f: copyAssets}`
rule in `ui/build.js` that copies the JSON into the dist directory at
build time.

If you modify `gen_presets.py` and regenerate, both copies are
updated atomically.

### Loading

`preset_library.ts` fetches the JSON once on page load via `fetch()`,
runs `snakeToCamelDeep()` recursively on each preset's patch (only
keys are rewritten; enum string values like `"ARPEGGIO"` are left
alone), and then calls `protos.SynthPatch.fromObject(camelPatch)`.

The result is exposed as a lazy, categorized, searchable
`PresetLibrary` object with methods `all()`, `byCategory()`,
`categories()`, `search(query)`, and `findByName(name)`.

### Picker UI

`preset_picker.ts` is a Mithril modal-ish component overlaid on the
main page. Category tabs along the top (All + per-category),
full-text search box, scrollable list of entries with category
indicator, description, and click-to-insert. Clicking an entry calls
`importPresetAsInstrument()` and closes the picker.

## Gotchas (the hard-won lessons)

If you're new to this codebase, save yourself the debugging hours by
reading these before writing code:

### 1. The plugin must be in the default-enabled list

Creating a plugin under `ui/src/plugins/<name>/` registers it but
does **not** enable it at startup. To make it load by default (so
`onTraceLoad` fires and your page/sidebar entries appear), add the
plugin ID to `ui/src/core/embedder/default_plugins.ts`. Otherwise
users have to toggle it on via the Plugins page every time.

### 2. NodeGraph's `.pf-canvas` width collapses inside flex containers

The NodeGraph widget renders as a plain block `.pf-canvas` div with
`height: 100%` (when `fillHeight: true` is set). When this div is
placed inside a `display: flex` parent, it behaves as a flex item and
its width shrinks to content — which for an empty canvas is 0 — and
the whole graph becomes invisible.

The fix is to wrap the NodeGraph in a `position: relative` parent
with `overflow: hidden`, then an absolutely-positioned
`top/left/right/bottom: 0` inner div that contains the NodeGraph.
This decouples the NodeGraph's size from flex layout.

See the `.rack-canvas-wrapper` / `.rack-canvas-inner` pattern (and
the same for instrument canvas) in `sound_synth_page.ts` and
`instrument_canvas.ts`.

### 3. Never call `Math.random()` in `view()` for node positions

This causes a redraw loop: each render passes a new random (x, y) to
the NodeGraph → NodeGraph fires `onNodeMove` → you persist the new
position → mithril redraws → new random again → forever.

Use `ui.x ?? defaultX` (NOT `ui.x || defaultX` — `0` is a valid
position) for fallbacks, and ensure the fallback is deterministic.

### 4. The shared instrument root has two positions

The instrument's master mixer module is rendered in TWO canvases:

- On the rack, as the instrument node (uses `ui.x`, `ui.y`)
- In the instrument editor, as the OUTPUT node (uses `ui.outX`,
  `ui.outY`)

If you read/write the same field from both canvases, moving OUTPUT in
the bottom canvas will teleport the instrument on the rack.

### 5. Deep-clone proto messages, don't shallow-copy

`protos.SynthModule.create(m)` does a shallow copy — the cloned
message shares nested config objects with the original. If you then
edit the clone's config (e.g. change the `baseFreqHz` of its
`classic_osc`), you'll also mutate the preset library entry.

Use `fromObject(toObject(m))` for a true deep clone.

### 6. Mithril auto-redraws after DOM event handlers

You generally don't need to call `m.redraw()` inside `onclick`,
`oninput`, etc. Mithril batches a redraw after the handler returns.
Explicit `m.redraw()` calls in those paths are redundant and can
obscure control flow. **Do** call it from async callbacks (setTimeout,
fetch, etc.) that fire outside the mithril event loop.

### 7. NodeGraph's `contextMenuItems` is a header button, not right-click

The NodeGraph widget renders a 3-dot "more_vert" button in each
node's title bar that opens a `PopupMenu` with the attached
`contextMenuItems`. It is **not** a real right-click handler. Users
open it via left-click on that button. (This is actually fine; we
also support Delete/Backspace on selected nodes.)

### 8. Plugins in `ui/src/assets/` must be real files, not symlinks

The dev server's `scanDir` / `walk` explicitly skips symbolic links
(`!stat.isSymbolicLink()`). If you link from `ui/src/assets/*` into
another directory, the file will never be copied into the dist and
the UI will fail to fetch it. Either duplicate the file or teach the
build script a per-file copy rule for that path.

## Code layout

After Milestone 1 the plugin looks like:

```
ui/src/plugins/dev.perfetto.SoundSynth/
  index.ts                # Plugin registration
  sound_synth_page.ts     # Top-level page: split layout, orchestration,
                          # Test button wiring, Render wiring
  rack_canvas.ts          # Top canvas (rack level)
  instrument_canvas.ts    # Bottom canvas (instrument internals)
  block_registry.ts       # BlockDescriptor[] for all 18 block types
  patch_state.ts          # Proto view, mutations, import/export,
                          # auto-layout helper
  preset_library.ts       # Fetch + parse music_synth_presets.json
  preset_picker.ts        # Preset picker modal
  track_browser.ts        # Left sidebar: process/track tree
  transport.ts            # Render + single-stream playback
```

Build integration:

- `ui/src/core/embedder/default_plugins.ts` — plugin enabled by default
- `ui/build.js` — `{r: /ui\/src\/assets\/(sound_synth\/.*[.]json)/, f: copyAssets}`
  rule for the preset JSON
- `ui/src/assets/sound_synth/music_synth_presets.json` — real copy of
  the preset library (kept in sync with `test/data/...` by
  `tools/trace_to_techno/gen_presets.py`)

## Reserved identifiers

- `__input__gate`, `__input__freq` — virtual module IDs for the
  instrument INPUT node. Never used for real modules.
- `__canvas_input__`, `__canvas_output__` — virtual canvas node IDs
  used only inside `instrument_canvas.ts` (not stored in the proto).
- Instrument IDs: `inst_<base36>`. Derived from `Date.now()` +
  counter.
- Instrument internal module IDs: `${instrumentId}__${localName}`.
- The rack master mixer ID is the literal string `master`.

## Milestone 1 — Foundation (complete)

Done:

- Design doc (this file).
- `patch_state.ts`: instrument grouping, virtual INPUT, preset
  import, test patch builder, rack render patch builder, layout
  helper with auto-layout on import.
- `block_registry.ts` with descriptors for all 18 block types.
- Full inline parameter panels for the 8 "core" blocks:
  `TestPatternSource`, `ClassicOsc`, `Adsr`, `MoogLadder`, `Svf`,
  `Waveshaper`, `Vca`, `Mixer`. Generic fallback for the rest.
- `preset_library.ts`: JSON fetch, snake→camel, `TestPatternSource`
  stripping, wire rewriting, categorized searchable API.
- `preset_picker.ts`: modal picker with category tabs and search.
- `rack_canvas.ts`: top canvas with trace source / instrument /
  master nodes, drag, wire, context menu delete, Delete key.
- `instrument_canvas.ts`: bottom canvas with virtual INPUT /
  real-module OUTPUT, inline param editing, block palette (+ Add
  Block), auto-layout via `NodeGraphApi`, independent OUTPUT position
  via `outX/outY`.
- `sound_synth_page.ts`: two-canvas split layout with proper flex /
  position:absolute chaining so `.pf-canvas` gets full width and
  height.
- `transport.ts`: single-stream audio playback with generation
  counter, auto-stop on render start, Stop button always visible
  while playing.

## Milestone 2 — Block panels and polish

Goal: every block fully editable with a well-designed param panel,
and a more polished look/feel.

### Tasks

1. **Full `renderParams` panels** for the 10 remaining block types
   (all currently using the generic read-only fallback):
   `NoiseOsc`, `FmOsc`, `PhaseDistortionOsc`, `FoldOsc`, `SyncOsc`,
   `SuperOsc`, `WavetableOsc`, `DrawbarOrgan`, `Lfo`, `Chorus`,
   `Delay`. (Plus the legacy `Vco` and `Envelope` — bare minimum is
   fine there since they're only kept for back-compat.)
2. **Visual polish**:
   - Distinct colors for wires by signal type (audio / cv / gate /
     freq) — the `PortKind` metadata is already collected.
   - Port labels styled consistently on both sides of nodes.
   - Collapsible nodes (show header only). Store the collapsed flag
     in `ui_state_json.collapsed`.
3. **Node inspector side panel** (alternative to inline editing):
   select a node → a right-hand panel shows the full param set with
   more room than the inline inline panel allows. Keep inline
   editing for the most common fields; move the rest to the
   inspector.
4. **Block palette UX**: searchable palette with keyboard shortcuts
   for the most common blocks. Drag-to-canvas to place at cursor
   position.
5. **Copy/paste modules inside an instrument** via Ctrl+C / Ctrl+V
   on a selected node. Cloning via `toObject`/`fromObject`.
6. **Better mute/solo visual feedback** — red/yellow overlays on the
   rack instrument node, not just button state.
7. **Empty-state art** for "no instrument selected" (a nice icon, a
   hint about clicking + Instrument, etc.).

### Pointers for the agent taking this on

- The `BlockDescriptor` registry already has stubs for every block —
  just fill in `renderParams` with properly sized sliders and
  dropdowns. Look at the proto field definitions in
  `protos/perfetto/trace_processor/synth.proto` (enum values,
  numeric ranges in comments) for guidance. Reuse the `slider()` and
  `dropdown()` helpers at the top of `block_registry.ts`.
- Port kinds (`audio` / `cv` / `gate` / `freq`) are already stored
  but not visually distinguished. This is low-hanging fruit for color
  coding.
- For collapsible nodes, store the collapsed state in
  `ui_state_json.collapsed` (bool, defaulting to false).
- For the inspector side panel, look at how DataExplorer's
  `node_panel.ts` is structured — it's a good model.
- Don't add random fallbacks in `view()`. Use `??`, not `||`. See
  Gotcha #3.

## Milestone 3 — Advanced features

Goal: turn the editor into a real playground with file sync, custom
presets, trace-aware rhythm, and undo/redo.

### Tasks

1. **File sync via textproto**. The WASM infrastructure is already
   done (`synthArgsToText` / `synthArgsToPb` in
   `ui/src/base/proto_utils_wasm.ts`). Build a `FileSyncManager`:
   - Uses `FileSystemFileHandle` via the File System Access API.
   - Debounced save (3s max, via `AsyncGuard` from
     `ui/src/base/async_guard.ts`).
   - Polls the file handle for external changes.
   - Conflict detection via content hash; conflict dialog with
     "Download both / Reload file / Overwrite file" options.
2. **User preset saving**. After sculpting an instrument, the user
   clicks "Save as preset" and a named entry is added to
   `localStorage`. These appear alongside built-in presets in the
   picker (maybe with a 👤 badge).
3. **Preset favorites**. ★ icon per preset; filter by starred only.
4. **BPM-derived clock**. Derive the master clock from trace vsync
   markers (or another configurable event source) so the rhythm of
   the playback matches the cadence of the trace. Requires a
   TP-side clock source block too.
5. **Rack-level effects**. Add an "FX chain" slot between each
   instrument and the master, and a global FX chain on the master.
6. **Undo/redo**. Snapshot-based: clone the entire
   `SynthesizeAudioArgs` proto on every significant mutation, stack
   up to ~50 snapshots. Revert by swapping the active snapshot.
7. **Performance tuning** for large patches (100+ modules) — diff
   only the visible canvas, throttle redraws where sensible.

### Pointers for the agent taking this on

- For file sync, read `src/base/async_guard.ts` for the debounce
  primitive. The canonical conflict-resolution UX is: show a modal
  with three buttons and two "Download" links so the user can diff
  externally.
- For user presets, lean on `localStorage.getItem('soundsynth_user_presets')`
  storing a JSON array in the same shape as the built-in preset
  file. Use `preset_library.ts` as the model for parsing.
- For BPM derivation, start with a constant fallback of 128 BPM if
  no source is set. The actual clock logic lives in TP; the UI just
  picks the source.
- For undo/redo, the proto can be deep-cloned via
  `protos.SynthesizeAudioArgs.encode(state).finish()` then
  `.decode(bytes)` for a guaranteed-independent copy. That's slower
  than `toObject`/`fromObject` but the proto is small (~KB).
