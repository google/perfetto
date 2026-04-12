# Background on Modular Synths

> This doc is a *conceptual* primer. For the concrete list of C++ blocks that
> TP currently ships, see
> [trace-processor-design.md](trace-processor-design.md#synth-blocks).
> For the full palette with algorithm notes, see
> [SPEC.md § Synth Blocks Reference](SPEC.md#synth-blocks-reference).

## Signal types

Everything in a modular synth is just a signal (a stream of floating point
samples at the sample rate, e.g. 44100 Hz). But conceptually there are two
kinds:

- **Audio signals** -- audible frequencies (20 Hz - 20 kHz). A sine wave
  oscillator outputs this.
- **Control signals (CV)** -- slow-moving signals (typically 0-20 Hz) that
  modulate parameters of other modules. Same math, different purpose.

Key insight: **a module doesn't know or care whether its input is audio or
control**. An LFO (Low Frequency Oscillator) is literally just an oscillator
running at e.g. 2 Hz instead of 440 Hz. You can plug anything into anything.

## Core modules

### Sound sources

- **VCO** (Voltage Controlled Oscillator) -- sine/saw/square/triangle. The "V"
  (voltage) means the frequency input is a signal, not a fixed number, so
  another module can continuously change the pitch. In TP this is the
  `ClassicOsc` block (polyBLEP-antialiased saw/square/triangle/sine with PWM
  and phase reset). Different synthesis techniques get their own dedicated
  oscillator blocks: `FmOsc` (phase modulation), `WavetableOsc` (single-cycle
  table scanning), `FoldOsc` (wavefolder), `PhaseDistortionOsc` (Casio CZ
  phase warp), `SyncOsc` (hard-sync), `SuperOsc` (JP-8000 7-saw stack),
  `NoiseOsc` (colored noise with tilt).

### Sound shapers

- **VCF** (Voltage Controlled Filter) -- low-pass, high-pass, band-pass. Has a
  cutoff frequency and resonance, both controllable by CV. This is what makes a
  synth sound "warm" or "squelchy" vs a raw oscillator. TP ships two: the
  `MoogLadder` block (24 dB/oct Huovilainen-style ladder with tanh-saturated
  stages) for warm bass, and the `Svf` block (Chamberlin 12 dB/oct
  state-variable filter with simultaneous LP/HP/BP/Notch) for everything else.
- **VCA** (Voltage Controlled Amplifier) -- a volume knob controlled by a
  signal. Feed it an envelope to shape the loudness over time. In TP this is
  the `Vca` block (unchanged from the original plumbing).

### Control sources

- **Envelope Generator (EG)** -- outputs a one-shot shape when triggered.
  Classic is ADSR: Attack (ramp up), Decay (drop to...), Sustain (hold level),
  Release (fade out after trigger ends). Patch into a VCA to give notes a
  shape -- otherwise the oscillator just drones. In TP this is the `Adsr`
  block -- a proper 4-stage exponential envelope using one-pole filters
  targeting overshoot points. (The legacy `Envelope` block is an
  attack/decay-only version kept for backward compatibility.)
- **LFO** -- slow oscillator used for vibrato (patch into VCO pitch), tremolo
  (patch into VCA), filter sweeps (patch into VCF cutoff), etc. In TP this is
  the `Lfo` block (6 waveforms: sine, triangle, square, saw up, saw down,
  sample-and-hold).

### Effects

TP also ships a few dedicated effect blocks:

- **Waveshaper** -- memoryless nonlinear distortion (tanh / hard-clip /
  wavefold / asymmetric). Essential for kick punch, bass grit, lead
  aggression.
- **Delay** -- feedback delay with a lowpass in the feedback path (the
  "dub-echo" darkening that every echo picks up).
- **Chorus** -- short multi-voice modulated delay. Gives Solina-style
  strings their lush ensemble character and Hammond/Vox/Farfisa organs
  their rotary-speaker "wobble". Each "voice" is a virtual tap into the
  same delay line, modulated by a sine LFO at a different phase offset,
  summed with dry.

### Organs (specialized)

- **DrawbarOrgan** -- the one non-minimal oscillator: a dedicated Hammond
  B3-style additive synthesiser. 9 sine partials at the classic drawbar
  ratios (16', 5⅓', 8', 4', 2⅔', 2', 1⅗', 1⅓', 1'), each with its own
  0..1 level. Registered as an oscillator (it takes the same `freq` input
  as everything else) but implemented as a pre-baked 9-partial additive
  engine rather than as 9 separate `ClassicOsc` sines, both for efficiency
  and to make drawbar settings a single-block decision. Vox Continental /
  Farfisa style organs are instead built with square-wave `ClassicOsc`s
  through filters + `Chorus` — that's subtractive, not additive.

## Where tempo comes in

None of the modules above know anything about tempo. A raw modular synth is
continuous -- it just makes sound. Tempo comes from the **clock + sequencer**
layer:

- **Clock** -- generates a pulse at regular intervals. 120 BPM = a pulse every
  0.5 seconds (or subdivisions: 8th notes = every 0.25s, 16th notes = every
  0.125s). It's just a square wave at a very low frequency (2 Hz for 120 BPM
  quarter notes).

- **Sequencer** -- receives clock pulses and on each pulse, outputs the next
  value from a pre-programmed list. A classic 16-step sequencer holds 16 values.
  Each clock tick advances one step. After step 16, it loops. The output is a CV
  signal (e.g. pitch values) and/or a **gate** signal.

- **Gate** -- a binary on/off signal. "Note is on" vs "note is off". The
  sequencer emits a gate-on at each step (or not, for rests). The gate triggers
  the envelope generator, which shapes the VCA, which gives discrete "notes"
  instead of a continuous drone.

In TP, the clock + sequencer roles are currently fused into the
`TestPatternSource` block, which outputs an 8-bar Am-G-F-E arpeggio (i-bVII-
bVI-V in A harmonic minor, Andalusian cadence) at 128 BPM. It emits two ports:
`out` (gate, 70% duty per note for ADSR release headroom) and `freq` (current
note frequency in Hz, held through the note + gap). This is enough to
audition any instrument preset without needing actual trace data. When the
trace-driven mapping is wired up in a later milestone, `TraceSliceSource` /
`TraceCounterSource` will take over the clock/sequencer role.

## Full chain example: rhythmic bass line

```
Clock (120 BPM, 16th notes)
  -> Sequencer (16 steps of pitch values + gate pattern)
      -> pitch CV -> VCO (saw wave)
      -> gate -> Envelope Generator (short ADSR)
                  -> VCA (shapes the volume)
                      -> VCF (low-pass, cutoff modulated by same envelope)
                          -> Output
```

The clock says **when**, the sequencer says **what pitch** and **whether a note
plays**, the envelope says **what shape** each note has.

Here's the concrete version of the same patch using TP block names (this is
literally what the `acid_bass_classic` preset looks like, minus the drive
stage):

```
TestPatternSource (Arpeggio, 128 BPM)
  ├── out  (gate, 70% duty) ──► Adsr.amp_env ── 0..1 ──► Vca.gain
  │                      ╰──► Adsr.filt_env ── 0..1 ──► MoogLadder.cutoff_mod
  │                                               (scaled ×3000 Hz)
  └── freq (Hz)         ──────► ClassicOsc.freq   (scaled ×0.5 = one octave down)

  ClassicOsc (saw) ──► MoogLadder (cutoff=600, reso=0.8) ──► Vca ──► master
```

Everything is just wires passing floats between blocks. The filter envelope
sweeping the cutoff is what gives the acid bass its squelch; the 0.5× scale
on the freq wire transposes the melody down an octave into the bass register.

## Tempo in the context of techno

A techno track typically has:

- One master clock (say 130 BPM).
- Multiple sequencers running off that clock (or clock divisions -- e.g. kick on
  quarter notes, hi-hat on 16ths, bass on 8ths).
- Each sequencer drives its own synth voice (kick drum synth, bass synth, pad,
  etc.).

**Clock dividers/multipliers** are modules that take the master clock and output
pulses at half speed, double speed, etc. This is how different rhythmic patterns
emerge from one tempo source.

## Mapping to the trace-to-techno problem

The trace provides both roles:

- **The clock/sequencer role**: trace events (slices, scheduling) are inherently
  discrete and timed -- they naturally provide "when" and "what".
- **The modulation role**: counter values, durations, frequencies of recurring
  events can drive filter cutoffs, pitches, envelopes.

The key design question is which trace properties become the clock/gate/sequencer
layer (rhythm) vs which become CV modulation (timbre/texture). This question is
intentionally *not* answered in the current milestone -- the preset library
today uses only `TestPatternSource` so every instrument can be auditioned
without a trace. Wiring trace events into the gate/freq inputs is the next
milestone; see [SPEC.md § Milestones](SPEC.md#milestones).
