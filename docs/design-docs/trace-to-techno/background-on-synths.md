# Background on Modular Synths

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
  another module can continuously change the pitch.

### Sound shapers

- **VCF** (Voltage Controlled Filter) -- low-pass, high-pass, band-pass. Has a
  cutoff frequency and resonance, both controllable by CV. This is what makes a
  synth sound "warm" or "squelchy" vs a raw oscillator.
- **VCA** (Voltage Controlled Amplifier) -- a volume knob controlled by a
  signal. Feed it an envelope to shape the loudness over time.

### Control sources

- **Envelope Generator (EG)** -- outputs a one-shot shape when triggered.
  Classic is ADSR: Attack (ramp up), Decay (drop to...), Sustain (hold level),
  Release (fade out after trigger ends). Patch into a VCA to give notes a
  shape -- otherwise the oscillator just drones.
- **LFO** -- slow oscillator used for vibrato (patch into VCO pitch), tremolo
  (patch into VCA), filter sweeps (patch into VCF cutoff), etc.

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
layer (rhythm) vs which become CV modulation (timbre/texture).
