
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
