## Trace-to-Instrument Mapping

This section defines how a trace's structure maps to a techno track. It is
informed by analysis of `test/data/aot-trace.gz` (a ~28.5s Android system
trace) which contains:

- **7,219 threads** across **736 processes**
- **5,342 tracks** (execution, counter, async slice, frame timeline, etc.)
- **459k slices**, **184k sched events**, **290k counter events**
- **8 CPUs** in a big.LITTLE configuration (CPUs 0–3 little, 4–7 big)
- Process mix: 46 system bins, 67 vendor bins, 159 kernel workers, 133 Google
  apps, 26 Android system apps, 1 system_server, zygote/init
- Natural rhythm markers: `renderRate 60.00 Hz`, vsync slices,
  `dpu_kthread0` (display pipeline kernel thread)
- Rich counter surface: per-process memory (mem.rss et al.), power rails,
  cpu_frequency, thermal zones, battery stats, DMA allocations, network bytes

### Mapping philosophy

The trace is a symphony of parallel activity. Mapping it to music requires
four conceptual axes:

1. **Time dilation**: The trace's wall-clock time is stretched (or compressed)
   to fit a techno track's length. For a 28s trace targeting a 3-minute track,
   the dilation factor is ~6.4x. The global time axis of the trace becomes
   the time axis of the waveform.

2. **Voice assignment (who plays what)**: Each trace element (a thread, a
   process, a counter, a CPU) is mapped to an *instrument slot*. We use a
   deterministic assignment rule: rank-based (by activity, by category) so
   that the same trace always produces the same assignment, enabling
   reproducibility and ML comparison.

3. **Note generation (when and what)**: Each discrete trace event (a slice
   start, a sched switch, a counter crossing a threshold) becomes a MIDI-like
   note-on event. The note's pitch, velocity, and duration derive from
   properties of the event.

4. **Timbre modulation (how it sounds)**: Slow-changing trace state (counter
   values over time) drives the *knobs* of the synth blocks—filter cutoff,
   LFO depth, distortion drive, etc.

### Time dilation

The trace's time becomes musical time via a constant dilation factor `D`:
```
music_time(sec) = (trace_time - trace_t0) / 1e9 * D
```

At 128 BPM (a common techno tempo), one bar = 1.875s, one beat = 0.469s,
one sixteenth note = 0.117s. For `D = 6.4`, a 100µs slice in the trace
becomes a 0.64ms musical event — below audibility but within a grain's
worth. A 10ms slice becomes 64ms — about a sixteenth note's tail.

**Tempo anchoring**: The 60 Hz vsync rate (`renderRate 60.00 Hz` slices) is
a perfect master clock. With `D = 6.4`, vsync becomes ~9.4 Hz — still too
fast for techno beats. We snap the tempo to `vsync_rate * D / N` where N is
chosen to land within 120–140 BPM. For D=6.4 and N=4: 60*6.4/4 = 96 events/sec
→ one beat per 16 vsyncs → 22.5 BPM. Not ideal. Better: set BPM directly
(say 128) and fit events to beat grid via quantization.

**Recommended approach**: Use BPM as a free parameter. The trace provides
*event timing*, the BPM provides the *grid*. Events are quantized to a
tempo grid (e.g., 16th-note grid at 128 BPM = 30Hz grid) while preserving
the relative density of trace activity.

### The 128-Instrument Bank

We organize instruments into 8 banks of 16 slots each, totaling 128
instruments. Each slot is a recipe: a block chain plus a mapping rule that
selects which trace elements drive it.

#### Bank 0 — Core Drums (slots 0-15)

The rhythmic backbone. Each slot is triggered by a specific recurring trace
event.

| Slot | Instrument   | Trigger source                        | Block chain                                  |
|------|--------------|---------------------------------------|----------------------------------------------|
|  0   | Kick         | vsync events (`renderRate` slices)    | Sine + pitch-ADSR → Waveshaper:soft          |
|  1   | Sub-kick     | CPU0 sched density > threshold        | Sine (45Hz) + amp-ADSR                       |
|  2   | Snare        | binder transaction start              | Noise:white → SVF:bp 3kHz → ADSR             |
|  3   | Clap         | frame-deadline misses (actual>expected)| Noise:white → SVF:bp 1.5kHz → multi-tap ADSR|
|  4   | Closed hat   | CPU0-1 sched switch                   | Noise:white → SVF:hp 8kHz → fast ADSR        |
|  5   | Open hat     | CPU2-3 sched switch                   | Noise:white → SVF:hp 6kHz → slow ADSR        |
|  6   | Ride         | `android_gpu_work_period` events      | FM2 inharmonic → SVF:hp → long ADSR          |
|  7   | Crash        | wakelock acquire events               | Noise:pink → Comb filter → reverb send       |
|  8   | Tom low      | CPU4 (big core) sched switch          | Sine + pitch envelope (120→80Hz)             |
|  9   | Tom mid      | CPU5 sched switch                     | Sine + pitch envelope (180→120Hz)            |
| 10   | Tom hi       | CPU6 sched switch                     | Sine + pitch envelope (240→160Hz)            |
| 11   | Rim shot     | InputDispatcher touch events          | FM2 (1:7.13 ratio) → short ADSR              |
| 12   | Shaker       | f2fs_iostat events                    | Noise:white → SVF:bp 5kHz → ADSR             |
| 13   | Cowbell      | DMA allocation events                 | FM2 (1:1.4) → short ADSR                     |
| 14   | Conga        | kworker sched switches                | KarplusStrong (200Hz) → ADSR                 |
| 15   | Perc glitch  | cross-CPU wakeups                     | Bitcrusher on noise → ADSR                   |

#### Bank 1 — Bass Voices (slots 16-31)

Low-frequency voices driven by the system's "gravity": long-lived processes,
memory pressure, and core kernel activity.

| Slot | Instrument       | Source                                 | Block chain                              |
|------|------------------|----------------------------------------|------------------------------------------|
| 16   | Sub-bass drone   | total RSS smoothed (all processes)     | Sine (35-50Hz) + slow filter LFO         |
| 17   | Main techno bass | surfaceflinger main thread activity    | PolyBLEP:saw → MoogLadder → Waveshaper   |
| 18   | Acid bass A      | foreground app main thread             | PolyBLEP:saw → DiodeLadder (high reso)   |
| 19   | Acid bass B      | foreground app RenderThread            | PolyBLEP:saw → DiodeLadder (alt tuning)  |
| 20   | Rolling bass     | system_server main thread              | PolyBLEP:square → MoogLadder             |
| 21   | Reese bass       | top 2 detuned kworker threads          | Supersaw (detune=0.3) → MoogLadder       |
| 22   | Wobble bass      | CPU freq changes (big cluster)         | PhaseDistortion → MoogLadder, LFO on warp|
| 23   | FM bass          | GPU work period bursts                 | FM2 (1:2, high index) → SVF:lp           |
| 24-31| Process bass 1-8 | Top 8 processes by total CPU time      | PolyBLEP variants → filter variants      |

The process-bass slots (24-31) are assigned deterministically: rank the top 8
processes by total on-CPU time (from `sched`), and assign each a bass voice
with a unique filter/waveform combo keyed by the process's rank.

#### Bank 2 — Lead Voices (slots 32-63)

32 lead synth voices. Each is mapped to one of the 32 most active threads in
the trace (ranked by slice count). This creates a recognizable "melody" per
trace: the same trace always yields the same dominant leads.

Pitch is determined by the slice name. We hash the slice name to a pitch
class within a predefined scale (e.g., A minor pentatonic — `A,C,D,E,G`).
This means the same slice name always plays the same note, creating musical
*motifs* that repeat as the same code path executes. The octave is chosen
per voice (from voice index).

| Slot Range | Thread category             | Block chain template                    |
|------------|-----------------------------|------------------------------------------|
| 32-39      | Top 8 most active threads   | PolyBLEP:saw → MoogLadder → Delay       |
| 40-47      | Next 8                      | Wavetable → SVF → Chorus                |
| 48-55      | Next 8                      | FM2 → SVF → Delay                       |
| 56-63      | Next 8                      | PhaseDist → Comb → Chorus               |

Each voice's note-on is triggered when a new slice begins on its assigned
thread. Note duration = slice duration * time_dilation, clamped to a 16th
note minimum. Velocity = log(slice duration) normalized to [0, 1].

#### Bank 3 — Pads & Atmosphere (slots 64-79)

Slow-evolving sustained voices driven by counter tracks. These create the
atmospheric bed that a techno track sits on.

| Slot | Instrument           | Driven by                                  | Block chain                         |
|------|----------------------|--------------------------------------------|-------------------------------------|
| 64   | Memory pad           | total anon RSS (sum across procs)          | Supersaw → MoogLadder → Reverb      |
| 65   | Thermal pad          | avg thermal zone temperature               | Wavetable → SVF → Chorus → Reverb   |
| 66   | Power pad            | total power rail energy                    | Supersaw (wide detune) → Reverb     |
| 67   | Battery pad          | battery capacity counter                   | Wavetable (slow pos morph) → Reverb |
| 68   | OOM tension pad      | sum of oom_score_adj                       | PolyBLEP → MoogLadder (high reso)   |
| 69   | Network pad          | network packet byte counts                 | Noise:pink → FormantFilter → Reverb |
| 70   | GPU memory pad       | process_gpu_memory                         | Wavetable → SVF → Reverb            |
| 71   | DMA heap pad         | android_dma_heap_change                    | FM2 (slow) → Reverb                 |
| 72-79| Process memory pads  | Top 8 procs by mem.rss.anon                | Supersaw variants → Reverb          |

Pads are triggered long (multi-second notes) and modulated continuously:
the counter's current value becomes the filter cutoff, wavetable position,
or LFO rate.

#### Bank 4 — Modulator / FX Voices (slots 80-95)

These slots don't produce notes directly — they produce *modulation signals*
routed to other slots, plus occasional FX sounds. Think of them as the
"patch bay" between the trace and the synth.

| Slot | Role                                    | Trace source                      | Target(s)                       |
|------|-----------------------------------------|-----------------------------------|---------------------------------|
| 80   | Global filter LFO                       | CPU0 cpu_frequency                | Mod: all-bank filter cutoff     |
| 81   | Global pitch drift                      | thermal zone delta                | Mod: oscillator detune          |
| 82   | Global distortion drive                 | power rail current                | Mod: waveshaper drive           |
| 83   | Reverb size                             | total system memory pressure      | Mod: reverb room_size           |
| 84   | Chorus depth                            | vmstat pgscan rate                | Mod: all chorus depth           |
| 85   | Delay feedback                          | network byte throughput           | Mod: delay feedback             |
| 86   | Sidechain bus                           | kick events                       | Ducks: bass, pads, leads        |
| 87   | Breakdown trigger                       | thermal throttle events           | Global: filter close + reverb   |
| 88   | Build-up trigger                        | wakelock contention               | Global: filter open + tension   |
| 89   | OOM-kill stinger                        | OOM kill events                   | FX: downward saw sweep          |
| 90   | Binder call chatter                     | binder tx rate                    | FX: ticker pattern              |
| 91   | Frame drop glitch                       | frame deadline miss               | FX: short bitcrush burst        |
| 92   | GPU freq pitch bend                     | GPU frequency changes             | Mod: bass osc fine pitch        |
| 93   | f2fs IO sweep                           | f2fs IO latency spikes            | FX: filter sweep                |
| 94   | Sysprop change FX                       | sysprop_counter                   | FX: pitched bell                |
| 95   | Wakeup stutter                          | cross-cluster wakeups             | FX: gated noise                 |

#### Bank 5 — Kernel Voices (slots 96-111)

The kernel threads are the "machine underneath the music" — clockwork,
metallic, inhuman. They get FM and Karplus-Strong voices with short envelopes.

| Slot Range | Source                                  | Block chain                         |
|------------|-----------------------------------------|-------------------------------------|
| 96-103     | Top 8 kworker threads by switches       | FM2 inharmonic → SVF:bp → tight ADSR|
| 104-107    | dpu_kthread0 + display pipeline         | KarplusStrong → Comb → ADSR         |
| 108-111    | irq/softirq threads                     | Noise+FM → Bitcrusher → ADSR        |

Kernel voice pitches are locked to the inharmonic series (1, 1.41, 2.72,
3.14...) for that "machinery" quality. They shouldn't sound melodic — they
should sound *structural*.

#### Bank 6 — App-layer Voices (slots 112-127)

The userspace "chatter" — the part of the trace a human would recognize.
These are the top 16 non-system processes by slice activity.

| Slot Range | Mapping                                 | Block chain template                    |
|------------|-----------------------------------------|------------------------------------------|
| 112-119    | Top 8 Google apps (com.google.*)        | Wavetable → MoogLadder → Delay          |
| 120-125    | Top 6 Android system apps               | PolyBLEP → SVF → Chorus                 |
| 126        | twoshay (touch firmware)                | Noise → SVF:bp → short ADSR             |
| 127        | traced_probes (the instrument itself)   | Self-referential: FM2 → bitcrush → fade |

Slot 127 is special: it's the process that *produced* the trace. We give it
a distinctive self-referential voice (an "inside joke" for the track).

### Creative Mapping Strategies

Beyond the static slot assignment, these strategies add musical structure:

**1. Slice name → pitch hash**

Every slice has a name (a string). Hash the name to a scale degree:
```
pitch_class = hash(slice.name) % scale_length
octave      = voice.base_octave + (hash(slice.name) >> 16) % 2
```
Using a minor pentatonic or Phrygian mode keeps things in a dark, techno-appropriate
key. Because hashing is deterministic, the same slice name always plays the
same note → repeated function calls create repeating musical motifs. This
transforms the trace's natural repetition (a vsync fires every 16.67ms, a
BinderTransaction happens thousands of times) into melodic hooks.

**2. Duration → rhythmic quantization**

Slice durations become note lengths, but quantized to the beat grid:
```
musical_dur = clamp(round_to_nearest_16th(slice.dur * D), 16th_note, bar)
```
Very short slices collapse to 16th-note grace notes (hi-hat territory). Long
slices become sustained notes. The distribution of durations in a real trace
(lots of short ones, few long ones) naturally produces the "dense at the top,
sparse at the bottom" texture of a techno track.

**3. CPU number → pan position**

Thread execution slices can be panned based on which CPU they ran on:
```
pan = (cpu / (num_cpus - 1)) * 2 - 1  // -1 = hard left, +1 = hard right
```
This creates a physical *spatialization* of the trace. In a big.LITTLE system,
little cores (LITTLE) pan left, big cores pan right — giving the listener a
sense of the scheduling decisions happening in real time.

**4. Process priority → velocity**

```
velocity = map(process.oom_score_adj, [-1000, 1000], [1.0, 0.3])
```
High-priority processes (foreground, -1000 oom) play loud. Background
processes (high oom_score) play quiet. When a process gets killed or demoted
(oom_score_adj rises), its voice fades into the background — a natural musical
gesture.

**5. Binder transactions → chord events**

A binder IPC is a conversation between two processes. When process A calls
process B:
```
on binder_tx(A -> B):
  trigger voice_A(note1) and voice_B(note1 + harmonic_interval) simultaneously
```
This creates spontaneous chord hits whenever processes communicate — the
"system is talking to itself" musicalized.

**6. Counter crossings → filter sweeps**

When a counter crosses a threshold (e.g., memory goes from "low" to "medium"
pressure), trigger a global filter sweep on the assigned bank. A GC event
(heap size drops) triggers a downward sweep; allocation spikes trigger upward.

**7. Sched switches as "ghost hits"**

Every sched switch is a percussion hint. Not all become audible — we
stochastically select (or density-gate) to avoid overwhelming the mix. But
the gated subset creates a "machine breathing" undercurrent of subtle
percussion that tracks the actual CPU load.

**8. Frame timeline as syncopation layer**

The `android_expected_frame_timeline` vs `android_actual_frame_timeline`
divergence is pure rhythmic tension. When frames hit their deadline, nothing
extra. When they miss, add a clap on the miss point. When they arrive early,
add a ghost note. This makes the "jank" of the system audible as syncopation.

**9. Thermal throttling → breakdown**

When thermal counters rise above a threshold, trigger a "breakdown": filter
cutoff slowly closes on all banks, reverb size grows, bass drops out. When
the temperature falls, everything opens back up → the "drop." The physical
heat of the SoC literally shapes the musical dynamics.

**10. Wakelock contention → tension build**

Wakelocks prevent the system from sleeping. Multiple concurrent wakelocks =
system fighting to stay awake = musical tension. Map wakelock count to a
rising white-noise swell (riser). When wakelocks release, the swell cuts out
→ silence → drop.

**11. OOM kill → the drop**

An OOM kill is the most dramatic event a system can produce. We treat it as a
*drop marker*: brief silence, then the full mix returns with extra bass and a
filter-open sweep. The system killing a process becomes a track climax.

**12. Process lifetime → song structure**

Short-lived processes (starts and stops during the trace) become *bar-level*
events. Long-lived processes (alive the whole trace) form the *constant*
layers. The ratio of long-lived to short-lived processes determines how
"stable" vs "chaotic" the track feels.

### Assignment determinism

Every mapping rule is deterministic: no randomness, no wall-clock dependencies.
The assignment `trace → instruments → notes → waveform` is a pure function.
This is critical for:

- **Reproducibility**: The same trace always produces the same .wav
- **ML training**: Traces with similar structure should produce similar-
  sounding audio, enabling audio-embedding-based trace similarity
- **Debugging**: You can re-run the synthesis and get bit-identical output

The selection of "top N threads by activity" uses stable sorting (ties broken
by thread ID) to ensure the same trace always picks the same threads.