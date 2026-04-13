#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Generates test/data/music_synth_presets.json.

Emits 128 presets (16 templates x 8 variations per template). Each preset is a
self-contained SynthPatch that takes input only from a TestPatternSource (the
8-bar Am-G-F-E arpeggio in A harmonic minor) and produces audio on a module
named "master".

The JSON schema is designed to map 1:1 onto the SynthPatch proto so the
renderer can use protobuf json_format.ParseDict to convert to binary.

Running this script regenerates the JSON in place. It has no external Python
dependencies.
"""

import json
import os
import sys
from pathlib import Path

# Where to write the output JSON. We write the same file in two places:
#   - test/data/...          -> canonical location, used by C++ tests and
#                                the preset render script
#   - ui/src/assets/sound_synth/... -> served by the UI dev server.
# Keeping them in sync avoids brittle symlinks (the UI build's file walker
# skips symbolic links).
REPO_ROOT = Path(__file__).resolve().parents[2]
OUTPUT_FILE = REPO_ROOT / "test" / "data" / "music_synth_presets.json"
UI_COPY = (
    REPO_ROOT / "ui" / "src" / "assets" / "sound_synth" /
    "music_synth_presets.json"
)

# ---------------------------------------------------------------------------
# Helpers for building modules and wires.
# ---------------------------------------------------------------------------


def _mod(id: str, kind: str, cfg: dict) -> dict:
  """Builds one SynthModule dict with the given oneof kind and config."""
  return {"id": id, kind: cfg}


def _wire(from_id: str,
          to_id: str,
          to_port: str,
          from_port: str = "out",
          scale: float | None = None,
          offset: float | None = None) -> dict:
  w = {
      "from_module": from_id,
      "from_port": from_port,
      "to_module": to_id,
      "to_port": to_port
  }
  if scale is not None:
    w["scale"] = scale
  if offset is not None:
    w["offset"] = offset
  return w


def _make_arp_source() -> dict:
  """The common 8-bar Am-G-F-E arpeggio driver used by every preset."""
  return _mod("arp", "test_pattern_source", {
      "mode": "ARPEGGIO",
      "bpm": 128,
      "bars": 8
  })


def _make_master() -> dict:
  return _mod("master", "mixer", {})


def _preset(name: str, category: str, description: str, modules: list[dict],
            wires: list[dict]) -> dict:
  """Wraps modules+wires into a preset entry. Prepends the shared arp source
  and master mixer so individual templates don't have to."""
  full_modules = [_make_arp_source()] + modules + [_make_master()]
  return {
      "name": name,
      "category": category,
      "description": description,
      "patch": {
          "modules": full_modules,
          "wires": wires
      },
  }


# ---------------------------------------------------------------------------
# Templates. Each returns a list of 8 preset dicts.
# ---------------------------------------------------------------------------


def _kick_variations() -> list[dict]:
  # A kick ignores the arpeggio freq — it has its own pitch envelope driven
  # by a short Adsr that sweeps a sine oscillator. Gate from arp retriggers
  # it on every note.
  variants = [
      # (name_suffix, decay_ms, pitch_start_hz, pitch_end_hz, drive, desc)
      ("classic", 300, 220.0, 45.0, 2.0, "Deep classic 909-ish kick"),
      ("hard", 200, 260.0, 50.0, 4.0, "Hard distorted industrial kick"),
      ("sub", 400, 90.0, 40.0, 1.5, "Long sub-heavy kick, low pitch sweep"),
      ("short", 120, 240.0, 55.0, 3.0, "Short punchy kick, fast decay"),
      ("long", 600, 200.0, 42.0, 1.2, "Long slow-release kick for dub"),
      ("tight", 80, 300.0, 60.0, 5.0, "Tight clicky kick with hard drive"),
      ("deep", 500, 180.0, 38.0, 2.0, "Deep sub kick, slow pitch sweep"),
      ("rave", 250, 350.0, 50.0, 3.5, "Rave-style kick, bright attack"),
  ]
  out = []
  for i, (suffix, decay, pf0, pf1, drive, desc) in enumerate(variants):
    # Pitch envelope: short decay to map onto the pitch range via scale/offset
    # on the wire. The Adsr outputs [0,1]; we map to pitch span using wire
    # scale=(pf0-pf1) and offset=pf1.
    pitch_decay = max(20.0, decay * 0.15)
    modules = [
        _mod(
            "pitch_env", "adsr", {
                "attack_ms": 0.1,
                "decay_ms": pitch_decay,
                "sustain": 0.0,
                "release_ms": 1.0
            }),
        _mod("amp_env", "adsr", {
            "attack_ms": 0.1,
            "decay_ms": decay,
            "sustain": 0.0,
            "release_ms": 1.0
        }),
        _mod("osc", "classic_osc", {
            "waveform": "SINE",
            "base_freq_hz": 0.0
        }),
        _mod("drive_stage", "waveshaper", {
            "mode": "SOFT_TANH",
            "drive": drive,
            "mix": 1.0
        }),
        _mod("vca", "vca", {}),
    ]
    wires = [
        _wire("arp", "pitch_env", "gate"),
        _wire("arp", "amp_env", "gate"),
        # pitch_env (0..1) → freq (pf1..pf0) on the sine oscillator.
        _wire("pitch_env", "osc", "freq", scale=(pf0 - pf1), offset=pf1),
        _wire("osc", "vca", "in"),
        _wire("amp_env", "vca", "gain"),
        _wire("vca", "drive_stage", "in"),
        _wire("drive_stage", "master", "in"),
    ]
    out.append(_preset(f"kick_{suffix}", "drum", desc, modules, wires))
  return out


def _sub_kick_variations() -> list[dict]:
  # A simple sub-bass hit (no pitch envelope) that uses the arpeggio's low
  # notes. Tracks the arp freq but transposed down an octave.
  variants = [
      ("short", 150, "Short thumpy sub-kick"),
      ("medium", 300, "Medium sub-kick with tail"),
      ("long", 500, "Long sustained sub-kick"),
      ("punch", 100, "Punchy sub-kick, no tail"),
      ("deep", 800, "Deep slow-release sub"),
      ("warm", 400, "Warm sub with slight saturation"),
      ("hard", 200, "Hard-driven sub-kick"),
      ("soft", 250, "Soft sine sub-kick"),
  ]
  out = []
  for suffix, decay, desc in variants:
    drive = 3.0 if "hard" in suffix else 1.5
    modules = [
        _mod("amp_env", "adsr", {
            "attack_ms": 0.1,
            "decay_ms": decay,
            "sustain": 0.0,
            "release_ms": 1.0
        }),
        _mod("osc", "classic_osc", {
            "waveform": "SINE",
            "base_freq_hz": 0.0
        }),
        _mod("drive_stage", "waveshaper", {
            "mode": "SOFT_TANH",
            "drive": drive,
            "mix": 1.0
        }),
        _mod("vca", "vca", {}),
    ]
    wires = [
        _wire("arp", "amp_env", "gate"),
        # Track arp freq / 2 (down an octave for sub-register).
        _wire("arp", "osc", "freq", from_port="freq", scale=0.5),
        _wire("osc", "vca", "in"),
        _wire("amp_env", "vca", "gain"),
        _wire("vca", "drive_stage", "in"),
        _wire("drive_stage", "master", "in"),
    ]
    out.append(_preset(f"sub_kick_{suffix}", "drum", desc, modules, wires))
  return out


def _noise_perc_variations(base_name: str,
                           category: str,
                           template_desc: str,
                           cutoff_pairs: list,
                           mode: str = "HIGHPASS") -> list[dict]:
  """Shared builder for noise-plus-filter percussion (hats, claps)."""
  out = []
  for i, (suffix, cutoff, q, tilt, decay, desc) in enumerate(cutoff_pairs):
    modules = [
        _mod("noise", "noise_osc", {
            "tilt": tilt,
            "seed": 42 + i
        }),
        _mod("filt", "svf", {
            "mode": mode,
            "base_cutoff_hz": cutoff,
            "base_q": q
        }),
        _mod("amp_env", "adsr", {
            "attack_ms": 0.1,
            "decay_ms": decay,
            "sustain": 0.0,
            "release_ms": 1.0
        }),
        _mod("vca", "vca", {}),
    ]
    wires = [
        _wire("arp", "amp_env", "gate"),
        _wire("noise", "filt", "in"),
        _wire("filt", "vca", "in"),
        _wire("amp_env", "vca", "gain"),
        _wire("vca", "master", "in"),
    ]
    out.append(
        _preset(f"{base_name}_{suffix}", category, f"{template_desc}: {desc}",
                modules, wires))
  return out


def _closed_hat_variations() -> list[dict]:
  return _noise_perc_variations("closed_hat", "drum", "Closed hi-hat", [
      ("classic", 8000, 2.0, 0.0, 40, "bright white"),
      ("crisp", 10000, 3.0, 0.0, 30, "very crisp short"),
      ("dusty", 6000, 2.0, 0.3, 50, "pink-tinged dusty"),
      ("metallic", 12000, 5.0, 0.0, 35, "high-Q metallic"),
      ("soft", 5000, 1.5, 0.4, 60, "pink softer hat"),
      ("tight", 9000, 4.0, 0.0, 25, "tight fast decay"),
      ("dark", 4000, 2.0, 0.5, 45, "dark pinkish hat"),
      ("bright", 11000, 3.0, 0.0, 35, "bright open feel"),
  ])


def _open_hat_variations() -> list[dict]:
  return _noise_perc_variations("open_hat", "drum", "Open hi-hat", [
      ("classic", 7000, 2.0, 0.0, 200, "bright long open hat"),
      ("washy", 5000, 1.5, 0.3, 300, "pink washy long"),
      ("bright", 9000, 2.5, 0.0, 250, "bright clear open"),
      ("dark", 4000, 2.0, 0.5, 220, "darker pink open"),
      ("short", 7000, 2.0, 0.0, 150, "shorter open"),
      ("long", 6000, 2.0, 0.2, 400, "very long open"),
      ("shimmer", 11000, 3.0, 0.0, 280, "shimmering top"),
      ("resonant", 8000, 4.0, 0.0, 230, "resonant high-Q open"),
  ])


def _snare_variations() -> list[dict]:
  # Snare = noise BP + sine body + amp env. Sine body ≈ 200 Hz.
  variants = [
      # (suffix, bp_cutoff, bp_q, noise_tilt, body_freq, decay_ms, desc)
      ("classic", 1500, 3.0, 0.3, 200.0, 180, "Classic techno snare"),
      ("tight", 2000, 4.0, 0.2, 220.0, 90, "Tight bright snare"),
      ("deep", 1200, 2.5, 0.4, 180.0, 250, "Deep fat snare"),
      ("crack", 2500, 5.0, 0.1, 240.0, 120, "Cracking bright snare"),
      ("pink", 1500, 3.0, 0.5, 190.0, 200, "Softer pink snare"),
      ("metallic", 3000, 8.0, 0.0, 260.0, 150, "Metallic ringing snare"),
      ("rock", 1800, 3.5, 0.3, 210.0, 180, "Rock-style snare"),
      ("rimshot", 4000, 6.0, 0.1, 300.0, 100, "Rim-shot style"),
  ]
  out = []
  for i, (suffix, bp_c, bp_q, tilt, body_f, decay, desc) in enumerate(variants):
    modules = [
        _mod("noise", "noise_osc", {
            "tilt": tilt,
            "seed": 123 + i
        }),
        _mod("noise_filt", "svf", {
            "mode": "BANDPASS",
            "base_cutoff_hz": bp_c,
            "base_q": bp_q
        }),
        _mod("body", "classic_osc", {
            "waveform": "SINE",
            "base_freq_hz": body_f
        }),
        _mod("amp_env", "adsr", {
            "attack_ms": 0.1,
            "decay_ms": decay,
            "sustain": 0.0,
            "release_ms": 1.0
        }),
        _mod("mix", "mixer", {}),
        _mod("vca", "vca", {}),
    ]
    wires = [
        _wire("arp", "amp_env", "gate"),
        _wire("noise", "noise_filt", "in"),
        _wire("noise_filt", "mix", "in"),
        _wire("body", "mix", "in"),
        _wire("mix", "vca", "in"),
        _wire("amp_env", "vca", "gain"),
        _wire("vca", "master", "in"),
    ]
    out.append(_preset(f"snare_{suffix}", "drum", desc, modules, wires))
  return out


def _clap_variations() -> list[dict]:
  # Single-stage clap (no multi-tap in v1). Future: chain 3 offset envelopes.
  return _noise_perc_variations(
      "clap",
      "drum",
      "Hand clap", [
          ("classic", 1500, 3.0, 0.2, 70, "classic bandpass clap"),
          ("tight", 1800, 4.0, 0.1, 50, "tight crack clap"),
          ("wide", 1200, 2.5, 0.3, 90, "wider fatter clap"),
          ("snappy", 2000, 5.0, 0.1, 60, "snappy bright clap"),
          ("dusty", 1000, 2.0, 0.4, 80, "dusty low clap"),
          ("bright", 2500, 4.0, 0.0, 55, "bright high clap"),
          ("long", 1500, 3.0, 0.2, 120, "long tail clap"),
          ("ghetto", 1300, 6.0, 0.1, 65, "high-Q ghetto clap"),
      ],
      mode="BANDPASS")


def _tom_variations() -> list[dict]:
  # Tom = sine + pitch env + amp env. Similar to kick but different freq.
  variants = [
      ("low", 60, "Low floor tom", 100.0, 70.0),
      ("low_mid", 80, "Low-mid tom", 130.0, 90.0),
      ("mid", 120, "Mid tom", 160.0, 110.0),
      ("high_mid", 150, "High-mid tom", 200.0, 140.0),
      ("high", 180, "High tom", 250.0, 170.0),
      ("tight", 70, "Tight punchy tom", 180.0, 120.0),
      ("long", 250, "Long resonant tom", 140.0, 95.0),
      ("rack", 200, "Rack tom", 220.0, 160.0),
  ]
  out = []
  for suffix, decay, desc, pf0, pf1 in variants:
    modules = [
        _mod(
            "pitch_env", "adsr", {
                "attack_ms": 0.1,
                "decay_ms": max(20.0, decay * 0.2),
                "sustain": 0.0,
                "release_ms": 1.0
            }),
        _mod("amp_env", "adsr", {
            "attack_ms": 0.1,
            "decay_ms": decay,
            "sustain": 0.0,
            "release_ms": 1.0
        }),
        _mod("osc", "classic_osc", {
            "waveform": "SINE",
            "base_freq_hz": 0.0
        }),
        _mod("vca", "vca", {}),
    ]
    wires = [
        _wire("arp", "pitch_env", "gate"),
        _wire("arp", "amp_env", "gate"),
        _wire("pitch_env", "osc", "freq", scale=(pf0 - pf1), offset=pf1),
        _wire("osc", "vca", "in"),
        _wire("amp_env", "vca", "gain"),
        _wire("vca", "master", "in"),
    ]
    out.append(_preset(f"tom_{suffix}", "drum", desc, modules, wires))
  return out


def _acid_bass_variations() -> list[dict]:
  # Saw → Moog ladder → filter env → amp env → soft drive. Tracks the arp freq
  # but transposed down 1 octave so it sits in the bass register.
  variants = [
      ("classic", 600, 0.80, 200, 3.0, "Classic 303 acid bass"),
      ("squelchy", 400, 0.92, 150, 2.0, "Squelchy high-reso acid"),
      ("dark", 800, 0.70, 250, 2.0, "Dark warmer acid"),
      ("bright", 1200, 0.75, 180, 4.0, "Bright aggressive acid"),
      ("fat", 500, 0.85, 220, 1.5, "Fat full-bodied acid"),
      ("screaming", 1000, 0.95, 120, 5.0, "Screaming high-reso acid"),
      ("mellow", 300, 0.60, 300, 1.5, "Mellow low-cutoff acid"),
      ("distorted", 700, 0.80, 200, 6.0, "Heavily distorted acid"),
  ]
  out = []
  for suffix, cutoff, reso, decay, drive, desc in variants:
    modules = [
        _mod("osc", "classic_osc", {
            "waveform": "SAW",
            "base_freq_hz": 0.0
        }),
        _mod(
            "filt_env", "adsr", {
                "attack_ms": 0.1,
                "decay_ms": decay,
                "sustain": 0.1,
                "release_ms": 100.0
            }),
        _mod("amp_env", "adsr", {
            "attack_ms": 0.5,
            "decay_ms": decay,
            "sustain": 0.5,
            "release_ms": 80.0
        }),
        _mod("filt", "moog_ladder", {
            "base_cutoff_hz": cutoff,
            "base_resonance": reso,
            "drive": 1.0
        }),
        _mod("drive_stage", "waveshaper", {
            "mode": "SOFT_TANH",
            "drive": drive,
            "mix": 0.7
        }),
        _mod("vca", "vca", {}),
    ]
    wires = [
        _wire("arp", "amp_env", "gate"),
        _wire("arp", "filt_env", "gate"),
        # Track arp freq / 2 (one octave down for bass register).
        _wire("arp", "osc", "freq", from_port="freq", scale=0.5),
        _wire("osc", "filt", "in"),
        # Filter envelope modulates cutoff by up to ~3 kHz.
        _wire("filt_env", "filt", "cutoff_mod", scale=3000.0),
        _wire("filt", "vca", "in"),
        _wire("amp_env", "vca", "gain"),
        _wire("vca", "drive_stage", "in"),
        _wire("drive_stage", "master", "in"),
    ]
    out.append(_preset(f"acid_bass_{suffix}", "bass", desc, modules, wires))
  return out


def _reese_bass_variations() -> list[dict]:
  # Supersaw → moog ladder → amp env. Detuned saws give the "reese" fatness.
  variants = [
      ("classic", 800, 0.3, 0.4, 0.6, "Classic reese bass"),
      ("wide", 700, 0.5, 0.6, 0.5, "Wide detuned reese"),
      ("tight", 900, 0.2, 0.3, 0.7, "Tight focused reese"),
      ("massive", 600, 0.8, 0.8, 0.4, "Massive wide reese"),
      ("dark", 500, 0.4, 0.5, 0.5, "Dark warm reese"),
      ("bright", 1200, 0.3, 0.4, 0.6, "Bright cutting reese"),
      ("gritty", 700, 0.5, 0.5, 0.5, "Gritty saturated reese"),
      ("smooth", 900, 0.2, 0.3, 0.6, "Smooth clean reese"),
  ]
  out = []
  for suffix, cutoff, detune, mix, reso, desc in variants:
    modules = [
        _mod("osc", "super_osc", {
            "base_freq_hz": 0.0,
            "detune": detune,
            "mix": mix
        }),
        _mod("filt", "moog_ladder", {
            "base_cutoff_hz": cutoff,
            "base_resonance": reso,
            "drive": 1.0
        }),
        _mod(
            "amp_env", "adsr", {
                "attack_ms": 5.0,
                "decay_ms": 200.0,
                "sustain": 0.8,
                "release_ms": 200.0
            }),
        _mod("vca", "vca", {}),
    ]
    wires = [
        _wire("arp", "amp_env", "gate"),
        _wire("arp", "osc", "freq", from_port="freq", scale=0.5),
        _wire("osc", "filt", "in"),
        _wire("filt", "vca", "in"),
        _wire("amp_env", "vca", "gain"),
        _wire("vca", "master", "in"),
    ]
    out.append(_preset(f"reese_bass_{suffix}", "bass", desc, modules, wires))
  return out


def _sub_bass_variations() -> list[dict]:
  # Pure sine sub-bass. Follows the arp two octaves down.
  variants = [
      ("pure", "Pure sine sub", 1.0),
      ("warm", "Warm saturated sub", 2.0),
      ("thick", "Thick harmonic sub", 3.0),
      ("hard", "Hard-driven sub", 5.0),
      ("gentle", "Gentle soft sub", 1.2),
      ("round", "Round smooth sub", 1.5),
      ("fat", "Fat distorted sub", 4.0),
      ("simple", "Simple clean sub", 1.0),
  ]
  out = []
  for suffix, desc, drive in variants:
    modules = [
        _mod("osc", "classic_osc", {
            "waveform": "SINE",
            "base_freq_hz": 0.0
        }),
        _mod(
            "amp_env", "adsr", {
                "attack_ms": 10.0,
                "decay_ms": 100.0,
                "sustain": 0.9,
                "release_ms": 200.0
            }),
        _mod("drive_stage", "waveshaper", {
            "mode": "SOFT_TANH",
            "drive": drive,
            "mix": 0.8
        }),
        _mod("vca", "vca", {}),
    ]
    wires = [
        _wire("arp", "amp_env", "gate"),
        _wire("arp", "osc", "freq", from_port="freq", scale=0.25),
        _wire("osc", "vca", "in"),
        _wire("amp_env", "vca", "gain"),
        _wire("vca", "drive_stage", "in"),
        _wire("drive_stage", "master", "in"),
    ]
    out.append(_preset(f"sub_bass_{suffix}", "bass", desc, modules, wires))
  return out


def _fm_bass_variations() -> list[dict]:
  # FM osc → SVF LP → amp env. Metallic / aggressive bass.
  variants = [
      ("metallic", 1.0, 5.0, 0.0, 1000, "Metallic FM bass (1:1)"),
      ("bell", 1.4, 8.0, 0.0, 1500, "Bell-like inharmonic FM"),
      ("saw", 1.0, 15.0, 0.8, 800, "Saw-like FM w/ feedback"),
      ("growl", 2.0, 12.0, 0.3, 600, "Growling FM bass"),
      ("clang", 7.13, 6.0, 0.0, 1200, "Clangy inharmonic FM"),
      ("dark", 1.0, 4.0, 0.0, 500, "Dark mellow FM bass"),
      ("bright", 2.0, 10.0, 0.0, 2000, "Bright cutting FM bass"),
      ("rubber", 0.5, 8.0, 0.5, 900, "Rubbery FM bass"),
  ]
  out = []
  for suffix, ratio, index, fb, cutoff, desc in variants:
    modules = [
        _mod(
            "osc", "fm_osc", {
                "base_freq_hz": 0.0,
                "mod_ratio": ratio,
                "mod_index": index,
                "feedback": fb
            }),
        _mod("filt", "svf", {
            "mode": "LOWPASS",
            "base_cutoff_hz": cutoff,
            "base_q": 1.0
        }),
        _mod(
            "amp_env", "adsr", {
                "attack_ms": 2.0,
                "decay_ms": 150.0,
                "sustain": 0.6,
                "release_ms": 150.0
            }),
        _mod("vca", "vca", {}),
    ]
    wires = [
        _wire("arp", "amp_env", "gate"),
        _wire("arp", "osc", "freq", from_port="freq", scale=0.5),
        _wire("osc", "filt", "in"),
        _wire("filt", "vca", "in"),
        _wire("amp_env", "vca", "gain"),
        _wire("vca", "master", "in"),
    ]
    out.append(_preset(f"fm_bass_{suffix}", "bass", desc, modules, wires))
  return out


def _saw_lead_variations() -> list[dict]:
  variants = [
      ("classic", 2000, 0.3, 1.0, 250, "Classic saw lead"),
      ("bright", 4000, 0.2, 1.0, 200, "Bright aggressive saw"),
      ("warm", 1500, 0.4, 1.0, 300, "Warm mellow saw"),
      ("squealing", 2500, 0.9, 1.0, 220, "Squealing high-reso saw"),
      ("dark", 1000, 0.3, 1.0, 400, "Dark low-pass saw"),
      ("wide", 2500, 0.3, 1.0, 250, "Wide lead with delay"),
      ("gritty", 2000, 0.5, 1.0, 200, "Gritty driven saw"),
      ("crystal", 5000, 0.1, 1.0, 180, "Crystal clear saw"),
  ]
  out = []
  for suffix, cutoff, reso, drive, decay, desc in variants:
    modules = [
        _mod("osc", "classic_osc", {
            "waveform": "SAW",
            "base_freq_hz": 0.0
        }),
        _mod("filt", "moog_ladder", {
            "base_cutoff_hz": cutoff,
            "base_resonance": reso,
            "drive": drive
        }),
        _mod(
            "amp_env", "adsr", {
                "attack_ms": 3.0,
                "decay_ms": decay,
                "sustain": 0.8,
                "release_ms": 200.0
            }),
        _mod("vca", "vca", {}),
        _mod("delay_fx", "delay", {
            "time_ms": 375,
            "feedback": 0.3,
            "damping": 0.4,
            "mix": 0.25
        }),
    ]
    wires = [
        _wire("arp", "amp_env", "gate"),
        _wire("arp", "osc", "freq", from_port="freq"),
        _wire("osc", "filt", "in"),
        _wire("filt", "vca", "in"),
        _wire("amp_env", "vca", "gain"),
        _wire("vca", "delay_fx", "in"),
        _wire("delay_fx", "master", "in"),
    ]
    out.append(_preset(f"saw_lead_{suffix}", "lead", desc, modules, wires))
  return out


def _square_lead_variations() -> list[dict]:
  variants = [
      ("classic", 0.5, 3000, 3.0, "Classic square lead"),
      ("narrow", 0.2, 3500, 2.0, "Narrow pulse lead"),
      ("wide", 0.7, 2500, 4.0, "Wide PWM lead"),
      ("hollow", 0.5, 4000, 2.0, "Hollow clarinet-y lead"),
      ("bright", 0.3, 5000, 1.5, "Bright cutting lead"),
      ("warm", 0.5, 1500, 3.0, "Warm low-pass lead"),
      ("resonant", 0.5, 3000, 8.0, "Resonant filter lead"),
      ("buzzy", 0.15, 4500, 2.5, "Buzzy narrow pulse"),
  ]
  out = []
  for suffix, pw, cutoff, q, desc in variants:
    modules = [
        _mod("osc", "classic_osc", {
            "waveform": "SQUARE",
            "base_freq_hz": 0.0,
            "pulse_width": pw
        }),
        _mod("filt", "svf", {
            "mode": "LOWPASS",
            "base_cutoff_hz": cutoff,
            "base_q": q
        }),
        _mod(
            "amp_env", "adsr", {
                "attack_ms": 3.0,
                "decay_ms": 200.0,
                "sustain": 0.8,
                "release_ms": 150.0
            }),
        _mod("vca", "vca", {}),
    ]
    wires = [
        _wire("arp", "amp_env", "gate"),
        _wire("arp", "osc", "freq", from_port="freq"),
        _wire("osc", "filt", "in"),
        _wire("filt", "vca", "in"),
        _wire("amp_env", "vca", "gain"),
        _wire("vca", "master", "in"),
    ]
    out.append(_preset(f"square_lead_{suffix}", "lead", desc, modules, wires))
  return out


def _wavetable_lead_variations() -> list[dict]:
  table_types = [
      ("sine_to_saw_low", "SINE_TO_SAW", 0.2, 2000, "Low-harmonic wavetable"),
      ("sine_to_saw_mid", "SINE_TO_SAW", 0.5, 3000, "Mid-harmonic wavetable"),
      ("sine_to_saw_high", "SINE_TO_SAW", 0.9, 4000, "High-harmonic wavetable"),
      ("pulse_narrow", "PULSE_SWEEP", 0.1, 3500, "Narrow pulse wavetable"),
      ("pulse_wide", "PULSE_SWEEP", 0.7, 2500, "Wide pulse wavetable"),
      ("bell_bright", "BELL", 0.8, 5000, "Bright bell wavetable"),
      ("vocal_ah", "VOCAL", 0.8, 3000, "Ah-vowel wavetable"),
      ("vocal_ee", "VOCAL", 0.1, 3500, "Ee-vowel wavetable"),
  ]
  out = []
  for suffix, tt, pos, cutoff, desc in table_types:
    modules = [
        _mod("osc", "wavetable_osc", {
            "table_type": tt,
            "base_freq_hz": 0.0,
            "base_position": pos
        }),
        _mod("filt", "svf", {
            "mode": "LOWPASS",
            "base_cutoff_hz": cutoff,
            "base_q": 2.0
        }),
        _mod(
            "amp_env", "adsr", {
                "attack_ms": 5.0,
                "decay_ms": 200.0,
                "sustain": 0.75,
                "release_ms": 200.0
            }),
        _mod("vca", "vca", {}),
    ]
    wires = [
        _wire("arp", "amp_env", "gate"),
        _wire("arp", "osc", "freq", from_port="freq"),
        _wire("osc", "filt", "in"),
        _wire("filt", "vca", "in"),
        _wire("amp_env", "vca", "gain"),
        _wire("vca", "master", "in"),
    ]
    out.append(
        _preset(f"wavetable_lead_{suffix}", "lead", desc, modules, wires))
  return out


def _pad_warm_variations() -> list[dict]:
  variants = [
      # (suffix, detune, mix, cutoff, lfo_rate, lfo_depth, desc)
      ("classic", 0.3, 0.4, 2000, 0.2, 500, "Classic warm supersaw pad"),
      ("wide", 0.7, 0.6, 2500, 0.15, 800, "Wide lush pad"),
      ("dark", 0.2, 0.4, 1200, 0.1, 300, "Dark ambient pad"),
      ("bright", 0.3, 0.5, 4000, 0.25, 600, "Bright airy pad"),
      ("evolving", 0.5, 0.5, 2000, 0.05, 1500, "Slowly evolving pad"),
      ("tight", 0.15, 0.3, 2500, 0.2, 400, "Tight focused pad"),
      ("massive", 0.8, 0.7, 1800, 0.12, 1000, "Massive wide pad"),
      ("cinematic", 0.4, 0.5, 2500, 0.08, 1200, "Cinematic evolving pad"),
  ]
  out = []
  for suffix, detune, mix, cutoff, lfo_rate, lfo_depth, desc in variants:
    modules = [
        _mod("osc", "super_osc", {
            "base_freq_hz": 0.0,
            "detune": detune,
            "mix": mix
        }),
        _mod("filt", "moog_ladder", {
            "base_cutoff_hz": cutoff,
            "base_resonance": 0.2,
            "drive": 1.0
        }),
        _mod("filt_lfo", "lfo", {
            "waveform": "SINE",
            "rate_hz": lfo_rate,
            "depth": 1.0,
            "bipolar": True
        }),
        _mod(
            "amp_env", "adsr", {
                "attack_ms": 500.0,
                "decay_ms": 500.0,
                "sustain": 0.8,
                "release_ms": 1000.0
            }),
        _mod("vca", "vca", {}),
        _mod("delay_fx", "delay", {
            "time_ms": 500,
            "feedback": 0.4,
            "damping": 0.5,
            "mix": 0.3
        }),
    ]
    wires = [
        _wire("arp", "amp_env", "gate"),
        _wire("arp", "osc", "freq", from_port="freq", scale=0.5),
        _wire("osc", "filt", "in"),
        _wire("filt_lfo", "filt", "cutoff_mod", scale=lfo_depth),
        _wire("filt", "vca", "in"),
        _wire("amp_env", "vca", "gain"),
        _wire("vca", "delay_fx", "in"),
        _wire("delay_fx", "master", "in"),
    ]
    out.append(_preset(f"pad_warm_{suffix}", "pad", desc, modules, wires))
  return out


def _fx_riser_variations() -> list[dict]:
  # Noise → BP with slow LFO sweep on cutoff + long amp env.
  variants = [
      ("short", 60, 0.1, 2000, 0.0, "Short fx riser"),
      ("medium", 150, 0.3, 3000, 0.2, "Medium fx riser"),
      ("long", 300, 0.5, 4000, 0.5, "Long fx riser"),
      ("epic", 500, 0.7, 5000, 0.8, "Epic long fx riser"),
      ("tight", 50, 0.1, 1500, 0.0, "Tight fx hit"),
      ("washy", 250, 0.4, 2500, 0.3, "Washy pink fx riser"),
      ("bright", 200, 0.2, 6000, 0.0, "Bright fx riser"),
      ("dark", 300, 0.6, 1500, 0.5, "Dark fx riser"),
  ]
  out = []
  for i, (suffix, decay, tilt, cutoff, slow_rate, desc) in enumerate(variants):
    lfo_mods = []
    lfo_wires = []
    if slow_rate > 0:
      lfo_mods.append(
          _mod(
              "cutoff_lfo", "lfo", {
                  "waveform": "TRIANGLE",
                  "rate_hz": slow_rate,
                  "depth": 1.0,
                  "bipolar": True
              }))
      lfo_wires.append(_wire("cutoff_lfo", "filt", "cutoff_mod", scale=1500.0))
    modules = [
        _mod("noise", "noise_osc", {
            "tilt": tilt,
            "seed": 7 + i
        }),
        _mod("filt", "svf", {
            "mode": "BANDPASS",
            "base_cutoff_hz": cutoff,
            "base_q": 4.0
        }),
        _mod(
            "amp_env", "adsr", {
                "attack_ms": decay * 0.5,
                "decay_ms": decay,
                "sustain": 0.2,
                "release_ms": decay * 0.5
            }),
        _mod("vca", "vca", {}),
    ] + lfo_mods
    wires = [
        _wire("arp", "amp_env", "gate"),
        _wire("noise", "filt", "in"),
        _wire("filt", "vca", "in"),
        _wire("amp_env", "vca", "gain"),
        _wire("vca", "master", "in"),
    ] + lfo_wires
    out.append(_preset(f"fx_riser_{suffix}", "fx", desc, modules, wires))
  return out


# ---------------------------------------------------------------------------
# Batch 2: SUBSTANCE-inspired fat layered bass, ANALOG STRINGS ensemble,
# and drawbar / combo organs. These presets use the new Chorus and
# DrawbarOrgan blocks in addition to the batch 1 primitives.
# ---------------------------------------------------------------------------


def _substance_shared_chain(prefix: str,
                            body_modules: list[dict],
                            body_output_id: str,
                            sub_freq_scale: float = 0.5,
                            sub_drive: float = 1.5,
                            body_drive: float = 3.0,
                            body_cutoff: float = 900.0,
                            body_reso: float = 0.25,
                            attack_ms: float = 2.0,
                            release_ms: float = 300.0) -> tuple[list, list]:
  """Builds the common 'fat layered bass' backbone used by every SUBSTANCE
  preset. Takes a caller-provided body oscillator chain (its final module's
  id is `body_output_id`) and wraps it with a sub-sine layer, a shared
  ADSR, and a drive stage.

  Architecture:
    TestPatternSource ──┬── Adsr amp_env ── Vca.gain
                        │
                        └── freq ──┬── ClassicOsc.sub (sine, ×sub_freq_scale)
                                   └── ...body osc chain (freq input)

    body ──► MoogLadder(body_cutoff, body_reso) ──► Waveshaper(body_drive)
                                                                  │
                                                                  ▼
                         sub ──► Waveshaper(sub_drive) ──► Mixer ─┤
                                                                  │
                                                                Vca ──► master
  """
  modules = [
      _mod(
          "amp_env", "adsr", {
              "attack_ms": attack_ms,
              "decay_ms": 200.0,
              "sustain": 0.85,
              "release_ms": release_ms
          }),
      # Sub-sine layer. No filter — pure weight.
      _mod("sub_osc", "classic_osc", {
          "waveform": "SINE",
          "base_freq_hz": 0.0
      }),
      _mod("sub_drive", "waveshaper", {
          "mode": "SOFT_TANH",
          "drive": sub_drive,
          "mix": 1.0
      }),
      # Body chain filter + drive.
      _mod("body_filt", "moog_ladder", {
          "base_cutoff_hz": body_cutoff,
          "base_resonance": body_reso,
          "drive": 1.0
      }),
      _mod("body_drive", "waveshaper", {
          "mode": "SOFT_TANH",
          "drive": body_drive,
          "mix": 0.85
      }),
      _mod("layer_mix", "mixer", {}),
      _mod("vca", "vca", {}),
  ] + body_modules

  wires = [
      _wire("arp", "amp_env", "gate"),
      # Sub tracks arp two octaves down, body one octave down.
      _wire(
          "arp",
          "sub_osc",
          "freq",
          from_port="freq",
          scale=sub_freq_scale * 0.5),
      _wire("sub_osc", "sub_drive", "in"),
      _wire("sub_drive", "layer_mix", "in"),
      # Body chain.
      _wire(body_output_id, "body_filt", "in"),
      _wire("body_filt", "body_drive", "in"),
      _wire("body_drive", "layer_mix", "in"),
      # Summed layers → amplitude VCA → master.
      _wire("layer_mix", "vca", "in"),
      _wire("amp_env", "vca", "gain"),
      _wire("vca", "master", "in"),
  ]
  return modules, wires


def _substance_saw_fat_variations() -> list[dict]:
  """Classic SUBSTANCE-style bass: sub sine + saw body through filter."""
  variants = [
      # (suffix, cutoff, body_drive, desc)
      ("classic", 900, 3.0, "Classic layered fat bass"),
      ("punchy", 700, 4.0, "Punchy short fat bass"),
      ("wide", 1200, 2.5, "Wider brighter fat bass"),
      ("dark", 500, 2.5, "Dark sub-heavy fat bass"),
      ("driven", 800, 6.0, "Heavily driven fat bass"),
      ("smooth", 1000, 2.0, "Smooth clean fat bass"),
      ("fat", 600, 4.0, "Max-fat bass"),
      ("rich", 1100, 3.5, "Rich harmonic fat bass"),
  ]
  out = []
  for suffix, cutoff, drive, desc in variants:
    body = [
        _mod("body_osc", "classic_osc", {
            "waveform": "SAW",
            "base_freq_hz": 0.0
        }),
    ]
    wires_extra = [
        _wire("arp", "body_osc", "freq", from_port="freq", scale=0.5),
    ]
    mods, wires = _substance_shared_chain(
        prefix="",
        body_modules=body,
        body_output_id="body_osc",
        body_cutoff=cutoff,
        body_drive=drive)
    wires = wires_extra + wires
    out.append(_preset(f"substance_saw_{suffix}", "bass", desc, mods, wires))
  return out


def _substance_square_fat_variations() -> list[dict]:
  """Sub + square body. Harder, hollow character."""
  variants = [
      ("narrow", 0.3, 1000, 3.0, "Narrow pulse fat bass"),
      ("wide", 0.5, 800, 4.0, "Wide pulse fat bass"),
      ("hollow", 0.5, 1400, 2.5, "Hollow hollowed-out fat bass"),
      ("buzzy", 0.2, 1100, 5.0, "Buzzy high-drive fat bass"),
      ("round", 0.5, 600, 2.0, "Round soft fat bass"),
      ("punchy", 0.4, 900, 4.0, "Punchy square fat bass"),
      ("dirty", 0.3, 700, 6.0, "Dirty driven square fat"),
      ("clean", 0.5, 1200, 2.0, "Clean square fat bass"),
  ]
  out = []
  for suffix, pw, cutoff, drive, desc in variants:
    body = [
        _mod("body_osc", "classic_osc", {
            "waveform": "SQUARE",
            "base_freq_hz": 0.0,
            "pulse_width": pw
        }),
    ]
    wires_extra = [
        _wire("arp", "body_osc", "freq", from_port="freq", scale=0.5),
    ]
    mods, wires = _substance_shared_chain(
        prefix="",
        body_modules=body,
        body_output_id="body_osc",
        body_cutoff=cutoff,
        body_drive=drive)
    wires = wires_extra + wires
    out.append(_preset(f"substance_square_{suffix}", "bass", desc, mods, wires))
  return out


def _substance_fold_fat_variations() -> list[dict]:
  """Sub + wavefolder body. West-coast fat bass character."""
  variants = [
      ("warm", 2.0, 0.0, 900, 2.0, "Warm folded fat bass"),
      ("complex", 4.0, 0.0, 1100, 3.0, "Complex folded fat bass"),
      ("screaming", 8.0, 0.0, 1500, 4.0, "Screaming folded bass"),
      ("asym", 3.0, 0.4, 800, 2.5, "Asymmetric folded bass"),
      ("dark", 2.5, 0.0, 500, 2.0, "Dark folded bass"),
      ("bright", 5.0, 0.0, 1800, 3.0, "Bright folded bass"),
      ("wobble", 3.5, -0.3, 700, 2.5, "Wobbling asym folded bass"),
      ("full", 4.5, 0.2, 1000, 3.5, "Full folded fat bass"),
  ]
  out = []
  for suffix, drive_fold, bias, cutoff, drive, desc in variants:
    body = [
        _mod("body_osc", "fold_osc", {
            "base_freq_hz": 0.0,
            "drive": drive_fold,
            "bias": bias
        }),
    ]
    wires_extra = [
        _wire("arp", "body_osc", "freq", from_port="freq", scale=0.5),
    ]
    mods, wires = _substance_shared_chain(
        prefix="",
        body_modules=body,
        body_output_id="body_osc",
        body_cutoff=cutoff,
        body_drive=drive)
    wires = wires_extra + wires
    out.append(_preset(f"substance_fold_{suffix}", "bass", desc, mods, wires))
  return out


def _substance_fm_fat_variations() -> list[dict]:
  """Sub + FM body. Metallic / aggressive fat bass."""
  variants = [
      ("bell", 1.0, 4.0, 0.0, 900, 2.5, "Bell-metallic FM fat"),
      ("clang", 7.13, 6.0, 0.0, 1200, 3.0, "Clangy FM fat bass"),
      ("saw_fb", 1.0, 10.0, 0.7, 700, 3.5, "Saw-FM feedback fat"),
      ("harmonic", 2.0, 5.0, 0.0, 800, 2.5, "Harmonic FM fat bass"),
      ("growl", 2.0, 10.0, 0.3, 600, 4.0, "Growling FM fat bass"),
      ("dark", 1.0, 3.0, 0.0, 500, 2.0, "Dark FM fat bass"),
      ("rubber", 0.5, 8.0, 0.5, 900, 3.0, "Rubbery FM fat bass"),
      ("bright", 2.0, 12.0, 0.0, 1400, 3.0, "Bright aggressive FM fat"),
  ]
  out = []
  for suffix, ratio, index, fb, cutoff, drive, desc in variants:
    body = [
        _mod(
            "body_osc", "fm_osc", {
                "base_freq_hz": 0.0,
                "mod_ratio": ratio,
                "mod_index": index,
                "feedback": fb
            }),
    ]
    wires_extra = [
        _wire("arp", "body_osc", "freq", from_port="freq", scale=0.5),
    ]
    mods, wires = _substance_shared_chain(
        prefix="",
        body_modules=body,
        body_output_id="body_osc",
        body_cutoff=cutoff,
        body_drive=drive)
    wires = wires_extra + wires
    out.append(_preset(f"substance_fm_{suffix}", "bass", desc, mods, wires))
  return out


def _substance_super_fat_variations() -> list[dict]:
  """Sub + SuperOsc body — massive wall-of-saws fat bass."""
  variants = [
      ("wide", 0.5, 0.6, 1000, 2.5, "Wide supersaw fat bass"),
      ("narrow", 0.2, 0.4, 900, 2.5, "Narrow supersaw fat bass"),
      ("massive", 0.8, 0.7, 1100, 3.5, "Massive wall-of-saws fat bass"),
      ("tight", 0.15, 0.3, 800, 2.5, "Tight focused supersaw fat"),
      ("driven", 0.4, 0.5, 900, 5.0, "Heavily driven supersaw fat"),
      ("dark", 0.3, 0.4, 600, 2.5, "Dark warm supersaw fat"),
      ("bright", 0.4, 0.6, 1600, 3.0, "Bright supersaw fat bass"),
      ("chorussy", 0.3, 0.5, 1000, 2.5, "Chorussy supersaw fat"),
  ]
  out = []
  for suffix, detune, mix, cutoff, drive, desc in variants:
    body = [
        _mod("body_osc", "super_osc", {
            "base_freq_hz": 0.0,
            "detune": detune,
            "mix": mix
        }),
    ]
    wires_extra = [
        _wire("arp", "body_osc", "freq", from_port="freq", scale=0.5),
    ]
    mods, wires = _substance_shared_chain(
        prefix="",
        body_modules=body,
        body_output_id="body_osc",
        body_cutoff=cutoff,
        body_drive=drive)
    wires = wires_extra + wires
    out.append(_preset(f"substance_super_{suffix}", "bass", desc, mods, wires))
  return out


def _substance_wavetable_fat_variations() -> list[dict]:
  """Sub + wavetable body — hybrid evolving fat bass."""
  variants = [
      ("sine_saw_low", "SINE_TO_SAW", 0.3, 800, 2.5, "Low-harmonic hybrid"),
      ("sine_saw_mid", "SINE_TO_SAW", 0.5, 1000, 3.0, "Mid-harmonic hybrid"),
      ("sine_saw_high", "SINE_TO_SAW", 0.9, 1200, 3.5, "High-harmonic hybrid"),
      ("pulse_narrow", "PULSE_SWEEP", 0.2, 1000, 3.0, "Narrow pulse hybrid"),
      ("pulse_wide", "PULSE_SWEEP", 0.7, 800, 3.0, "Wide pulse hybrid"),
      ("bell_fat", "BELL", 0.5, 900, 3.0, "Bell-hybrid fat bass"),
      ("vocal_low", "VOCAL", 0.3, 700, 2.5, "Vocal-ish fat bass"),
      ("vocal_mid", "VOCAL", 0.7, 1100, 3.0, "Vocal-mid fat bass"),
  ]
  out = []
  for suffix, table, pos, cutoff, drive, desc in variants:
    body = [
        _mod("body_osc", "wavetable_osc", {
            "table_type": table,
            "base_freq_hz": 0.0,
            "base_position": pos
        }),
    ]
    wires_extra = [
        _wire("arp", "body_osc", "freq", from_port="freq", scale=0.5),
    ]
    mods, wires = _substance_shared_chain(
        prefix="",
        body_modules=body,
        body_output_id="body_osc",
        body_cutoff=cutoff,
        body_drive=drive)
    wires = wires_extra + wires
    out.append(_preset(f"substance_wt_{suffix}", "bass", desc, mods, wires))
  return out


# ---------------------------------------------------------------------------
# Analog Strings (Output ANALOG STRINGS-inspired).
#
# Common architecture: SuperOsc → MoogLadder → Chorus → Delay → Vca.
# Slow ADSR (long attack + long release) for the classic "swell".
# ---------------------------------------------------------------------------


def _strings_shared_chain(prefix: str,
                          attack_ms: float,
                          release_ms: float,
                          cutoff: float,
                          reso: float,
                          detune: float,
                          super_mix: float,
                          chorus_rate: float,
                          chorus_depth: float,
                          chorus_mix: float,
                          delay_ms: float,
                          delay_mix: float,
                          freq_scale: float = 1.0) -> tuple[list, list]:
  modules = [
      _mod("osc", "super_osc", {
          "base_freq_hz": 0.0,
          "detune": detune,
          "mix": super_mix
      }),
      _mod("filt", "moog_ladder", {
          "base_cutoff_hz": cutoff,
          "base_resonance": reso,
          "drive": 1.0
      }),
      _mod(
          "chorus", "chorus", {
              "rate_hz": chorus_rate,
              "depth_ms": chorus_depth,
              "mid_delay_ms": 18.0,
              "mix": chorus_mix,
              "voices": 3
          }),
      _mod(
          "amp_env", "adsr", {
              "attack_ms": attack_ms,
              "decay_ms": 400.0,
              "sustain": 0.85,
              "release_ms": release_ms
          }),
      _mod("vca", "vca", {}),
      _mod("delay_fx", "delay", {
          "time_ms": delay_ms,
          "feedback": 0.35,
          "damping": 0.5,
          "mix": delay_mix
      }),
  ]
  wires = [
      _wire("arp", "amp_env", "gate"),
      _wire("arp", "osc", "freq", from_port="freq", scale=freq_scale),
      _wire("osc", "filt", "in"),
      _wire("filt", "chorus", "in"),
      _wire("chorus", "vca", "in"),
      _wire("amp_env", "vca", "gain"),
      _wire("vca", "delay_fx", "in"),
      _wire("delay_fx", "master", "in"),
  ]
  return modules, wires


def _strings_solina_variations() -> list[dict]:
  """Classic Solina-style string ensemble."""
  variants = [
      # (suffix, attack, release, cutoff, detune, chorus_rate, desc)
      ("classic", 400, 1200, 2500, 0.35, 0.5, "Classic Solina strings"),
      ("warm", 500, 1400, 1800, 0.30, 0.4, "Warm Solina strings"),
      ("bright", 350, 1000, 3500, 0.30, 0.6, "Bright Solina strings"),
      ("wide", 450, 1300, 2500, 0.55, 0.5, "Wide Solina strings"),
      ("tight", 250, 800, 2800, 0.25, 0.4, "Tight Solina strings"),
      ("lush", 600, 1800, 2200, 0.45, 0.3, "Lush slow Solina"),
      ("vintage", 400, 1200, 2000, 0.40, 0.7, "Vintage Solina strings"),
      ("floating", 700, 2000, 2400, 0.50, 0.25, "Floating slow Solina"),
  ]
  out = []
  for suffix, atk, rel, cut, det, chr_r, desc in variants:
    mods, wires = _strings_shared_chain(
        prefix="",
        attack_ms=atk,
        release_ms=rel,
        cutoff=cut,
        reso=0.2,
        detune=det,
        super_mix=0.55,
        chorus_rate=chr_r,
        chorus_depth=4.0,
        chorus_mix=0.6,
        delay_ms=420,
        delay_mix=0.25)
    out.append(
        _preset(f"strings_solina_{suffix}", "strings", desc, mods, wires))
  return out


def _strings_ensemble_variations() -> list[dict]:
  """Wide, heavily chorused string ensemble."""
  variants = [
      ("wide", 500, 1500, 3000, 0.70, 6.0, "Wide heavy-chorus ensemble"),
      ("massive", 600, 1800, 2800, 0.85, 8.0, "Massive ensemble strings"),
      ("shimmer", 400, 1400, 4000, 0.55, 5.0, "Shimmering ensemble"),
      ("dark_wide", 550, 1700, 2000, 0.70, 6.0, "Dark wide ensemble"),
      ("slow_wide", 800, 2200, 3000, 0.65, 4.0, "Slow wide ensemble"),
      ("full", 500, 1500, 3500, 0.75, 7.0, "Full ensemble strings"),
      ("breathing", 600, 1800, 2500, 0.65, 3.0, "Breathing ensemble"),
      ("spacious", 550, 1900, 3200, 0.70, 5.5, "Spacious wide ensemble"),
  ]
  out = []
  for suffix, atk, rel, cut, det, chr_depth, desc in variants:
    mods, wires = _strings_shared_chain(
        prefix="",
        attack_ms=atk,
        release_ms=rel,
        cutoff=cut,
        reso=0.15,
        detune=det,
        super_mix=0.65,
        chorus_rate=0.4,
        chorus_depth=chr_depth,
        chorus_mix=0.7,
        delay_ms=500,
        delay_mix=0.3)
    out.append(
        _preset(f"strings_ensemble_{suffix}", "strings", desc, mods, wires))
  return out


def _strings_cinematic_variations() -> list[dict]:
  """Very slow, cinematic string pad. Long attack, long delay."""
  variants = [
      ("slow", 1200, 2500, 2500, 0.35, 0.3, 500, "Slow cinematic strings"),
      ("epic", 1500, 3000, 3000, 0.55, 0.25, 600, "Epic cinematic"),
      ("tender", 1000, 2200, 2000, 0.30, 0.4, 450, "Tender cinematic"),
      ("rising", 1800, 2800, 3500, 0.40, 0.2, 550, "Rising cinematic"),
      ("haunting", 1400, 2700, 1800, 0.30, 0.35, 700, "Haunting cinematic"),
      ("bloom", 2000, 3500, 2800, 0.45, 0.2, 600, "Blooming cinematic"),
      ("fragile", 1100, 2400, 2200, 0.25, 0.45, 500, "Fragile cinematic"),
      ("grand", 1600, 3200, 3200, 0.60, 0.25, 700, "Grand cinematic"),
  ]
  out = []
  for suffix, atk, rel, cut, det, chr_r, dly, desc in variants:
    mods, wires = _strings_shared_chain(
        prefix="",
        attack_ms=atk,
        release_ms=rel,
        cutoff=cut,
        reso=0.15,
        detune=det,
        super_mix=0.55,
        chorus_rate=chr_r,
        chorus_depth=4.5,
        chorus_mix=0.55,
        delay_ms=dly,
        delay_mix=0.4)
    out.append(
        _preset(f"strings_cinematic_{suffix}", "strings", desc, mods, wires))
  return out


def _strings_warm_variations() -> list[dict]:
  """Warm dark strings, heavy lowpass."""
  variants = [
      ("warm", 400, 1500, 1500, 0.25, "Warm dark strings"),
      ("cello", 500, 1800, 1200, 0.20, "Cello-like warm strings"),
      ("velvet", 600, 2000, 1400, 0.30, "Velvety warm strings"),
      ("bass", 500, 1600, 900, 0.30, "Bass string ensemble"),
      ("mellow", 450, 1500, 1600, 0.25, "Mellow warm strings"),
      ("round", 550, 1700, 1300, 0.35, "Round warm strings"),
      ("vintage", 500, 1600, 1100, 0.40, "Vintage warm strings"),
      ("smooth", 500, 1800, 1500, 0.20, "Smooth warm strings"),
  ]
  out = []
  for suffix, atk, rel, cut, det, desc in variants:
    mods, wires = _strings_shared_chain(
        prefix="",
        attack_ms=atk,
        release_ms=rel,
        cutoff=cut,
        reso=0.2,
        detune=det,
        super_mix=0.5,
        chorus_rate=0.35,
        chorus_depth=3.5,
        chorus_mix=0.55,
        delay_ms=450,
        delay_mix=0.25)
    out.append(_preset(f"strings_warm_{suffix}", "strings", desc, mods, wires))
  return out


def _strings_bright_variations() -> list[dict]:
  """Bright orchestral-style strings."""
  variants = [
      ("orchestral", 350, 1400, 5000, 0.35, "Bright orchestral strings"),
      ("crisp", 300, 1200, 6000, 0.30, "Crisp bright strings"),
      ("airy", 400, 1500, 4500, 0.40, "Airy bright strings"),
      ("sparkle", 350, 1300, 5500, 0.35, "Sparkling bright strings"),
      ("piercing", 300, 1100, 7000, 0.30, "Piercing bright strings"),
      ("sweet", 400, 1400, 4000, 0.35, "Sweet bright strings"),
      ("cutting", 350, 1300, 6500, 0.30, "Cutting bright strings"),
      ("glassy", 400, 1500, 5000, 0.40, "Glassy bright strings"),
  ]
  out = []
  for suffix, atk, rel, cut, det, desc in variants:
    mods, wires = _strings_shared_chain(
        prefix="",
        attack_ms=atk,
        release_ms=rel,
        cutoff=cut,
        reso=0.15,
        detune=det,
        super_mix=0.55,
        chorus_rate=0.5,
        chorus_depth=4.0,
        chorus_mix=0.55,
        delay_ms=420,
        delay_mix=0.25)
    out.append(
        _preset(f"strings_bright_{suffix}", "strings", desc, mods, wires))
  return out


def _strings_dream_variations() -> list[dict]:
  """Heavily chorused, washed-out dreamy strings."""
  variants = [
      ("float", 700, 2200, 2500, 0.55, 10.0, "Floating dream strings"),
      ("drift", 900, 2500, 2200, 0.60, 12.0, "Drifting dream strings"),
      ("haze", 800, 2400, 2800, 0.65, 9.0, "Hazy dream strings"),
      ("mist", 850, 2300, 2000, 0.55, 11.0, "Misty dream strings"),
      ("sleep", 1000, 2800, 1800, 0.50, 10.0, "Sleepy dream strings"),
      ("echo", 800, 2500, 2500, 0.60, 8.0, "Echo dream strings"),
      ("nova", 700, 2200, 3200, 0.65, 12.0, "Nova-bright dreamy"),
      ("quiet", 900, 2600, 2000, 0.50, 9.0, "Quiet dream strings"),
  ]
  out = []
  for suffix, atk, rel, cut, det, chr_depth, desc in variants:
    mods, wires = _strings_shared_chain(
        prefix="",
        attack_ms=atk,
        release_ms=rel,
        cutoff=cut,
        reso=0.15,
        detune=det,
        super_mix=0.6,
        chorus_rate=0.3,
        chorus_depth=chr_depth,
        chorus_mix=0.75,
        delay_ms=700,
        delay_mix=0.5)
    out.append(_preset(f"strings_dream_{suffix}", "strings", desc, mods, wires))
  return out


# ---------------------------------------------------------------------------
# Organs: Hammond drawbars + Vox / Farfisa combo organs.
# ---------------------------------------------------------------------------


def _hammond_drawbars(levels: list[float]) -> dict:
  """Turns a 9-element drawbar list into a DrawbarOrganConfig dict."""
  keys = [
      "db16", "db5_1_3", "db8", "db4", "db2_2_3", "db2", "db1_3_5", "db1_1_3",
      "db1"
  ]
  return {k: v for k, v in zip(keys, levels) if v > 0.0}


def _organ_hammond_chain(drawbar_levels: list[float], chorus_rate: float,
                         chorus_depth: float,
                         chorus_mix: float) -> tuple[list, list]:
  dd = _hammond_drawbars(drawbar_levels)
  dd["base_freq_hz"] = 0.0
  modules = [
      _mod("osc", "drawbar_organ", dd),
      _mod(
          "chorus", "chorus", {
              "rate_hz": chorus_rate,
              "depth_ms": chorus_depth,
              "mid_delay_ms": 8.0,
              "mix": chorus_mix,
              "voices": 3
          }),
      _mod("amp_env", "adsr", {
          "attack_ms": 3.0,
          "decay_ms": 20.0,
          "sustain": 1.0,
          "release_ms": 40.0
      }),
      _mod("vca", "vca", {}),
  ]
  wires = [
      _wire("arp", "amp_env", "gate"),
      _wire("arp", "osc", "freq", from_port="freq"),
      _wire("osc", "chorus", "in"),
      _wire("chorus", "vca", "in"),
      _wire("amp_env", "vca", "gain"),
      _wire("vca", "master", "in"),
  ]
  return modules, wires


def _organ_hammond_jazz_variations() -> list[dict]:
  """Jazz drawbar settings — traditional soft/medium presets."""

  # Drawbar levels on the classic 0..8 scale, divided by 8.
  def db(s):
    return [int(c) / 8.0 for c in s]

  variants = [
      # name, drawbar string, leslie speed, desc
      ("888000000", db("888000000"), 0.8, "Classic jazz three front drawbars"),
      ("808000000", db("808000000"), 0.8, "Jazz 8-0-8 mellow"),
      ("088000000", db("088000000"), 1.0, "Jazz 0-8-8 bright"),
      ("888800000", db("888800000"), 0.8, "Jazz with octave"),
      ("808080080", db("808080080"), 1.2, "Jazz harmonically rich"),
      ("688600000", db("688600000"), 0.9, "Jimmy-Smith-ish jazz"),
      ("868000000", db("868000000"), 0.7, "Mellow jazz organ"),
      ("828000000", db("828000000"), 0.8, "Soft jazz organ"),
  ]
  out = []
  for suffix, levels, leslie_speed, desc in variants:
    mods, wires = _organ_hammond_chain(
        drawbar_levels=levels,
        chorus_rate=leslie_speed,
        chorus_depth=2.0,
        chorus_mix=0.35)
    out.append(
        _preset(f"organ_hammond_jazz_{suffix}", "organ", desc, mods, wires))
  return out


def _organ_hammond_rock_variations() -> list[dict]:
  """Rock drawbar settings — full drawbars, heavier chorus (fast leslie)."""

  def db(s):
    return [int(c) / 8.0 for c in s]

  variants = [
      ("888800008", db("888800008"), 6.0, "Rock full drawbars + top"),
      ("888888888", db("888888888"), 6.5, "Full drawbars rock organ"),
      ("888800880", db("888800880"), 5.5, "Rock with hi harmonics"),
      ("080808080", db("080808080"), 7.0, "Hollow rock organ"),
      ("888800008_b", db("888700006"), 6.0, "Rock with slight top"),
      ("888888880", db("888888880"), 6.5, "Full rock organ"),
      ("888888008", db("888888008"), 5.5, "Rock heavy body"),
      ("808880080", db("808880080"), 6.0, "Rock-harmonic emphasis"),
  ]
  out = []
  for suffix, levels, leslie_speed, desc in variants:
    mods, wires = _organ_hammond_chain(
        drawbar_levels=levels,
        chorus_rate=leslie_speed,
        chorus_depth=3.5,
        chorus_mix=0.5)
    out.append(
        _preset(f"organ_hammond_rock_{suffix}", "organ", desc, mods, wires))
  return out


def _organ_vox_variations() -> list[dict]:
  """Vox Continental style — square waves (subtractive organ)."""
  variants = [
      ("classic", 0.5, 3500, "Classic Vox Continental"),
      ("warm", 0.5, 2500, "Warm Vox"),
      ("bright", 0.5, 5000, "Bright Vox"),
      ("narrow", 0.3, 3500, "Narrow pulse Vox"),
      ("wide", 0.7, 3500, "Wide pulse Vox"),
      ("dark", 0.5, 1800, "Dark Vox organ"),
      ("cutting", 0.4, 4500, "Cutting Vox organ"),
      ("hollow", 0.5, 4200, "Hollow Vox sound"),
  ]
  out = []
  for suffix, pw, cutoff, desc in variants:
    modules = [
        _mod("osc", "classic_osc", {
            "waveform": "SQUARE",
            "base_freq_hz": 0.0,
            "pulse_width": pw
        }),
        _mod("osc_oct", "classic_osc", {
            "waveform": "SQUARE",
            "base_freq_hz": 0.0,
            "pulse_width": pw
        }),
        _mod("mix", "mixer", {}),
        _mod("filt", "svf", {
            "mode": "LOWPASS",
            "base_cutoff_hz": cutoff,
            "base_q": 1.0
        }),
        _mod(
            "chorus", "chorus", {
                "rate_hz": 4.0,
                "depth_ms": 2.5,
                "mid_delay_ms": 10.0,
                "mix": 0.35,
                "voices": 3
            }),
        _mod("amp_env", "adsr", {
            "attack_ms": 3.0,
            "decay_ms": 20.0,
            "sustain": 1.0,
            "release_ms": 40.0
        }),
        _mod("vca", "vca", {}),
    ]
    wires = [
        _wire("arp", "amp_env", "gate"),
        _wire("arp", "osc", "freq", from_port="freq"),
        _wire("arp", "osc_oct", "freq", from_port="freq", scale=2.0),
        _wire("osc", "mix", "in"),
        _wire("osc_oct", "mix", "in"),
        _wire("mix", "filt", "in"),
        _wire("filt", "chorus", "in"),
        _wire("chorus", "vca", "in"),
        _wire("amp_env", "vca", "gain"),
        _wire("vca", "master", "in"),
    ]
    out.append(_preset(f"organ_vox_{suffix}", "organ", desc, modules, wires))
  return out


def _organ_farfisa_variations() -> list[dict]:
  """Farfisa-style combo organ. Sharper, brighter than Vox."""
  variants = [
      ("classic", 4500, 2.0, "Classic Farfisa combo"),
      ("bright", 6000, 1.5, "Bright Farfisa"),
      ("cheesy", 4000, 3.0, "Cheesy 60s Farfisa"),
      ("punchy", 5000, 2.5, "Punchy Farfisa"),
      ("soft", 3000, 1.5, "Softer Farfisa"),
      ("reedy", 5500, 3.5, "Reedy Farfisa"),
      ("vintage", 4200, 2.5, "Vintage Farfisa combo"),
      ("full", 4800, 2.0, "Full Farfisa sound"),
  ]
  out = []
  for suffix, cutoff, q, desc in variants:
    modules = [
        # Two octaves of narrow pulse → hollow Farfisa character.
        _mod("osc", "classic_osc", {
            "waveform": "SQUARE",
            "base_freq_hz": 0.0,
            "pulse_width": 0.25
        }),
        _mod("osc_oct", "classic_osc", {
            "waveform": "SQUARE",
            "base_freq_hz": 0.0,
            "pulse_width": 0.25
        }),
        _mod("osc_dbl", "classic_osc", {
            "waveform": "SQUARE",
            "base_freq_hz": 0.0,
            "pulse_width": 0.3
        }),
        _mod("mix", "mixer", {}),
        _mod("filt", "svf", {
            "mode": "LOWPASS",
            "base_cutoff_hz": cutoff,
            "base_q": q
        }),
        _mod(
            "chorus", "chorus", {
                "rate_hz": 5.0,
                "depth_ms": 2.0,
                "mid_delay_ms": 8.0,
                "mix": 0.3,
                "voices": 3
            }),
        _mod("amp_env", "adsr", {
            "attack_ms": 2.0,
            "decay_ms": 15.0,
            "sustain": 1.0,
            "release_ms": 25.0
        }),
        _mod("vca", "vca", {}),
    ]
    wires = [
        _wire("arp", "amp_env", "gate"),
        _wire("arp", "osc", "freq", from_port="freq"),
        _wire("arp", "osc_oct", "freq", from_port="freq", scale=2.0),
        _wire("arp", "osc_dbl", "freq", from_port="freq", scale=4.0),
        _wire("osc", "mix", "in"),
        _wire("osc_oct", "mix", "in"),
        _wire("osc_dbl", "mix", "in"),
        _wire("mix", "filt", "in"),
        _wire("filt", "chorus", "in"),
        _wire("chorus", "vca", "in"),
        _wire("amp_env", "vca", "gain"),
        _wire("vca", "master", "in"),
    ]
    out.append(
        _preset(f"organ_farfisa_{suffix}", "organ", desc, modules, wires))
  return out


# ---------------------------------------------------------------------------
# Main entry point.
# ---------------------------------------------------------------------------

TEMPLATES = [
    # Batch 1 — 16 templates × 8 variations = 128.
    _kick_variations,
    _sub_kick_variations,
    _snare_variations,
    _clap_variations,
    _closed_hat_variations,
    _open_hat_variations,
    _tom_variations,
    _acid_bass_variations,
    _reese_bass_variations,
    _sub_bass_variations,
    _fm_bass_variations,
    _saw_lead_variations,
    _square_lead_variations,
    _wavetable_lead_variations,
    _pad_warm_variations,
    _fx_riser_variations,

    # Batch 2 — 16 templates × 8 variations = 128 more.
    # SUBSTANCE-inspired fat layered bass (6 templates × 8 = 48).
    _substance_saw_fat_variations,
    _substance_square_fat_variations,
    _substance_fold_fat_variations,
    _substance_fm_fat_variations,
    _substance_super_fat_variations,
    _substance_wavetable_fat_variations,
    # ANALOG STRINGS-inspired ensemble strings (6 templates × 8 = 48).
    _strings_solina_variations,
    _strings_ensemble_variations,
    _strings_cinematic_variations,
    _strings_warm_variations,
    _strings_bright_variations,
    _strings_dream_variations,
    # Organs (4 templates × 8 = 32).
    _organ_hammond_jazz_variations,
    _organ_hammond_rock_variations,
    _organ_vox_variations,
    _organ_farfisa_variations,
]


def main() -> int:
  presets = []
  for template in TEMPLATES:
    variations = template()
    if len(variations) != 8:
      print(
          f"ERROR: template {template.__name__} returned "
          f"{len(variations)} variations, expected 8",
          file=sys.stderr)
      return 1
    presets.extend(variations)

  expected = 8 * len(TEMPLATES)
  if len(presets) != expected:
    print(
        f"ERROR: generated {len(presets)} presets, expected {expected}",
        file=sys.stderr)
    return 1

  doc = {
      "version": 1,
      "generated_by": "tools/trace_to_techno/gen_presets.py",
      "preset_count": len(presets),
      "presets": presets,
  }

  OUTPUT_FILE.parent.mkdir(parents=True, exist_ok=True)
  with OUTPUT_FILE.open("w") as f:
    json.dump(doc, f, indent=2, sort_keys=False)
    f.write("\n")
  print(f"Wrote {len(presets)} presets to {OUTPUT_FILE}")

  # Also write to the UI assets directory so the UI dev server serves it.
  UI_COPY.parent.mkdir(parents=True, exist_ok=True)
  with UI_COPY.open("w") as f:
    json.dump(doc, f, indent=2, sort_keys=False)
    f.write("\n")
  print(f"Wrote {len(presets)} presets to {UI_COPY}")
  return 0


if __name__ == "__main__":
  sys.exit(main())
