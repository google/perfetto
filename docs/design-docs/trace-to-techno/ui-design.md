# Trace-To-Techno: UI Design (SoundSynth plugin)

This document is the spec and handoff notes for the UI side of the
Trace-To-Techno project. It describes the two-canvas graph-editor
architecture, the state model, the preset import semantics, and the
milestone plan.

For the TP/engine side, see [trace-processor-design.md](trace-processor-design.md).
For the conceptual background on modular synths, see
[background-on-synths.md](background-on-synths.md).

## Big picture

The SoundSynth plugin page is **two node-graph canvases stacked vertically**:

```
┌─────────────────────────────────────────────────────────────┐
│  Track Browser (left) │  RACK CANVAS (top)                  │
│                       │                                     │
│  Process tree         │  Trace sources, instrument cards,   │
│  Click to add source  │  master output. Macro wiring.       │
│                       │                                     │
│                       ├─────────────────────────────────────┤
│                       │  INSTRUMENT EDITOR (bottom)         │
│                       │                                     │
│                       │  Internal synth graph of the        │
│                       │  currently-edited instrument.       │
│                       │  Empty state when nothing selected. │
├───────────────────────┴─────────────────────────────────────┤
│  Transport: [▶ Render] [Play]                               │
└─────────────────────────────────────────────────────────────┘
```

- **Top canvas (Rack)**: macro view. Trace sources → instrument nodes →
  master. Each instrument is a single node regardless of internal
  complexity.
- **Bottom canvas (Instrument Editor)**: micro view. The internal patch of
  the currently-edited instrument, with every `SynthModule` rendered as its
  own node. Only visible when an instrument is selected; replaced by an
  empty-state message otherwise.

Both canvases mutate the same underlying `SynthesizeAudioArgs` proto.
Logically it is ONE graph — the two canvases are just different projections
of the same underlying flat module/wire list.

## Why two canvases?

The flat proto has no built-in notion of "instrument". A typical preset
has 7-10 modules (oscillator, envelopes, filter, drive, VCA, master).
With 5 instruments on the rack that's 40-50 nodes on one canvas, which
quickly becomes unreadable.

The two-layer approach mirrors how hardware modular rigs are used in
practice: a patch-bay layer at the rack level, and internal voices that
you only peek inside of when you need to tweak.

It also lines up perfectly with the preset library: each preset is one
"voice" worth of modules, so loading a preset corresponds exactly to
dropping an instrument node onto the rack.

## State model

### Single source of truth

The entire UI state is a `protos.ISynthesizeAudioArgs` object. Everything
reactive renders directly from it. UI-only state (node positions, display
names, mute/solo, the currently-edited-instrument id) is stashed into the
`ui_state_json` string fields of:

- `SynthPatch.ui_state_json` — page-level state
- `SynthModule.ui_state_json` — per-module state

The JSON blobs are validated and typed by Zod schemas in
`patch_state.ts`.

### Instrument grouping: ID prefixing

Since the proto is flat, we use a naming convention to group modules into
instruments.

- Each instrument has an `instrumentId` of the form `inst_<random>`.
- Every module belonging to that instrument has an ID of the form
  `${instrumentId}__${localName}`.
- The **instrument root** is the module whose `ui_state_json.nodeKind` is
  `instrument_root`. In practice this is the internal `master` mixer
  (renamed to `${instrumentId}__master` on import). Its `ui_state_json`
  stores: `displayName`, `presetId`, `muted`, `soloed`, `level`, `x`,
  `y` (rack position), `gateSource` (module id of the rack-level gate
  source), `freqSource` (module id of the rack-level freq source).
- The rack-level wire from the instrument root to the rack master mixer
  is a real `SynthWire`.

### Virtual INPUT node

Inside an instrument, there is no proto module representing the "input
from the rack". Instead, wires that should receive the external signal
use **reserved virtual module IDs**:

- `__input__gate` — the gate/trigger signal from the rack
- `__input__freq` — the pitch CV from the rack

These IDs never appear on real modules. When the UI renders the instrument
editor canvas, it synthesizes a virtual INPUT node at the top-left with
output ports `gate` and `freq`. Wires drawn from these ports get stored as
real `SynthWire` entries with `from_module: "__input__gate"` (or `freq`).

When a patch is actually sent to TP (either for rack rendering or for the
Test button), `buildRenderPatch()` walks all wires and rewrites virtual
`from_module` IDs:

- **Rack render**: `__input__gate` → `<instrument.gateSource>`, or the
  whole wire is dropped if no source is bound. Same for `__input__freq`.
- **Test render**: `__input__gate` → the temporary `TestPatternSource`
  module ID. `__input__freq` → the same `TestPatternSource`, using its
  `freq` output port.

TP never sees the virtual IDs.

### Instrument OUTPUT node

The instrument root (`${instrumentId}__master`) is a real module — a
Mixer. It IS the "output" of the instrument. In the bottom canvas we
render it with a distinctive style and label it "OUTPUT", but it's a
normal node that can be selected and (for consistency) show its inputs
(which are the final stages of the synth chain).

### Preset import semantics

When the user loads a preset from the library:

1. Parse the preset's JSON patch, converting snake_case field names to
   camelCase (protobufjs expects camelCase).
2. Generate a fresh `instrumentId`.
3. **Strip the `TestPatternSource` module(s)** — they exist only to make
   the preset self-contained for rendering during generation. Record the
   stripped module's local ID (usually `"arp"`).
4. Walk all wires and rewrite:
   - Any `from_module` referencing the stripped test source with port
     `out` → `from_module: "__input__gate"` (same port kept)
   - Any `from_module` referencing the stripped test source with port
     `freq` → `from_module: "__input__freq"`
5. Prefix every remaining module's ID with `${instrumentId}__`.
6. Rewrite all wire endpoints (except virtual INPUT IDs) to match the
   prefixed names.
7. Mark the former internal `master` (now
   `${instrumentId}__master`) as the instrument root via
   `ui_state_json.nodeKind = "instrument_root"` with display name,
   preset id, etc.
8. Append all new modules and wires to the `SynthPatch`.
9. Add a rack-level wire
   `${instrumentId}__master → <rack master mixer>.in`.

After import the instrument is fully functional for the Test button. To
hear it in the real rack mix, the user must wire a rack-level trace
source into its gate input.

### Test button semantics

When the user clicks Test on an instrument:

1. Build a new `SynthPatch` containing:
   - A fresh `TestPatternSource` (mode: ARPEGGIO, bpm: 128, bars: 8)
   - The instrument's modules, with virtual `__input__gate` /
     `__input__freq` references replaced by this test source's `out` /
     `freq` ports
   - A fresh rack `master` Mixer receiving the instrument root's output
2. Send the patch to TP via `synthesizeAudio` with `duration_seconds:
   16` (no trace required — uses the engine's preset-preview path).
3. Decode the returned WAV, play it via WebAudio.

Nothing about this patch is ever saved. It's built on the fly and
discarded after the play.

## Block descriptor registry

All synth block types are described in a central registry
(`block_registry.ts`):

```typescript
interface BlockDescriptor {
  protoField: string;                      // "classic_osc"
  displayName: string;                     // "Classic Osc"
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

`PortKind` is informational only — it can be used to color wires by
signal type (audio / cv / gate / freq) in future milestones.

Every block type in the proto has an entry. Blocks that don't ship a
fully-designed param panel in the current milestone use a generic
fallback that introspects the proto field and auto-renders a slider or
text input for every field it finds.

## Rack canvas (top)

Nodes:

- **Trace source** — one per `TraceSliceSource` at the rack level. Output
  port `out`. Inline controls for glob and signal type. Color: green.
- **Instrument** — one per `instrument_root` module. Input ports:
  `gate`, `freq`. Output port `out`. Content:
  - Editable name
  - Preset chip (category color)
  - Mute / Solo buttons
  - Level slider
  - Signal chain preview string (e.g. `ClassicOsc → Moog → VCA`)
  - **Test** button (green)
  - **Edit** button (loads into the bottom canvas)
- **Master** — the rack output mixer. Input port `in`. One instance,
  never removed.

Connections on the rack:

- Trace source output → instrument gate or freq input (stored in the
  instrument root's `ui_state_json.gateSource` / `freqSource`)
- Instrument root output → rack master input (stored as a real
  `SynthWire`)

Interactions:

- Drag nodes — updates `ui_state_json.x/y`
- Drag from trace source output to instrument input port — binds the
  source
- Drag from instrument output to master input — wires the instrument
  into the mix (this happens automatically at import but can be
  removed)
- Right-click → Delete — removes the instrument (all its internal
  modules + wires) or the trace source
- Toolbar buttons: "+ Preset ▼" (opens preset picker), "+ Trace Source"
  (creates a new blank trace source node)

## Instrument Editor canvas (bottom)

Only visible when an instrument is selected (via clicking on the
instrument's Edit button or the node itself).

Nodes:

- **INPUT** (virtual, synthetic) — fixed at top-left. Output ports
  `gate`, `freq`. Not backed by a proto module; wires drawn from it use
  the reserved `__input__gate` / `__input__freq` IDs.
- Every internal module of the instrument, rendered using its
  `BlockDescriptor`. Full inline parameter panels.
- **OUTPUT** — the instrument root (a Mixer). Fixed at bottom-right.
  Input port `in`. It's a real proto module but styled distinctively.

Interactions:

- Drag nodes — updates `ui_state_json.x/y`
- Connect ports — adds a `SynthWire`
- Context menu on a wire → Delete
- Toolbar: "+ Add Block ▼" (opens a palette of all available block
  types), "Load preset ▼" (replaces the instrument's internals with a
  preset), "Test" button, "Close" button
- Inline parameter panels on each node tied directly to the proto config

The canvas is empty-state by default with a message: "Select an
instrument on the rack above and click Edit to view its internal
patch".

## Preset library

### Storage

The preset library lives at `test/data/music_synth_presets.json`
(checked in, generated by `tools/trace_to_techno/gen_presets.py`). It
contains 256 presets across 7 categories:

| Category | Count |
|---|---|
| bass    | 80 |
| drum    | 56 |
| strings | 48 |
| organ   | 32 |
| lead    | 24 |
| pad     | 8  |
| fx      | 8  |

### Access from the UI

The file is served as a static asset (same pattern as
`trace_processor.wasm`). On page load, `preset_library.ts` fetches it
once via `fetch()`, parses the JSON, converts snake_case field names to
camelCase recursively, and exposes a searchable API:

```typescript
interface PresetLibrary {
  all(): PresetEntry[];
  byCategory(): Map<string, PresetEntry[]>;
  search(query: string): PresetEntry[];
  load(name: string): protos.ISynthPatch | null;
}

interface PresetEntry {
  name: string;
  category: string;
  description: string;
  patch: protos.ISynthPatch;  // already snake→camel converted
}
```

### Picker UI

`preset_picker.ts` is a Mithril modal-ish component:

- Category tabs across the top
- Search box
- Scrollable list of names with description tooltips
- Each entry has a "Test" button (plays the preset standalone) and an
  "Add" button (inserts into the rack)

## Milestone plan

### Milestone 1 — Two-canvas foundation (current session)

Goal: a working two-canvas editor with real preset loading, rack/instrument
round-trip, and the Test button.

- Design doc (this file)
- `patch_state.ts` refactor: instrument grouping, virtual INPUT, preset
  import, test patch builder, rack render patch builder
- `block_registry.ts`: full panels for **8 key blocks**:
  - `TestPatternSource`, `ClassicOsc`, `Adsr`, `MoogLadder`, `Svf`,
    `Waveshaper`, `Vca`, `Mixer`
  - Generic fallback for the remaining 10 blocks
- `preset_library.ts`: JSON fetch, snake→camel, TestPatternSource
  stripping, wire rewriting
- `preset_picker.ts`: categorized searchable list
- `rack_canvas.ts`: top canvas (replaces old `graph_editor.ts`)
- `instrument_canvas.ts`: bottom canvas with virtual INPUT/OUTPUT
- `sound_synth_page.ts`: two-canvas split layout
- Delete: `voice_editor.ts`, `presets.ts`

Not in scope: full param panels for the other 10 blocks, polish, undo,
file sync.

### Milestone 2 — Block panels and polish

Goal: make every block fully editable with a well-designed param panel,
and polish the overall look and feel.

- Full `renderParams` panels for the remaining 10 blocks:
  `NoiseOsc`, `Lfo`, `Delay`, `WavetableOsc`, `FmOsc`,
  `PhaseDistortionOsc`, `FoldOsc`, `SyncOsc`, `SuperOsc`, `Chorus`,
  `DrawbarOrgan`
- Visual polish: distinct colors for signal types (audio/cv/gate/freq),
  port labels styled consistently, connection curves
- Node inspector panel (alternative to inline editing — select a node,
  see its params in a dedicated side panel)
- Collapsible nodes (show header only when minimized)
- Copy/paste modules inside an instrument (keyboard shortcuts)
- Block palette with search + drag-to-canvas
- Better mute/solo visual feedback (red/yellow overlays, not just
  button state)
- Empty-state art for "no instrument selected"

Notes for the future agent:
- The BlockDescriptor registry already has stubs for every block —
  just fill in `renderParams` with well-sized sliders and dropdowns.
  Look at the proto field definitions (enum values, numeric defaults)
  for guidance.
- Port kinds (`audio` / `cv` / `gate` / `freq`) are already stored but
  not visually distinguished. This is low-hanging fruit for color
  coding.
- For collapsible nodes, store the collapsed state in
  `ui_state_json.collapsed` (bool).

### Milestone 3 — Advanced features

Goal: turn the editor into a real playground with file sync, custom
presets, and trace-aware rhythm.

- **File sync via textproto**. The WASM infrastructure is already done
  (`synthArgsToText` / `synthArgsToPb` in `ui/src/base/proto_utils_wasm.ts`).
  Build a `FileSyncManager`:
  - Uses `FileSystemFileHandle` via the File System Access API
  - Debounced save (3s max, via `AsyncGuard`) on UI mutations
  - Polls the file handle for external changes
  - Conflict detection via content hash; conflict dialog with
    "Download both / Reload file / Overwrite file" options
- **User preset saving**. After sculpting an instrument in the editor,
  the user clicks "Save as preset" and a named entry is added to
  `localStorage`. These appear alongside built-in presets in the
  picker.
- **Preset favorites**. Star icon per preset; filter by starred only.
- **BPM-derived clock**. Derive the master clock from trace vsync
  markers or the chosen time window, so the rhythm of the playback
  matches the cadence of the trace. This is a TP-side change too (new
  clock source block).
- **Rack-level effects**. Add an "FX chain" slot between each
  instrument and the master, and a global FX chain on the master
  itself.
- **Undo/redo**. Snapshot-based (clone the proto on every significant
  mutation, stack up to 50 snapshots).
- **Performance tuning** for large patches (100+ modules) — diffing
  only the visible canvas, throttling redraws.

Notes for the future agent:
- For file sync, read `src/base/async_guard.ts` for the debounce
  primitive. The canonical conflict-resolution UX is: show a modal
  with three buttons and two "Download" links so the user can diff
  externally.
- For user presets, lean on `localStorage.getItem('soundsynth_user_presets')`
  storing a JSON array in the same shape as the built-in preset file.
- For BPM derivation, start with a constant fallback of 128 BPM if no
  source is set. The real logic lives in TP; the UI just picks the
  source.

## Code layout

After Milestone 1 the plugin should look like:

```
ui/src/plugins/dev.perfetto.SoundSynth/
  index.ts                # Plugin registration
  sound_synth_page.ts     # Top-level page: split layout, orchestration
  rack_canvas.ts          # Top canvas (rack level)
  instrument_canvas.ts    # Bottom canvas (instrument internals)
  block_registry.ts       # BlockDescriptor[] for all 18 block types
  patch_state.ts          # Proto view, mutations, import/export
  preset_library.ts       # Fetch + parse music_synth_presets.json
  preset_picker.ts        # Preset picker UI
  track_browser.ts        # Unchanged: left sidebar
  transport.ts            # Unchanged: render + playback
```

## Reserved identifiers

- `__input__gate`, `__input__freq` — virtual module IDs for the
  instrument INPUT node. Never used for real modules.
- Instrument IDs: `inst_<base36>`. Derived from `Date.now()` +
  counter.
- Instrument internal module IDs: `${instrumentId}__${localName}`.
- The rack master mixer ID is the literal string `master`.
