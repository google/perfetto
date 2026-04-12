# Trace-To-Techno: TraceProcessor Implementation

This document describes the current architecture of the synth engine inside
TraceProcessor (TP), as implemented during Milestone 2.

## Code layout

```
protos/perfetto/trace_processor/
  synth.proto                  # Proto definitions for the synth patch config

src/trace_processor/sound_synth/
  synth_module.h/cc            # Base class for all synth modules
  modules.h/cc                 # Concrete module implementations
  synth_engine.h/cc            # Top-level engine: parses config, wires modules, renders
  synth_engine_unittest.cc     # Unit tests
  BUILD.gn

src/trace_processor/rpc/
  rpc.h/cc                     # TPM_SYNTHESIZE_AUDIO RPC endpoint (dispatch + handler)

src/trace_processor/shell/
  techno_subcommand.h/cc       # `trace_processor_shell techno` CLI subcommand
```

## RPC interface

A single stateless RPC method `TPM_SYNTHESIZE_AUDIO` (enum value 20) in
`trace_processor.proto`. The request carries the full synth patch config and
optional time range; the response carries the rendered WAV bytes.

```protobuf
// Request
message SynthesizeAudioArgs {
  optional SynthPatch patch = 1;
  optional int64 start_ts = 2;  // nanoseconds, omit for full trace
  optional int64 end_ts = 3;
}

// Response
message SynthesizeAudioResult {
  optional bytes wav_data = 1;  // Complete WAV file
  optional string error = 2;
}
```

The RPC is wired through `Rpc::ParseRpcRequest()` in `rpc.cc`, which calls
`Rpc::SynthesizeAudioInternal()`. This instantiates a `SynthEngine`, calls
`Render()`, and returns the WAV blob.

## Synth patch config (synth.proto)

The patch is a proto defining modules and wires:

```protobuf
message SynthPatch {
  repeated SynthModule modules = 1;
  repeated SynthWire wires = 2;
}
```

Each `SynthModule` has a string `id` and a `oneof` for its type-specific config.
Each `SynthWire` connects `from_module.from_port` to `to_module.to_port`, with
optional `scale` and `offset` for linear signal transforms.

## Module system

All modules inherit from `SynthModule` (C++ namespace:
`perfetto::trace_processor::sound_synth`). The base class provides:

- Named input ports (set by the engine via `SetInput(port, buffer_ptr)`)
- Named output ports (registered by subclasses via `AddOutput(port)`)
- A `type()` enum for safe downcasting (no RTTI in the codebase)
- A `Process(num_samples)` virtual method

### Currently implemented modules

| Module             | Inputs                  | Outputs | Description |
|--------------------|-------------------------|---------|-------------|
| `TraceSliceSource` | (none -- pre-filled)    | `out`   | Signal derived from trace slice data. Output buffer is populated by the engine before the render pass. |
| `Vco`              | `freq_mod` (optional)   | `out`   | Voltage Controlled Oscillator. Waveforms: sine, saw, square. `base_freq_hz` sets the fundamental. |
| `Vca`              | `in`, `gain` (optional) | `out`   | Voltage Controlled Amplifier. Multiplies `in` by `gain`. Falls back to `initial_gain` if no CV connected. |
| `Envelope`         | `trigger`               | `out`   | Attack-Decay envelope generator. Rising edge on trigger starts attack ramp to `peak`, then decays to 0. |
| `Mixer`            | `in`, `in.1`, `in.2`... | `out`   | Sums all connected inputs. The engine auto-numbers mixer input ports. |

### Trace source modules

`TraceSliceSource` is a special module whose output buffer is pre-filled by the
`SynthEngine` before the processing pass. The engine queries the trace via SQL:

```sql
SELECT s.ts, s.dur FROM slice s WHERE s.dur > 0 AND s.ts >= ? AND s.ts < ? ORDER BY s.ts
```

For each slice, the corresponding sample range in the output buffer is set to
1.0 (gate signal). The signal type is configured in the proto
(`TraceSliceSourceConfig.SignalType`: GATE, TRIGGER, DENSITY).

**Current limitation**: the engine does not yet filter by `track_name_glob` or
`slice_name_glob` -- it reads all slices. This is the next thing to implement.

`TraceCounterSource` is defined in the proto but not yet implemented in C++.

## Render pipeline

`SynthEngine::Render()` executes these steps:

1. **Parse patch proto** -- creates module instances, stores wire definitions.
2. **Determine time range** -- if `start_ts`/`end_ts` are 0, queries
   `min(ts)`/`max(ts+dur)` from the slice table.
3. **Compute sample count** -- `duration_seconds * 48000`.
4. **Populate trace sources** -- runs SQL queries, converts timestamps to sample
   indices, fills source buffers.
5. **Topological sort** -- DFS-based sort so modules are processed after their
   dependencies.
6. **Connect wires** -- sets input port pointers on each module. For wires with
   `scale`/`offset`, creates intermediate transform buffers.
7. **Process** -- calls `Process(num_samples)` on each module in topo order.
8. **Encode WAV** -- takes the `master` module's (or last module's) output and
   encodes it as a WAV file (48kHz, 32-bit float, mono).

## Audio format

- Sample rate: 48000 Hz (constant, `kSampleRate`)
- Internal representation: `std::vector<float>` (`SignalBuffer`)
- Output WAV: 48kHz mono, either 32-bit IEEE float (default) or 24-bit PCM
  (`--pcm24` flag on the CLI)

## CLI usage

```sh
# Default 32-bit float output
trace_processor_shell techno -o output.wav trace.perfetto-trace

# 24-bit PCM output
trace_processor_shell techno -o output.wav --pcm24 trace.perfetto-trace
```

The `techno` subcommand currently uses a hardcoded default patch:
- `TraceSliceSource` (gate from all slices) -> `Envelope` (2ms attack, 80ms
  decay) -> `VCA` <- `VCO` (110Hz sine) -> `Mixer` (master output)

## What's not yet implemented

- **Track name glob filtering** on trace sources (reads all slices currently).
- **TraceCounterSource** C++ module (proto is defined).
- **UI integration** -- the RPC endpoint is wired but no UI page exists yet.
- **BPM/clock derivation** from trace events (to be discussed in Milestone 3).
- **More synth modules**: LFO, VCF (filter), clock dividers, sequencers.
- **Custom patch input** -- the CLI uses a hardcoded patch; needs a way to pass
  a patch proto file or inline config.

## Example patch (proto text format)

```
modules {
  id: "src"
  trace_slice_source {
    track_name_glob: "RenderThread*"
    signal_type: GATE
  }
}
modules {
  id: "env"
  envelope { attack_ms: 2  decay_ms: 80  peak: 1.0 }
}
modules {
  id: "osc"
  vco { waveform: SINE  base_freq_hz: 110 }
}
modules {
  id: "amp"
  vca {}
}
modules {
  id: "master"
  mixer {}
}
wires { from_module: "src"  to_module: "env"     to_port: "trigger" }
wires { from_module: "env"  to_module: "amp"     to_port: "gain" }
wires { from_module: "osc"  to_module: "amp"     to_port: "in" }
wires { from_module: "amp"  to_module: "master"  to_port: "in" }
```

This creates a 110Hz sine bass that plays whenever any slice is active in the
trace -- the envelope shapes each "note" with a 2ms attack and 80ms decay.
