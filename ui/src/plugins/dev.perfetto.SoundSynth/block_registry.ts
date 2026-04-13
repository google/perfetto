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
// Milestone 1 ships full panels for 8 key blocks; the others use a
// generic fallback that introspects the proto oneof and auto-renders.

import m from 'mithril';
import protos from '../../protos';

export type PortKind = 'audio' | 'cv' | 'gate' | 'freq';

export interface PortSpec {
  readonly name: string;
  readonly kind: PortKind;
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

// --- Generic descriptors for the remaining blocks (Milestone 1 stubs) ---

const GENERIC_HUE: Record<string, number> = {
  classic_osc: 30,
  fm_osc: 40,
  phase_distortion_osc: 50,
  fold_osc: 60,
  sync_osc: 70,
  super_osc: 80,
  wavetable_osc: 90,
  noise_osc: 100,
  drawbar_organ: 110,
  lfo: 260,
  chorus: 160,
  delay: 170,
  envelope: 250,
  vco: 25,
};

// Oscillators: all have a freq_in port and out port.
function oscPorts(): {inputs: PortSpec[]; outputs: PortSpec[]} {
  return {
    inputs: [
      {name: 'freq', kind: 'freq'},
      {name: 'freq_mod', kind: 'cv'},
    ],
    outputs: [{name: 'out', kind: 'audio'}],
  };
}

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

addGeneric(
  'fm_osc', 'FM Osc', '2-op FM oscillator',
  'oscillator', oscPorts(),
  (): protos.ISynthModule => ({
    fmOsc: {baseFreqHz: 220, modRatio: 1, modIndex: 1, feedback: 0},
  }),
);
addGeneric(
  'phase_distortion_osc', 'Phase Dist', 'Casio CZ-style phase warp',
  'oscillator', oscPorts(),
  (): protos.ISynthModule => ({phaseDistortionOsc: {baseFreqHz: 220}}),
);
addGeneric(
  'fold_osc', 'Fold Osc', 'Wavefolder oscillator',
  'oscillator', oscPorts(),
  (): protos.ISynthModule => ({foldOsc: {baseFreqHz: 220}}),
);
addGeneric(
  'sync_osc', 'Sync Osc', 'Hardsync oscillator',
  'oscillator', oscPorts(),
  (): protos.ISynthModule => ({syncOsc: {}}),
);
addGeneric(
  'super_osc', 'SuperSaw', 'JP-8000 7-saw supersaw',
  'oscillator', oscPorts(),
  (): protos.ISynthModule => ({superOsc: {baseFreqHz: 220}}),
);
addGeneric(
  'wavetable_osc', 'Wavetable', 'Wavetable oscillator (4 tables)',
  'oscillator', {
    inputs: [
      {name: 'freq', kind: 'freq'},
      {name: 'freq_mod', kind: 'cv'},
      {name: 'position_mod', kind: 'cv'},
    ],
    outputs: [{name: 'out', kind: 'audio'}],
  },
  (): protos.ISynthModule => ({wavetableOsc: {baseFreqHz: 220}}),
);
addGeneric(
  'noise_osc', 'Noise', 'Tilted white/pink/brown noise',
  'oscillator',
  {inputs: [], outputs: [{name: 'out', kind: 'audio'}]},
  (): protos.ISynthModule => ({noiseOsc: {tilt: 0.5}}),
);
addGeneric(
  'drawbar_organ', 'Drawbar Organ', 'Hammond B3 additive synth',
  'oscillator',
  {
    inputs: [{name: 'freq', kind: 'freq'}],
    outputs: [{name: 'out', kind: 'audio'}],
  },
  (): protos.ISynthModule => ({drawbarOrgan: {baseFreqHz: 220}}),
);
addGeneric(
  'lfo', 'LFO', 'Low-frequency oscillator',
  'modulator',
  {inputs: [], outputs: [{name: 'out', kind: 'cv'}]},
  (): protos.ISynthModule => ({lfo: {rateHz: 2, bipolar: true}}),
);
addGeneric(
  'chorus', 'Chorus', 'Multi-voice modulated delay',
  'effect',
  {
    inputs: [{name: 'in', kind: 'audio'}],
    outputs: [{name: 'out', kind: 'audio'}],
  },
  (): protos.ISynthModule => ({chorus: {rateHz: 0.5, depthMs: 4, mix: 0.5}}),
);
addGeneric(
  'delay', 'Delay', 'Circular-buffer feedback delay',
  'effect',
  {
    inputs: [{name: 'in', kind: 'audio'}],
    outputs: [{name: 'out', kind: 'audio'}],
  },
  (): protos.ISynthModule => ({delay: {
    timeMs: 250, feedback: 0.4, mix: 0.3,
  }}),
);
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
