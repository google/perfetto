// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Block descriptor registry for the SoundSynth plugin.
//
// Each block type in synth.proto has an entry here describing its:
//   - display name, category, hue (for UI rendering)
//   - input / output ports (for wire routing)
//   - a factory that creates a default ISynthModule
//   - a renderParams() function that renders inline parameter controls
//
// Milestone 2: every block has a hand-tuned param panel; only the
// legacy blocks (vco/envelope) still fall back to the generic
// auto-introspecting renderer.

import m from 'mithril';
import protos from '../../protos';

export type PortKind = 'audio' | 'cv' | 'gate' | 'freq';

export interface PortSpec {
  readonly name: string;
  readonly kind: PortKind;
}

/** Per-PortKind visual colors (used in port badges and palette). */
export const PORT_KIND_COLORS: Record<PortKind, string> = {
  audio: '#e8a33d',  // amber - audio signal
  cv:    '#5fa7ee',  // blue - control voltage
  gate:  '#7cd66a',  // green - gate / trigger
  freq:  '#d770d7',  // magenta - pitch CV
};

/**
 * Builds an m.Children to use as NodePort.content for a port. Renders
 * a small colored dot followed by the port name, so the user can tell
 * at a glance what kind of signal a port carries.
 */
export function portContent(name: string, kind: PortKind): m.Children {
  return m('span', {
    style: {
      display: 'inline-flex', alignItems: 'center', gap: '4px',
      fontSize: '10px', color: '#333',
    },
    title: `${kind} signal`,
  },
    m('span', {
      style: {
        display: 'inline-block', width: '7px', height: '7px',
        borderRadius: '50%', background: PORT_KIND_COLORS[kind],
        boxShadow: '0 0 0 1px rgba(0,0,0,0.2)',
      },
    }),
    name,
  );
}

export type BlockCategory =
  | 'source' | 'oscillator' | 'filter'
  | 'effect' | 'modulator' | 'utility';

export interface BlockDescriptor {
  /** Proto oneof field name, e.g. "classic_osc". */
  readonly protoField: string;
  /** Short human-readable name, e.g. "Classic Osc". */
  readonly displayName: string;
  /** One-line description shown in the palette. */
  readonly description: string;
  readonly category: BlockCategory;
  /** Hue 0-360 used to color the node. */
  readonly hue: number;
  readonly inputs: ReadonlyArray<PortSpec>;
  readonly outputs: ReadonlyArray<PortSpec>;
  /** Factory returning a default ISynthModule (without id). */
  readonly createDefault: () => protos.ISynthModule;
  /** Renders inline parameter controls inside the node body. */
  readonly renderParams: (
    mod: protos.ISynthModule, onChange: () => void,
  ) => m.Children;
}

// --- Helper widgets ---

const fieldLabelStyle = {
  fontSize: '10px', color: '#666', width: '52px', flexShrink: '0',
};

function slider(
  label: string, value: number, min: number, max: number,
  unit: string, decimals: number, onchange: (v: number) => void,
): m.Child {
  return m('div', {
    style: {
      display: 'flex', alignItems: 'center', gap: '4px',
      marginBottom: '2px',
    },
  },
    m('span', {style: fieldLabelStyle}, label),
    m('input[type=range]', {
      style: {flex: '1', minWidth: '60px'},
      min: String(min), max: String(max),
      step: max > 1000 ? '1' : (max > 10 ? '0.1' : '0.01'),
      value: String(value),
      oninput: (e: InputEvent) => {
        onchange(parseFloat((e.target as HTMLInputElement).value));
      },
      onclick: (e: Event) => e.stopPropagation(),
      onmousedown: (e: Event) => e.stopPropagation(),
    }),
    m('span', {
      style: {
        fontSize: '10px', color: '#333', width: '44px',
        textAlign: 'right', fontFamily: 'monospace',
      },
    }, `${value.toFixed(decimals)}${unit}`),
  );
}

/**
 * Logarithmic slider, used for frequency-like params where linear
 * interpolation feels horrible. The DOM slider always operates on a
 * 0..1000 integer range internally; we map to/from log space.
 */
function logSlider(
  label: string, value: number, min: number, max: number,
  unit: string, onchange: (v: number) => void,
): m.Child {
  const safeMin = Math.max(min, 1e-6);
  const lo = Math.log(safeMin);
  const hi = Math.log(max);
  const t = Math.max(0, Math.min(1000,
    Math.round(((Math.log(Math.max(value, safeMin)) - lo) / (hi - lo)) * 1000),
  ));
  const decimals = value < 10 ? 2 : (value < 100 ? 1 : 0);
  return m('div', {
    style: {
      display: 'flex', alignItems: 'center', gap: '4px',
      marginBottom: '2px',
    },
  },
    m('span', {style: fieldLabelStyle}, label),
    m('input[type=range]', {
      style: {flex: '1', minWidth: '60px'},
      min: '0', max: '1000', step: '1', value: String(t),
      oninput: (e: InputEvent) => {
        const raw = parseFloat((e.target as HTMLInputElement).value);
        const mapped = Math.exp(lo + (raw / 1000) * (hi - lo));
        onchange(mapped);
      },
      onclick: (e: Event) => e.stopPropagation(),
      onmousedown: (e: Event) => e.stopPropagation(),
    }),
    m('span', {
      style: {
        fontSize: '10px', color: '#333', width: '52px',
        textAlign: 'right', fontFamily: 'monospace',
      },
    }, `${value.toFixed(decimals)}${unit}`),
  );
}

function dropdown<T extends string | number>(
  label: string, value: T,
  options: Array<{value: T; label: string}>,
  onchange: (v: T) => void,
): m.Child {
  return m('div', {
    style: {
      display: 'flex', alignItems: 'center', gap: '4px',
      marginBottom: '2px',
    },
  },
    m('span', {style: fieldLabelStyle}, label),
    m('select', {
      style: {flex: '1', fontSize: '11px', padding: '1px 2px'},
      value: String(value),
      onchange: (e: Event) => {
        const raw = (e.target as HTMLSelectElement).value;
        onchange((typeof value === 'number' ? Number(raw) : raw) as T);
      },
      onclick: (e: Event) => e.stopPropagation(),
      onmousedown: (e: Event) => e.stopPropagation(),
    },
      options.map((o) =>
        m('option', {value: String(o.value)}, o.label),
      ),
    ),
  );
}

function checkbox(
  label: string, value: boolean, onchange: (v: boolean) => void,
): m.Child {
  return m('div', {
    style: {
      display: 'flex', alignItems: 'center', gap: '4px',
      marginBottom: '2px',
    },
  },
    m('span', {style: fieldLabelStyle}, label),
    m('input[type=checkbox]', {
      checked: value,
      onchange: (e: Event) => {
        onchange((e.target as HTMLInputElement).checked);
      },
      onclick: (e: Event) => e.stopPropagation(),
      onmousedown: (e: Event) => e.stopPropagation(),
    }),
  );
}

/**
 * Compact vertical drawbar (Hammond-drawbar style). Used by the
 * DrawbarOrgan panel.
 *
 * Implemented as a click+drag track: we set the value from the
 * pointer's Y position. We avoid <input type=range> with vertical
 * writing-mode here because legacy `appearance: slider-vertical` is
 * gone in Chrome 145+ and the modern equivalent renders inconsistently.
 */
function verticalSlider(
  label: string, value: number, onchange: (v: number) => void,
): m.Child {
  const TRACK_HEIGHT = 60;
  const filled = Math.max(0, Math.min(1, value));
  const updateFromY = (clientY: number, trackEl: HTMLElement) => {
    const rect = trackEl.getBoundingClientRect();
    const t = 1 - Math.max(0, Math.min(1, (clientY - rect.top) / rect.height));
    onchange(t);
  };
  return m('div', {
    style: {
      display: 'flex', flexDirection: 'column', alignItems: 'center',
      gap: '2px', width: '18px',
    },
  },
    m('div.drawbar-track', {
      style: {
        position: 'relative',
        width: '10px',
        height: `${TRACK_HEIGHT}px`,
        background: '#1a1a1c',
        border: '1px solid #555',
        borderRadius: '2px',
        cursor: 'ns-resize',
      },
      onpointerdown: (e: PointerEvent) => {
        e.stopPropagation();
        const el = e.currentTarget as HTMLElement;
        el.setPointerCapture(e.pointerId);
        updateFromY(e.clientY, el);
        const onMove = (ev: PointerEvent) => {
          updateFromY(ev.clientY, el);
        };
        const onUp = (ev: PointerEvent) => {
          el.releasePointerCapture(ev.pointerId);
          el.removeEventListener('pointermove', onMove);
          el.removeEventListener('pointerup', onUp);
        };
        el.addEventListener('pointermove', onMove);
        el.addEventListener('pointerup', onUp);
      },
      onmousedown: (e: Event) => e.stopPropagation(),
    },
      // The drawbar "slug" — a beige tab that fills from the bottom up.
      m('div', {
        style: {
          position: 'absolute',
          left: '0', right: '0',
          top: `${(1 - filled) * 100}%`,
          bottom: '0',
          background: 'linear-gradient(180deg,#f5f5dc,#e6d8a0)',
          boxShadow: 'inset 0 1px 0 rgba(255,255,255,0.6)',
        },
      }),
      // Black tip line at the top of the slug.
      m('div', {
        style: {
          position: 'absolute',
          left: '0', right: '0',
          top: `calc(${(1 - filled) * 100}% - 1px)`,
          height: '2px',
          background: '#000',
          pointerEvents: 'none',
        },
      }),
    ),
    m('span', {
      style: {
        fontSize: '8px', fontFamily: 'monospace', color: '#bbb',
      },
    }, label),
  );
}

// --- Full descriptors for the 8 key blocks ---

const descriptors: BlockDescriptor[] = [];

// TestPatternSource -----------------------------------------------------------
descriptors.push({
  protoField: 'test_pattern_source',
  displayName: 'Test Pattern',
  description: 'Synthetic gate + freq signal for preview (Am-G-F-E arpeggio)',
  category: 'source',
  hue: 140,
  inputs: [],
  outputs: [
    {name: 'out', kind: 'gate'},
    {name: 'freq', kind: 'freq'},
  ],
  createDefault: (): protos.ISynthModule => ({
    testPatternSource: {
      mode: protos.TestPatternSourceConfig.Mode.ARPEGGIO,
      bpm: 128,
      bars: 4,
    },
  }),
  renderParams: (mod, onChange) => {
    const cfg = mod.testPatternSource!;
    return m('div', {style: {fontSize: '11px'}},
      dropdown('Mode', cfg.mode ?? 0, [
        {value: 0, label: 'Arpeggio'},
        {value: 1, label: 'Impulses'},
      ], (v) => { cfg.mode = v; onChange(); }),
      slider('BPM', cfg.bpm ?? 128, 60, 200, '', 0,
        (v) => { cfg.bpm = v; onChange(); }),
      slider('Bars', cfg.bars ?? 4, 1, 32, '', 0,
        (v) => { cfg.bars = Math.round(v); onChange(); }),
    );
  },
});

// ClassicOsc -----------------------------------------------------------------
descriptors.push({
  protoField: 'classic_osc',
  displayName: 'Classic Osc',
  description: 'Bandlimited saw/square/triangle/sine with PWM',
  category: 'oscillator',
  hue: 30,
  inputs: [
    {name: 'freq', kind: 'freq'},
    {name: 'freq_mod', kind: 'cv'},
    {name: 'reset', kind: 'gate'},
  ],
  outputs: [{name: 'out', kind: 'audio'}],
  createDefault: (): protos.ISynthModule => ({
    classicOsc: {
      waveform: protos.ClassicOscConfig.Waveform.SAW,
      baseFreqHz: 220,
      pulseWidth: 0.5,
    },
  }),
  renderParams: (mod, onChange) => {
    const cfg = mod.classicOsc!;
    return m('div', {style: {fontSize: '11px'}},
      dropdown('Wave', cfg.waveform ?? 0, [
        {value: 0, label: 'Saw'},
        {value: 1, label: 'Square'},
        {value: 2, label: 'Triangle'},
        {value: 3, label: 'Sine'},
      ], (v) => { cfg.waveform = v; onChange(); }),
      slider('Freq', cfg.baseFreqHz ?? 220, 20, 4000, 'Hz', 0,
        (v) => { cfg.baseFreqHz = v; onChange(); }),
      slider('PW', cfg.pulseWidth ?? 0.5, 0.05, 0.95, '', 2,
        (v) => { cfg.pulseWidth = v; onChange(); }),
    );
  },
});

// Adsr ------------------------------------------------------------------------
descriptors.push({
  protoField: 'adsr',
  displayName: 'ADSR',
  description: '4-stage exponential envelope (A/D/S/R)',
  category: 'modulator',
  hue: 270,
  inputs: [{name: 'gate', kind: 'gate'}],
  outputs: [{name: 'out', kind: 'cv'}],
  createDefault: (): protos.ISynthModule => ({
    adsr: {
      attackMs: 5, decayMs: 100, sustain: 0.7, releaseMs: 200,
    },
  }),
  renderParams: (mod, onChange) => {
    const cfg = mod.adsr!;
    return m('div', {style: {fontSize: '11px'}},
      slider('Attack', cfg.attackMs ?? 5, 0.1, 2000, 'ms', 1,
        (v) => { cfg.attackMs = v; onChange(); }),
      slider('Decay', cfg.decayMs ?? 100, 1, 3000, 'ms', 0,
        (v) => { cfg.decayMs = v; onChange(); }),
      slider('Sustain', cfg.sustain ?? 0.7, 0, 1, '', 2,
        (v) => { cfg.sustain = v; onChange(); }),
      slider('Release', cfg.releaseMs ?? 200, 1, 5000, 'ms', 0,
        (v) => { cfg.releaseMs = v; onChange(); }),
    );
  },
});

// MoogLadder ------------------------------------------------------------------
descriptors.push({
  protoField: 'moog_ladder',
  displayName: 'Moog Ladder',
  description: '24 dB/oct lowpass with tanh saturation',
  category: 'filter',
  hue: 200,
  inputs: [
    {name: 'in', kind: 'audio'},
    {name: 'cutoff_mod', kind: 'cv'},
    {name: 'reso_mod', kind: 'cv'},
  ],
  outputs: [{name: 'out', kind: 'audio'}],
  createDefault: (): protos.ISynthModule => ({
    moogLadder: {
      baseCutoffHz: 1000,
      baseResonance: 0.2,
      drive: 1.0,
    },
  }),
  renderParams: (mod, onChange) => {
    const cfg = mod.moogLadder!;
    return m('div', {style: {fontSize: '11px'}},
      slider('Cutoff', cfg.baseCutoffHz ?? 1000, 20, 12000, 'Hz', 0,
        (v) => { cfg.baseCutoffHz = v; onChange(); }),
      slider('Reso', cfg.baseResonance ?? 0.2, 0, 1, '', 2,
        (v) => { cfg.baseResonance = v; onChange(); }),
      slider('Drive', cfg.drive ?? 1, 0.5, 8, '', 1,
        (v) => { cfg.drive = v; onChange(); }),
    );
  },
});

// Svf -------------------------------------------------------------------------
descriptors.push({
  protoField: 'svf',
  displayName: 'SVF',
  description: 'State-variable filter (LP/HP/BP/Notch)',
  category: 'filter',
  hue: 220,
  inputs: [
    {name: 'in', kind: 'audio'},
    {name: 'cutoff_mod', kind: 'cv'},
    {name: 'q_mod', kind: 'cv'},
  ],
  outputs: [{name: 'out', kind: 'audio'}],
  createDefault: (): protos.ISynthModule => ({
    svf: {
      mode: protos.SvfConfig.Mode.LOWPASS,
      baseCutoffHz: 1000,
      baseQ: 1.0,
    },
  }),
  renderParams: (mod, onChange) => {
    const cfg = mod.svf!;
    return m('div', {style: {fontSize: '11px'}},
      dropdown('Mode', cfg.mode ?? 0, [
        {value: 0, label: 'Lowpass'},
        {value: 1, label: 'Highpass'},
        {value: 2, label: 'Bandpass'},
        {value: 3, label: 'Notch'},
      ], (v) => { cfg.mode = v; onChange(); }),
      slider('Cutoff', cfg.baseCutoffHz ?? 1000, 20, 8000, 'Hz', 0,
        (v) => { cfg.baseCutoffHz = v; onChange(); }),
      slider('Q', cfg.baseQ ?? 1, 0.5, 50, '', 1,
        (v) => { cfg.baseQ = v; onChange(); }),
    );
  },
});

// Waveshaper ------------------------------------------------------------------
descriptors.push({
  protoField: 'waveshaper',
  displayName: 'Waveshaper',
  description: 'Memoryless distortion (tanh, clip, fold, asym)',
  category: 'effect',
  hue: 10,
  inputs: [{name: 'in', kind: 'audio'}],
  outputs: [{name: 'out', kind: 'audio'}],
  createDefault: (): protos.ISynthModule => ({
    waveshaper: {
      mode: protos.WaveshaperConfig.Mode.SOFT_TANH,
      drive: 2.0,
      mix: 1.0,
    },
  }),
  renderParams: (mod, onChange) => {
    const cfg = mod.waveshaper!;
    return m('div', {style: {fontSize: '11px'}},
      dropdown('Mode', cfg.mode ?? 0, [
        {value: 0, label: 'Soft Tanh'},
        {value: 1, label: 'Hard Clip'},
        {value: 2, label: 'Fold'},
        {value: 3, label: 'Asymmetric'},
      ], (v) => { cfg.mode = v; onChange(); }),
      slider('Drive', cfg.drive ?? 2, 1, 20, '', 1,
        (v) => { cfg.drive = v; onChange(); }),
      slider('Mix', cfg.mix ?? 1, 0, 1, '', 2,
        (v) => { cfg.mix = v; onChange(); }),
    );
  },
});

// Vca -------------------------------------------------------------------------
descriptors.push({
  protoField: 'vca',
  displayName: 'VCA',
  description: 'Voltage-controlled amplifier (out = in * gain)',
  category: 'utility',
  hue: 190,
  inputs: [
    {name: 'in', kind: 'audio'},
    {name: 'gain', kind: 'cv'},
  ],
  outputs: [{name: 'out', kind: 'audio'}],
  createDefault: (): protos.ISynthModule => ({
    vca: {initialGain: 1.0},
  }),
  renderParams: (mod, onChange) => {
    const cfg = mod.vca!;
    return m('div', {style: {fontSize: '11px'}},
      slider('Gain', cfg.initialGain ?? 1, 0, 2, '', 2,
        (v) => { cfg.initialGain = v; onChange(); }),
    );
  },
});

// Mixer -----------------------------------------------------------------------
descriptors.push({
  protoField: 'mixer',
  displayName: 'Mixer',
  description: 'Sums all connected inputs',
  category: 'utility',
  hue: 180,
  inputs: [{name: 'in', kind: 'audio'}],
  outputs: [{name: 'out', kind: 'audio'}],
  createDefault: (): protos.ISynthModule => ({mixer: {}}),
  renderParams: () => m('div', {
    style: {fontSize: '10px', color: '#888', padding: '4px 0'},
  }, 'Sums all inputs'),
});

// --- Full descriptors for the remaining blocks (Milestone 2) ---

// Oscillators: all have a freq_in port and out port.
const OSC_INPUTS: ReadonlyArray<PortSpec> = [
  {name: 'freq', kind: 'freq'},
  {name: 'freq_mod', kind: 'cv'},
];
const OSC_OUTPUTS: ReadonlyArray<PortSpec> = [{name: 'out', kind: 'audio'}];

// FmOsc -----------------------------------------------------------------------
descriptors.push({
  protoField: 'fm_osc',
  displayName: 'FM Osc',
  description: '2-operator phase-modulation FM oscillator (Chowning)',
  category: 'oscillator',
  hue: 40,
  inputs: OSC_INPUTS,
  outputs: OSC_OUTPUTS,
  createDefault: (): protos.ISynthModule => ({
    fmOsc: {baseFreqHz: 220, modRatio: 1, modIndex: 1, feedback: 0},
  }),
  renderParams: (mod, onChange) => {
    const cfg = mod.fmOsc!;
    return m('div', {style: {fontSize: '11px'}},
      logSlider('Freq', cfg.baseFreqHz ?? 220, 20, 4000, 'Hz',
        (v) => { cfg.baseFreqHz = v; onChange(); }),
      slider('Ratio', cfg.modRatio ?? 1, 0, 16, 'x', 2,
        (v) => { cfg.modRatio = v; onChange(); }),
      slider('Index', cfg.modIndex ?? 1, 0, 32, '', 2,
        (v) => { cfg.modIndex = v; onChange(); }),
      slider('Feedback', cfg.feedback ?? 0, 0, 1, '', 2,
        (v) => { cfg.feedback = v; onChange(); }),
    );
  },
});

// PhaseDistortionOsc ----------------------------------------------------------
descriptors.push({
  protoField: 'phase_distortion_osc',
  displayName: 'Phase Dist',
  description: 'Casio CZ-style phase-distortion oscillator',
  category: 'oscillator',
  hue: 50,
  inputs: OSC_INPUTS,
  outputs: OSC_OUTPUTS,
  createDefault: (): protos.ISynthModule => ({
    phaseDistortionOsc: {
      mode: protos.PhaseDistortionOscConfig.Mode.SAW_WARP,
      baseFreqHz: 220, amount: 0.5,
    },
  }),
  renderParams: (mod, onChange) => {
    const cfg = mod.phaseDistortionOsc!;
    return m('div', {style: {fontSize: '11px'}},
      dropdown('Mode', cfg.mode ?? 0, [
        {value: 0, label: 'Saw Warp'},
        {value: 1, label: 'Pulse Warp'},
      ], (v) => { cfg.mode = v; onChange(); }),
      logSlider('Freq', cfg.baseFreqHz ?? 220, 20, 4000, 'Hz',
        (v) => { cfg.baseFreqHz = v; onChange(); }),
      slider('Amount', cfg.amount ?? 0.5, 0, 1, '', 2,
        (v) => { cfg.amount = v; onChange(); }),
    );
  },
});

// FoldOsc ---------------------------------------------------------------------
descriptors.push({
  protoField: 'fold_osc',
  displayName: 'Fold Osc',
  description: 'Smooth wavefolder oscillator (sin(drive·sin))',
  category: 'oscillator',
  hue: 60,
  inputs: OSC_INPUTS,
  outputs: OSC_OUTPUTS,
  createDefault: (): protos.ISynthModule => ({
    foldOsc: {baseFreqHz: 220, drive: 3, bias: 0},
  }),
  renderParams: (mod, onChange) => {
    const cfg = mod.foldOsc!;
    return m('div', {style: {fontSize: '11px'}},
      logSlider('Freq', cfg.baseFreqHz ?? 220, 20, 4000, 'Hz',
        (v) => { cfg.baseFreqHz = v; onChange(); }),
      slider('Drive', cfg.drive ?? 3, 1, 20, '', 1,
        (v) => { cfg.drive = v; onChange(); }),
      slider('Bias', cfg.bias ?? 0, -1, 1, '', 2,
        (v) => { cfg.bias = v; onChange(); }),
    );
  },
});

// SyncOsc ---------------------------------------------------------------------
descriptors.push({
  protoField: 'sync_osc',
  displayName: 'Sync Osc',
  description: 'Hardsync oscillator (master+slave)',
  category: 'oscillator',
  hue: 70,
  inputs: OSC_INPUTS,
  outputs: OSC_OUTPUTS,
  createDefault: (): protos.ISynthModule => ({
    syncOsc: {baseFreqHz: 110, syncRatio: 2},
  }),
  renderParams: (mod, onChange) => {
    const cfg = mod.syncOsc!;
    return m('div', {style: {fontSize: '11px'}},
      logSlider('Master', cfg.baseFreqHz ?? 110, 20, 2000, 'Hz',
        (v) => { cfg.baseFreqHz = v; onChange(); }),
      slider('Ratio', cfg.syncRatio ?? 2, 1, 16, 'x', 2,
        (v) => { cfg.syncRatio = v; onChange(); }),
    );
  },
});

// SuperOsc --------------------------------------------------------------------
descriptors.push({
  protoField: 'super_osc',
  displayName: 'SuperSaw',
  description: 'JP-8000 7-saw supersaw',
  category: 'oscillator',
  hue: 80,
  inputs: OSC_INPUTS,
  outputs: OSC_OUTPUTS,
  createDefault: (): protos.ISynthModule => ({
    superOsc: {baseFreqHz: 220, detune: 0.3, mix: 0.5},
  }),
  renderParams: (mod, onChange) => {
    const cfg = mod.superOsc!;
    return m('div', {style: {fontSize: '11px'}},
      logSlider('Freq', cfg.baseFreqHz ?? 220, 20, 4000, 'Hz',
        (v) => { cfg.baseFreqHz = v; onChange(); }),
      slider('Detune', cfg.detune ?? 0.3, 0, 1, '', 2,
        (v) => { cfg.detune = v; onChange(); }),
      slider('Mix', cfg.mix ?? 0.5, 0, 1, '', 2,
        (v) => { cfg.mix = v; onChange(); }),
    );
  },
});

// WavetableOsc ----------------------------------------------------------------
descriptors.push({
  protoField: 'wavetable_osc',
  displayName: 'Wavetable',
  description: 'Procedural wavetable oscillator (4 tables)',
  category: 'oscillator',
  hue: 90,
  inputs: [
    {name: 'freq', kind: 'freq'},
    {name: 'freq_mod', kind: 'cv'},
    {name: 'position_mod', kind: 'cv'},
  ],
  outputs: OSC_OUTPUTS,
  createDefault: (): protos.ISynthModule => ({
    wavetableOsc: {
      tableType: protos.WavetableOscConfig.TableType.SINE_TO_SAW,
      baseFreqHz: 220, basePosition: 0,
    },
  }),
  renderParams: (mod, onChange) => {
    const cfg = mod.wavetableOsc!;
    return m('div', {style: {fontSize: '11px'}},
      dropdown('Table', cfg.tableType ?? 0, [
        {value: 0, label: 'Sine→Saw'},
        {value: 1, label: 'Pulse Sweep'},
        {value: 2, label: 'Bell'},
        {value: 3, label: 'Vocal'},
      ], (v) => { cfg.tableType = v; onChange(); }),
      logSlider('Freq', cfg.baseFreqHz ?? 220, 20, 4000, 'Hz',
        (v) => { cfg.baseFreqHz = v; onChange(); }),
      slider('Pos', cfg.basePosition ?? 0, 0, 1, '', 2,
        (v) => { cfg.basePosition = v; onChange(); }),
    );
  },
});

// NoiseOsc --------------------------------------------------------------------
descriptors.push({
  protoField: 'noise_osc',
  displayName: 'Noise',
  description: 'Tilted noise (white → pink → brown)',
  category: 'oscillator',
  hue: 100,
  inputs: [],
  outputs: [{name: 'out', kind: 'audio'}],
  createDefault: (): protos.ISynthModule => ({
    noiseOsc: {tilt: 0.5, seed: 0},
  }),
  renderParams: (mod, onChange) => {
    const cfg = mod.noiseOsc!;
    const tilt = cfg.tilt ?? 0.5;
    const flavor =
      tilt < 0.2 ? 'white' :
      tilt < 0.7 ? 'pink' : 'brown';
    return m('div', {style: {fontSize: '11px'}},
      slider('Tilt', tilt, 0, 1, '', 2,
        (v) => { cfg.tilt = v; onChange(); }),
      m('div', {
        style: {
          fontSize: '9px', color: '#888', textAlign: 'right',
          fontStyle: 'italic',
        },
      }, `(${flavor})`),
      slider('Seed', cfg.seed ?? 0, 0, 1024, '', 0,
        (v) => { cfg.seed = Math.round(v); onChange(); }),
    );
  },
});

// DrawbarOrgan ----------------------------------------------------------------
const DRAWBAR_DEFS: ReadonlyArray<{
  field: keyof protos.IDrawbarOrganConfig;
  label: string;
}> = [
  {field: 'db16',    label: '16'},
  {field: 'db5_1_3', label: '5⅓'},
  {field: 'db8',     label: '8'},
  {field: 'db4',     label: '4'},
  {field: 'db2_2_3', label: '2⅔'},
  {field: 'db2',     label: '2'},
  {field: 'db1_3_5', label: '1⅗'},
  {field: 'db1_1_3', label: '1⅓'},
  {field: 'db1',     label: '1'},
];

const DRAWBAR_PRESETS: ReadonlyArray<{
  name: string;
  values: ReadonlyArray<number>;
}> = [
  {name: 'Full Organ',  values: [1, 1, 1, 1, 1, 1, 1, 1, 1]},
  {name: 'Jazz',        values: [0.875, 0.875, 0.875, 0, 0, 0, 0, 0, 0]},
  {name: 'Gospel',      values: [0.75, 0.625, 0.875, 0, 0, 0, 0, 0, 0]},
  {name: 'Bright Lead', values: [0.875, 0.625, 0.875, 0.625, 0.625, 0.625, 0.5, 0.375, 0.875]},
  {name: 'Whistle',     values: [0, 0, 0.875, 0, 0, 0, 0, 0, 0.875]},
];

descriptors.push({
  protoField: 'drawbar_organ',
  displayName: 'Drawbar Organ',
  description: 'Hammond B3 9-drawbar additive synth',
  category: 'oscillator',
  hue: 110,
  inputs: [{name: 'freq', kind: 'freq'}],
  outputs: [{name: 'out', kind: 'audio'}],
  createDefault: (): protos.ISynthModule => ({
    drawbarOrgan: {
      baseFreqHz: 220,
      db16: 0.875, db5_1_3: 0.875, db8: 0.875,
      db4: 0, db2_2_3: 0, db2: 0,
      db1_3_5: 0, db1_1_3: 0, db1: 0,
    },
  }),
  renderParams: (mod, onChange) => {
    const cfg = mod.drawbarOrgan! as Record<string, number | undefined>;
    return m('div', {style: {fontSize: '11px', minWidth: '220px'}},
      logSlider('Freq', (cfg.baseFreqHz as number) ?? 220, 20, 2000, 'Hz',
        (v) => { cfg.baseFreqHz = v; onChange(); }),
      // 9 vertical drawbars in a row.
      m('div', {
        style: {
          display: 'flex', justifyContent: 'space-between',
          padding: '4px 2px 2px 2px', background: '#2a2a2e',
          borderRadius: '3px', marginTop: '4px',
        },
      },
        DRAWBAR_DEFS.map((d) => verticalSlider(
          d.label,
          (cfg[d.field] as number) ?? 0,
          (v) => { cfg[d.field] = v; onChange(); },
        )),
      ),
      // Preset chips.
      m('div', {
        style: {
          display: 'flex', flexWrap: 'wrap', gap: '2px', marginTop: '4px',
        },
      },
        DRAWBAR_PRESETS.map((p) =>
          m('button', {
            style: {
              fontSize: '9px', padding: '1px 4px', cursor: 'pointer',
              border: '1px solid #ccc', background: '#f8f8f8',
              borderRadius: '2px',
            },
            onclick: (e: Event) => {
              e.stopPropagation();
              for (let i = 0; i < DRAWBAR_DEFS.length; i++) {
                cfg[DRAWBAR_DEFS[i].field] = p.values[i];
              }
              onChange();
            },
            onmousedown: (e: Event) => e.stopPropagation(),
          }, p.name),
        ),
      ),
    );
  },
});

// Lfo -------------------------------------------------------------------------
descriptors.push({
  protoField: 'lfo',
  displayName: 'LFO',
  description: 'Low-frequency oscillator (control-rate modulation)',
  category: 'modulator',
  hue: 260,
  inputs: [],
  outputs: [{name: 'out', kind: 'cv'}],
  createDefault: (): protos.ISynthModule => ({
    lfo: {
      waveform: protos.LfoConfig.Waveform.SINE,
      rateHz: 2, depth: 1, bipolar: true, seed: 0,
    },
  }),
  renderParams: (mod, onChange) => {
    const cfg = mod.lfo!;
    return m('div', {style: {fontSize: '11px'}},
      dropdown('Wave', cfg.waveform ?? 0, [
        {value: 0, label: 'Sine'},
        {value: 1, label: 'Triangle'},
        {value: 2, label: 'Square'},
        {value: 3, label: 'Saw Up'},
        {value: 4, label: 'Saw Down'},
        {value: 5, label: 'Sample & Hold'},
      ], (v) => { cfg.waveform = v; onChange(); }),
      logSlider('Rate', cfg.rateHz ?? 2, 0.01, 50, 'Hz',
        (v) => { cfg.rateHz = v; onChange(); }),
      slider('Depth', cfg.depth ?? 1, 0, 1, '', 2,
        (v) => { cfg.depth = v; onChange(); }),
      checkbox('Bipolar', cfg.bipolar ?? true,
        (v) => { cfg.bipolar = v; onChange(); }),
    );
  },
});

// Delay -----------------------------------------------------------------------
descriptors.push({
  protoField: 'delay',
  displayName: 'Delay',
  description: 'Feedback delay with damping (dub-style)',
  category: 'effect',
  hue: 170,
  inputs: [{name: 'in', kind: 'audio'}],
  outputs: [{name: 'out', kind: 'audio'}],
  createDefault: (): protos.ISynthModule => ({
    delay: {timeMs: 250, feedback: 0.4, damping: 0.3, mix: 0.4},
  }),
  renderParams: (mod, onChange) => {
    const cfg = mod.delay!;
    return m('div', {style: {fontSize: '11px'}},
      logSlider('Time', cfg.timeMs ?? 250, 1, 2000, 'ms',
        (v) => { cfg.timeMs = v; onChange(); }),
      slider('Feedback', cfg.feedback ?? 0.4, 0, 0.95, '', 2,
        (v) => { cfg.feedback = v; onChange(); }),
      slider('Damping', cfg.damping ?? 0.3, 0, 0.99, '', 2,
        (v) => { cfg.damping = v; onChange(); }),
      slider('Mix', cfg.mix ?? 0.4, 0, 1, '', 2,
        (v) => { cfg.mix = v; onChange(); }),
    );
  },
});

// Chorus ----------------------------------------------------------------------
descriptors.push({
  protoField: 'chorus',
  displayName: 'Chorus',
  description: 'Multi-voice modulated-delay chorus',
  category: 'effect',
  hue: 160,
  inputs: [{name: 'in', kind: 'audio'}],
  outputs: [{name: 'out', kind: 'audio'}],
  createDefault: (): protos.ISynthModule => ({
    chorus: {
      rateHz: 0.5, depthMs: 4, midDelayMs: 15, mix: 0.5, voices: 3,
    },
  }),
  renderParams: (mod, onChange) => {
    const cfg = mod.chorus!;
    return m('div', {style: {fontSize: '11px'}},
      logSlider('Rate', cfg.rateHz ?? 0.5, 0.01, 10, 'Hz',
        (v) => { cfg.rateHz = v; onChange(); }),
      slider('Depth', cfg.depthMs ?? 4, 0, 40, 'ms', 1,
        (v) => { cfg.depthMs = v; onChange(); }),
      slider('Center', cfg.midDelayMs ?? 15, 1, 30, 'ms', 1,
        (v) => { cfg.midDelayMs = v; onChange(); }),
      slider('Voices', cfg.voices ?? 3, 1, 8, '', 0,
        (v) => { cfg.voices = Math.round(v); onChange(); }),
      slider('Mix', cfg.mix ?? 0.5, 0, 1, '', 2,
        (v) => { cfg.mix = v; onChange(); }),
    );
  },
});

// --- Generic descriptors for legacy blocks ---

const GENERIC_HUE: Record<string, number> = {
  envelope: 250,
  vco: 25,
};

function buildGenericParams(fieldName: string, mod: protos.ISynthModule) {
  const obj = (mod as Record<string, unknown>)[camelize(fieldName)];
  if (!obj || typeof obj !== 'object') {
    return m('div', {style: {fontSize: '10px', color: '#888'}},
      '(no params)');
  }
  const entries = Object.entries(obj as Record<string, unknown>);
  return m('div', {
    style: {fontSize: '10px', color: '#777', padding: '2px 0'},
  },
    entries.length === 0
      ? m('span', '(no params)')
      : entries.map(([k, v]) =>
          m('div', {style: {display: 'flex', gap: '4px'}},
            m('span', {style: {color: '#aaa', width: '70px'}}, k),
            m('span', {style: {
              fontFamily: 'monospace', color: '#555',
            }}, formatValue(v)),
          ),
        ),
  );
}

function formatValue(v: unknown): string {
  if (typeof v === 'number') return v.toFixed(2);
  if (typeof v === 'string') return v;
  if (typeof v === 'boolean') return v ? 'true' : 'false';
  if (v === null || v === undefined) return '-';
  return JSON.stringify(v);
}

function camelize(snake: string): string {
  return snake.replace(/_([a-z])/g, (_, c: string) => c.toUpperCase());
}

function addGeneric(
  protoField: string, displayName: string, description: string,
  category: BlockCategory,
  ports: {inputs: PortSpec[]; outputs: PortSpec[]},
  createDefault: () => protos.ISynthModule,
) {
  const hue = GENERIC_HUE[protoField] ?? 150;
  descriptors.push({
    protoField, displayName, description, category, hue,
    inputs: ports.inputs, outputs: ports.outputs,
    createDefault,
    renderParams: (mod) => buildGenericParams(protoField, mod),
  });
}

// Legacy blocks (kept for backward compat with old presets / UI).
addGeneric(
  'vco', 'VCO (legacy)', 'Naive oscillator (prefer ClassicOsc)',
  'oscillator',
  {
    inputs: [{name: 'freq_mod', kind: 'cv'}],
    outputs: [{name: 'out', kind: 'audio'}],
  },
  (): protos.ISynthModule => ({vco: {baseFreqHz: 220}}),
);
addGeneric(
  'envelope', 'Envelope (legacy)', 'AD envelope (prefer ADSR)',
  'modulator',
  {
    inputs: [{name: 'trigger', kind: 'gate'}],
    outputs: [{name: 'out', kind: 'cv'}],
  },
  (): protos.ISynthModule => ({envelope: {attackMs: 5, decayMs: 200}}),
);

// --- Index + lookup ---

const byField = new Map<string, BlockDescriptor>();
for (const d of descriptors) byField.set(d.protoField, d);

export function getAllDescriptors(): ReadonlyArray<BlockDescriptor> {
  return descriptors;
}

export function getDescriptorByField(
  protoField: string,
): BlockDescriptor | undefined {
  return byField.get(protoField);
}

/**
 * Given a SynthModule, find the BlockDescriptor for its populated oneof.
 */
export function getDescriptorForModule(
  mod: protos.ISynthModule,
): BlockDescriptor | undefined {
  for (const d of descriptors) {
    const camel = camelize(d.protoField);
    const value = (mod as unknown as Record<string, unknown>)[camel];
    if (value !== undefined && value !== null) return d;
  }
  return undefined;
}

/** Group descriptors by category for palette rendering. */
export function descriptorsByCategory(): Map<BlockCategory, BlockDescriptor[]> {
  const out = new Map<BlockCategory, BlockDescriptor[]>();
  for (const d of descriptors) {
    const list = out.get(d.category) ?? [];
    list.push(d);
    out.set(d.category, list);
  }
  return out;
}
