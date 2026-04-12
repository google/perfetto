# Trace-To-Techno: TraceProcessor Implementation

This document describes the current architecture of the synth engine inside
TraceProcessor (TP). It covers the code layout, the module system, the
block catalog, the render pipeline, the preset format, and CLI usage.

For the conceptual background on modular synths, see
[background-on-synths.md](background-on-synths.md). For the full spec
including block algorithm notes and the trace→instrument mapping plan, see
[SPEC.md](SPEC.md).

## Code layout

```
protos/perfetto/trace_processor/
  synth.proto                  # Synth patch config + RPC args.

src/trace_processor/sound_synth/
  synth_module.h/cc            # Base class for all synth blocks.
  synth_engine.h/cc            # Top-level engine: parses config, wires
                               # blocks, renders WAV.
  sources.h/cc                 # TraceSliceSource, TestPatternSource.
  utility.h/cc                 # Vca, Mixer.
  oscillators.h/cc             # ClassicOsc, NoiseOsc, WavetableOsc, FmOsc,
                               # PhaseDistortionOsc, FoldOsc, SyncOsc,
                               # SuperOsc, + legacy Vco.
  filters.h/cc                 # MoogLadder, Svf.
  effects.h/cc                 # Waveshaper, Delay.
  modulators.h/cc              # Adsr, Lfo, + legacy Envelope.
  synth_engine_unittest.cc     # Unit tests for every block + engine.
  BUILD.gn

src/trace_processor/rpc/
  rpc.h/cc                     # TPM_SYNTHESIZE_AUDIO endpoint dispatch.

src/trace_processor/shell/
  techno_subcommand.h/cc       # `trace_processor_shell techno` CLI.

test/data/
  music_synth_presets.json     # 128 preset definitions (checked in).

tools/trace_to_techno/
  gen_presets.py               # Regenerates music_synth_presets.json.
  render_all_presets.py        # Renders every preset to out/preset_wavs/.
```

## RPC interface

A single stateless RPC method `TPM_SYNTHESIZE_AUDIO` (enum value 20) in
`trace_processor.proto`. The request carries a full synth patch and either a
trace time range or an explicit duration; the response carries the rendered
WAV bytes.

```protobuf
// Request
message SynthesizeAudioArgs {
  optional SynthPatch patch = 1;
  optional int64 start_ts = 2;         // nanoseconds, omit for full trace
  optional int64 end_ts = 3;
  optional double duration_seconds = 4; // explicit render length; when set,
                                        // the trace window is ignored and the
                                        // engine renders exactly this long
                                        // (useful for preset preview).
}

// Response
message SynthesizeAudioResult {
  optional bytes wav_data = 1;  // Complete WAV file
  optional string error = 2;
}
```

`Rpc::SynthesizeAudioInternal()` parses the args, instantiates a
`SynthEngine`, calls `Render()`, and returns the WAV blob.

## Synth patch config (synth.proto)

The patch is a proto defining blocks and wires:

```protobuf
message SynthPatch {
  repeated SynthModule modules = 1;
  repeated SynthWire wires = 2;
}
```

Each `SynthModule` has a string `id` and a `oneof` for its type-specific
config (one message per block type). Each `SynthWire` connects
`from_module.from_port` to `to_module.to_port`, with optional `scale` and
`offset` for linear signal transforms (e.g. `pitch_env.out * 175 + 45` maps
a 0..1 envelope to a 45..220 Hz kick drum pitch sweep).

## Module system

All modules inherit from `SynthModule` (namespace
`perfetto::trace_processor::sound_synth`). The base class provides:

- Named input ports, set by the engine via `SetInput(port, buffer_ptr)`.
- Named output ports, registered by subclasses via `AddOutput(port)` which
  returns a stable `SignalBuffer*` (backed by a `std::deque` so multiple
  registrations never invalidate earlier pointers).
- A `type()` enum for non-RTTI downcasting.
- A `Process(num_samples)` virtual method.

All signals are mono float at `kSampleRate = 48000` Hz. A `SignalBuffer` is
`std::vector<float>` sized to the render length.

## Synth blocks

The engine currently ships **16 production blocks** plus 2 legacy blocks
(kept for backward compatibility while UI code migrates). Each block has its
own proto config in `synth.proto`, is dispatched from
`SynthEngine::BuildModules`, and has at least one unit test in
`synth_engine_unittest.cc`.

### Sources

| Module              | Proto config               | Summary |
|---------------------|---------------------------|---------|
| `TraceSliceSource`  | `TraceSliceSourceConfig`  | Reads slices from matching tracks and produces a gate/trigger/density signal. Pre-filled by the engine before the render pass. |
| `TestPatternSource` | `TestPatternSourceConfig` | **Upgraded**: 8-bar Am-G-F-E arpeggio at 128 BPM (A harmonic minor Andalusian cadence). Outputs `out` (gate, 70% duty) and `freq` (Hz, held through the gap). Legacy `IMPULSES` mode still available via the `mode` enum. |

### Oscillators

| Module              | Proto config               | Synthesis technique |
|---------------------|---------------------------|---------|
| `ClassicOsc`        | `ClassicOscConfig`        | polyBLEP-antialiased saw/square/triangle/sine with PWM, phase reset input, absolute `freq` input port. |
| `NoiseOsc`          | `NoiseOscConfig`          | Colored noise with continuous `tilt` (0 = white, 0.5 = pink via Paul Kellet's 6-pole filter, 1 = brown). xorshift32 PRNG with configurable seed. |
| `WavetableOsc`      | `WavetableOscConfig`      | Procedural wavetable (no data files). 32 frames × 1024 samples, 4 built-in tables: `SINE_TO_SAW`, `PULSE_SWEEP`, `BELL`, `VOCAL`. Linear frame/sample interpolation. |
| `FmOsc`             | `FmOscConfig`             | 2-operator phase-modulation FM (Chowning). Carrier sine modulated by sine modulator with configurable ratio / index / self-feedback. |
| `PhaseDistortionOsc`| `PhaseDistortionOscConfig`| Casio CZ-style phase warping before sin() lookup. Modes: `SAW_WARP`, `PULSE_WARP`. |
| `FoldOsc`           | `FoldOscConfig`           | Wavefolder oscillator using smooth `sin(drive * sin(2π·phase) + bias)` form. No fold creases, no fold aliasing. |
| `SyncOsc`           | `SyncOscConfig`           | Hardsync between two phase accumulators. Slave waveform is polyBLEP saw. Reset-point aliasing accepted. |
| `SuperOsc`          | `SuperOscConfig`          | JP-8000 7-saw supersaw. Fixed initial phases for reproducibility. Detune / mix params. |
| `DrawbarOrgan`      | `DrawbarOrganConfig`      | Hammond B3-style additive synthesis. 9 sine partials at drawbar ratios (16', 5⅓', 8', 4', 2⅔', 2', 1⅗', 1⅓', 1'), each with its own level. Normalized so all-drawbars-out produces unit amplitude. |
| ~~`Vco`~~           | `VcoConfig`               | **LEGACY.** Naive saw/square/sine (aliasing). Kept for UI back-compat. Prefer `ClassicOsc`. |

### Filters

| Module       | Proto config         | Summary |
|--------------|---------------------|---------|
| `MoogLadder` | `MoogLadderConfig`  | Huovilainen-style 24 dB/oct 4-pole lowpass with per-stage tanh saturation (fast `x/(1+|x|)` approximation) and bass-loss compensation. Self-oscillates at resonance ≈ 1. `cutoff_mod` / `reso_mod` input ports. |
| `Svf`        | `SvfConfig`         | Chamberlin state-variable filter, double-iterated per sample for stability. Modes: LP/HP/BP/Notch. Linear (no saturation), fast, versatile. |

### Effects

| Module       | Proto config         | Summary |
|--------------|---------------------|---------|
| `Waveshaper` | `WaveshaperConfig`  | Memoryless distortion. Modes: `SOFT_TANH` (normalized), `HARD_CLIP`, `FOLD` (iterative reflect), `ASYMMETRIC` (DC-biased tanh). Dry/wet mix. |
| `Delay`      | `DelayConfig`       | Circular-buffer feedback delay up to 2 s. Lowpass in feedback path for "dub-darkening" echoes. |
| `Chorus`     | `ChorusConfig`      | Multi-voice modulated-delay chorus. Short delay line read by N virtual taps whose delay times are each modulated by a sine LFO at phase offsets of 1/N cycle. Gives string ensembles their lushness and organs their rotary-speaker feel. |

### Modulators

| Module       | Proto config         | Summary |
|--------------|---------------------|---------|
| `Adsr`       | `AdsrConfig`        | Proper 4-stage exponential ADSR. One-pole filter-target approach: each stage aims at an overshoot point and transitions when crossing the actual target. Rising edge → attack; falling edge → release; re-triggers click-free from the current level. |
| `Lfo`        | `LfoConfig`         | Sub-audio oscillator. Waveforms: sine, triangle, square, saw_up, saw_down, sample_and_hold. Bipolar / unipolar output. S&H has a deterministic PRNG seed. |
| ~~`Envelope`~~| `EnvelopeConfig`   | **LEGACY.** Attack/Decay only, linear. Kept for UI back-compat. Prefer `Adsr`. |

### Utility

| Module    | Proto config   | Summary |
|-----------|---------------|---------|
| `Vca`     | `VcaConfig`   | Voltage-controlled amplifier. `out = in * gain`. |
| `Mixer`   | `MixerConfig` | Sums all connected inputs. The engine auto-numbers connections as `in`, `in.1`, `in.2`, ... |

All block implementations and their proto configs are covered by unit tests
in `synth_engine_unittest.cc` (43 tests total). The tests verify frequency
response (filters), harmonic content (oscillators), envelope shape (ADSR),
reproducibility (noise, S&H), chorus modulation, drawbar additive harmonic
content, and bounds/sanity for every block.

## Render pipeline

`SynthEngine::Render()` executes these steps:

1. **Resolve render duration.** First match wins:
   - If `duration_seconds > 0`, use that and skip the trace query entirely
     (preset/preview mode).
   - Else if `start_ts` / `end_ts` are non-zero, use the given trace window
     and apply the 48× time dilation.
   - Else query `min(ts)` / `max(ts + dur)` from the `slice` table for the
     full trace range.
   - Hard cap at 120 seconds of final audio (~23 MB at 48 kHz mono float) to
     avoid OOM on pathological requests.
2. **Parse patch proto** (`BuildModules`) -- creates module instances from
   the `oneof` dispatch in each `SynthModule` config, stores wire
   definitions.
3. **Populate trace sources** (`PopulateTraceSources`) -- runs SQL queries,
   fills `TraceSliceSource` output buffers. Skipped in preset-preview mode.
4. **Topological sort** (`TopoSort`) -- DFS-based sort so modules are
   processed after their dependencies.
5. **Connect wires** (`ConnectWires`) -- sets input port pointers on each
   module. For wires with non-trivial `scale` / `offset`, allocates an
   intermediate transform buffer and records a *deferred* `TransformOp`
   (see below). The destination's input port points at the (empty)
   transform buffer.
6. **Process.** For each module in topo order: `mod->Process(num_samples)`,
   then `ApplyPostProcessTransforms(mod, num_samples)` which fills any
   transform buffers whose source is `mod`. This guarantees the source's
   output is populated before the transform runs, and that downstream
   modules see the correct transformed buffer when they run.
7. **Encode WAV.** Takes the output of the `master` module (or the last
   module in topo order) and encodes it as a WAV file.

### Deferred wire transforms

Early in the preset work we hit a crash: `ConnectWires` was applying
`scale`/`offset` transforms eagerly, before source modules had been
processed. The source buffer was empty, so the transform read past the end
of a zero-sized vector.

The fix: allocate the transform buffer at wire-connect time but fill it
*after* the source module runs, via a list of `TransformOp` records keyed
by source module. The destination still gets a stable buffer pointer at
connect time, so it doesn't need to know anything changed.

### Output-port pointer stability

A related gotcha: `AddOutput()` used to return a pointer into a
`std::vector<OutputPort>`, which is invalidated when a second `AddOutput`
call triggers a reallocation. This latent bug showed up when
`TestPatternSource` was upgraded to register two output ports (`out` and
`freq`). Fixed by changing the container to `std::deque<OutputPort>`, which
provides stable pointers on `push_back`.

## Audio format

- Sample rate: 48000 Hz (constant, `kSampleRate`).
- Internal representation: `std::vector<float>` (`SignalBuffer`).
- Output WAV: 48 kHz mono, either 32-bit IEEE float (default) or 24-bit PCM
  (`--pcm24` flag on the CLI).

## Preset library

**256 presets** live in a single checked-in file:
**`test/data/music_synth_presets.json`**. Structure:

```json
{
  "version": 1,
  "generated_by": "tools/trace_to_techno/gen_presets.py",
  "preset_count": 128,
  "presets": [
    {
      "name": "kick_classic",
      "category": "drum",
      "description": "Deep classic 909-ish kick",
      "patch": {
        "modules": [ ... SynthPatch.modules ... ],
        "wires":   [ ... SynthPatch.wires   ... ]
      }
    },
    ...
  ]
}
```

Each `patch` maps 1:1 onto the `SynthPatch` proto — JSON field names are the
proto field names (snake_case). The Python renderer uses
`google.protobuf.json_format.ParseDict()` to convert preset-patch JSON into
a binary `SynthPatch` proto for TP to consume.

### 32 templates × 8 variations = 256

Each template is a Python function in `tools/trace_to_techno/gen_presets.py`
that emits 8 variations with different parameter sweeps (decay time, filter
cutoff, resonance, drive, detune, etc.). Every preset is a self-contained
patch that includes a `TestPatternSource` (arpeggio mode) wired to an
`Adsr.gate` and (for pitched presets) to the oscillator's `freq` port.

**Batch 1 — techno kit** (128 presets, 16 templates)

| Category | Templates |
|---|---|
| **Drums** (56 presets) | `kick`, `sub_kick`, `snare`, `clap`, `closed_hat`, `open_hat`, `tom` |
| **Bass** (32) | `acid_bass`, `reese_bass`, `sub_bass`, `fm_bass` |
| **Leads** (24) | `saw_lead`, `square_lead`, `wavetable_lead` |
| **Pads** (8) | `pad_warm` |
| **FX** (8) | `fx_riser` |

**Batch 2 — SUBSTANCE fat bass, ANALOG STRINGS, organs** (128 presets, 16 templates)

| Category | Templates |
|---|---|
| **Substance-inspired fat bass** (48) | `substance_saw`, `substance_square`, `substance_fold`, `substance_fm`, `substance_super`, `substance_wt` |
| **Analog strings** (48) | `strings_solina`, `strings_ensemble`, `strings_cinematic`, `strings_warm`, `strings_bright`, `strings_dream` |
| **Organs** (32) | `organ_hammond_jazz`, `organ_hammond_rock`, `organ_vox`, `organ_farfisa` |

The batch 2 templates exercise two new blocks: `Chorus` (essential for both
string ensembles and organ rotary-speaker feel) and `DrawbarOrgan` (Hammond
additive). They also share a "fat-layered-bass" architecture for the
SUBSTANCE-style patches: a sub-sine layer + a body-oscillator layer (saw /
square / fold / FM / supersaw / wavetable) are summed through independent
drive stages and a Moog ladder filter, giving the characteristic
"fat-and-rich" stacked character without relying on samples.

Summary of the category distribution across all 256 presets:

| Category | Count |
|---|---|
| bass (acid + reese + sub + fm + substance) | 80 |
| drum (kick + snare + hat + tom + clap)     | 56 |
| strings (solina + ensemble + warm + dream + cinematic + bright) | 48 |
| organ (hammond + vox + farfisa)            | 32 |
| lead (saw + square + wavetable)            | 24 |
| pad (pad_warm)                             | 8 |
| fx (fx_riser)                              | 8 |
| **total**                                  | **256** |

### Regenerating

```sh
# Regenerate the JSON from the Python source of truth.
python3 tools/trace_to_techno/gen_presets.py

# Render every preset to WAVs under out/preset_wavs/.
OUT=out/linux_clang_release \
  python3 tools/trace_to_techno/render_all_presets.py

# Render a subset matching a name filter.
OUT=out/linux_clang_release \
  python3 tools/trace_to_techno/render_all_presets.py --filter acid_bass
```

The render script locates `protoc` and `trace_processor_shell` from the same
build directory (`$OUT` or the most recently-built `out/*`), compiles
`synth.proto` to Python at startup, converts each preset to a binary
`.pb` file, and invokes `trace_processor_shell techno --patch-file … -o …`
in parallel across all CPUs.

## CLI usage

The `techno` subcommand supports two modes:

```sh
# 1. Trace-driven (original demo mode). Loads a trace and runs a built-in
#    demo patch (trace slices → envelope → sine bass).
trace_processor_shell techno -o out.wav trace.perfetto-trace

# 2. Patch-driven. Loads a binary SynthPatch proto and renders it for a
#    fixed duration. No trace needed.
trace_processor_shell techno \
  --patch-file preset.pb \
  --duration-secs 16 \
  -o out.wav

# Optional: 24-bit PCM output (more compatible with players).
trace_processor_shell techno -o out.wav --pcm24 trace.perfetto-trace
```

Flags:

| Flag | Meaning |
|---|---|
| `-o / --output FILE` | Output WAV path (required). |
| `--patch-file FILE` | Binary `SynthPatch` proto (skips trace loading if no positional arg). |
| `--duration-secs N` | Explicit render length in seconds. |
| `--pcm24` | Output 24-bit PCM instead of 32-bit float. |

## What's next (not yet implemented)

- **Track name glob filtering** on `TraceSliceSource`: currently reads ALL
  slices regardless of `track_name_glob`.
- **`TraceCounterSource`**: defined in the proto, not yet implemented in C++.
- **BPM / clock derivation** from trace events: `TestPatternSource` is
  hardcoded at 128 BPM. The UI/config will need a way to derive the master
  clock from the trace (e.g. from `renderRate 60.00 Hz` vsync markers).
- **Trace → instrument mapping**: the 128 presets currently all drive from
  `TestPatternSource`. The next milestone wires actual trace events into the
  preset gate/freq ports.
- **Oversampling for the Moog ladder filter and waveshaper**: accept some
  aliasing for now; re-evaluate if it's audible in real material.
- **polyBLAMP for triangle oscillator**: the current triangle is a naive
  piecewise-linear shape. polyBLAMP correction at peak/valley is noted as a
  TODO.
- **Retiring the legacy `Vco` and `Envelope` blocks** once the UI migrates
  off them (marked with `TODO(trace-to-techno)` in the source).

## Example patch (proto text format)

A minimal "acid bass" patch, as the preset generator emits it for
`acid_bass_classic`:

```
modules {
  id: "arp"
  test_pattern_source {
    mode: ARPEGGIO
    bpm: 128
    bars: 8
  }
}
modules {
  id: "osc"
  classic_osc { waveform: SAW  base_freq_hz: 0.0 }
}
modules {
  id: "filt_env"
  adsr { attack_ms: 0.1  decay_ms: 200  sustain: 0.1  release_ms: 100 }
}
modules {
  id: "amp_env"
  adsr { attack_ms: 0.5  decay_ms: 200  sustain: 0.5  release_ms: 80 }
}
modules {
  id: "filt"
  moog_ladder { base_cutoff_hz: 600  base_resonance: 0.80  drive: 1.0 }
}
modules {
  id: "vca"
  vca {}
}
modules {
  id: "drive_stage"
  waveshaper { mode: SOFT_TANH  drive: 3.0  mix: 0.7 }
}
modules {
  id: "master"
  mixer {}
}

# Gate from arp to both envelopes.
wires { from_module: "arp"  to_module: "amp_env"  to_port: "gate" }
wires { from_module: "arp"  to_module: "filt_env" to_port: "gate" }

# Oscillator tracks arp freq, one octave down (×0.5).
wires { from_module: "arp"  from_port: "freq"
        to_module: "osc"    to_port: "freq"
        scale: 0.5 }

# Filter envelope sweeps cutoff by up to 3 kHz.
wires { from_module: "filt_env"  to_module: "filt"
        to_port: "cutoff_mod"    scale: 3000.0 }

# Signal chain: osc → filter → vca → drive → master.
wires { from_module: "osc"         to_module: "filt"         to_port: "in" }
wires { from_module: "filt"        to_module: "vca"          to_port: "in" }
wires { from_module: "amp_env"     to_module: "vca"          to_port: "gain" }
wires { from_module: "vca"         to_module: "drive_stage"  to_port: "in" }
wires { from_module: "drive_stage" to_module: "master"       to_port: "in" }
```

This is the complete patch — every preset in `music_synth_presets.json`
looks like this (with the `TestPatternSource` arp driver shared across
them all).
