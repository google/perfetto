# Trace-To-Techno

## Vision

Trace-To-Techno is an experimental project to convert a trace file into a waveform (a techno music track).
The overall idea is to be able to map a trace, which is a complex set of tracks, slices, counters and flows into a music file, in a way that trace input contributes to the "entropy" and spreads, in a reproducible fashion, in the audio spectrum.
The reason why we want to do this is to later use the generate wav to apply the existing ML research to find similar traces (or better portions of a trace) from a corpus of millions of traces. This is predicated on our ability to create enough "musical entropy" and have the right intuions about how to map trace elements to synths. 

The intuition is that, in a way, a trace is like a techno sound track: An Android trace often consists in hundreds of threads that are working together to put pixels on screen with a semi-regular cadence of 60/100/120 FPS (Frames per second), fps is very device-dependent.
A techno track has  a cadence of 120-150 BPM. so with some time dilation they should mapw ell into each other

## Design docs

- [Background on Modular Synths](background-on-synths.md) -- how modular
  synthesizers work and how the concepts map to this project.
- [TraceProcessor Implementation](trace-processor-design.md) -- current
  architecture of the synth engine in TP: code layout, RPC interface, module
  system, render pipeline, and CLI usage.

## Overall architecture

TraceProcessor (TP) should become a modular synthesizer. I am thinking of synths like Monark or "Massive X" or the open-source "Surge XT".
The beauty of them is that they are all pure mathematical functions.

TP should have the building blocks that allow turning the input trace (or a portion of it, i.e. selected tracks and time range) into a wav.
The reason why I want in TP is because it I envision a dual use-case:
1. Development/experimentation phase: the UI will configure the blocks in TP, determine the wiring -> let TP do the synth -> get the waveform back and play in the UI via webaudio. This enables research
2. Running in batch: once we have figured out the config, I want to be able to batch-convert some traces into .wav using purely TP cmdline, without the UI.

The overall view is that all the blocks required for the synth should live in TP. However, HOW the blocks are connected to each other (Think about the synth wiring) and HOW they are connected to the trace inputs should be defined in a file (proto or JSON TBD) which can be dynamically pushed to TP to rewire things.

This is the overall idea, which will require some later refinement
- TP has the synth code in the form of C++ blocks. THey take as input the config proto, which defines the wiring, and the trace database itself and emits a bytes blob with the .wav.
- The UI will have a frontend, similar to the Data Explore page, to setup the synth wiring and generate the proto. The proto will be pushed into TP when the wiring is changed.
- TP has a custom RPC endpoint in trace_processor.proto, which can be used to pass the synth config, and to require the synthesis of a given set of tracks for a given time range. This RPC returns the synth waveform.
- The UI will then play this waveform using webaudio

## Milestones

Each milestone should be a dedicated agent session.

### 1 Brainstorm to decide a plan

We need to decide how the overall architecture looks like.
For sure TP will have some C++ code for the synth blocks, which can be dynamically connected.
The thing to decide is how the various controls (think of the various knobs you have on a synth) work.
For sure we want everythign to be deterministic, nobody is going to "play live" or adjust knobs while we play.
All The VFO inputs and all the control knobs must be statically mapped to properties of the trace (e.g. specific tracks and slices).
TP must be able to generate the synth everything without any extra input or human interventon, using as input only: The trace contents itself (i.e. the various tables) and proto that defines the wiring.


### 2 Basic architecture

This is to setup the codebase come up with the right interaction between TP and the UI, before we delve into specific blocks.
As a result from this milestone we want few very basic blocks and a simple config language to map them to tracks (e.v. via basic regexes) to nail the UX of and the interaction of UI and TP.


### 2 Coming up with synth blocks

In this milestone we want to write the C++ code for the blocks.
We should do some research about which synth modules are more appropriate for techno.

### 3 Discuss the mapping of trace to synth

We should research and discuss here how to map portions of the trace to functions that drive the synth.
E.g. things like "the kernel threads will operate in the base octaves / bass line", we need to figure out the tempo generation and so on.

---

## Synth Blocks Reference

This section defines the set of DSP blocks available for the modular synth
engine. The design is informed by the architectures of Monark (Reaktor),
Massive X, and Surge XT (open-source), adapted for a techno-focused palette
that can be driven entirely by trace data.

Every block is a pure function: `(config, input_buffer) -> output_buffer`.
Blocks are stateful (they hold internal DSP state like filter memories, phase
accumulators, etc.) but deterministic: given the same config and input sequence,
they always produce the same output.

### Block Categories

```
┌─────────────────────────────────────────────────────────────────┐
│                     OSCILLATORS (sound sources)                 │
│  PolyBLEP VA │ Wavetable │ FM2 │ Sine │ Noise │ KarplusStrong  │
│  PhaseDist   │ Supersaw                                        │
├─────────────────────────────────────────────────────────────────┤
│                     FILTERS (spectral shaping)                  │
│  MoogLadder │ DiodeLadder │ SVF │ CombFilter │ FormantFilter   │
├─────────────────────────────────────────────────────────────────┤
│                     EFFECTS (signal processing)                 │
│  Waveshaper │ Bitcrusher │ Delay │ Reverb │ Chorus │ Sidechain │
├─────────────────────────────────────────────────────────────────┤
│                     MODULATION (control signals)                │
│  LFO │ ADSR │ EnvelopeFollower │ SampleAndHold                 │
└─────────────────────────────────────────────────────────────────┘
```

---

### Oscillators

#### 1. PolyBLEP VA Oscillator

The workhorse. Generates anti-aliased classic waveforms using polynomial
Band-Limited Step (polyBLEP) corrections at discontinuities. This is the
approach used by Monark for its Minimoog emulation.

**Waveforms:** Sawtooth, Square/Pulse, Triangle

**Algorithm:** A naive waveform (phase accumulator with wrap) is generated,
then a small polynomial correction is applied at each discontinuity to suppress
aliasing. For saw/square, the correction handles the step discontinuity
(polyBLEP). For triangle, the correction handles the slope discontinuity
(polyBLAMP — the integrated form).

The correction operates on only 2 samples per discontinuity, making it
extremely efficient:

```
polyblep(t, dt):
  if t < dt:       t/=dt; return t+t - t*t - 1
  if t > 1.0-dt:   t=(t-1)/dt; return t*t + t+t + 1
  return 0
```

**Parameters:**
- `frequency` (Hz) — driven by note pitch
- `waveform` — saw / square / triangle (enum)
- `pulse_width` (0.1–0.9) — for square wave, ratio of high vs low
- `drift` (0.0–1.0) — adds low-frequency noise to pitch, modeling analog VCO
  instability (the "alive" quality from Monark). Implemented as filtered pink
  noise below ~5 Hz added to the phase increment.

**Techno use:** Acid basslines (saw into diode filter), hard leads (square),
pad layers.

#### 2. Wavetable Oscillator

Enables complex, evolving timbres by scanning through a table of single-cycle
waveforms. This is the core of Massive X.

**Algorithm:**
1. Store N single-cycle waveforms (e.g. 256 frames of 2048 samples each).
2. For anti-aliasing, pre-compute mipmap levels via FFT: for each octave,
   zero harmonics above Nyquist, IFFT back. ~1-2 mipmaps per octave.
3. A phase accumulator indexes into the current frame. Use Hermite (4-point
   cubic) interpolation between samples for quality.
4. The `position` parameter selects which frame (with crossfade between
   adjacent frames).
5. Select mipmap level based on playback frequency; crossfade between adjacent
   mipmap levels for smooth transitions.

**Parameters:**
- `frequency` (Hz)
- `position` (0.0–1.0) — scans through the wavetable frames
- `table_id` — which wavetable to use (we'll ship a curated set)

**Techno use:** Evolving bass, atmospheric textures, modern leads. The
`position` parameter is a great target for trace-driven modulation (e.g.
CPU utilization morphs the timbre).

#### 3. FM Oscillator (2-operator)

Classic Chowning FM synthesis. Two sine-wave operators where one modulates
the other's phase. Creates metallic, bell-like, and aggressive digital timbres
that are impossible with subtractive synthesis.

**Algorithm:**
```
modulator = sin(2π * mod_freq * t)
output    = sin(2π * carrier_freq * t + mod_index * modulator)
```

The carrier:modulator frequency ratio determines harmonicity:
- Integer ratios (1:1, 1:2, 1:3...) → harmonic spectra
- Non-integer ratios (1:1.41, 1:7.13...) → inharmonic/metallic spectra

**Parameters:**
- `carrier_freq` (Hz) — typically the note pitch
- `mod_ratio` (0.0–16.0) — modulator frequency as ratio of carrier
- `mod_index` (0.0–32.0) — modulation depth, controls harmonic richness
- `feedback` (0.0–1.0) — operator self-modulation (output fed back to own
  phase), produces saw-like waveforms at high values
- `mod_ratio_fine` (-1.0–1.0) — detune from integer ratio for inharmonicity

**Techno use:** Metallic percussion, hi-hats, industrial textures, aggressive
bass with high mod_index. The mod_index is an excellent trace-modulation
target (maps naturally to "intensity" of trace activity).

#### 4. Sine Oscillator

A pure sine wave, essential for kick drum synthesis. Uses a quadrature
oscillator (recursive sin/cos pair) for efficiency and phase continuity,
as in Surge XT.

**Algorithm (quadrature):**
```
sin_out = sin_state * cos_inc + cos_state * sin_inc
cos_out = cos_state * cos_inc - sin_state * sin_inc
```
Where `cos_inc = cos(2π * freq / sr)`, `sin_inc = sin(2π * freq / sr)`.

**Parameters:**
- `frequency` (Hz)
- `phase_reset` (bool) — reset phase to zero on trigger (critical for kicks)

**Techno use:** Primary kick drum body, sub-bass, test tones. Combined with a
pitch envelope (fast sweep from ~200Hz down to ~45Hz) this creates the iconic
techno kick.

#### 5. Noise Generator

Generates white, pink, and brown noise. Essential for percussion (hi-hats,
snares), textures, and as a modulation source.

**Algorithms:**
- **White:** xorshift128 PRNG scaled to [-1, 1]
- **Pink (1/f):** Paul Kellet's 6-pole parallel filter method. Six one-pole
  filters at different time constants, summed to approximate 1/f spectrum.
  Accurate to ±0.05 dB above 9.2 Hz.
- **Brown (1/f²):** Leaky integrator on white noise:
  `brown[n] = 0.998 * brown[n-1] + white[n] * 0.02`

**Parameters:**
- `color` — white / pink / brown (enum)
- `level` (0.0–1.0)

**Techno use:** Hi-hat (white noise → tight bandpass → fast amp envelope),
snare body (pink noise → short envelope), atmospheric sweeps (pink, slow
filter sweep), modulation source.

#### 6. Karplus-Strong (Physical Modeling)

A delay-line-based plucked string / metallic sound generator. Inspired by
Surge XT's String oscillator.

**Algorithm:**
1. Fill a delay line of length N = sampleRate/frequency with a noise burst
   (the "excitation").
2. Each sample: read the oldest value, apply a one-pole lowpass
   `y[n] = (1-d)*x[n] + d*y[n-1]`, write back into the delay line.
3. Higher harmonics decay faster than lower ones, converging to a near-sinusoid
   at the fundamental.

**Parameters:**
- `frequency` (Hz) — determines delay line length
- `damping` (0.0–1.0) — controls the lowpass coefficient in feedback, higher =
  faster harmonic decay = duller sound
- `excitation` — burst noise / external input (enum)
- `feedback` (0.0–0.999) — overall decay time

**Enhancements:** Fractional delay via allpass interpolation for precise tuning.
Drum mode: probability-based sign flip in feedback creates drum-like sounds.

**Techno use:** Metallic percussion, plucked textures, hi-hat-like sounds,
unusual resonant tones. The feedback parameter maps well to trace duration.

#### 7. Phase Distortion Oscillator

A sine oscillator whose phase is warped by a nonlinear transfer function,
producing filter-sweep-like spectral changes without an actual filter. Inspired
by the Casio CZ series and Massive X's "Bend" mode.

**Algorithm:** A standard sine lookup, but the linear phase ramp is passed
through a warping function before the table read:
```
output = sin(2π * warp(phase, amount))
```

Different warp functions produce different timbres:
- Saw-like: accelerate first half, decelerate second half
- Square-like: fast rise, hold peak, fast fall, hold trough
- Resonant: emphasize a narrow portion of the phase, creating a formant peak

**Parameters:**
- `frequency` (Hz)
- `warp_type` — saw / square / resonant (enum)
- `amount` (0.0–1.0) — distortion depth. At 0 = pure sine, at 1 = maximum
  warp

**Techno use:** Aggressive, buzzy basses (similar to acid but different
character), evolving pads when amount is modulated.

#### 8. Supersaw Oscillator

Seven detuned sawtooth oscillators stacked together, based on the Roland
JP-8000 algorithm. Creates massive, wide sounds.

**Algorithm:**
- 1 center sawtooth at fundamental frequency
- 6 side sawtooths detuned symmetrically around center
- Detuning follows a nonlinear (exponential) spread curve
- Each saw uses PolyBLEP anti-aliasing
- Free-running phases (no sync) for natural beating

**Parameters:**
- `frequency` (Hz)
- `detune` (0.0–1.0) — controls frequency spread of the 6 side oscillators
- `mix` (0.0–1.0) — balance between center and detuned oscillators

**Techno use:** Epic breakdown chords, trance-influenced pads, massive lead
layers. The detune parameter maps well to trace "spread" (e.g. thread count
or scheduling jitter).

---

### Filters

#### 1. Moog Ladder Filter (24 dB/oct)

The classic warm bass filter, faithfully modeled after the Minimoog's
transistor ladder as in Monark. Uses the Huovilainen non-linear digital model
with zero-delay-feedback topology.

**Algorithm:** Four cascaded one-pole lowpass stages with tanh() saturation
in each stage and a global negative feedback path for resonance:
```
input_fb = tanh(input - 4.0 * resonance * (stage4_prev - 0.5 * input))
g = 1 - exp(-2π * cutoff / sampleRate)

stage1 += g * (tanh(input_fb) - tanh(stage1))
stage2 += g * (tanh(stage1)   - tanh(stage2))
stage3 += g * (tanh(stage2)   - tanh(stage3))
stage4 += g * (tanh(stage3)   - tanh(stage4))

output = stage4
```

The tanh() nonlinearity is the key to the "warm" character: it generates
harmonics, compresses peaks naturally, and prevents resonance from going to
infinity (self-oscillation produces a clean sine).

**Parameters:**
- `cutoff` (20–20000 Hz)
- `resonance` (0.0–1.0, mapped to 0–4 internally; self-oscillation at 1.0)
- `drive` (0.0–1.0) — input gain before the filter, increases saturation

**Optimization:** Replace tanh(x) with fast rational approximation:
`x * (27 + x*x) / (27 + 9*x*x)`

**Techno use:** Deep bass filtering, warm sweeps, classic techno bass.

#### 2. Diode Ladder Filter (18 dB/oct, TB-303 Style)

THE acid filter. Models the Roland TB-303's diode ladder topology which
produces the distinctive "squelchy" resonance that defines acid techno.

**Key differences from Moog ladder:**
- 4 diode stages but the last operates differently → effective 18 dB/oct
  (3-pole) rolloff instead of 24 dB/oct
- Stages are NOT electrically isolated — they interact nonlinearly
- Resonance has a more aggressive, "screaming" quality
- Bass doesn't drop away as much at high resonance (unlike Moog)

**Algorithm:** Similar ZDF approach to the Moog ladder but with diode
saturation curves (asymmetric, sharper) instead of tanh. Requires at least
2x oversampling to reduce frequency warping and aliasing from the
nonlinearities.

**Parameters:**
- `cutoff` (20–20000 Hz)
- `resonance` (0.0–1.0)
- `accent` (0.0–1.0) — simultaneously increases filter envelope depth AND
  shortens envelope decay AND boosts output, modeling the 303's accent circuit

**Techno use:** Acid basslines (the raison d'être), squelchy leads, anything
that needs to "scream."

#### 3. State Variable Filter (SVF, 12 dB/oct)

The most versatile filter topology. Provides simultaneous lowpass, highpass,
bandpass, and notch outputs from a single computation. Based on the Chamberlin
digital SVF, run at 2x to ensure stability at high frequencies.

**Algorithm (Chamberlin, per-sample, run inner loop 2x):**
```
f = 2.0 * sin(π * cutoff / sampleRate)
q = 1.0 / Q

lowpass  += f * bandpass
highpass  = input - lowpass - q * bandpass
bandpass += f * highpass
notch     = highpass + lowpass
```

All four outputs available simultaneously — the routing config determines
which one is used.

**Parameters:**
- `cutoff` (20–20000 Hz)
- `resonance` (0.5–50.0, as Q factor)
- `mode` — lowpass / highpass / bandpass / notch (enum, selects output tap)

**Techno use:** DJ-style filter sweeps (LP/HP), resonant percussion shaping
(BP at high Q), notch for phasing effects. The multi-mode nature makes this
the default "utility filter."

#### 4. Comb Filter

A short delay line with feedback, creating a harmonic series of resonant peaks
(or notches). Essential for metallic textures and Karplus-Strong-adjacent
sounds.

**Algorithm (feedback comb):**
```
output = input + feedback * buffer[readPos]
buffer[writePos] = output
```

Resonant frequencies appear at multiples of `sampleRate / delayLength`.
For tuned combs: `delayLength = sampleRate / frequency`.

**Parameters:**
- `frequency` (Hz) — determines delay length
- `feedback` (-0.999–0.999) — positive = resonant peaks at harmonics, negative
  = resonant peaks at odd harmonics only (hollow, clarinet-like)
- `damping` (0.0–1.0) — one-pole lowpass in the feedback loop, higher
  harmonics decay faster

**Techno use:** Metallic percussion, sci-fi textures, flanger-like effects
when frequency is modulated by an LFO.

#### 5. Formant Filter

Parallel bandpass filters tuned to vowel formant frequencies, creating
speech-like resonances. Three second-order biquad bandpasses summed together.

**Vowel presets (F1, F2, F3 in Hz):**
```
"ah": 700, 1220, 2600   (open, aggressive)
"ee": 270, 2290, 3010   (bright, cutting)
"oo": 300,  870, 2240   (dark, round)
"eh": 530, 1840, 2480   (mid, nasal)
"oh": 590,  880, 2540   (round, warm)
```

**Parameters:**
- `vowel` (0.0–1.0) — morphs continuously between vowel presets by
  interpolating formant frequencies
- `resonance` (1.0–30.0) — Q factor of each bandpass (higher = more
  pronounced formants)

**Techno use:** "Talking" bass, robotic vocal textures, alien atmospheres.
Mapping trace data to the vowel parameter creates an eerie "the machine is
speaking" effect.

---

### Effects

#### 1. Waveshaper / Distortion

From subtle warmth to face-melting destruction. Multiple transfer function
modes, all requiring 2–4x oversampling to avoid aliasing.

**Modes:**
- **Soft saturation:** `y = tanh(drive * x)` — warm, tube-like, odd harmonics
- **Hard clip:** `y = clamp(drive * x, -1, 1)` — harsh, many harmonics
- **Wavefold:** reflects signal at ±threshold, creating dense spectra
  (West Coast synthesis style)
- **Asymmetric:** `y = tanh(x + bias) - tanh(bias)` — adds even harmonics
  (fuller, more "analog")
- **Bitwise:** XOR/AND/OR operations on the sample's integer representation

**Parameters:**
- `mode` — soft / hard / fold / asymmetric / bitwise (enum)
- `drive` (1.0–100.0) — input gain
- `mix` (0.0–1.0) — dry/wet blend

**Techno use:** Bass grit, lead aggression, percussion crunch. Essential for
giving digital sources an analog edge.

#### 2. Bitcrusher

Reduces bit depth and/or sample rate for lo-fi digital destruction.

**Algorithm:**
```
// Bit depth reduction
max_val = 2^(bits-1) - 1
output = round(input * max_val) / max_val

// Sample rate reduction
if (++counter >= factor) { counter = 0; hold = input; }
output = hold
```

**Parameters:**
- `bits` (1–16) — bit depth
- `downsample` (1–64) — sample rate reduction factor

**Techno use:** Lo-fi textures, retro digital character, industrial grit.
8-bit at 1/4 rate gives aggressive industrial vibes.

#### 3. Delay

Tempo-synced stereo delay with filtered feedback for dub techno atmospheres.

**Algorithm:**
```
buffer[writePos] = input + feedback * lowpass(buffer[readPos])
output = mix * buffer[readPos] + (1 - mix) * input
```

**Configurations:**
- **Stereo:** Independent L/R delay times
- **Ping-pong:** L feeds R, R feeds L, creating bouncing echoes

**Parameters:**
- `time_l`, `time_r` (in samples or BPM-synced subdivisions)
- `feedback` (0.0–0.95) — echo decay
- `damping` (0.0–1.0) — lowpass in feedback (each echo gets darker)
- `mix` (0.0–1.0) — dry/wet
- `mode` — stereo / pingpong (enum)

**Techno use:** Dub techno atmospheres (long delay, high feedback, heavy
damping), rhythmic echoes on percussion, spatial width.

#### 4. Reverb (Freeverb)

Algorithmic reverb based on the Schroeder/Moorer architecture as implemented
in Freeverb. Lightweight and sufficient for our purposes.

**Architecture:**
- 8 parallel lowpass-feedback-comb filters → sum → 4 series allpass filters
- Comb delay lengths (at 44.1 kHz): {1617, 1557, 1491, 1422, 1356, 1277,
  1188, 1116} samples
- Allpass delay lengths: {225, 556, 441, 341} samples
- Stereo: add 23 samples to all right-channel delay lengths

**Parameters:**
- `room_size` (0.0–1.0) — maps to comb feedback gain (RT60)
- `damping` (0.0–1.0) — lowpass coefficient in comb feedback
- `mix` (0.0–1.0) — dry/wet

**Techno use:** Atmospheric depth on pads, cavernous percussion, spatial
context. Use sparingly on bass (muddies the low end).

#### 5. Chorus

Modulated delay line creating movement and stereo width.

**Algorithm:**
```
delay_time = mid_delay + depth * sin(2π * rate * t + phase_offset)
output = (1 - mix) * input + mix * delay_line.read(delay_time)
```

**Parameters:**
- `rate` (0.1–5.0 Hz) — LFO speed
- `depth` (0.0–1.0) — modulation amount (maps to 0–10ms delay swing)
- `mix` (0.0–1.0)
- `voices` (1–4) — number of modulated delay taps with spread phases

**Techno use:** Pad width, subtle detuning on leads, thickening synth layers.
Stereo offset (L/R LFOs 90° apart) creates wide stereo image.

#### 6. Sidechain Compressor

THE signature techno pumping effect. Ducks the signal based on the amplitude
of a sidechain input (typically the kick drum channel).

**Algorithm:**
```
// Level detection on sidechain signal
level_dB = 20 * log10(envelope_follow(sidechain))

// Gain computation
if (level_dB > threshold_dB)
  reduction_dB = (level_dB - threshold_dB) * (1 - 1/ratio)

// Smoothing (attack/release)
smoothed = attack/release one-pole on reduction_dB

// Apply
output = input * dB_to_linear(-smoothed)
```

**Parameters:**
- `threshold` (-60–0 dB) — level above which compression starts
- `ratio` (1:1–inf:1) — compression amount (inf = limiter)
- `attack` (0.1–50 ms) — how fast compression engages
- `release` (50–500 ms) — how fast compression releases (THIS controls the
  pump shape)
- `sidechain_source` — which channel drives the compression

**Techno use:** Rhythmic pumping on pads/bass (fast attack, 100–400ms release),
creating the breathing groove that defines techno and house. The release time
is the most important parameter — it shapes the rhythmic feel.

---

### Modulation Sources

Modulation sources generate control signals (not audio) that drive parameters
of other blocks. They are evaluated once per block (not per-sample) for
efficiency.

#### 1. LFO (Low Frequency Oscillator)

Periodic modulation at sub-audio rates.

**Waveforms:**
- Sine, Triangle, Saw Up, Saw Down, Square, Sample & Hold (random steps)

**Parameters:**
- `rate` (0.01–20.0 Hz, or BPM-synced divisions)
- `waveform` — sine / triangle / saw_up / saw_down / square / s_and_h (enum)
- `depth` (0.0–1.0) — output amplitude
- `phase` (0.0–1.0) — initial phase offset (useful for stereo/multi-voice)

**Techno use:** Filter cutoff sweeps (sine, slow), tremolo (sine, fast),
rhythmic gating (square, tempo-synced), random modulation (S&H).

#### 2. ADSR Envelope

Shapes the temporal evolution of each note. Uses exponential segments with
configurable curvature for natural-sounding dynamics.

**Algorithm:** One-pole exponential approach toward target values with
configurable target ratios (small ratio = steep exponential curve, large =
nearly linear).

```
// Per-sample (conceptual):
switch (state):
  ATTACK:  output += (1.0 + overshoot - output) * attack_coeff
  DECAY:   output += (sustain - output) * decay_coeff
  SUSTAIN: output = sustain_level
  RELEASE: output += (0.0 - undershoot - output) * release_coeff
```

**Parameters:**
- `attack` (0–5000 ms)
- `decay` (0–10000 ms)
- `sustain` (0.0–1.0) — level, not time
- `release` (0–10000 ms)

**Techno use:** Kick pitch envelope (A=0, D=50ms, S=0, R=50ms), acid filter
envelope (A=0, D=100–300ms, S=0), pad amplitude (A=500ms, S=0.8, R=3000ms).

#### 3. Envelope Follower

Extracts the amplitude envelope of an input signal, converting it to a
control signal. Useful for making one trace channel's activity modulate
another channel's parameters.

**Algorithm:**
```
abs_input = |input|
if abs_input > envelope:
  envelope += (abs_input - envelope) * attack_coeff
else:
  envelope += (abs_input - envelope) * release_coeff
```

**Parameters:**
- `attack` (0.1–100 ms) — how fast it tracks rising levels
- `release` (1–1000 ms) — how fast it tracks falling levels
- `source` — which signal to follow

**Techno use:** Dynamic filtering driven by trace activity (busy thread →
brighter filter), cross-modulation between trace channels ("when thread A
is active, thread B's sound gets louder").

#### 4. Sample and Hold

Samples a source signal at regular intervals and holds the value until the
next sample. Creates stepped, staircase-like modulation.

**Algorithm:**
```
if trigger_detected:
  held_value = source_signal
output = held_value
```

Optional slew limiter for smooth gliding between values:
`output += slew_rate * (held_value - output)`

**Parameters:**
- `rate` (0.1–50 Hz, or BPM-synced) — sampling rate
- `slew` (0.0–1.0) — smoothing between steps (0 = hard steps, 1 = smooth
  glide)
- `source` — what signal to sample (typically noise for random modulation)

**Techno use:** Generative patches (random filter cutoff per beat), glitchy
modulation, trace-driven randomness (sample a trace metric at regular
intervals to create stepped control patterns).

---

### Techno Synthesis Recipes

These recipes show how to combine blocks into useful instruments, guiding the
trace-to-synth mapping decisions.

#### Hard Kick
```
[Sine Osc] → [Waveshaper:soft] → output
  ↑ freq modulated by [ADSR: A=0, D=30ms, S=0] (200Hz → 45Hz)
  ↑ amp modulated by [ADSR: A=0, D=300ms, S=0]
  + [Noise:white] → [SVF:bandpass, 3kHz, Q=10] → [ADSR: A=0, D=2ms] → mix (click layer)
```

#### Acid Bass
```
[PolyBLEP:saw] → [DiodeLadder: high reso] → [Waveshaper:soft] → output
  ↑ filter cutoff modulated by [ADSR: A=0, D=150ms, S=0.1]
  ↑ accent modulates envelope depth + amplitude simultaneously
  ↑ portamento (exponential glide) between notes
```

#### Atmospheric Pad
```
[Supersaw: detune=0.4] → [MoogLadder: cutoff=2kHz] → [Chorus] → [Reverb] → output
  ↑ filter cutoff modulated by [LFO:sine, 0.1Hz]
  ↑ amp modulated by [ADSR: A=1s, S=0.8, R=3s]
  ↑ detune modulated by [LFO:triangle, 0.05Hz]
```

#### Metallic Percussion
```
[KarplusStrong: damping=0.3] → [SVF:bandpass] → [Bitcrusher: 12bit] → output
  ↑ excitation = white noise burst, 5ms
  ↑ amp modulated by [ADSR: A=0, D=200ms, S=0]
```

#### Industrial Texture
```
[FM2: ratio=7.13, index=12] → [Waveshaper:fold] → [CombFilter] → output
  ↑ mod_index modulated by [EnvelopeFollower on trace activity]
  ↑ comb freq modulated by [SampleAndHold: rate=2Hz, source=noise]
```

#### Robotic Voice
```
[PolyBLEP:saw] → [FormantFilter: vowel swept by LFO] → output
  ↑ vowel modulated by [LFO:triangle, 0.5Hz] or trace-driven
  ↑ amp modulated by [ADSR: A=10ms, D=100ms, S=0.6, R=200ms]
```

---

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

### Config proto sketch

The wiring is defined in a config proto with three main sections:

```proto
message SynthConfig {
  // Global parameters
  float bpm = 1;           // 128 default
  float time_dilation = 2; // compute from trace_duration / track_duration
  string scale = 3;        // "a_minor_pentatonic", "phrygian", etc.

  // Instrument bank definitions (128 slots)
  repeated InstrumentSlot slots = 4;
}

message InstrumentSlot {
  int32 index = 1;                   // 0..127
  string name = 2;                   // human-readable
  BlockChain chain = 3;              // the DSP graph
  TriggerRule trigger = 4;           // SQL query + mapping rules
  ModulationRules mod = 5;           // parameter-to-trace mappings
}

message TriggerRule {
  string sql_query = 1;              // e.g. SELECT ts, dur, name FROM slice WHERE track_id=...
  string pitch_expr = 2;             // e.g. "hash(name) % 5"
  string velocity_expr = 3;          // e.g. "log(dur) / 10"
  string duration_expr = 4;          // e.g. "dur * D"
}
```

The UI builds this config visually; TP consumes it to drive the synth blocks.
Default configs for "Android system trace" vs "browser trace" vs "server
trace" can be shipped as presets.
