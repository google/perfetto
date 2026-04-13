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

// Patch state helpers for the SoundSynth plugin.
//
// The single source of truth is a `protos.ISynthesizeAudioArgs` object.
// Everything else is a derived view over it. See
// docs/design-docs/trace-to-techno/ui-design.md for the full spec.

import {z} from 'zod';
import protos from '../../protos';

// --- Reserved module IDs ---

/** Virtual module ID: the gate signal coming into an instrument. */
export const VIRTUAL_INPUT_GATE = '__input__gate';
/** Virtual module ID: the pitch/freq CV coming into an instrument. */
export const VIRTUAL_INPUT_FREQ = '__input__freq';
/** The ID of the rack-level master mixer. */
export const RACK_MASTER_ID = 'master';

export function isVirtualInputId(id: string | undefined | null): boolean {
  return id === VIRTUAL_INPUT_GATE || id === VIRTUAL_INPUT_FREQ;
}

// --- UI state schemas ---

/** Top-level page UI state (stored in SynthPatch.ui_state_json). */
export const PatchUiStateSchema = z.object({
  editingInstrumentId: z.string().nullable().default(null),
});
export type PatchUiState = z.infer<typeof PatchUiStateSchema>;

/** Node kinds used by the UI for rendering/grouping. */
const NodeKindSchema = z.enum([
  'trace_source',    // TraceSliceSource at the rack level
  'instrument_root', // The instrument's own output mixer (rack-visible)
  'instrument_internal', // Any non-root module belonging to an instrument
  'master',          // The rack master mixer
  'unknown',         // Fallback
]);

/** UI state stored per module (SynthModule.ui_state_json). */
export const ModuleUiStateSchema = z.object({
  nodeKind: NodeKindSchema.default('unknown'),
  displayName: z.string().default(''),
  // Canvas position. For trace sources and the instrument root module,
  // this is the node's position on the rack canvas (top). For
  // instrument_internal modules, it's the position inside the
  // instrument editor canvas (bottom).
  x: z.number().default(0),
  y: z.number().default(0),
  // The instrument root's OUTPUT node position in the instrument editor
  // (bottom canvas). Stored separately from x/y because the same module
  // is rendered in both canvases (as an Instrument on the rack and as
  // the OUTPUT inside the instrument editor).
  outX: z.number().nullable().default(null),
  outY: z.number().nullable().default(null),
  // Instrument-root specific fields:
  presetId: z.string().default(''),
  muted: z.boolean().default(false),
  soloed: z.boolean().default(false),
  level: z.number().default(0.8),
  // IDs of the rack-level modules bound to this instrument's virtual
  // gate / freq inputs. Empty string = not bound.
  gateSource: z.string().default(''),
  freqSource: z.string().default(''),
});
export type ModuleUiState = z.infer<typeof ModuleUiStateSchema>;

// --- JSON parsing helpers ---

export function parsePatchUiState(
  json: string | null | undefined,
): PatchUiState {
  if (!json) return PatchUiStateSchema.parse({});
  try {
    return PatchUiStateSchema.parse(JSON.parse(json));
  } catch {
    return PatchUiStateSchema.parse({});
  }
}

export function parseModuleUiState(
  json: string | null | undefined,
): ModuleUiState {
  if (!json) return ModuleUiStateSchema.parse({});
  try {
    return ModuleUiStateSchema.parse(JSON.parse(json));
  } catch {
    return ModuleUiStateSchema.parse({});
  }
}

export function writeModuleUiState(
  mod: protos.ISynthModule,
  patch: Partial<ModuleUiState>,
) {
  const current = parseModuleUiState(mod.uiStateJson);
  mod.uiStateJson = JSON.stringify({...current, ...patch});
}

export function writePatchUiState(
  patch: protos.ISynthPatch,
  state: Partial<PatchUiState>,
) {
  const current = parsePatchUiState(patch.uiStateJson);
  patch.uiStateJson = JSON.stringify({...current, ...state});
}

// --- Derived views ---

/** A trace source module at the rack level. */
export interface TraceSourceEntity {
  moduleIdx: number;
  module: protos.ISynthModule;
  uiState: ModuleUiState;
}

/** An instrument: a group of modules sharing a common ID prefix. */
export interface InstrumentEntity {
  instrumentId: string;             // The shared prefix (without trailing __)
  rootModuleIdx: number;
  rootModule: protos.ISynthModule;  // The output mixer (`${id}__master`)
  uiState: ModuleUiState;
  // Modules whose IDs start with `${instrumentId}__`. Always non-empty
  // (at least contains the root).
  internalModuleIdxs: number[];
}

/** The rack master mixer. */
export interface MasterEntity {
  moduleIdx: number;
  module: protos.ISynthModule;
  uiState: ModuleUiState;
}

/** Computed view over a SynthPatch. */
export interface PatchView {
  traceSources: TraceSourceEntity[];
  instruments: InstrumentEntity[];
  master: MasterEntity | null;
}

/**
 * Checks whether a module ID belongs to a particular instrument's internal
 * namespace. The root itself matches; any `${instrumentId}__*` does too.
 */
export function moduleBelongsToInstrument(
  moduleId: string,
  instrumentId: string,
): boolean {
  return moduleId.startsWith(`${instrumentId}__`);
}

/**
 * Extracts the instrument ID from a prefixed module ID.
 * `inst_abc__osc` → `inst_abc`
 * Returns null if the ID is not in instrument namespace format.
 */
export function instrumentIdOf(moduleId: string): string | null {
  const idx = moduleId.indexOf('__');
  if (idx < 0) return null;
  return moduleId.substring(0, idx);
}

/** Compute the derived view. */
export function computePatchView(
  patch: protos.ISynthPatch | null | undefined,
): PatchView {
  const traceSources: TraceSourceEntity[] = [];
  const instrumentMap = new Map<string, InstrumentEntity>();
  let master: MasterEntity | null = null;

  if (!patch?.modules) {
    return {traceSources, instruments: [], master};
  }
  const modules = patch.modules;

  for (let i = 0; i < modules.length; i++) {
    const mod = modules[i];
    const ui = parseModuleUiState(mod.uiStateJson);
    const id = mod.id ?? '';

    // Rack master.
    if (id === RACK_MASTER_ID && mod.mixer) {
      master = {moduleIdx: i, module: mod, uiState: ui};
      continue;
    }
    // Rack-level trace source (no `__` namespace).
    if (mod.traceSliceSource &&
        instrumentIdOf(id) === null) {
      traceSources.push({moduleIdx: i, module: mod, uiState: ui});
      continue;
    }
    // Instrument member: has `__` namespace.
    const instId = instrumentIdOf(id);
    if (instId !== null) {
      let entity = instrumentMap.get(instId);
      if (!entity) {
        entity = {
          instrumentId: instId,
          rootModuleIdx: -1,
          rootModule: mod,  // Placeholder; overwritten when we find the root.
          uiState: parseModuleUiState(mod.uiStateJson),
          internalModuleIdxs: [],
        };
        instrumentMap.set(instId, entity);
      }
      entity.internalModuleIdxs.push(i);
      if (ui.nodeKind === 'instrument_root') {
        entity.rootModuleIdx = i;
        entity.rootModule = mod;
        entity.uiState = ui;
      }
    }
  }

  // Discard instruments that never registered a root (shouldn't happen
  // normally, but keeps things safe).
  const instruments: InstrumentEntity[] = [];
  for (const inst of instrumentMap.values()) {
    if (inst.rootModuleIdx >= 0) {
      instruments.push(inst);
    }
  }

  return {traceSources, instruments, master};
}

// --- Mutation helpers ---

let _counter = 0;
function uniqueSuffix(): string {
  return `${Date.now().toString(36)}${(_counter++).toString(36)}`;
}

/** Generate a fresh instrument ID. */
export function freshInstrumentId(): string {
  return `inst_${uniqueSuffix()}`;
}

/** Create the initial patch with just a rack master mixer. */
export function createEmptyPatch(): protos.ISynthesizeAudioArgs {
  const masterUi: ModuleUiState = ModuleUiStateSchema.parse({
    nodeKind: 'master',
    displayName: 'Master Out',
    x: 760,
    y: 180,
  });
  return {
    patch: {
      modules: [{
        id: RACK_MASTER_ID,
        mixer: {},
        uiStateJson: JSON.stringify(masterUi),
      }],
      wires: [],
      uiStateJson: JSON.stringify(PatchUiStateSchema.parse({})),
    },
  };
}

/** Ensure the rack master mixer exists. */
export function ensureMaster(patch: protos.ISynthPatch): void {
  if (!patch.modules) patch.modules = [];
  if (!patch.wires) patch.wires = [];
  const hasMaster = patch.modules.some((m) => m.id === RACK_MASTER_ID);
  if (!hasMaster) {
    const ui: ModuleUiState = ModuleUiStateSchema.parse({
      nodeKind: 'master',
      displayName: 'Master Out',
      x: 760,
      y: 180,
    });
    patch.modules.push({
      id: RACK_MASTER_ID,
      mixer: {},
      uiStateJson: JSON.stringify(ui),
    });
  }
}

/**
 * Add a new trace source node to the rack. Returns the new module id.
 */
export function addTraceSource(
  patch: protos.ISynthPatch,
  trackNameGlob: string,
  displayName: string,
  x: number,
  y: number,
): string {
  if (!patch.modules) patch.modules = [];
  const id = `src_${uniqueSuffix()}`;
  const ui: ModuleUiState = ModuleUiStateSchema.parse({
    nodeKind: 'trace_source',
    displayName,
    x,
    y,
  });
  patch.modules.push({
    id,
    traceSliceSource: {
      trackNameGlob,
      signalType: protos.TraceSliceSourceConfig.SignalType.GATE,
    },
    uiStateJson: JSON.stringify(ui),
  });
  return id;
}

/**
 * Remove a module by id. Also removes any wire that references it
 * (as source or destination).
 */
export function removeModule(patch: protos.ISynthPatch, id: string): void {
  if (!patch.modules || !patch.wires) return;
  patch.modules = patch.modules.filter((m) => m.id !== id);
  patch.wires = patch.wires.filter(
    (w) => w.fromModule !== id && w.toModule !== id,
  );
  // Clear any instrument root ui_state bindings that referenced this id.
  for (const m of patch.modules) {
    const ui = parseModuleUiState(m.uiStateJson);
    if (ui.nodeKind === 'instrument_root') {
      let changed = false;
      if (ui.gateSource === id) { ui.gateSource = ''; changed = true; }
      if (ui.freqSource === id) { ui.freqSource = ''; changed = true; }
      if (changed) m.uiStateJson = JSON.stringify(ui);
    }
  }
}

/**
 * Remove an entire instrument: all modules in its namespace, all wires
 * referencing those modules, and the rack wire into the master.
 */
export function removeInstrument(
  patch: protos.ISynthPatch,
  instrumentId: string,
): void {
  if (!patch.modules || !patch.wires) return;
  const prefix = `${instrumentId}__`;
  patch.modules = patch.modules.filter((m) => !(m.id ?? '').startsWith(prefix));
  patch.wires = patch.wires.filter((w) => {
    const f = w.fromModule ?? '';
    const t = w.toModule ?? '';
    return !f.startsWith(prefix) && !t.startsWith(prefix);
  });
}

/**
 * Bind an instrument's gate or freq input to a rack-level source module.
 */
export function setInstrumentSource(
  _patch: protos.ISynthPatch,
  instrumentRoot: protos.ISynthModule,
  kind: 'gate' | 'freq',
  sourceId: string,
): void {
  const ui = parseModuleUiState(instrumentRoot.uiStateJson);
  if (kind === 'gate') ui.gateSource = sourceId;
  else ui.freqSource = sourceId;
  instrumentRoot.uiStateJson = JSON.stringify(ui);
}

// --- Render patch builders ---

/**
 * Resolve a virtual INPUT module id to a real source module id, or
 * return null if no binding exists.
 */
function resolveVirtualSource(
  fromModule: string,
  bindings: Map<string, {gate: string; freq: string}>,
  instrumentIdForWire: string,
): string | null {
  if (fromModule !== VIRTUAL_INPUT_GATE &&
      fromModule !== VIRTUAL_INPUT_FREQ) {
    return fromModule;  // Not virtual, return as-is.
  }
  const b = bindings.get(instrumentIdForWire);
  if (!b) return null;
  if (fromModule === VIRTUAL_INPUT_GATE) return b.gate || null;
  return b.freq || null;
}

/**
 * Build the render patch for the Render button (whole rack).
 *
 * - Walks all non-muted instruments (respecting solo)
 * - Includes all trace sources referenced by any audible instrument
 * - Rewrites virtual INPUT wires to point at real rack sources
 * - Drops any virtual wires for unbound inputs
 */
export function buildRenderPatch(
  view: PatchView,
  allModules: protos.ISynthModule[],
  allWires: protos.ISynthWire[],
): protos.ISynthPatch {
  const hasSoloed = view.instruments.some((i) => i.uiState.soloed);

  // Build a gate/freq binding map and collect audible instrument ids.
  const bindings = new Map<string, {gate: string; freq: string}>();
  const includeInstIds = new Set<string>();
  const usedRackSourceIds = new Set<string>();
  for (const inst of view.instruments) {
    const audible = hasSoloed ? inst.uiState.soloed : !inst.uiState.muted;
    if (!audible) continue;
    includeInstIds.add(inst.instrumentId);
    bindings.set(inst.instrumentId, {
      gate: inst.uiState.gateSource,
      freq: inst.uiState.freqSource,
    });
    if (inst.uiState.gateSource) usedRackSourceIds.add(inst.uiState.gateSource);
    if (inst.uiState.freqSource) usedRackSourceIds.add(inst.uiState.freqSource);
  }

  const modules: protos.ISynthModule[] = [];
  const wires: protos.ISynthWire[] = [];

  // Include used trace sources.
  for (const src of view.traceSources) {
    if (usedRackSourceIds.has(src.module.id ?? '')) {
      modules.push(src.module);
    }
  }

  // Include all internal modules of audible instruments.
  for (const inst of view.instruments) {
    if (!includeInstIds.has(inst.instrumentId)) continue;
    for (const idx of inst.internalModuleIdxs) {
      modules.push(allModules[idx]);
    }
  }

  // Include the master.
  if (view.master) modules.push(view.master.module);

  // Rewrite wires:
  //   - virtual INPUT from_module → real source id (or drop wire)
  //   - keep only wires whose endpoints are in the included set
  const includedIds = new Set<string>(modules.map((m) => m.id ?? ''));
  for (const wire of allWires) {
    const fromRaw = wire.fromModule ?? '';
    const toRaw = wire.toModule ?? '';

    // Figure out which instrument's namespace this wire belongs to
    // (if any). We use the to-module's namespace because the virtual
    // INPUT always appears as a from-module, and its destination is
    // inside the instrument.
    const toInstId = instrumentIdOf(toRaw);
    let resolvedFrom: string | null = fromRaw;
    if (isVirtualInputId(fromRaw)) {
      if (toInstId !== null) {
        resolvedFrom = resolveVirtualSource(fromRaw, bindings, toInstId);
      } else {
        resolvedFrom = null;  // Virtual wire outside any instrument = drop.
      }
    }
    if (resolvedFrom === null) continue;
    if (!includedIds.has(resolvedFrom)) continue;
    if (!includedIds.has(toRaw)) continue;

    wires.push({
      ...wire,
      fromModule: resolvedFrom,
    });
  }

  return {modules, wires};
}

/**
 * Build a standalone test patch for a single instrument. Used by the
 * Test button on a rack instrument or in the bottom canvas.
 *
 * This:
 * - Takes the instrument's internal modules as-is
 * - Prepends a fresh TestPatternSource (ARPEGGIO, 128 BPM)
 * - Rewrites virtual INPUT wires to point at the test source
 * - Adds a fresh master Mixer receiving the instrument root's output
 *
 * Nothing is ever saved; this is constructed on the fly for one render.
 */
export function buildTestPatch(
  inst: InstrumentEntity,
  allModules: protos.ISynthModule[],
  allWires: protos.ISynthWire[],
): protos.ISynthPatch {
  const TEST_SRC_ID = '__test_arp__';
  const TEST_MASTER_ID = '__test_master__';

  const modules: protos.ISynthModule[] = [];

  // Include all internal modules of this instrument.
  for (const idx of inst.internalModuleIdxs) {
    modules.push(allModules[idx]);
  }

  // Prepend the test pattern source.
  modules.unshift({
    id: TEST_SRC_ID,
    testPatternSource: {
      mode: protos.TestPatternSourceConfig.Mode.ARPEGGIO,
      bpm: 128,
      bars: 4,
    },
  });

  // Append a standalone master mixer.
  modules.push({
    id: TEST_MASTER_ID,
    mixer: {},
  });

  const includedIds = new Set<string>(modules.map((m) => m.id ?? ''));

  const wires: protos.ISynthWire[] = [];
  for (const wire of allWires) {
    const fromRaw = wire.fromModule ?? '';
    const toRaw = wire.toModule ?? '';

    // Only keep wires that are inside this instrument.
    const toInstId = instrumentIdOf(toRaw);
    const fromInstId = instrumentIdOf(fromRaw);
    const isFromVirtual = isVirtualInputId(fromRaw);
    if (toInstId !== inst.instrumentId) continue;
    if (!isFromVirtual && fromInstId !== inst.instrumentId) continue;

    let resolvedFrom = fromRaw;
    let fromPort = wire.fromPort ?? 'out';
    if (fromRaw === VIRTUAL_INPUT_GATE) {
      resolvedFrom = TEST_SRC_ID;
      fromPort = 'out';
    } else if (fromRaw === VIRTUAL_INPUT_FREQ) {
      resolvedFrom = TEST_SRC_ID;
      fromPort = 'freq';
    }

    if (!includedIds.has(resolvedFrom)) continue;
    if (!includedIds.has(toRaw)) continue;

    wires.push({
      ...wire,
      fromModule: resolvedFrom,
      fromPort,
    });
  }

  // Wire the instrument root → test master.
  wires.push({
    fromModule: inst.rootModule.id ?? '',
    fromPort: 'out',
    toModule: TEST_MASTER_ID,
    toPort: 'in',
  });

  return {modules, wires};
}

// --- Preset import ---

/**
 * Import a preset patch into the main patch as a new instrument.
 * The preset JSON is expected already converted to camelCase and
 * decoded via protobufjs.
 *
 * Steps:
 *   1. Strip any TestPatternSource modules from the preset.
 *   2. Record which module IDs the test source occupied so we can
 *      rewrite dangling wires to virtual INPUT.
 *   3. Prefix every remaining module ID with `${instrumentId}__`.
 *   4. Rewrite all wire endpoints (keeping virtual INPUT for wires
 *      that were pointing at the stripped test source).
 *   5. Mark the former internal `master` mixer as the instrument root.
 *   6. Append everything to the target patch; add a rack wire from the
 *      root to the rack master.
 *
 * Returns the new instrument id.
 */
export function importPresetAsInstrument(
  target: protos.ISynthPatch,
  preset: {
    patch: protos.ISynthPatch;
    name: string;
    category: string;
    description: string;
  },
  displayName: string,
  x: number,
  y: number,
): string {
  ensureMaster(target);
  if (!target.modules) target.modules = [];
  if (!target.wires) target.wires = [];

  const instrumentId = freshInstrumentId();
  const prefix = `${instrumentId}__`;

  // 1. Identify test pattern source ids to strip.
  const stripIds = new Set<string>();
  for (const m of preset.patch.modules ?? []) {
    if (m.testPatternSource) stripIds.add(m.id ?? '');
  }

  // 2. Build a map from old ID → new (prefixed) ID for the survivors.
  const idMap = new Map<string, string>();
  for (const m of preset.patch.modules ?? []) {
    const oldId = m.id ?? '';
    if (stripIds.has(oldId)) continue;
    idMap.set(oldId, `${prefix}${oldId}`);
  }

  // 3. Clone and rewrite modules. We deep-clone via toObject/fromObject
  // to get a completely independent tree (protos.SynthModule.create does
  // a shallow copy that reuses nested config objects).
  let rootFound = false;
  for (const m of preset.patch.modules ?? []) {
    const oldId = m.id ?? '';
    if (stripIds.has(oldId)) continue;

    const plain = protos.SynthModule.toObject(
      m as protos.SynthModule, {defaults: false, enums: Number});
    const cloned = protos.SynthModule.fromObject(plain);
    cloned.id = idMap.get(oldId) ?? oldId;

    // Check the original module's oneof. When deep-cloning, we keep the
    // mixer test on the cloned form which preserves the oneof structure.
    const isMaster = oldId === 'master' && cloned.mixer != null;
    if (isMaster) {
      const ui = ModuleUiStateSchema.parse({
        nodeKind: 'instrument_root',
        displayName,
        presetId: preset.name,
        x,
        y,
        level: 0.8,
      });
      cloned.uiStateJson = JSON.stringify(ui);
      rootFound = true;
    } else {
      const ui = ModuleUiStateSchema.parse({
        nodeKind: 'instrument_internal',
      });
      cloned.uiStateJson = JSON.stringify(ui);
    }

    target.modules.push(cloned);
  }

  // 4. Clone and rewrite wires. Wires that previously referenced the
  // test source get their from_module rewritten to virtual INPUT ids.
  for (const w of preset.patch.wires ?? []) {
    const fromOld = w.fromModule ?? '';
    const toOld = w.toModule ?? '';
    const fromPort = w.fromPort ?? 'out';
    if (stripIds.has(toOld)) continue;  // Dropping: wire into test source.

    let newFrom: string;
    if (stripIds.has(fromOld)) {
      if (fromPort === 'freq') newFrom = VIRTUAL_INPUT_FREQ;
      else newFrom = VIRTUAL_INPUT_GATE;
    } else {
      newFrom = idMap.get(fromOld) ?? fromOld;
    }
    const newTo = idMap.get(toOld) ?? toOld;

    target.wires.push({
      ...w,
      fromModule: newFrom,
      // When the source is virtualized, fromPort must be "out" because
      // the virtual node only exposes an "out" port to downstream modules.
      // The gate-vs-freq distinction is encoded in the virtual ID itself.
      fromPort: isVirtualInputId(newFrom) ? 'out' : fromPort,
      toModule: newTo,
    });
  }

  // 5. If the preset had no `master` mixer, we can't identify a root.
  // Fall back: treat the last module as the root and add a marker.
  if (!rootFound) {
    const lastIdx = target.modules.length - 1;
    if (lastIdx >= 0) {
      const last = target.modules[lastIdx];
      const ui = ModuleUiStateSchema.parse({
        nodeKind: 'instrument_root',
        displayName,
        presetId: preset.name,
        x,
        y,
        level: 0.8,
      });
      last.uiStateJson = JSON.stringify(ui);
    }
  }

  // 6. Add rack wire from instrument root to rack master.
  const rootId = `${prefix}master`;
  target.wires.push({
    fromModule: rootId,
    fromPort: 'out',
    toModule: RACK_MASTER_ID,
    toPort: 'in',
  });

  // 7. Auto-layout the instrument's internal modules. Compute a
  // topological depth from the instrument's virtual INPUT (gate/freq
  // sources) towards the instrument root, then assign x/y based on
  // depth and order within depth. Modules unreachable from the input
  // (e.g. disconnected envelope-only chains) fall back to depth 0.
  layoutInstrumentModules(target, instrumentId);

  return instrumentId;
}

/**
 * Assign (x, y) positions to all modules belonging to an instrument.
 * The layout is a simple column-based topological layout: nodes are
 * placed in columns by their BFS depth from the virtual INPUT. Within
 * a column, nodes are stacked vertically.
 *
 * Column 0 is the INPUT (virtual, not laid out here). Columns 1..N-1
 * are internal modules. Column N is the instrument root (OUTPUT).
 */
function layoutInstrumentModules(
  patch: protos.ISynthPatch,
  instrumentId: string,
): void {
  const prefix = `${instrumentId}__`;
  const rootId = `${prefix}master`;
  const modules = patch.modules ?? [];
  const wires = patch.wires ?? [];

  // Collect this instrument's internal module IDs.
  const internalIds = new Set<string>();
  for (const m of modules) {
    const id = m.id ?? '';
    if (id.startsWith(prefix)) internalIds.add(id);
  }

  // Build adjacency: for each module, the set of predecessor module IDs.
  // Virtual INPUT wires are treated as depth 0 and contribute no real
  // predecessor for the layout.
  const preds = new Map<string, Set<string>>();
  for (const id of internalIds) preds.set(id, new Set());
  for (const w of wires) {
    const from = w.fromModule ?? '';
    const to = w.toModule ?? '';
    if (!internalIds.has(to)) continue;
    if (isVirtualInputId(from)) continue;
    if (internalIds.has(from)) preds.get(to)!.add(from);
  }

  // BFS depth from nodes with no predecessors (which are either
  // connected only to the virtual input, or truly disconnected).
  const depth = new Map<string, number>();
  const queue: string[] = [];
  for (const id of internalIds) {
    if (preds.get(id)!.size === 0) {
      depth.set(id, 0);
      queue.push(id);
    }
  }
  while (queue.length > 0) {
    const cur = queue.shift()!;
    const curDepth = depth.get(cur) ?? 0;
    // For every wire from cur to another internal node, bump its depth.
    for (const w of wires) {
      if (w.fromModule !== cur) continue;
      const to = w.toModule ?? '';
      if (!internalIds.has(to)) continue;
      const existing = depth.get(to);
      const newDepth = curDepth + 1;
      if (existing === undefined || newDepth > existing) {
        depth.set(to, newDepth);
        queue.push(to);
      }
    }
  }

  // Any still-unknown nodes (cycles or disconnected) → depth 0.
  for (const id of internalIds) {
    if (!depth.has(id)) depth.set(id, 0);
  }

  // Force the instrument root to be in the rightmost column (max depth
  // + 1) so it always appears at the end of the chain.
  let maxDepth = 0;
  for (const d of depth.values()) {
    if (d > maxDepth) maxDepth = d;
  }
  if (internalIds.has(rootId)) {
    depth.set(rootId, maxDepth + 1);
  }
  maxDepth = Math.max(maxDepth + 1, 1);

  // Group modules by depth.
  const byDepth = new Map<number, string[]>();
  for (const id of internalIds) {
    const d = depth.get(id) ?? 0;
    const list = byDepth.get(d) ?? [];
    list.push(id);
    byDepth.set(d, list);
  }

  // Layout constants.
  const COL_SPACING = 220;
  const ROW_SPACING = 140;
  const X_OFFSET = 200;  // leave room for the virtual INPUT node at x=30
  const Y_OFFSET = 60;

  // Assign positions. For the instrument root (OUTPUT), we write to
  // outX/outY so we don't clobber its rack position (x/y). For
  // everything else, x/y are the instrument-editor-canvas positions
  // (those modules are never shown on the rack).
  const idToModule = new Map<string, protos.ISynthModule>();
  for (const m of modules) idToModule.set(m.id ?? '', m);

  for (const [d, ids] of byDepth.entries()) {
    // Sort for deterministic ordering within a column.
    ids.sort();
    for (let i = 0; i < ids.length; i++) {
      const mod = idToModule.get(ids[i]);
      if (!mod) continue;
      const ui = parseModuleUiState(mod.uiStateJson);
      const posX = X_OFFSET + d * COL_SPACING;
      const posY = Y_OFFSET + i * ROW_SPACING;
      if (ids[i] === rootId) {
        // Instrument root: persist the OUTPUT position separately from
        // its rack position.
        ui.outX = posX;
        ui.outY = posY;
      } else {
        ui.x = posX;
        ui.y = posY;
      }
      mod.uiStateJson = JSON.stringify(ui);
    }
  }
}

// --- Instrument editor mutations ---

/** Add a new module inside an instrument's namespace. */
export function addModuleToInstrument(
  patch: protos.ISynthPatch,
  instrumentId: string,
  mod: protos.ISynthModule,
  localName: string,
  x: number,
  y: number,
): string {
  if (!patch.modules) patch.modules = [];
  const id = `${instrumentId}__${localName}_${uniqueSuffix()}`;
  const cloned: protos.ISynthModule = protos.SynthModule.create(mod);
  cloned.id = id;
  const ui = ModuleUiStateSchema.parse({
    nodeKind: 'instrument_internal',
    x,
    y,
  });
  cloned.uiStateJson = JSON.stringify(ui);
  patch.modules.push(cloned);
  return id;
}

/** Add a wire. Does nothing if an identical wire already exists. */
export function addWire(
  patch: protos.ISynthPatch,
  wire: protos.ISynthWire,
): void {
  if (!patch.wires) patch.wires = [];
  const exists = patch.wires.some(
    (w) =>
      w.fromModule === wire.fromModule &&
      w.fromPort === wire.fromPort &&
      w.toModule === wire.toModule &&
      w.toPort === wire.toPort,
  );
  if (exists) return;
  patch.wires.push(wire);
}

/** Remove a wire at index `idx` in patch.wires. */
export function removeWireAt(
  patch: protos.ISynthPatch,
  idx: number,
): void {
  if (!patch.wires || idx < 0 || idx >= patch.wires.length) return;
  patch.wires.splice(idx, 1);
}
