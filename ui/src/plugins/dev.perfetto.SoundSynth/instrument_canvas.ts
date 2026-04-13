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

// The bottom (instrument editor) canvas. Shows the internal synth
// graph of the currently-edited instrument, with virtual INPUT/OUTPUT
// nodes and full per-block parameter editing.

import m from 'mithril';
import protos from '../../protos';
import {
  Node,
  Connection,
  NodeGraph,
} from '../../widgets/nodegraph';
import {MenuItem} from '../../widgets/menu';
import {
  InstrumentEntity,
  VIRTUAL_INPUT_GATE,
  VIRTUAL_INPUT_FREQ,
  isVirtualInputId,
  addModuleToInstrument,
  addWire,
  removeModule,
  removeWireAt,
  writeModuleUiState,
  parseModuleUiState,
} from './patch_state';
import {
  BlockDescriptor,
  getDescriptorForModule,
  getAllDescriptors,
  descriptorsByCategory,
  BlockCategory,
  portContent,
} from './block_registry';

// Reserved canvas node IDs (not stored in proto).
const NODE_INPUT = '__canvas_input__';
const NODE_OUTPUT = '__canvas_output__';

export interface InstrumentCanvasAttrs {
  patch: protos.ISynthPatch;
  instrument: InstrumentEntity;
  onTest: () => void;
  onClose: () => void;
  onChange: () => void;
}

export class InstrumentCanvas
implements m.ClassComponent<InstrumentCanvasAttrs> {
  private showBlockPalette = false;
  // Track which instrument we've already auto-laid-out so we only do
  // it once per instrument (after the DOM is mounted and we have real
  // node dimensions).
  private lastLaidOutInstrumentId: string | null = null;
  // Flag: schedule an auto-layout on the next frame because we just
  // switched to a new instrument or a new block was added.
  private pendingAutoLayout = false;
  // Currently-selected canvas node id (used for Delete key + visual
  // highlight). Virtual INPUT/OUTPUT are not selectable.
  private selectedNodeId: string | null = null;

  view(vnode: m.Vnode<InstrumentCanvasAttrs>) {
    const {patch, instrument, onTest, onClose, onChange} = vnode.attrs;

    const modules = patch.modules ?? [];
    const wires = patch.wires ?? [];

    // Detect instrument changes and schedule an auto-layout pass on
    // the next frame. The NodeGraph's autoLayout uses DOM-measured
    // node dimensions, so we need a render to happen first before we
    // can measure.
    if (instrument.instrumentId !== this.lastLaidOutInstrumentId) {
      this.pendingAutoLayout = true;
      this.lastLaidOutInstrumentId = instrument.instrumentId;
    }

    // Collect internal modules (by their indexes, already in
    // instrument.internalModuleIdxs).
    const internalMods = instrument.internalModuleIdxs
      .map((idx) => modules[idx])
      .filter((m) => m !== undefined);

    // --- Build nodes ---
    const nodes: Node[] = [];

    // Virtual INPUT node at top-left.
    nodes.push(this.buildInputNode());

    // One node per internal module.
    // The OUTPUT (instrument root) is styled specially.
    const rootId = instrument.rootModule.id ?? '';
    for (const mod of internalMods) {
      if (mod.id === rootId) {
        nodes.push(this.buildOutputNode(mod));
      } else {
        const node = this.buildModuleNode(mod, onChange);
        // Attach a Delete context menu item so users can right-click
        // the 3-dot button in the node header and remove the block.
        const modId = mod.id ?? '';
        const mutableNode = node as {contextMenuItems?: m.Children};
        mutableNode.contextMenuItems = m(MenuItem, {
          label: 'Delete block',
          icon: 'delete',
          onclick: () => {
            removeModule(patch, modId);
            if (this.selectedNodeId === `mod:${modId}`) {
              this.selectedNodeId = null;
            }
            onChange();
          },
        });
        nodes.push(node);
      }
    }

    // --- Build connections ---
    // Every wire whose to_module is inside this instrument (or whose
    // from_module is a virtual INPUT) becomes a canvas connection.
    const connections: Connection[] = [];
    const wireCanvasIdxs: number[] = [];  // parallel: canvas idx -> wire idx
    const nodeIdByModuleId = new Map<string, string>();
    for (const mod of internalMods) {
      const id = mod.id ?? '';
      if (id === rootId) {
        nodeIdByModuleId.set(id, NODE_OUTPUT);
      } else {
        nodeIdByModuleId.set(id, `mod:${id}`);
      }
    }
    nodeIdByModuleId.set(VIRTUAL_INPUT_GATE, NODE_INPUT);
    nodeIdByModuleId.set(VIRTUAL_INPUT_FREQ, NODE_INPUT);

    // Port resolution: look at the block descriptor for the destination to
    // determine the toPort index, similarly for the source for fromPort.
    for (let wi = 0; wi < wires.length; wi++) {
      const wire = wires[wi];
      const fromRaw = wire.fromModule ?? '';
      const toRaw = wire.toModule ?? '';

      // Must be a wire that "belongs" to this instrument.
      const fromIsInternal = (() => {
        if (isVirtualInputId(fromRaw)) return true;
        const mod = modules.find((m) => m.id === fromRaw);
        if (!mod) return false;
        return (mod.id ?? '').startsWith(`${instrument.instrumentId}__`);
      })();
      const toIsInternal = (() => {
        const mod = modules.find((m) => m.id === toRaw);
        if (!mod) return false;
        return (mod.id ?? '').startsWith(`${instrument.instrumentId}__`);
      })();

      if (!fromIsInternal || !toIsInternal) continue;

      const fromNodeId = nodeIdByModuleId.get(fromRaw);
      const toNodeId = nodeIdByModuleId.get(toRaw);
      if (!fromNodeId || !toNodeId) continue;

      // Compute port indexes.
      let fromPortIdx = 0;
      if (fromNodeId === NODE_INPUT) {
        // Virtual INPUT has two outputs: 0 = gate, 1 = freq.
        fromPortIdx = fromRaw === VIRTUAL_INPUT_FREQ ? 1 : 0;
      } else {
        // Look up the block descriptor for the source module.
        const fromMod = modules.find((m) => m.id === fromRaw);
        if (fromMod) {
          const desc = getDescriptorForModule(fromMod);
          const portName = wire.fromPort ?? 'out';
          fromPortIdx = desc
            ? Math.max(0, desc.outputs.findIndex((p) => p.name === portName))
            : 0;
        }
      }

      let toPortIdx = 0;
      if (toNodeId === NODE_OUTPUT) {
        toPortIdx = 0;
      } else {
        const toMod = modules.find((m) => m.id === toRaw);
        if (toMod) {
          const desc = getDescriptorForModule(toMod);
          const portName = wire.toPort ?? 'in';
          toPortIdx = desc
            ? Math.max(0, desc.inputs.findIndex((p) => p.name === portName))
            : 0;
        }
      }

      connections.push({
        fromNode: fromNodeId,
        fromPort: fromPortIdx,
        toNode: toNodeId,
        toPort: toPortIdx,
      });
      wireCanvasIdxs.push(wi);
    }

    return m('.instrument-canvas-container', {
      style: {
        display: 'flex',
        flexDirection: 'column',
        height: '100%',
        position: 'relative',
      },
    },
      // Toolbar.
      this.renderToolbar(
        patch, instrument, onTest, onClose, onChange),
      // NodeGraph. Wrap in position:relative so the child .pf-canvas
      // can fill the wrapper via absolute positioning. Using a plain
      // block wrapper (not flex) ensures the .pf-canvas gets full width.
      m('.instrument-canvas-graph-wrapper', {
        style: {
          flex: '1 1 0',
          minHeight: '0',
          position: 'relative',
          overflow: 'hidden',
        },
      },
        m('div', {
          style: {
            position: 'absolute',
            top: '0', left: '0', right: '0', bottom: '0',
          },
        },
        m(NodeGraph, {
          nodes,
          connections,
          fillHeight: true,
          hideControls: true,
          selectedNodeIds: this.selectedNodeId
            ? new Set<string>([this.selectedNodeId])
            : new Set<string>(),
          onNodeSelect: (nodeId) => {
            // Only real module nodes are selectable for deletion.
            if (nodeId.startsWith('mod:')) {
              this.selectedNodeId = nodeId;
            } else {
              this.selectedNodeId = null;
            }
          },
          onSelectionClear: () => {
            this.selectedNodeId = null;
          },
          onReady: (api) => {
            // If we've just switched to a new instrument, trigger an
            // auto-layout pass on the next frame. The NodeGraph's
            // autoLayout uses real measured DOM sizes so it avoids the
            // overlap issues of the hand-rolled layout.
            if (this.pendingAutoLayout) {
              this.pendingAutoLayout = false;
              // Defer to next tick so the DOM has rendered the nodes
              // and we can measure them.
              requestAnimationFrame(() => {
                api.autoLayout();
                // Fit the whole graph into view afterwards.
                requestAnimationFrame(() => api.recenter());
              });
            }
          },
          onNodeMove: (nodeId, x, y) => {
            // The virtual INPUT node isn't backed by a real module, so
            // its position can't be persisted. It stays at its fixed
            // top-left spot (but users can still drag it visually — we
            // just ignore the move event).
            if (nodeId === NODE_INPUT) return;
            // The OUTPUT node IS backed by a real module (the
            // instrument's internal master mixer), so we DO persist
            // its position. Crucially, we write to outX/outY rather
            // than x/y because the same module is rendered on the
            // rack canvas as an instrument node whose position is
            // stored in x/y — if we overwrote those here, moving
            // OUTPUT would teleport the rack instrument.
            if (nodeId === NODE_OUTPUT) {
              writeModuleUiState(instrument.rootModule, {outX: x, outY: y});
              onChange();
              return;
            }
            if (nodeId.startsWith('mod:')) {
              const modId = nodeId.substring(4);
              const mod = modules.find((m) => m.id === modId);
              if (mod) {
                writeModuleUiState(mod, {x, y});
                onChange();
              }
            }
          },
          onConnect: (conn) => {
            this.handleConnect(conn, patch, instrument, nodes, onChange);
          },
          onConnectionRemove: (idx) => {
            const wireIdx = wireCanvasIdxs[idx];
            if (wireIdx === undefined) return;
            removeWireAt(patch, wireIdx);
            onChange();
          },
          onNodeRemove: (nodeId) => {
            if (nodeId === NODE_INPUT || nodeId === NODE_OUTPUT) return;
            if (nodeId.startsWith('mod:')) {
              removeModule(patch, nodeId.substring(4));
              onChange();
            }
          },
        }),
        ),
        // Block palette overlay.
        this.showBlockPalette
          ? this.renderBlockPalette(patch, instrument, onChange)
          : null,
      ),
    );
  }

  private renderToolbar(
    _patch: protos.ISynthPatch,
    instrument: InstrumentEntity,
    onTest: () => void,
    onClose: () => void,
    onChange: () => void,
  ): m.Child {
    return m('.instrument-toolbar', {
      style: {
        display: 'flex',
        alignItems: 'center',
        padding: '6px 12px',
        borderBottom: '1px solid #e0e0e0',
        gap: '8px',
        background: '#f8f8fa',
        fontSize: '12px',
      },
    },
      m('span',
        {style: {color: '#555', fontWeight: 'bold'}},
        'Editing:'),
      m('input[type=text]', {
        style: {
          fontSize: '12px',
          fontWeight: 'bold',
          padding: '2px 4px',
          border: '1px solid transparent',
          borderBottom: '1px solid #ccc',
          background: 'transparent',
          minWidth: '180px',
        },
        value: instrument.uiState.displayName,
        oninput: (e: InputEvent) => {
          writeModuleUiState(instrument.rootModule, {
            displayName: (e.target as HTMLInputElement).value,
          });
          onChange();
        },
      }),
      m('span',
        {style: {fontSize: '10px', color: '#888'}},
        instrument.uiState.presetId
          ? `(${instrument.uiState.presetId})`
          : ''),
      m('.spacer', {style: {flex: '1'}}),
      m('button', {
        style: {
          padding: '4px 12px',
          border: '1px solid #888',
          background: '#f0f0f0',
          borderRadius: '3px',
          cursor: 'pointer',
          fontSize: '11px',
          fontWeight: 'bold',
        },
        onclick: () => {
          this.showBlockPalette = !this.showBlockPalette;
        },
      }, this.showBlockPalette ? 'Close Palette' : '+ Add Block'),
      m('button', {
        style: {
          padding: '4px 12px',
          border: 'none',
          background: '#4caf50',
          color: 'white',
          borderRadius: '3px',
          cursor: 'pointer',
          fontSize: '11px',
          fontWeight: 'bold',
        },
        onclick: onTest,
      }, '\u25B6 Test'),
      m('button', {
        style: {
          padding: '4px 12px',
          border: '1px solid #ccc',
          background: 'white',
          borderRadius: '3px',
          cursor: 'pointer',
          fontSize: '11px',
        },
        onclick: onClose,
      }, 'Close'),
    );
  }

  private renderBlockPalette(
    patch: protos.ISynthPatch,
    instrument: InstrumentEntity,
    onChange: () => void,
  ): m.Child {
    const byCat = descriptorsByCategory();
    const categoryOrder: BlockCategory[] = [
      'oscillator', 'filter', 'modulator', 'effect',
      'source', 'utility',
    ];
    return m('.block-palette', {
      style: {
        position: 'absolute',
        top: '8px',
        right: '8px',
        width: '260px',
        maxHeight: 'calc(100% - 16px)',
        overflowY: 'auto',
        background: 'white',
        border: '1px solid #ccc',
        borderRadius: '4px',
        boxShadow: '0 2px 10px rgba(0,0,0,0.15)',
        zIndex: '10',
        padding: '8px',
      },
    },
      m('div', {
        style: {
          fontSize: '11px',
          fontWeight: 'bold',
          color: '#555',
          marginBottom: '6px',
        },
      }, 'Add Block'),
      categoryOrder.map((cat) => {
        const descs = byCat.get(cat);
        if (!descs || descs.length === 0) return null;
        return m('div', {style: {marginBottom: '8px'}},
          m('div', {
            style: {
              fontSize: '9px',
              color: '#888',
              textTransform: 'uppercase',
              fontWeight: 'bold',
              marginBottom: '2px',
            },
          }, cat),
          descs.map((d) => this.paletteButton(
            d, patch, instrument, onChange)),
        );
      }),
    );
  }

  private paletteButton(
    desc: BlockDescriptor,
    patch: protos.ISynthPatch,
    instrument: InstrumentEntity,
    onChange: () => void,
  ): m.Child {
    return m('button', {
      style: {
        display: 'block',
        width: '100%',
        padding: '4px 8px',
        fontSize: '11px',
        textAlign: 'left',
        border: '1px solid #ddd',
        borderLeft: `3px solid hsl(${desc.hue}, 65%, 50%)`,
        background: `hsl(${desc.hue}, 65%, 97%)`,
        borderRadius: '3px',
        cursor: 'pointer',
        marginBottom: '2px',
      },
      title: desc.description,
      onclick: () => {
        const mod = desc.createDefault();
        // Place new blocks in a staggered grid based on how many
        // internal modules the instrument already has. Deterministic
        // so we don't hit the redraw loop that `Math.random()` caused.
        const nExisting = instrument.internalModuleIdxs.length;
        const col = Math.floor(nExisting / 4);
        const row = nExisting % 4;
        const x = 300 + col * 220;
        const y = 60 + row * 140;
        addModuleToInstrument(
          patch, instrument.instrumentId, mod, desc.protoField, x, y,
        );
        this.showBlockPalette = false;
        onChange();
      },
    },
      m('div', {style: {fontWeight: 'bold'}}, desc.displayName),
      m('div', {
        style: {fontSize: '9px', color: '#666'},
      }, desc.description),
    );
  }

  private handleConnect(
    conn: Connection,
    patch: protos.ISynthPatch,
    instrument: InstrumentEntity,
    nodes: Node[],
    onChange: () => void,
  ) {
    // Look up the nodes to resolve module ids + port names.
    const fromNode = nodes.find((n) => n.id === conn.fromNode);
    const toNode = nodes.find((n) => n.id === conn.toNode);
    if (!fromNode || !toNode) return;

    // From node: either INPUT, OUTPUT, or mod:*.
    let fromModuleId: string;
    let fromPortName = 'out';
    if (fromNode.id === NODE_INPUT) {
      fromModuleId = conn.fromPort === 1
        ? VIRTUAL_INPUT_FREQ
        : VIRTUAL_INPUT_GATE;
    } else if (fromNode.id === NODE_OUTPUT) {
      // Can't wire FROM output.
      return;
    } else if (fromNode.id.startsWith('mod:')) {
      fromModuleId = fromNode.id.substring(4);
      const mod = patch.modules?.find((m) => m.id === fromModuleId);
      if (mod) {
        const desc = getDescriptorForModule(mod);
        if (desc && desc.outputs[conn.fromPort]) {
          fromPortName = desc.outputs[conn.fromPort].name;
        }
      }
    } else {
      return;
    }

    // To node: OUTPUT or mod:*.
    let toModuleId: string;
    let toPortName = 'in';
    if (toNode.id === NODE_INPUT) {
      // Can't wire INTO input.
      return;
    } else if (toNode.id === NODE_OUTPUT) {
      toModuleId = instrument.rootModule.id ?? '';
      toPortName = 'in';
    } else if (toNode.id.startsWith('mod:')) {
      toModuleId = toNode.id.substring(4);
      const mod = patch.modules?.find((m) => m.id === toModuleId);
      if (mod) {
        const desc = getDescriptorForModule(mod);
        if (desc && desc.inputs[conn.toPort]) {
          toPortName = desc.inputs[conn.toPort].name;
        }
      }
    } else {
      return;
    }

    addWire(patch, {
      fromModule: fromModuleId,
      fromPort: fromPortName,
      toModule: toModuleId,
      toPort: toPortName,
    });
    onChange();
  }

  private buildInputNode(): Node {
    return {
      id: NODE_INPUT,
      x: 30,
      y: 60,
      hue: 160,
      titleBar: {title: 'INPUT', icon: 'input'},
      outputs: [
        {direction: 'right' as const, content: portContent('gate', 'gate')},
        {direction: 'right' as const, content: portContent('freq', 'freq')},
      ],
      content: m('div', {
        style: {
          padding: '8px 12px', fontSize: '10px',
          color: '#666', textAlign: 'center',
        },
      }, m('div', 'From rack binding'),
         m('div', {style: {fontSize: '9px', color: '#999'}},
           '(wire from rack trace source)')),
      className: 'pf-node--virtual',
    };
  }

  private buildOutputNode(rootModule: protos.ISynthModule): Node {
    const ui = parseModuleUiState(rootModule.uiStateJson);
    // The OUTPUT node has its OWN position (outX/outY) independent of
    // the instrument's rack position (x/y), so moving it in the
    // instrument editor doesn't drag the rack-level instrument node.
    return {
      id: NODE_OUTPUT,
      x: ui.outX ?? 760,
      y: ui.outY ?? 280,
      hue: 200,
      titleBar: {title: 'OUTPUT', icon: 'output'},
      inputs: [{
        direction: 'left' as const,
        content: portContent('in', 'audio'),
      }],
      content: m('div', {
        style: {
          padding: '8px 12px', fontSize: '10px',
          color: '#666', textAlign: 'center',
        },
      }, m('div', 'Instrument output'),
         m('div', {style: {fontSize: '9px', color: '#999'}},
           '(routes to rack master)')),
      className: 'pf-node--virtual',
    };
  }

  private buildModuleNode(
    mod: protos.ISynthModule,
    onChange: () => void,
  ): Node {
    const ui = parseModuleUiState(mod.uiStateJson);
    const desc = getDescriptorForModule(mod);
    const displayName = desc?.displayName ?? (mod.id ?? 'Unknown');
    const hue = desc?.hue ?? 150;

    const inputs = (desc?.inputs ?? []).map(
      (p) => ({
        direction: 'left' as const,
        content: portContent(p.name, p.kind),
      }),
    );
    const outputs = (desc?.outputs ?? []).map(
      (p) => ({
        direction: 'right' as const,
        content: portContent(p.name, p.kind),
      }),
    );

    // Use ?? instead of || so that x=0 is treated as a valid position.
    // Also: deterministic fallback (no Math.random!) so the node doesn't
    // move on every redraw and cause an infinite loop via onNodeMove.
    return {
      id: `mod:${mod.id}`,
      x: ui.x ?? 280,
      y: ui.y ?? 120,
      hue,
      titleBar: {title: displayName, icon: 'widgets'},
      inputs,
      outputs,
      content: m('div', {
        style: {padding: '4px 8px', minWidth: '190px'},
      },
        desc
          ? desc.renderParams(mod, onChange)
          : m('div',
              {style: {fontSize: '10px', color: '#888'}},
              'Unknown block'),
      ),
    };
  }
}

// Suppress unused import warning: getAllDescriptors is used by the
// palette menu but TypeScript sometimes misreports in strict-unused mode.
void getAllDescriptors;
