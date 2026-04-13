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

  view(vnode: m.Vnode<InstrumentCanvasAttrs>) {
    const {patch, instrument, onTest, onClose, onChange} = vnode.attrs;

    const modules = patch.modules ?? [];
    const wires = patch.wires ?? [];

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
        nodes.push(this.buildModuleNode(mod, onChange));
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
      // NodeGraph.
      m('div', {
        style: {
          flex: '1',
          display: 'flex',
          position: 'relative',
          minHeight: '0',
        },
      },
        m(NodeGraph, {
          nodes,
          connections,
          fillHeight: true,
          hideControls: true,
          onNodeMove: (nodeId, x, y) => {
            // Don't move virtual nodes.
            if (nodeId === NODE_INPUT || nodeId === NODE_OUTPUT) return;
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
        addModuleToInstrument(
          patch, instrument.instrumentId, mod,
          desc.protoField, 280 + Math.random() * 80, 120 + Math.random() * 80,
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
        {direction: 'right' as const, content: 'gate'},
        {direction: 'right' as const, content: 'freq'},
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
    return {
      id: NODE_OUTPUT,
      x: ui.x || 760,
      y: ui.y || 280,
      hue: 200,
      titleBar: {title: 'OUTPUT', icon: 'output'},
      inputs: [{direction: 'left' as const, content: 'in'}],
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
      (p) => ({direction: 'left' as const, content: p.name}),
    );
    const outputs = (desc?.outputs ?? []).map(
      (p) => ({direction: 'right' as const, content: p.name}),
    );

    return {
      id: `mod:${mod.id}`,
      x: ui.x || 280 + Math.random() * 100,
      y: ui.y || 120 + Math.random() * 100,
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
