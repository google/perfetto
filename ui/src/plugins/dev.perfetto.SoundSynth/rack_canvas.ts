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

// The top (rack) canvas.
// Nodes: TraceSource, Instrument, Master.
// Connections:
//   - TraceSource.out -> Instrument.gate or Instrument.freq (stored in
//     the instrument root's ui_state_json as gateSource/freqSource)
//   - Instrument.out -> Master.in (stored as a real SynthWire)

import m from 'mithril';
import protos from '../../protos';
import {
  Node,
  Connection,
  NodeGraph,
  NodePort,
} from '../../widgets/nodegraph';
import {MenuItem} from '../../widgets/menu';
import {
  PatchView,
  TraceSourceEntity,
  InstrumentEntity,
  MasterEntity,
  RACK_MASTER_ID,
  removeInstrument,
  removeModule,
  setInstrumentSource,
  writeModuleUiState,
  addWire,
  removeWireAt,
} from './patch_state';
import {getDescriptorForModule} from './block_registry';

const CATEGORY_HUES: Record<string, number> = {
  drum: 0,
  bass: 210,
  lead: 35,
  pad: 280,
  fx: 320,
  strings: 180,
  organ: 50,
};

export interface RackCanvasAttrs {
  patch: protos.ISynthPatch;
  view: PatchView;
  selectedInstrumentId: string | null;
  selectedTraceSourceId: string | null;
  /** Called when user clicks "Edit" on an instrument. */
  onEditInstrument: (instrumentId: string) => void;
  /** Called when user clicks "Test" on an instrument. */
  onTestInstrument: (instrumentId: string) => void;
  /** Called when a trace source node is selected (null = deselected). */
  onSelectTraceSource: (traceSourceId: string | null) => void;
  /** Called after any mutation to trigger a redraw. */
  onChange: () => void;
}

export class RackCanvas implements m.ClassComponent<RackCanvasAttrs> {
  // Tracks the currently-selected canvas node id. Used to let the user
  // press Delete/Backspace to remove any selectable node (trace source,
  // instrument). This is purely UI state, not persisted.
  private selectedNodeId: string | null = null;

  view(vnode: m.Vnode<RackCanvasAttrs>) {
    const {
      patch, view, selectedInstrumentId, selectedTraceSourceId,
      onEditInstrument, onTestInstrument, onSelectTraceSource, onChange,
    } = vnode.attrs;

    // --- Build nodes ---
    const nodes: Node[] = [];

    for (const src of view.traceSources) {
      const node = this.buildTraceSourceNode(src, onChange);
      const srcId = src.module.id ?? '';
      // Add a Delete context menu item.
      const mutableNode = node as {contextMenuItems?: m.Children};
      mutableNode.contextMenuItems = m(MenuItem, {
        label: 'Delete trace source',
        icon: 'delete',
        onclick: () => {
          removeModule(patch, srcId);
          if (this.selectedNodeId === `src:${srcId}`) {
            this.selectedNodeId = null;
          }
          onChange();
        },
      });
      nodes.push(node);
    }
    for (const inst of view.instruments) {
      const node = this.buildInstrumentNode(
        inst, patch, selectedInstrumentId,
        onEditInstrument, onTestInstrument, onChange,
      );
      const instId = inst.instrumentId;
      const mutableNode = node as {contextMenuItems?: m.Children};
      mutableNode.contextMenuItems = m(MenuItem, {
        label: 'Delete instrument',
        icon: 'delete',
        onclick: () => {
          removeInstrument(patch, instId);
          if (this.selectedNodeId === `inst:${instId}`) {
            this.selectedNodeId = null;
          }
          onChange();
        },
      });
      nodes.push(node);
    }
    if (view.master) {
      nodes.push(this.buildMasterNode(view.master));
    }

    // --- Build connections ---
    const connections: Connection[] = [];

    // Map module ID to node ID (some module IDs collapse to a node ID).
    const moduleIdToNode = new Map<string, {nodeId: string; portIdx: number}>();
    for (const src of view.traceSources) {
      const mid = src.module.id ?? '';
      moduleIdToNode.set(mid, {nodeId: `src:${mid}`, portIdx: 0});
    }
    for (const inst of view.instruments) {
      const iid = inst.instrumentId;
      // The instrument's "logical output" = the root module (final mixer).
      const rootId = inst.rootModule.id ?? '';
      moduleIdToNode.set(rootId, {nodeId: `inst:${iid}`, portIdx: 0});
    }
    if (view.master) {
      moduleIdToNode.set(view.master.module.id ?? '',
        {nodeId: 'master', portIdx: 0});
    }

    // Gate/Freq rack bindings → canvas wires.
    // Gate = input port 0, Freq = input port 1 on the instrument node.
    for (const inst of view.instruments) {
      const iid = inst.instrumentId;
      if (inst.uiState.gateSource) {
        const src = moduleIdToNode.get(inst.uiState.gateSource);
        if (src) {
          connections.push({
            fromNode: src.nodeId, fromPort: 0,
            toNode: `inst:${iid}`, toPort: 0,
          });
        }
      }
      if (inst.uiState.freqSource) {
        const src = moduleIdToNode.get(inst.uiState.freqSource);
        if (src) {
          connections.push({
            fromNode: src.nodeId, fromPort: 0,
            toNode: `inst:${iid}`, toPort: 1,
          });
        }
      }
    }

    // Instrument → Master real wires.
    for (const w of patch.wires ?? []) {
      if (w.toModule === RACK_MASTER_ID && w.toPort === 'in') {
        const from = moduleIdToNode.get(w.fromModule ?? '');
        if (from?.nodeId.startsWith('inst:')) {
          connections.push({
            fromNode: from.nodeId, fromPort: 0,
            toNode: 'master', toPort: 0,
          });
        }
      }
    }

    // Selection set used by the NodeGraph for Delete key + visual
    // highlight. Only contains what the user explicitly clicked — we
    // do NOT auto-select the currently-edited instrument (its "editing"
    // state is shown differently, via the instrument node's own visual
    // treatment), so pressing Delete won't accidentally kill it.
    const selectedIds = new Set<string>();
    if (this.selectedNodeId) selectedIds.add(this.selectedNodeId);

    return m(NodeGraph, {
      nodes,
      connections,
      selectedNodeIds: selectedIds,
      fillHeight: true,
      hideControls: true,
      onNodeMove: (nodeId, x, y) => {
        const mod = this.resolveNodeModule(nodeId, patch, view);
        if (mod) {
          writeModuleUiState(mod, {x, y});
          onChange();
        }
      },
      onNodeSelect: (nodeId) => {
        this.selectedNodeId = nodeId;
        if (nodeId.startsWith('inst:')) {
          onEditInstrument(nodeId.substring(5));
        }
      },
      onSelectionClear: () => {
        this.selectedNodeId = null;
      },
      onConnect: (conn) => {
        this.handleConnect(conn, patch, view, onChange);
      },
      onConnectionRemove: (idx) => {
        const conn = connections[idx];
        if (!conn) return;
        this.handleDisconnect(conn, patch, view, onChange);
      },
      onNodeRemove: (nodeId) => {
        if (nodeId.startsWith('src:')) {
          removeModule(patch, nodeId.substring(4));
          if (this.selectedNodeId === nodeId) this.selectedNodeId = null;
          onChange();
        } else if (nodeId.startsWith('inst:')) {
          removeInstrument(patch, nodeId.substring(5));
          if (this.selectedNodeId === nodeId) this.selectedNodeId = null;
          onChange();
        }
      },
    });
  }

  private handleConnect(
    conn: Connection,
    patch: protos.ISynthPatch,
    view: PatchView,
    onChange: () => void,
  ) {
    // Supported connection types:
    //   src → inst[0]  (gate binding)
    //   src → inst[1]  (freq binding)
    //   inst → master  (real SynthWire into rack master)
    if (conn.fromNode.startsWith('src:') &&
        conn.toNode.startsWith('inst:')) {
      const srcId = conn.fromNode.substring(4);
      const instId = conn.toNode.substring(5);
      const inst = view.instruments.find((i) => i.instrumentId === instId);
      if (!inst) return;
      const kind = conn.toPort === 0 ? 'gate' : 'freq';
      setInstrumentSource(patch, inst.rootModule, kind, srcId);
      onChange();
      return;
    }
    if (conn.fromNode.startsWith('inst:') && conn.toNode === 'master') {
      const instId = conn.fromNode.substring(5);
      const inst = view.instruments.find((i) => i.instrumentId === instId);
      if (!inst) return;
      addWire(patch, {
        fromModule: inst.rootModule.id ?? '',
        fromPort: 'out',
        toModule: RACK_MASTER_ID,
        toPort: 'in',
      });
      onChange();
      return;
    }
  }

  private handleDisconnect(
    conn: Connection,
    patch: protos.ISynthPatch,
    view: PatchView,
    onChange: () => void,
  ) {
    if (conn.fromNode.startsWith('src:') &&
        conn.toNode.startsWith('inst:')) {
      const instId = conn.toNode.substring(5);
      const inst = view.instruments.find((i) => i.instrumentId === instId);
      if (!inst) return;
      const kind = conn.toPort === 0 ? 'gate' : 'freq';
      setInstrumentSource(patch, inst.rootModule, kind, '');
      onChange();
      return;
    }
    if (conn.fromNode.startsWith('inst:') && conn.toNode === 'master') {
      const instId = conn.fromNode.substring(5);
      const inst = view.instruments.find((i) => i.instrumentId === instId);
      if (!inst) return;
      const fromId = inst.rootModule.id ?? '';
      const idx = (patch.wires ?? []).findIndex(
        (w) =>
          w.fromModule === fromId &&
          w.toModule === RACK_MASTER_ID &&
          w.toPort === 'in',
      );
      if (idx >= 0) {
        removeWireAt(patch, idx);
        onChange();
      }
      return;
    }
  }

  private resolveNodeModule(
    nodeId: string,
    patch: protos.ISynthPatch,
    view: PatchView,
  ): protos.ISynthModule | null {
    if (nodeId.startsWith('src:')) {
      const id = nodeId.substring(4);
      return patch.modules?.find((m) => m.id === id) ?? null;
    }
    if (nodeId.startsWith('inst:')) {
      const iid = nodeId.substring(5);
      const inst = view.instruments.find((i) => i.instrumentId === iid);
      return inst ? inst.rootModule : null;
    }
    if (nodeId === 'master' && view.master) return view.master.module;
    return null;
  }

  // --- Node builders ---

  private buildTraceSourceNode(
    src: TraceSourceEntity,
    onChange: () => void,
  ): Node {
    const ui = src.uiState;
    const mod = src.module;
    const sliceSrc = mod.traceSliceSource;
    const glob = sliceSrc?.trackNameGlob ?? '*';
    const signalType = sliceSrc?.signalType ?? 0;

    return {
      id: `src:${mod.id}`,
      x: ui.x,
      y: ui.y,
      hue: 140,
      titleBar: {
        title: ui.displayName || 'Trace Source',
        icon: 'track_changes',
      },
      outputs: [{direction: 'right' as const, content: 'out'}],
      content: m('div', {
        style: {padding: '6px 8px', fontSize: '11px', minWidth: '190px'},
      },
        m('div', {style: {marginBottom: '4px'}},
          m('span', {style: {color: '#666', marginRight: '4px'}}, 'Glob'),
          m('input[type=text]', {
            style: {
              width: '130px', fontSize: '11px', padding: '1px 4px',
            },
            value: glob,
            oninput: (e: InputEvent) => {
              if (!mod.traceSliceSource) return;
              mod.traceSliceSource.trackNameGlob =
                (e.target as HTMLInputElement).value;
              onChange();
            },
            onclick: (e: Event) => e.stopPropagation(),
            onmousedown: (e: Event) => e.stopPropagation(),
          }),
        ),
        m('div',
          m('span', {style: {color: '#666', marginRight: '4px'}}, 'Signal'),
          m('select', {
            style: {fontSize: '11px'},
            value: String(signalType),
            onchange: (e: Event) => {
              if (!mod.traceSliceSource) return;
              mod.traceSliceSource.signalType = parseInt(
                (e.target as HTMLSelectElement).value, 10);
              onChange();
            },
            onclick: (e: Event) => e.stopPropagation(),
            onmousedown: (e: Event) => e.stopPropagation(),
          },
            m('option', {value: '0'}, 'Gate'),
            m('option', {value: '1'}, 'Trigger'),
            m('option', {value: '2'}, 'Density'),
          ),
        ),
      ),
    };
  }

  private buildInstrumentNode(
    inst: InstrumentEntity,
    patch: protos.ISynthPatch,
    selectedInstrumentId: string | null,
    onEdit: (id: string) => void,
    onTest: (id: string) => void,
    onChange: () => void,
  ): Node {
    const ui = inst.uiState;
    const instId = inst.instrumentId;
    const hue = CATEGORY_HUES[this.categoryOf(ui.presetId)] ?? 200;
    const isSelected = selectedInstrumentId === instId;

    const inputs: NodePort[] = [
      {direction: 'left' as const, content: 'gate'},
      {direction: 'left' as const, content: 'freq'},
    ];
    const outputs: NodePort[] = [
      {direction: 'right' as const, content: 'out'},
    ];

    const chainPreview = this.computeChainPreview(inst, patch);

    return {
      id: `inst:${instId}`,
      x: ui.x,
      y: ui.y,
      hue,
      titleBar: {
        title: ui.displayName || 'Instrument',
        icon: 'music_note',
      },
      inputs,
      outputs,
      invalid: ui.muted,
      className: isSelected ? 'pf-node--selected' : undefined,
      content: m('div', {
        style: {padding: '6px 8px', fontSize: '11px', minWidth: '240px'},
      },
        // Header row: preset + M/S buttons.
        m('div', {
          style: {
            display: 'flex', alignItems: 'center',
            marginBottom: '6px', gap: '4px',
          },
        },
          m('span', {
            style: {
              fontSize: '9px',
              color: `hsl(${hue}, 65%, 30%)`,
              fontWeight: 'bold',
              textTransform: 'uppercase',
            },
          }, ui.presetId || 'custom'),
          m('.spacer', {style: {flex: '1'}}),
          this.renderToggle('M', ui.muted, '#e53935', () => {
            writeModuleUiState(inst.rootModule, {muted: !ui.muted});
            onChange();
          }),
          this.renderToggle('S', ui.soloed, '#ffc107', () => {
            writeModuleUiState(inst.rootModule, {soloed: !ui.soloed});
            onChange();
          }),
        ),
        // Chain preview.
        m('div', {
          style: {
            fontSize: '9px',
            color: '#777',
            fontFamily: 'monospace',
            marginBottom: '6px',
            overflow: 'hidden',
            textOverflow: 'ellipsis',
            whiteSpace: 'nowrap',
            maxWidth: '220px',
          },
          title: chainPreview,
        }, chainPreview),
        // Level slider.
        m('div', {
          style: {
            display: 'flex', alignItems: 'center',
            gap: '4px', marginBottom: '6px',
          },
        },
          m('span', {
            style: {color: '#666', fontSize: '10px', width: '32px'},
          }, 'Level'),
          m('input[type=range]', {
            style: {flex: '1'},
            min: '0', max: '1', step: '0.01',
            value: String(ui.level),
            oninput: (e: InputEvent) => {
              const v = parseFloat((e.target as HTMLInputElement).value);
              writeModuleUiState(inst.rootModule, {level: v});
              onChange();
            },
            onclick: (e: Event) => e.stopPropagation(),
            onmousedown: (e: Event) => e.stopPropagation(),
          }),
          m('span', {
            style: {
              fontSize: '10px', width: '26px', textAlign: 'right',
              fontFamily: 'monospace',
            },
          }, ui.level.toFixed(2)),
        ),
        // Test + Edit buttons.
        m('div', {style: {display: 'flex', gap: '4px'}},
          m('button', {
            style: {
              flex: '1', padding: '4px 6px', fontSize: '10px',
              cursor: 'pointer', border: '1px solid #888',
              background: '#4caf50', color: 'white',
              borderRadius: '3px', fontWeight: 'bold',
            },
            onclick: (e: Event) => {
              e.stopPropagation();
              onTest(instId);
            },
            onmousedown: (e: Event) => e.stopPropagation(),
          }, '\u25B6 Test'),
          m('button', {
            style: {
              flex: '1', padding: '4px 6px', fontSize: '10px',
              cursor: 'pointer', border: '1px solid #888',
              background: isSelected ? '#3f51b5' : '#f5f5f5',
              color: isSelected ? 'white' : '#333',
              borderRadius: '3px', fontWeight: 'bold',
            },
            onclick: (e: Event) => {
              e.stopPropagation();
              onEdit(instId);
            },
            onmousedown: (e: Event) => e.stopPropagation(),
          }, isSelected ? 'Editing' : 'Edit'),
        ),
      ),
    };
  }

  private buildMasterNode(master: MasterEntity): Node {
    const ui = master.uiState;
    return {
      id: 'master',
      x: ui.x,
      y: ui.y,
      hue: 200,
      titleBar: {title: 'Master Out', icon: 'volume_up'},
      inputs: [{direction: 'left' as const, content: 'in'}],
      content: m('div', {
        style: {
          padding: '12px 20px', fontSize: '11px',
          color: '#666', textAlign: 'center',
        },
      }, 'Final mix'),
    };
  }

  private renderToggle(
    label: string, active: boolean, color: string, onClick: () => void,
  ): m.Child {
    return m('button', {
      style: {
        width: '20px', height: '20px', borderRadius: '3px',
        border: '1px solid #ccc',
        background: active ? color : 'white',
        color: active ? 'white' : '#333',
        fontSize: '10px', fontWeight: 'bold',
        cursor: 'pointer', padding: '0',
      },
      onclick: (e: Event) => {
        e.stopPropagation();
        onClick();
      },
      onmousedown: (e: Event) => e.stopPropagation(),
    }, label);
  }

  private categoryOf(presetId: string): string {
    // Derive a category prefix from the preset name.
    // Preset names look like "kick_classic", "bass_acid_classic", etc.
    if (!presetId) return 'custom';
    const first = presetId.split('_')[0];
    switch (first) {
      case 'kick': case 'snare': case 'clap': case 'tom':
      case 'open': case 'closed':
        return 'drum';
      case 'acid': case 'sub': case 'reese': case 'fm': case 'substance':
        return 'bass';
      case 'saw': case 'square': case 'wavetable':
        return 'lead';
      case 'pad': return 'pad';
      case 'fx': return 'fx';
      case 'strings': return 'strings';
      case 'organ': return 'organ';
      default: return 'custom';
    }
  }

  private computeChainPreview(
    inst: InstrumentEntity,
    patch: protos.ISynthPatch,
  ): string {
    // Walk the signal chain from the instrument's internal modules and
    // produce a compact "OscA → Moog → VCA → Drive" preview.
    const modulesById = new Map<string, protos.ISynthModule>();
    for (const idx of inst.internalModuleIdxs) {
      const m = patch.modules![idx];
      modulesById.set(m.id ?? '', m);
    }

    // Find the root (output) of the instrument.
    const rootId = inst.rootModule.id ?? '';

    // BFS backwards from root up to 5 steps, collecting distinct block types.
    const visited = new Set<string>();
    const chain: string[] = [];
    const queue: string[] = [rootId];
    while (queue.length > 0 && chain.length < 5) {
      const id = queue.shift()!;
      if (visited.has(id)) continue;
      visited.add(id);
      const mod = modulesById.get(id);
      if (!mod) continue;
      const desc = getDescriptorForModule(mod);
      if (desc && desc.protoField !== 'mixer') {
        // Prepend so we go source → ... → output direction.
        chain.unshift(desc.displayName);
      }
      // Follow predecessor wires.
      for (const w of patch.wires ?? []) {
        if (w.toModule === id) {
          const from = w.fromModule ?? '';
          if (!visited.has(from) && modulesById.has(from)) {
            queue.push(from);
          }
        }
      }
    }

    return chain.length > 0
      ? chain.join(' \u2192 ')
      : '(empty chain)';
  }
}
