// Copyright (C) 2025 The Android Open Source Project
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

import {produce} from 'immer';
import m from 'mithril';
import {shortUuid} from '../../../base/uuid';
import {Button, ButtonGroup, ButtonVariant} from '../../../widgets/button';
import {Checkbox} from '../../../widgets/checkbox';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {
  Connection,
  Node,
  NodeGraph,
  NodeGraphAPI,
  NodeGraphAttrs,
  NodePort,
} from '../../../widgets/nodegraph';
import {Select} from '../../../widgets/select';
import {TextInput} from '../../../widgets/text_input';
import {renderDocSection, renderWidgetShowcase} from '../widgets_page_utils';
import {maybeUndefined} from '../../../base/utils';

const MAX_HISTORY_DEPTH = 500;

// Base node data interface
interface BaseNodeData {
  readonly id: string;
  x: number;
  y: number;
  next?: NodeData;
  // Ordered list of source node IDs feeding into each manifest input slot.
  // undefined means the slot is unconnected.
  inputNodeIds?: (string | undefined)[];
}

// Individual node type interfaces
interface TableNodeData extends BaseNodeData {
  readonly type: 'table';
  readonly table: string;
}

interface SelectNodeData extends BaseNodeData {
  readonly type: 'select';
  readonly columns: Record<string, boolean>;
}

interface FilterNodeData extends BaseNodeData {
  readonly type: 'filter';
  readonly filterExpression: string;
}

interface SortNodeData extends BaseNodeData {
  readonly type: 'sort';
  readonly sortColumn: string;
  readonly sortOrder: 'ASC' | 'DESC';
}

interface JoinNodeData extends BaseNodeData {
  readonly type: 'join';
  readonly joinType: 'INNER' | 'LEFT' | 'RIGHT' | 'FULL';
  readonly joinOn: string;
}

interface UnionNodeData extends BaseNodeData {
  readonly type: 'union';
  readonly unionType: 'UNION' | 'UNION ALL';
}

// Discriminated union of all pipeline node types
type NodeData =
  | TableNodeData
  | SelectNodeData
  | FilterNodeData
  | SortNodeData
  | JoinNodeData
  | UnionNodeData;

// Labels are a separate entity: free-floating annotations, no ports, no docking
interface LabelData {
  readonly id: string;
  x: number;
  y: number;
  text: string;
}

// Store interface (only data that should be in undo/redo history)
interface NodeGraphStore {
  readonly nodes: NodeData[]; // only root nodes; docked nodes are nested via .next
  readonly labels: LabelData[];
}

// Single source of truth per node type: metadata, factory, and renderer.
// C is the specific NodeData subtype (e.g. TableNodeData).
interface NodeTypeManifest<C extends NodeData> {
  readonly title: string;
  readonly icon: string;
  readonly hue: number;
  readonly inputs?: ReadonlyArray<{
    readonly id: string;
    readonly label: string;
  }>;
  create(id: string, x: number, y: number): C;
  render(node: C, update: (updates: Partial<C>) => void): m.Children;
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
const NODE_CONFIGS: {[K in NodeData['type']]: NodeTypeManifest<any>} = {
  table: {
    title: 'Table',
    icon: 'table_chart',
    hue: 200,
    create: (id, x, y): TableNodeData => ({
      type: 'table',
      id,
      x,
      y,
      table: 'slice',
    }),
    render: (node: TableNodeData, update) =>
      m(
        Select,
        {
          value: node.table,
          onchange: (e: Event) =>
            update({table: (e.target as HTMLSelectElement).value}),
        },
        [
          m('option', {value: 'slice'}, 'slice'),
          m('option', {value: 'sched'}, 'sched'),
          m('option', {value: 'thread'}, 'thread'),
          m('option', {value: 'process'}, 'process'),
        ],
      ),
  },

  select: {
    title: 'Select',
    icon: 'checklist',
    hue: 100,
    inputs: [{label: 'Input', id: 'input'}],
    create: (id, x, y): SelectNodeData => ({
      type: 'select',
      id,
      x,
      y,
      columns: {
        id: true,
        name: true,
        cpu: false,
        duration: false,
        timestamp: false,
      },
    }),
    render: (node: SelectNodeData, update) =>
      m(
        '',
        {style: {display: 'flex', flexDirection: 'column', gap: '4px'}},
        Object.entries(node.columns).map(([col, checked]) =>
          m(Checkbox, {
            label: col,
            checked,
            onchange: () =>
              update({columns: {...node.columns, [col]: !checked}}),
          }),
        ),
      ),
  },

  filter: {
    title: 'Filter',
    icon: 'filter_alt',
    hue: 50,
    inputs: [{label: 'Input', id: 'input'}],
    create: (id, x, y): FilterNodeData => ({
      type: 'filter',
      id,
      x,
      y,
      filterExpression: '',
    }),
    render: (node: FilterNodeData, update) =>
      m(TextInput, {
        placeholder: 'Filter expression...',
        value: node.filterExpression,
        oninput: (e: InputEvent) =>
          update({filterExpression: (e.target as HTMLInputElement).value}),
      }),
  },

  sort: {
    title: 'Sort',
    icon: 'sort',
    hue: 150,
    inputs: [{label: 'Input', id: 'input'}],
    create: (id, x, y): SortNodeData => ({
      type: 'sort',
      id,
      x,
      y,
      sortColumn: '',
      sortOrder: 'ASC',
    }),
    render: (node: SortNodeData, update) =>
      m('', {style: {display: 'flex', flexDirection: 'column', gap: '4px'}}, [
        m(TextInput, {
          placeholder: 'Sort column...',
          value: node.sortColumn,
          oninput: (e: InputEvent) =>
            update({sortColumn: (e.target as HTMLInputElement).value}),
        }),
        m(
          Select,
          {
            value: node.sortOrder,
            onchange: (e: Event) =>
              update({
                sortOrder: (e.target as HTMLSelectElement).value as
                  | 'ASC'
                  | 'DESC',
              }),
          },
          [
            m('option', {value: 'ASC'}, 'ASC'),
            m('option', {value: 'DESC'}, 'DESC'),
          ],
        ),
      ]),
  },

  join: {
    title: 'Join',
    icon: 'join',
    hue: 300,
    inputs: [
      {label: 'Left', id: 'left'},
      {label: 'Right', id: 'right'},
    ],
    create: (id, x, y): JoinNodeData => ({
      type: 'join',
      id,
      x,
      y,
      joinType: 'INNER',
      joinOn: '',
    }),
    render: (node: JoinNodeData, update) =>
      m('', {style: {display: 'flex', flexDirection: 'column', gap: '4px'}}, [
        m(
          Select,
          {
            value: node.joinType,
            onchange: (e: Event) =>
              update({
                joinType: (e.target as HTMLSelectElement)
                  .value as JoinNodeData['joinType'],
              }),
          },
          [
            m('option', {value: 'INNER'}, 'INNER'),
            m('option', {value: 'LEFT'}, 'LEFT'),
            m('option', {value: 'RIGHT'}, 'RIGHT'),
            m('option', {value: 'FULL'}, 'FULL'),
          ],
        ),
        m(TextInput, {
          placeholder: 'ON condition...',
          value: node.joinOn,
          oninput: (e: InputEvent) =>
            update({joinOn: (e.target as HTMLInputElement).value}),
        }),
      ]),
  },

  union: {
    title: 'Union',
    icon: 'merge',
    hue: 240,
    inputs: [
      {label: 'Input 1', id: 'input-1'},
      {label: 'Input 2', id: 'input-2'},
    ],
    create: (id, x, y): UnionNodeData => ({
      type: 'union',
      id,
      x,
      y,
      unionType: 'UNION ALL',
    }),
    render: (node: UnionNodeData, update) =>
      m(
        Select,
        {
          value: node.unionType,
          onchange: (e: Event) =>
            update({
              unionType: (e.target as HTMLSelectElement)
                .value as UnionNodeData['unionType'],
            }),
        },
        [
          m('option', {value: 'UNION'}, 'UNION'),
          m('option', {value: 'UNION ALL'}, 'UNION ALL'),
        ],
      ),
  },
};

// Assign stable per-node port IDs to a list of port templates.
// Input port i on node `nodeId` gets id `${nodeId}-in-${i}`.
function makeInputPorts(
  nodeId: string,
  templates: ReadonlyArray<{id: string; label: string}> | undefined,
): NodePort[] | undefined {
  if (!templates) return undefined;
  return templates.map((t) => ({
    direction: 'west' as const,
    id: `${nodeId}-in-${t.id}`,
    label: t.label,
  }));
}

function createLabel(id: string, x: number, y: number): LabelData {
  return {id, x, y, text: 'Label'};
}

function renderLabelContent(
  label: LabelData,
  editing: boolean,
  onStartEdit: () => void,
  onStopEdit: () => void,
  update: (updates: Partial<Omit<LabelData, 'id'>>) => void,
): m.Children {
  return m('textarea', {
    readonly: !editing,
    style: {
      resize: 'both',
      width: '160px',
      height: '60px',
      border: 'none',
      background: 'transparent',
      font: 'inherit',
      pointerEvents: editing ? 'auto' : 'none',
      cursor: editing ? 'text' : 'inherit',
    },
    value: label.text,
    onchange: (e: Event) =>
      update({text: (e.target as HTMLTextAreaElement).value}),
    onkeydown: (e: KeyboardEvent) => {
      if (editing) {
        e.stopPropagation();
        if (e.key === 'Escape') onStopEdit();
      }
    },
    onpointerdown: (e: PointerEvent) => {
      if (editing) e.stopPropagation();
    },
    ondblclick: (e: MouseEvent) => {
      e.stopPropagation();
      onStartEdit();
    },
    onblur: () => onStopEdit(),
  });
}

interface NodeGraphDemoAttrs {
  readonly titleBars?: boolean;
  readonly headerIcons?: boolean;
  readonly accentBars?: boolean;
  readonly contextMenus?: boolean;
}

export function NodeGraphDemo(): m.Component<NodeGraphDemoAttrs> {
  let graphApi: NodeGraphAPI | undefined;
  let editingLabelId: string | undefined;

  // Pipeline 1:  slice ──► filter ──┐
  //                                   ├──► join ──► sort ──► select
  //              sched ──────────────┘
  const sliceId = shortUuid();
  const schedId = shortUuid();
  const filterId = shortUuid();
  const joinId = shortUuid();
  const sortId = shortUuid();
  const selectId = shortUuid();
  const label1Id = shortUuid();

  // Pipeline 2:  thread  ──┐
  //                         ├──► union ──► sort2
  //              process ──┘
  const threadId = shortUuid();
  const processId = shortUuid();
  const unionId = shortUuid();
  const sort2Id = shortUuid();
  const label2Id = shortUuid();

  let store: NodeGraphStore = {
    nodes: [
      // Pipeline 1 — slice+filter are stacked (docked)
      {
        ...NODE_CONFIGS.table.create(sliceId, 50, 80),
        next: {
          ...NODE_CONFIGS.filter.create(filterId, 0, 0),
          filterExpression: 'dur > 1000',
        },
      },
      {...NODE_CONFIGS.table.create(schedId, 50, 300), table: 'sched'},
      {
        ...NODE_CONFIGS.join.create(joinId, 570, 180),
        joinType: 'INNER',
        joinOn: 'utid',
        inputNodeIds: [filterId, schedId], // left=filter, right=sched
      },
      {
        ...NODE_CONFIGS.sort.create(sortId, 830, 180),
        sortColumn: 'dur',
        sortOrder: 'DESC',
        inputNodeIds: [joinId],
      },
      {
        ...NODE_CONFIGS.select.create(selectId, 1090, 180),
        columns: {
          id: true,
          name: true,
          dur: true,
          cpu: false,
          timestamp: false,
        },
        inputNodeIds: [sortId],
      },

      // Pipeline 2
      {...NODE_CONFIGS.table.create(threadId, 50, 700), table: 'thread'},
      {...NODE_CONFIGS.table.create(processId, 50, 900), table: 'process'},
      {
        ...NODE_CONFIGS.union.create(unionId, 310, 790),
        unionType: 'UNION ALL',
        inputNodeIds: [threadId, processId],
      },
      {
        ...NODE_CONFIGS.sort.create(sort2Id, 570, 790),
        sortColumn: 'name',
        sortOrder: 'ASC',
        inputNodeIds: [unionId],
      },
    ],
    labels: [
      {
        ...createLabel(label1Id, 50, 490),
        text: 'Slice ⨝ Sched pipeline\nFilters short slices,\njoins on thread, sorts by duration.',
      },
      {
        ...createLabel(label2Id, 50, 1010),
        text: 'Thread ∪ Process\nAll named entities,\nsorted alphabetically.',
      },
    ],
  };

  // History management
  const history: NodeGraphStore[] = [store];
  let historyIndex = 0;

  // Selection state (separate from undo/redo history)
  const selectedNodeIds = new Set<string>();

  // Helper to find a node by id, searching recursively through linked chains
  function findNodeById(nodes: NodeData[], id: string): NodeData | undefined {
    for (const root of nodes) {
      let cur: NodeData | undefined = root;
      while (cur) {
        if (cur.id === id) return cur;
        cur = cur.next;
      }
    }
    return undefined;
  }

  // Helper to find the parent node (node that has this node as .next)
  function findParent(
    nodes: NodeData[],
    targetId: string,
  ): NodeData | undefined {
    for (const root of nodes) {
      let cur: NodeData | undefined = root;
      while (cur) {
        if (cur.next?.id === targetId) return cur;
        cur = cur.next;
      }
    }
    return undefined;
  }

  // Flatten all nodes (root + docked chains) into a single array.
  function flattenNodes(nodes: NodeData[]): NodeData[] {
    const result: NodeData[] = [];
    for (const root of nodes) {
      let cur: NodeData | undefined = root;
      while (cur) {
        result.push(cur);
        cur = cur.next;
      }
    }
    return result;
  }

  // Derive Connection objects from nodes' inputNodeIds.
  function deriveConnections(nodes: NodeData[]): Connection[] {
    const connections: Connection[] = [];
    for (const node of flattenNodes(nodes)) {
      if (!node.inputNodeIds) continue;
      const manifest = NODE_CONFIGS[node.type];
      node.inputNodeIds.forEach((sourceId, slotIndex) => {
        if (!sourceId) return;
        const inputDef = manifest.inputs?.[slotIndex];
        if (!inputDef) return;
        connections.push({
          fromPort: `${sourceId}-out`,
          toPort: `${node.id}-in-${inputDef.id}`,
        });
      });
    }
    return connections;
  }

  // Find which node and input slot a toPort string refers to.
  function findPortSlot(
    nodes: NodeData[],
    toPort: string,
  ): {node: NodeData; slotIndex: number} | undefined {
    for (const node of flattenNodes(nodes)) {
      const manifest = NODE_CONFIGS[node.type];
      const slotIndex =
        manifest.inputs?.findIndex(
          (input) => `${node.id}-in-${input.id}` === toPort,
        ) ?? -1;
      if (slotIndex !== -1) return {node, slotIndex};
    }
    return undefined;
  }

  // Update store with history
  const updateStore = (updater: (draft: NodeGraphStore) => void) => {
    // Apply the update
    const newStore = produce(store, updater);

    store = newStore;

    // Remove any future history if we're not at the end
    if (historyIndex < history.length - 1) {
      history.splice(historyIndex + 1);
    }

    // Add new state to history
    history.push(store);
    historyIndex = history.length - 1;

    // Limit history to prevent memory issues
    if (history.length > MAX_HISTORY_DEPTH) {
      history.shift();
      historyIndex--;
    }

    m.redraw();
  };

  const undo = () => {
    if (historyIndex > 0) {
      historyIndex--;
      store = history[historyIndex];
      m.redraw();
      console.log(`Undo to state ${historyIndex}`);
    }
  };

  const redo = () => {
    if (historyIndex < history.length - 1) {
      historyIndex++;
      store = history[historyIndex];
      m.redraw();
      console.log(`Redo to state ${historyIndex}`);
    }
  };

  const canUndo = () => historyIndex > 0;
  const canRedo = () => historyIndex < history.length - 1;

  const updateNode = (
    nodeId: string,
    updates: Partial<Omit<NodeData, 'id'>>,
  ) => {
    updateStore((draft) => {
      const node = findNodeById(draft.nodes, nodeId);
      if (node) {
        Object.assign(node, updates);
      }
    });
  };

  const updateLabel = (
    labelId: string,
    updates: Partial<Omit<LabelData, 'id'>>,
  ) => {
    updateStore((draft) => {
      const label = draft.labels.find((l) => l.id === labelId);
      if (label) Object.assign(label, updates);
    });
  };

  const removeNode = (nodeId: string) => {
    updateStore((draft) => {
      // Check if it's a label first
      const labelIdx = draft.labels.findIndex((l) => l.id === nodeId);
      if (labelIdx !== -1) {
        draft.labels.splice(labelIdx, 1);
        selectedNodeIds.delete(nodeId);
        return;
      }

      const nodeToDelete = findNodeById(draft.nodes, nodeId);
      if (!nodeToDelete) return;

      // Clear any inputNodeIds referencing this node from other nodes.
      for (const n of flattenNodes(draft.nodes)) {
        if (!n.inputNodeIds) continue;
        for (let i = 0; i < n.inputNodeIds.length; i++) {
          if (n.inputNodeIds[i] === nodeId) n.inputNodeIds[i] = undefined;
        }
      }

      const parent = findParent(draft.nodes, nodeId);
      if (parent) {
        // Splice out from chain, promoting its child
        parent.next = nodeToDelete.next;
      } else {
        // Root node: remove from array, optionally promoting child to root
        const idx = draft.nodes.findIndex((n) => n.id === nodeId);
        if (idx !== -1) {
          if (nodeToDelete.next) {
            draft.nodes.splice(idx, 1, nodeToDelete.next as NodeData);
          } else {
            draft.nodes.splice(idx, 1);
          }
        }
      }
    });

    // Clear from selection (outside of store update)
    selectedNodeIds.delete(nodeId);

    console.log(`removeNode: ${nodeId}`);
  };

  // Stress test function
  const runStressTest = () => {
    updateStore((draft) => {
      // Clear existing state
      draft.nodes.length = 0;
      draft.labels.length = 0;

      // Create 100 random nodes
      const nodeTypes = Object.keys(NODE_CONFIGS) as NodeData['type'][];
      const nodeIds: string[] = [];
      for (let i = 0; i < 100; i++) {
        const id = shortUuid();
        const type = nodeTypes[Math.floor(Math.random() * nodeTypes.length)];
        const x = Math.random() * 2000;
        const y = Math.random() * 2000;
        draft.nodes.push(NODE_CONFIGS[type].create(id, x, y));
        nodeIds.push(id);
      }

      // Wire ~150 random connections via inputNodeIds.
      for (let i = 0; i < 150; i++) {
        const fromId = nodeIds[Math.floor(Math.random() * nodeIds.length)];
        const toId = nodeIds[Math.floor(Math.random() * nodeIds.length)];
        if (fromId === toId) continue;

        const toNode = findNodeById(draft.nodes, toId);
        if (!toNode) continue;

        const numInputs = NODE_CONFIGS[toNode.type].inputs?.length ?? 0;
        if (numInputs === 0) continue;

        const slotIndex = Math.floor(Math.random() * numInputs);
        if (!toNode.inputNodeIds) toNode.inputNodeIds = [];
        toNode.inputNodeIds[slotIndex] = fromId;
      }

      console.log(`Stress test: Created ${draft.nodes.length} nodes`);
    });

    // Clear selection after stress test
    selectedNodeIds.clear();
  };

  function renderNodeContextMenu(node: NodeData) {
    return [
      m(MenuItem, {
        label: 'Delete',
        icon: 'delete',
        onclick: () => {
          removeNode(node.id);
        },
      }),
    ];
  }

  return {
    view: ({attrs}: m.Vnode<NodeGraphDemoAttrs>) => {
      // Produces the "add downstream node" context menu for output ports.
      // Only pipeline nodes (not table) can be appended downstream.
      function renderAddNodeMenu(toNode: string) {
        const appendableTypes: NodeData['type'][] = [
          'select',
          'filter',
          'sort',
          'join',
          'union',
        ];
        return [
          ...appendableTypes.map((type) => {
            const manifest = NODE_CONFIGS[type];
            return m(MenuItem, {
              label: manifest.title,
              icon: manifest.icon,
              onclick: () => addNode(type, toNode),
              style: {borderLeft: `4px solid hsl(${manifest.hue}, 60%, 50%)`},
            });
          }),
          m(MenuItem, {
            label: 'Label',
            icon: 'label',
            onclick: () => addLabel(),
          }),
        ];
      }

      const addNode = (type: NodeData['type'], toNodeId?: string) => {
        const manifest = NODE_CONFIGS[type];
        const id = shortUuid();

        let x: number;
        let y: number;

        // Use API to find optimal placement if available
        if (graphApi && !toNodeId) {
          const tempNode = manifest.create(id, 0, 0);
          const placement = graphApi.findPlacementForNode({
            id,
            inputs: makeInputPorts(id, manifest.inputs),
            outputs: [
              {
                id: `${id}-out`,
                label: 'Output',
                direction: 'east',
                contextMenuItems: renderAddNodeMenu(id),
              },
            ],
            content: manifest.render(tempNode, () => {}),
            accentBar: attrs.accentBars,
            headerBar: attrs.titleBars
              ? {
                  title: manifest.title,
                  icon: attrs.headerIcons ? manifest.icon : undefined,
                }
              : undefined,
            hue: manifest.hue,
            contextMenuItems: attrs.contextMenus
              ? renderNodeContextMenu(tempNode)
              : undefined,
          });
          x = placement.x;
          y = placement.y;
        } else {
          // Fallback to random position
          x = 100 + Math.random() * 200;
          y = 50 + Math.random() * 200;
        }

        const newNode = manifest.create(id, x, y);
        updateStore((draft) => {
          if (toNodeId) {
            const parentNode = findNodeById(draft.nodes, toNodeId);
            if (parentNode) {
              // Insert newNode between parent and its current next
              const existingNext = parentNode.next;
              parentNode.next = newNode;
              newNode.next = existingNext;
            } else {
              draft.nodes.push(newNode);
            }

            // Re-route any node whose first input was toNodeId so it now
            // comes from the new node's output.
            for (const n of flattenNodes(draft.nodes)) {
              if (!n.inputNodeIds) continue;
              for (let i = 0; i < n.inputNodeIds.length; i++) {
                if (n.inputNodeIds[i] === toNodeId) {
                  n.inputNodeIds[i] = id;
                }
              }
            }
          } else {
            draft.nodes.push(newNode);
          }
        });
      };

      const addLabel = () => {
        const id = shortUuid();
        const x = 100 + Math.random() * 200;
        const y = 50 + Math.random() * 200;
        updateStore((draft) => {
          draft.labels.push(createLabel(id, x, y));
        });
      };

      // Shared helper: builds the common Node fields from a NodeData + manifest.
      // originalManifest is the unsliced manifest used for canDockTop; it
      // defaults to manifest when not supplied (i.e. for root nodes).
      function buildNodeFields(
        nodeData: NodeData,
        manifest: NodeTypeManifest<NodeData>,
        originalManifest?: NodeTypeManifest<NodeData>,
      ) {
        const orig = originalManifest ?? manifest;
        return {
          inputs: makeInputPorts(nodeData.id, manifest.inputs),
          outputs: nodeData.next
            ? []
            : [
                {
                  id: `${nodeData.id}-out`,
                  label: 'Output',
                  direction: 'east' as const,
                  contextMenuItems: renderAddNodeMenu(nodeData.id),
                },
              ],
          content: manifest.render(nodeData, (updates) =>
            updateNode(nodeData.id, updates),
          ),
          accentBar: attrs.accentBars,
          titleBar: attrs.titleBars
            ? {
                title: manifest.title,
                icon: attrs.headerIcons ? manifest.icon : undefined,
              }
            : undefined,
          hue: manifest.hue,
          contextMenuItems: attrs.contextMenus
            ? renderNodeContextMenu(nodeData)
            : undefined,
          canDockTop: (orig.inputs?.length ?? 0) > 0,
          canDockBottom: true,
        };
      }

      // Render a model node and its chain
      function renderNodeChain(nodeData: NodeData): Node {
        const manifest = NODE_CONFIGS[nodeData.type];
        return {
          id: nodeData.id,
          x: nodeData.x,
          y: nodeData.y,
          ...buildNodeFields(nodeData, manifest),
          next: nodeData.next ? renderChildNode(nodeData.next) : undefined,
        };
      }

      // Render child node: hide input[0] (implicitly fed by the parent above).
      function renderChildNode(nodeData: NodeData): Omit<Node, 'x' | 'y'> {
        const manifest = NODE_CONFIGS[nodeData.type];
        const manifestWithoutFirstInput = {
          ...manifest,
          inputs: manifest.inputs?.slice(1),
        };
        return {
          id: nodeData.id,
          ...buildNodeFields(nodeData, manifestWithoutFirstInput, manifest),
          next: nodeData.next ? renderChildNode(nodeData.next) : undefined,
        };
      }

      function renderLabelNode(label: LabelData): Node {
        return {
          id: label.id,
          x: label.x,
          y: label.y,
          hue: 0,
          content: renderLabelContent(
            label,
            editingLabelId === label.id,
            () => {
              editingLabelId = label.id;
              m.redraw();
            },
            () => {
              editingLabelId = undefined;
              m.redraw();
            },
            (updates) => updateLabel(label.id, updates),
          ),
          canDockTop: false,
          canDockBottom: false,
          className: 'pf-ngd__label',
        };
      }

      // Render model state into NodeGraph nodes
      function renderNodes(): Node[] {
        return [
          ...store.nodes.map(renderNodeChain),
          ...store.labels.map(renderLabelNode),
        ];
      }

      const nodeGraphAttrs: NodeGraphAttrs = {
        toolbarItems: [
          m(
            PopupMenu,
            {
              trigger: m(Button, {
                label: 'Add Node',
                title: 'Add a new node to the graph',
                icon: 'add',
                variant: ButtonVariant.Filled,
              }),
            },
            [
              ...(Object.keys(NODE_CONFIGS) as NodeData['type'][]).map(
                (type) => {
                  const manifest = NODE_CONFIGS[type];
                  return m(MenuItem, {
                    label: manifest.title,
                    icon: manifest.icon,
                    onclick: () => addNode(type),
                    style: {
                      borderLeft: `4px solid hsl(${manifest.hue}, 60%, 50%)`,
                    },
                  });
                },
              ),
              m(MenuItem, {
                label: 'Label',
                icon: 'label',
                onclick: () => addLabel(),
              }),
            ],
          ),
          m(
            ButtonGroup,
            m(Button, {
              icon: 'undo',
              title: 'Undo',
              disabled: !canUndo(),
              variant: ButtonVariant.Filled,
              onclick: undo,
            }),
            m(Button, {
              icon: 'redo',
              title: 'Redo',
              disabled: !canRedo(),
              variant: ButtonVariant.Filled,
              onclick: redo,
            }),
          ),
          m(
            ButtonGroup,
            m(Button, {
              title:
                'Add a large number of random nodes and connections for performance testing',
              icon: 'science',
              variant: ButtonVariant.Filled,
              onclick: () => runStressTest(),
            }),
            m(Button, {
              title: 'Remove all nodes from the graph',
              icon: 'delete',
              variant: ButtonVariant.Filled,
              onclick: () => {
                updateStore((draft) => {
                  draft.nodes.length = 0;
                  draft.labels.length = 0;
                });
                selectedNodeIds.clear();
              },
            }),
          ),
        ],
        style: {height: '500px'},
        nodes: renderNodes(),
        connections: deriveConnections(store.nodes),
        selectedNodeIds: selectedNodeIds,
        onReady: (api: NodeGraphAPI) => {
          console.log('onReady');
          graphApi = api;
        },
        onNodeMove: (nodeId: string, x: number, y: number) => {
          console.log(
            `onNodeMove: ${nodeId} to (${x.toFixed(1)}, ${y.toFixed(1)})`,
          );
          updateStore((draft) => {
            const label = draft.labels.find((l) => l.id === nodeId);
            if (label) {
              label.x = x;
              label.y = y;
              return;
            }
            const parent = findParent(draft.nodes, nodeId);
            if (parent) {
              // Undock: detach from parent chain, add as new root
              const child = parent.next!;
              parent.next = undefined;
              child.x = x;
              child.y = y;
              draft.nodes.push(child as NodeData);
            } else {
              const node = findNodeById(draft.nodes, nodeId);
              if (node) {
                node.x = x;
                node.y = y;
              }
            }
          });
        },
        onConnect: (conn: Connection) => {
          console.log(`onConnect: ${conn.fromPort} -> ${conn.toPort}`);
          updateStore((draft) => {
            const slot = findPortSlot(draft.nodes, conn.toPort);
            if (!slot) return;
            const sourceId = conn.fromPort.replace(/-out$/, '');
            if (!slot.node.inputNodeIds) slot.node.inputNodeIds = [];
            slot.node.inputNodeIds[slot.slotIndex] = sourceId;
          });
        },
        onDisconnect: (index: number) => {
          console.log(`onDisconnect: index ${index}`);
          updateStore((draft) => {
            const conn = maybeUndefined(deriveConnections(draft.nodes)[index]);
            if (conn === undefined) return;
            const slot = findPortSlot(draft.nodes, conn.toPort);
            if (!slot?.node.inputNodeIds) return;
            slot.node.inputNodeIds[slot.slotIndex] = undefined;
          });
        },
        onSelect: (nodeIds: string[]) => {
          console.log(`onSelect: [${nodeIds.join(', ')}]`);
          selectedNodeIds.clear();
          nodeIds.forEach((nodeId) => selectedNodeIds.add(nodeId));
          m.redraw();
        },
        onSelectionAdd: (nodeId: string) => {
          console.log(`onSelectionAdd: ${nodeId}`);
          selectedNodeIds.add(nodeId);
          m.redraw();
        },
        onSelectionRemove: (nodeId: string) => {
          console.log(`onSelectionRemove: ${nodeId}`);
          selectedNodeIds.delete(nodeId);
          m.redraw();
        },
        onSelectionClear: () => {
          console.log('onSelectionClear');
          selectedNodeIds.clear();
          m.redraw();
        },
        onNodeDock: (nodeId, targetId) => {
          console.log(`onDock: ${nodeId} -> ${targetId}`);
          updateStore((draft) => {
            const target = findNodeById(draft.nodes, targetId);
            const child = findNodeById(draft.nodes, nodeId);
            if (target && child) {
              // Remove child from root array if it's there
              const childRootIdx = draft.nodes.findIndex(
                (n) => n.id === child.id,
              );
              if (childRootIdx !== -1) {
                draft.nodes.splice(childRootIdx, 1);
              }

              // If the child already exists somewhere else in the graph, remove it
              const parent = findParent(draft.nodes, child.id);
              if (parent) {
                // Undock: detach from parent chain
                parent.next = undefined;
              }

              // If the target node already has children, insert the new child
              // in between the target and its existing children
              if (target.next) {
                // Find the end of the chain of child nodes - this is where
                // we'll attach the target's existing children
                let nextChild = child;
                while (nextChild.next) {
                  nextChild = nextChild.next;
                }
                nextChild.next = target.next;
              }
              target.next = child;
            }
            // Clear inputNodeIds that are no longer valid after docking:
            // - child's slot 0 (implicitly fed by the docked parent)
            // - any node whose input was targetId (target's output is now hidden)
            if (child) {
              if (child.inputNodeIds) child.inputNodeIds[0] = undefined;
              for (const n of flattenNodes(draft.nodes)) {
                if (!n.inputNodeIds) continue;
                for (let i = 0; i < n.inputNodeIds.length; i++) {
                  if (n.inputNodeIds[i] === targetId) {
                    n.inputNodeIds[i] = undefined;
                  }
                }
              }
            }
          });
        },
        onViewportMove: ({offset, zoom}) => {
          console.log(
            `onViewportMove: (${offset.x.toFixed(1)}, ${offset.y.toFixed(1)}) zoom=${zoom.toFixed(2)}`,
          );
        },
      };

      return m(NodeGraph, nodeGraphAttrs);
    },
  };
}

export function renderNodeGraph() {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'NodeGraph'),
      m(
        'p',
        'An interactive graph visualization component for displaying nodes and connections between them.',
      ),
    ),
    renderWidgetShowcase({
      noPadding: true,
      renderWidget: (opts) => m(NodeGraphDemo, opts),
      initialOpts: {
        accentBars: false,
        titleBars: true,
        headerIcons: true,
        contextMenus: true,
      },
    }),

    renderDocSection('User Interaction Guide', [
      m('p', [
        'NodeGraph provides an interactive canvas for creating and manipulating node-based graphs. ',
        'Users can navigate, create nodes, connect them, and organize complex workflows.',
      ]),
      m('h3', 'Navigation'),
      m('ul', [
        m('li', [
          m('strong', 'Scroll horizontally and vertically'),
          ' to pan the canvas.',
        ]),
        m('li', [
          m('strong', 'Click and drag on the canvas'),
          ' to pan the canvas.',
        ]),
        m('li', [m('strong', 'Pinch or Ctrl+Scroll'), ' to zoom in and out.']),
      ]),
      m('h3', 'Nodes'),
      m('ul', [
        m('li', [
          m('strong', 'Drag nodes'),
          ' to reposition them on the canvas.',
        ]),
        m('li', [
          m(
            'strong',
            'Drag nodes with top ports below nodes with bottom ports',
          ),
          ' to dock the nodes together.',
        ]),
        m('li', [
          m('strong', 'Click nodes'),
          ' to select them (hold Shift for multiselect).',
        ]),
        m('li', [
          m('strong', 'Shift + click and drag'),
          ' to box select multiple nodes.',
        ]),
        m('li', [
          m('strong', 'Press Delete or Backspace'),
          ' to remove the selected node(s).',
        ]),
        m('li', [
          m('strong', 'Right-click output ports'),
          ' to quickly add nodes docked below.',
        ]),
      ]),
      m('h3', 'Connections'),
      m('ul', [
        m('li', [
          m('strong', 'Drag from output ports to input ports'),
          ' to connect nodes together.',
        ]),
        m('li', [
          m('strong', 'Drag outputs away from ports'),
          ' to disconnect and remove connections.',
        ]),
      ]),
    ]),
  ];
}
