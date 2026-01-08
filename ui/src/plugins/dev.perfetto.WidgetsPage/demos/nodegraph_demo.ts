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

import m from 'mithril';
import {produce} from 'immer';
import {uuidv4} from '../../../base/uuid';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Checkbox} from '../../../widgets/checkbox';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {
  Connection,
  Label,
  Node,
  NodeGraph,
  NodeGraphApi,
  NodeGraphAttrs,
  NodePort,
} from '../../../widgets/nodegraph';
import {Select} from '../../../widgets/select';
import {TextInput} from '../../../widgets/text_input';
import {renderDocSection, renderWidgetShowcase} from '../widgets_page_utils';
import {Icons} from '../../../base/semantic_icons';

// Base node data interface
interface BaseNodeData {
  readonly id: string;
  x: number;
  y: number;
  nextId?: string;
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

interface ResultNodeData extends BaseNodeData {
  readonly type: 'result';
}

// Discriminated union of all node types
type NodeData =
  | TableNodeData
  | SelectNodeData
  | FilterNodeData
  | SortNodeData
  | JoinNodeData
  | UnionNodeData
  | ResultNodeData;

// Store interface (only data that should be in undo/redo history)
interface NodeGraphStore {
  readonly nodes: Map<string, NodeData>;
  readonly connections: Connection[];
  readonly labels: Label[];
  readonly invalidNodes: Set<string>; // Track which nodes are marked as invalid
}

// Node metadata configuration
interface NodeConfig {
  readonly inputs?: ReadonlyArray<NodePort>;
  readonly outputs?: ReadonlyArray<NodePort>;
  readonly canDockTop?: boolean;
  readonly canDockBottom?: boolean;
  readonly hue: number;
}

const NODE_CONFIGS: Record<NodeData['type'], NodeConfig> = {
  table: {
    outputs: [{content: 'Output', direction: 'bottom'}],
    canDockBottom: true,
    hue: 200,
  },
  select: {
    inputs: [{content: 'Input', direction: 'top'}],
    outputs: [{content: 'Output', direction: 'bottom'}],
    canDockTop: true,
    canDockBottom: true,
    hue: 100,
  },
  filter: {
    inputs: [{content: 'Input', direction: 'top'}],
    outputs: [{content: 'Output', direction: 'bottom'}],
    canDockTop: true,
    canDockBottom: true,
    hue: 50,
  },
  sort: {
    inputs: [{content: 'Input', direction: 'top'}],
    outputs: [{content: 'Output', direction: 'bottom'}],
    canDockTop: true,
    canDockBottom: true,
    hue: 150,
  },
  join: {
    inputs: [
      {content: 'Left', direction: 'top'},
      {content: 'Right', direction: 'left'},
    ],
    outputs: [{content: 'Output', direction: 'bottom'}],
    canDockTop: true,
    canDockBottom: true,
    hue: 300,
  },
  union: {
    inputs: [
      {content: 'Input 1', direction: 'top'},
      {content: 'Input 2', direction: 'left'},
    ],
    outputs: [{content: 'Output', direction: 'bottom'}],
    canDockTop: true,
    canDockBottom: true,
    hue: 240,
  },
  result: {
    inputs: [{content: 'Input', direction: 'top'}],
    canDockTop: true,
    hue: 0,
  },
};

// Factory functions for creating node data
function createTableNode(id: string, x: number, y: number): TableNodeData {
  return {
    type: 'table',
    id,
    x,
    y,
    table: 'slice',
  };
}

function createSelectNode(id: string, x: number, y: number): SelectNodeData {
  return {
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
  };
}

function createFilterNode(id: string, x: number, y: number): FilterNodeData {
  return {
    type: 'filter',
    id,
    x,
    y,
    filterExpression: '',
  };
}

function createSortNode(id: string, x: number, y: number): SortNodeData {
  return {
    type: 'sort',
    id,
    x,
    y,
    sortColumn: '',
    sortOrder: 'ASC',
  };
}

function createJoinNode(id: string, x: number, y: number): JoinNodeData {
  return {
    type: 'join',
    id,
    x,
    y,
    joinType: 'INNER',
    joinOn: '',
  };
}

function createUnionNode(id: string, x: number, y: number): UnionNodeData {
  return {
    type: 'union',
    id,
    x,
    y,
    unionType: 'UNION ALL',
  };
}

function createResultNode(id: string, x: number, y: number): ResultNodeData {
  return {
    type: 'result',
    id,
    x,
    y,
  };
}

// Pure render functions for each node type
function renderTableNode(
  node: TableNodeData,
  updateNode: (updates: Partial<Omit<TableNodeData, 'type' | 'id'>>) => void,
): m.Children {
  return m(
    Select,
    {
      value: node.table,
      onchange: (e: Event) => {
        updateNode({table: (e.target as HTMLSelectElement).value});
      },
    },
    [
      m('option', {value: 'slice'}, 'slice'),
      m('option', {value: 'sched'}, 'sched'),
      m('option', {value: 'thread'}, 'thread'),
      m('option', {value: 'process'}, 'process'),
    ],
  );
}

function renderSelectNode(
  node: SelectNodeData,
  updateNode: (updates: Partial<Omit<SelectNodeData, 'type' | 'id'>>) => void,
): m.Children {
  return m(
    '',
    {style: {display: 'flex', flexDirection: 'column', gap: '4px'}},
    Object.entries(node.columns).map(([col, checked]) =>
      m(Checkbox, {
        label: col,
        checked,
        onchange: () => {
          updateNode({
            columns: {
              ...node.columns,
              [col]: !checked,
            },
          });
        },
      }),
    ),
  );
}

function renderFilterNode(
  node: FilterNodeData,
  updateNode: (updates: Partial<Omit<FilterNodeData, 'type' | 'id'>>) => void,
): m.Children {
  return m(TextInput, {
    placeholder: 'Filter expression...',
    value: node.filterExpression,
    oninput: (e: InputEvent) => {
      const target = e.target as HTMLInputElement;
      updateNode({filterExpression: target.value});
    },
  });
}

function renderSortNode(
  node: SortNodeData,
  updateNode: (updates: Partial<Omit<SortNodeData, 'type' | 'id'>>) => void,
): m.Children {
  return m(
    '',
    {style: {display: 'flex', flexDirection: 'column', gap: '4px'}},
    [
      m(TextInput, {
        placeholder: 'Sort column...',
        value: node.sortColumn,
        oninput: (e: InputEvent) => {
          const target = e.target as HTMLInputElement;
          updateNode({sortColumn: target.value});
        },
      }),
      m(
        Select,
        {
          value: node.sortOrder,
          onchange: (e: Event) => {
            updateNode({
              sortOrder: (e.target as HTMLSelectElement).value as
                | 'ASC'
                | 'DESC',
            });
          },
        },
        [
          m('option', {value: 'ASC'}, 'ASC'),
          m('option', {value: 'DESC'}, 'DESC'),
        ],
      ),
    ],
  );
}

function renderJoinNode(
  node: JoinNodeData,
  updateNode: (updates: Partial<Omit<JoinNodeData, 'type' | 'id'>>) => void,
): m.Children {
  return m(
    '',
    {style: {display: 'flex', flexDirection: 'column', gap: '4px'}},
    [
      m(
        Select,
        {
          value: node.joinType,
          onchange: (e: Event) => {
            updateNode({
              joinType: (e.target as HTMLSelectElement)
                .value as JoinNodeData['joinType'],
            });
          },
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
        oninput: (e: InputEvent) => {
          const target = e.target as HTMLInputElement;
          updateNode({joinOn: target.value});
        },
      }),
    ],
  );
}

function renderUnionNode(
  node: UnionNodeData,
  updateNode: (updates: Partial<Omit<UnionNodeData, 'type' | 'id'>>) => void,
): m.Children {
  return m(
    Select,
    {
      value: node.unionType,
      onchange: (e: Event) => {
        updateNode({
          unionType: (e.target as HTMLSelectElement)
            .value as UnionNodeData['unionType'],
        });
      },
    },
    [
      m('option', {value: 'UNION'}, 'UNION'),
      m('option', {value: 'UNION ALL'}, 'UNION ALL'),
    ],
  );
}

function renderResultNode(): m.Children {
  return 'Result';
}

// Master renderer with type narrowing
function renderNodeContent(
  node: NodeData,
  updateNode: (updates: Partial<Omit<NodeData, 'id'>>) => void,
): m.Children {
  switch (node.type) {
    case 'table':
      return renderTableNode(node, updateNode);
    case 'select':
      return renderSelectNode(node, updateNode);
    case 'filter':
      return renderFilterNode(node, updateNode);
    case 'sort':
      return renderSortNode(node, updateNode);
    case 'join':
      return renderJoinNode(node, updateNode);
    case 'union':
      return renderUnionNode(node, updateNode);
    case 'result':
      return renderResultNode();
  }
}

interface NodeGraphDemoAttrs {
  readonly multiselect?: boolean;
  readonly titleBars?: boolean;
  readonly accentBars?: boolean;
  readonly colors?: boolean;
  readonly contextMenus?: boolean;
  readonly contextMenuOnHover?: boolean;
}

export function NodeGraphDemo(): m.Component<NodeGraphDemoAttrs> {
  let graphApi: NodeGraphApi | undefined;

  // Initialize store with a single table node
  const initialId = uuidv4();
  let store: NodeGraphStore = {
    nodes: new Map([[initialId, createTableNode(initialId, 150, 100)]]),
    connections: [],
    labels: [
      {
        id: uuidv4(),
        x: 400,
        y: 100,
        width: 180,
        content: m('.pf-simple-label-text', 'Simple text label'),
      },
      {
        id: uuidv4(),
        x: 400,
        y: 200,
        width: 180,
        content: m(
          '.pf-simple-label-button',
          m(Button, {
            label: 'Click me!',
            onclick: () => {
              console.log('Label button clicked!');
            },
          }),
        ),
      },
    ],
    invalidNodes: new Set<string>(),
  };

  // History management
  const history: NodeGraphStore[] = [store];
  let historyIndex = 0;

  // Selection state (separate from undo/redo history)
  const selectedNodeIds = new Set<string>();

  // Helper to find the parent node (node that has this node as nextId)
  function findDockedParent(
    nodes: Map<string, NodeData>,
    nodeId: string,
  ): NodeData | undefined {
    for (const node of nodes.values()) {
      if (node.nextId === nodeId) {
        return node;
      }
    }
    return undefined;
  }

  // Helper to find input nodes via connections
  function findConnectedInputs(
    nodes: Map<string, NodeData>,
    connections: Connection[],
    nodeId: string,
  ): Map<number, NodeData> {
    const inputs = new Map<number, NodeData>();
    for (const conn of connections) {
      if (conn.toNode === nodeId) {
        const inputNode = nodes.get(conn.fromNode);
        if (inputNode) {
          inputs.set(conn.toPort, inputNode);
        }
      }
    }
    return inputs;
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

    // Limit history to prevent memory issues (keep last 50 states)
    if (history.length > 50) {
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
      const node = draft.nodes.get(nodeId);
      if (node) {
        Object.assign(node, updates);
      }
    });
  };

  const removeNode = (nodeId: string) => {
    updateStore((draft) => {
      const nodeToDelete = draft.nodes.get(nodeId);
      if (!nodeToDelete) return;

      // Dock any child node to its parent
      for (const parent of draft.nodes.values()) {
        if (parent.nextId === nodeId) {
          parent.nextId = nodeToDelete.nextId;
        }
      }

      // Remove any connections to/from this node
      for (let i = draft.connections.length - 1; i >= 0; i--) {
        const c = draft.connections[i];
        if (c.fromNode === nodeId || c.toNode === nodeId) {
          draft.connections.splice(i, 1);
        }
      }

      // Finally remove the node
      draft.nodes.delete(nodeId);
    });

    // Clear from selection (outside of store update)
    selectedNodeIds.delete(nodeId);

    console.log(`removeNode: ${nodeId}`);
  };

  const removeLabel = (labelId: string) => {
    updateStore((draft) => {
      const labelIndex = draft.labels.findIndex((l) => l.id === labelId);
      if (labelIndex !== -1) {
        draft.labels.splice(labelIndex, 1);
      }
    });

    // Clear from selection (outside of store update)
    selectedNodeIds.delete(labelId);

    console.log(`removeLabel: ${labelId}`);
  };

  // Stress test function
  const runStressTest = () => {
    updateStore((draft) => {
      // Clear existing state
      draft.nodes.clear();
      draft.connections.length = 0;
      draft.invalidNodes.clear();

      // Node factory options
      const nodeFactories = [
        createTableNode,
        createSelectNode,
        createFilterNode,
        createSortNode,
        createJoinNode,
        createUnionNode,
        createResultNode,
      ];

      // Create 100 random nodes
      const nodeIds: string[] = [];
      for (let i = 0; i < 100; i++) {
        const id = uuidv4();
        const factory =
          nodeFactories[Math.floor(Math.random() * nodeFactories.length)];
        const x = Math.random() * 2000;
        const y = Math.random() * 2000;
        const newNode = factory(id, x, y);

        draft.nodes.set(id, newNode);
        nodeIds.push(id);
      }

      // Create some stacked (docked) nodes
      // Aim for ~20 stacks (20% of nodes)
      const usedInStacks = new Set<string>();
      let stacksCreated = 0;
      for (let i = 0; i < 20; i++) {
        // Find a parent node that can dock (has canDockBottom)
        const availableParents = nodeIds.filter((id) => {
          const node = draft.nodes.get(id)!;
          const config = NODE_CONFIGS[node.type];
          return config.canDockBottom && !node.nextId && !usedInStacks.has(id);
        });

        if (availableParents.length === 0) break;

        const parentId =
          availableParents[Math.floor(Math.random() * availableParents.length)];
        const parent = draft.nodes.get(parentId)!;

        // Find a child node that can dock (has canDockTop)
        const availableChildren = nodeIds.filter((id) => {
          const node = draft.nodes.get(id)!;
          const config = NODE_CONFIGS[node.type];
          return config.canDockTop && id !== parentId && !usedInStacks.has(id);
        });

        if (availableChildren.length === 0) continue;

        const childId =
          availableChildren[
            Math.floor(Math.random() * availableChildren.length)
          ];

        // Stack the child under the parent
        parent.nextId = childId;
        usedInStacks.add(parentId);
        usedInStacks.add(childId);
        stacksCreated++;
      }

      // Create random connections between nodes
      // Aim for ~150 connections (1.5 per node on average)
      const numConnections = 150;
      for (let i = 0; i < numConnections; i++) {
        // Pick random nodes
        const fromNodeId = nodeIds[Math.floor(Math.random() * nodeIds.length)];
        const toNodeId = nodeIds[Math.floor(Math.random() * nodeIds.length)];

        if (fromNodeId === toNodeId) continue;

        const fromNode = draft.nodes.get(fromNodeId)!;
        const toNode = draft.nodes.get(toNodeId)!;

        // Check if nodes have compatible ports
        const fromConfig = NODE_CONFIGS[fromNode.type];
        const toConfig = NODE_CONFIGS[toNode.type];
        const numOutputs = fromConfig.outputs?.length ?? 0;
        const numInputs = toConfig.inputs?.length ?? 0;

        if (numOutputs === 0 || numInputs === 0) continue;

        // Random output and input ports
        const fromPort = Math.floor(Math.random() * numOutputs);
        const toPort = Math.floor(Math.random() * numInputs);

        // Check if this connection already exists
        const exists = draft.connections.some(
          (c) =>
            c.fromNode === fromNodeId &&
            c.toNode === toNodeId &&
            c.fromPort === fromPort &&
            c.toPort === toPort,
        );

        if (!exists) {
          draft.connections.push({
            fromNode: fromNodeId,
            fromPort,
            toNode: toNodeId,
            toPort,
          });
        }
      }

      console.log(
        `Stress test: Created ${draft.nodes.size} nodes, ${draft.connections.length} connections, and ${stacksCreated} stacks`,
      );
    });

    // Clear selection after stress test
    selectedNodeIds.clear();
  };

  // Build SQL query from a node by traversing upwards
  function buildSqlFromNode(
    nodes: Map<string, NodeData>,
    connections: Connection[],
    nodeId: string,
  ): string {
    const node = nodes.get(nodeId);
    if (!node) return '';

    // First check for docked parent
    const dockedParent = findDockedParent(nodes, nodeId);
    const connectedInputs = findConnectedInputs(nodes, connections, nodeId);

    switch (node.type) {
      case 'table': {
        return node.table || 'unknown_table';
      }

      case 'select': {
        const selectedCols = Object.entries(node.columns)
          .filter(([_, checked]) => checked)
          .map(([col]) => col);
        const colList = selectedCols.length > 0 ? selectedCols.join(', ') : '*';

        const inputSql = dockedParent
          ? buildSqlFromNode(nodes, connections, dockedParent.id)
          : connectedInputs.get(0)
            ? buildSqlFromNode(nodes, connections, connectedInputs.get(0)!.id)
            : '';

        if (!inputSql) return `SELECT ${colList}`;
        return `SELECT ${colList} FROM (${inputSql})`;
      }

      case 'filter': {
        const filterExpr = node.filterExpression || '';

        const inputSql = dockedParent
          ? buildSqlFromNode(nodes, connections, dockedParent.id)
          : connectedInputs.get(0)
            ? buildSqlFromNode(nodes, connections, connectedInputs.get(0)!.id)
            : '';

        if (!inputSql) return '';
        if (!filterExpr) return inputSql;
        return `SELECT * FROM (${inputSql}) WHERE ${filterExpr}`;
      }

      case 'sort': {
        const sortColumn = node.sortColumn || '';
        const sortOrder = node.sortOrder || 'ASC';

        const inputSql = dockedParent
          ? buildSqlFromNode(nodes, connections, dockedParent.id)
          : connectedInputs.get(0)
            ? buildSqlFromNode(nodes, connections, connectedInputs.get(0)!.id)
            : '';

        if (!inputSql) return '';
        if (!sortColumn) return inputSql;
        return `SELECT * FROM (${inputSql}) ORDER BY ${sortColumn} ${sortOrder}`;
      }

      case 'join': {
        const joinType = node.joinType || 'INNER';
        const joinOn = node.joinOn || 'true';

        // Join needs two inputs: one docked (or from top connection) and one from left connection
        const leftInput = dockedParent
          ? buildSqlFromNode(nodes, connections, dockedParent.id)
          : connectedInputs.get(0)
            ? buildSqlFromNode(nodes, connections, connectedInputs.get(0)!.id)
            : '';

        const rightInput = connectedInputs.get(1)
          ? buildSqlFromNode(nodes, connections, connectedInputs.get(1)!.id)
          : '';

        if (!leftInput || !rightInput) return leftInput || rightInput || '';
        return `SELECT * FROM (${leftInput}) ${joinType} JOIN (${rightInput}) ON ${joinOn}`;
      }

      case 'union': {
        const unionType = node.unionType || '';

        const inputs: string[] = [];

        // Collect all inputs (docked + connections)
        if (dockedParent) {
          inputs.push(buildSqlFromNode(nodes, connections, dockedParent.id));
        }
        for (const [_, inputNode] of connectedInputs) {
          inputs.push(buildSqlFromNode(nodes, connections, inputNode.id));
        }

        const validInputs = inputs.filter((sql) => sql);
        if (validInputs.length === 0) return '';
        if (validInputs.length === 1) return validInputs[0];
        return validInputs.map((sql) => `(${sql})`).join(` ${unionType} `);
      }

      case 'result': {
        const inputSql = dockedParent
          ? buildSqlFromNode(nodes, connections, dockedParent.id)
          : connectedInputs.get(0)
            ? buildSqlFromNode(nodes, connections, connectedInputs.get(0)!.id)
            : '';
        return inputSql;
      }
    }
  }

  function renderNodeContextMenu(node: NodeData) {
    const isInvalid = store.invalidNodes.has(node.id);
    return [
      m(MenuItem, {
        label: isInvalid ? 'Mark as Valid' : 'Mark as Invalid',
        icon: isInvalid ? 'check_circle' : 'error',
        onclick: () => {
          updateStore((draft) => {
            if (draft.invalidNodes.has(node.id)) {
              draft.invalidNodes.delete(node.id);
            } else {
              draft.invalidNodes.add(node.id);
            }
          });
          console.log(
            `Context Menu: Toggle invalid state for ${node.id}: ${!isInvalid}`,
          );
        },
      }),
      m(MenuItem, {
        label: 'Delete',
        icon: 'delete',
        onclick: () => {
          removeNode(node.id);
          console.log(`Context Menu: onNodeRemove: ${node.id}`);
        },
      }),
    ];
  }

  // Find root nodes (not referenced by any other node's nextId)
  function getRootNodeIds(nodes: Map<string, NodeData>): string[] {
    const referenced = new Set<string>();
    for (const node of nodes.values()) {
      if (node.nextId) referenced.add(node.nextId);
    }
    return Array.from(nodes.keys()).filter((id) => !referenced.has(id));
  }

  return {
    view: ({attrs}: m.Vnode<NodeGraphDemoAttrs>) => {
      // Log the SQL queries for all result nodes
      const queries = [];
      for (const node of store.nodes.values()) {
        if (node.type === 'result') {
          const sql = buildSqlFromNode(store.nodes, store.connections, node.id);
          queries.push(sql);
        }
      }
      if (queries.length > 0) {
        console.log('Generated SQL queries for result nodes:', queries);
      }

      function renderAddNodeMenu(toNode: string) {
        return [
          m(MenuItem, {
            label: 'Select',
            icon: Icons.Filter,
            onclick: () => addNode(createSelectNode, toNode),
            style: {
              borderLeft: `4px solid hsl(${NODE_CONFIGS.select.hue}, 60%, 50%)`,
            },
          }),
          m(MenuItem, {
            label: 'Filter',
            icon: Icons.Filter,
            onclick: () => addNode(createFilterNode, toNode),
            style: {
              borderLeft: `4px solid hsl(${NODE_CONFIGS.filter.hue}, 60%, 50%)`,
            },
          }),
          m(MenuItem, {
            label: 'Sort',
            icon: 'sort',
            onclick: () => addNode(createSortNode, toNode),
            style: {
              borderLeft: `4px solid hsl(${NODE_CONFIGS.sort.hue}, 60%, 50%)`,
            },
          }),
          m(MenuItem, {
            label: 'Join',
            icon: 'join',
            onclick: () => addNode(createJoinNode, toNode),
            style: {
              borderLeft: `4px solid hsl(${NODE_CONFIGS.join.hue}, 60%, 50%)`,
            },
          }),
          m(MenuItem, {
            label: 'Union',
            icon: 'merge',
            onclick: () => addNode(createUnionNode, toNode),
            style: {
              borderLeft: `4px solid hsl(${NODE_CONFIGS.union.hue}, 60%, 50%)`,
            },
          }),
          m(MenuItem, {
            label: 'Result',
            icon: 'output',
            onclick: () => addNode(createResultNode, toNode),
            style: {
              borderLeft: `4px solid hsl(${NODE_CONFIGS.result.hue}, 60%, 50%)`,
            },
          }),
        ];
      }

      const addNode = (
        factory: (id: string, x: number, y: number) => NodeData,
        toNodeId?: string,
      ) => {
        const id = uuidv4();

        let x: number;
        let y: number;

        // Use API to find optimal placement if available
        if (graphApi && !toNodeId) {
          const tempNode = factory(id, 0, 0);
          const config = NODE_CONFIGS[tempNode.type];
          const placement = graphApi.findPlacementForNode({
            id,
            inputs: config.inputs,
            outputs: config.outputs?.map((out) => {
              return {...out, contextMenuItems: renderAddNodeMenu(tempNode.id)};
            }),
            content: renderNodeContent(tempNode, () => {}),
            canDockBottom: config.canDockBottom,
            canDockTop: config.canDockTop,
            accentBar: attrs.accentBars,
            titleBar: attrs.titleBars
              ? {title: tempNode.type.toUpperCase()}
              : undefined,
            hue: attrs.colors ? config.hue : undefined,
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

        const newNode = factory(id, x, y);

        updateStore((draft) => {
          draft.nodes.set(newNode.id, newNode);

          if (toNodeId) {
            const parentNode = draft.nodes.get(toNodeId);
            if (parentNode) {
              newNode.nextId = parentNode.nextId;
              parentNode.nextId = id;
            }

            // Find any connection connected to the bottom port of this node
            const bottomConnectionIdx = draft.connections.findIndex(
              (c) => c.fromNode === toNodeId && c.fromPort === 0,
            );
            if (bottomConnectionIdx > -1) {
              draft.connections[bottomConnectionIdx] = {
                ...draft.connections[bottomConnectionIdx],
                fromNode: id,
                fromPort: 0,
              };
            }
          }
        });
      };

      // Render a model node and its chain
      function renderNodeChain(nodeData: NodeData): Node {
        const hasNext = nodeData.nextId !== undefined;
        const nextModel = hasNext
          ? store.nodes.get(nodeData.nextId!)
          : undefined;

        const config = NODE_CONFIGS[nodeData.type];

        return {
          id: nodeData.id,
          x: nodeData.x,
          y: nodeData.y,
          inputs: config.inputs,
          outputs: config.outputs?.map((out) => {
            return {...out, contextMenuItems: renderAddNodeMenu(nodeData.id)};
          }),
          content: renderNodeContent(nodeData, (updates) =>
            updateNode(nodeData.id, updates),
          ),
          canDockBottom: config.canDockBottom,
          canDockTop: config.canDockTop,
          next: nextModel ? renderChildNode(nextModel) : undefined,
          accentBar: attrs.accentBars,
          titleBar: attrs.titleBars
            ? {title: nodeData.type.toUpperCase()}
            : undefined,
          hue: attrs.colors ? config.hue : undefined,
          contextMenuItems: attrs.contextMenus
            ? renderNodeContextMenu(nodeData)
            : undefined,
          invalid: store.invalidNodes.has(nodeData.id),
        };
      }

      // Render child node (keep all ports visible)
      function renderChildNode(nodeData: NodeData): Omit<Node, 'x' | 'y'> {
        const hasNext = nodeData.nextId !== undefined;
        const nextModel = hasNext
          ? store.nodes.get(nodeData.nextId!)
          : undefined;

        const config = NODE_CONFIGS[nodeData.type];

        return {
          id: nodeData.id,
          inputs: config.inputs,
          outputs: config.outputs?.map((out) => {
            return {...out, contextMenuItems: renderAddNodeMenu(nodeData.id)};
          }),
          content: renderNodeContent(nodeData, (updates) =>
            updateNode(nodeData.id, updates),
          ),
          canDockBottom: config.canDockBottom,
          canDockTop: config.canDockTop,
          next: nextModel ? renderChildNode(nextModel) : undefined,
          accentBar: attrs.accentBars,
          titleBar: attrs.titleBars
            ? {title: nodeData.type.toUpperCase()}
            : undefined,
          hue: attrs.colors ? config.hue : undefined,
          contextMenuItems: attrs.contextMenus
            ? renderNodeContextMenu(nodeData)
            : undefined,
          invalid: store.invalidNodes.has(nodeData.id),
        };
      }

      // Render model state into NodeGraph nodes
      function renderNodes(): Node[] {
        const rootIds = getRootNodeIds(store.nodes);
        return rootIds
          .map((id) => {
            const model = store.nodes.get(id);
            if (!model) return null;
            return renderNodeChain(model);
          })
          .filter((n): n is Node => n !== null);
      }

      const nodeGraphAttrs: NodeGraphAttrs = {
        toolbarItems: [
          m(Button, {
            label: 'Undo',
            icon: 'undo',
            disabled: !canUndo(),
            onclick: undo,
          }),
          m(Button, {
            label: 'Redo',
            icon: 'redo',
            disabled: !canRedo(),
            onclick: redo,
          }),
          m(
            PopupMenu,
            {
              trigger: m(Button, {
                label: 'Add Node',
                icon: 'add',
                variant: ButtonVariant.Filled,
              }),
            },
            [
              m(MenuItem, {
                label: 'Table',
                icon: 'table_chart',
                onclick: () => addNode(createTableNode),
                style: {
                  borderLeft: `4px solid hsl(${NODE_CONFIGS.table.hue}, 60%, 50%)`,
                },
              }),
              m(MenuItem, {
                label: 'Select',
                icon: Icons.Filter,
                onclick: () => addNode(createSelectNode),
                style: {
                  borderLeft: `4px solid hsl(${NODE_CONFIGS.select.hue}, 60%, 50%)`,
                },
              }),
              m(MenuItem, {
                label: 'Filter',
                icon: Icons.Filter,
                onclick: () => addNode(createFilterNode),
                style: {
                  borderLeft: `4px solid hsl(${NODE_CONFIGS.filter.hue}, 60%, 50%)`,
                },
              }),
              m(MenuItem, {
                label: 'Sort',
                icon: 'sort',
                onclick: () => addNode(createSortNode),
                style: {
                  borderLeft: `4px solid hsl(${NODE_CONFIGS.sort.hue}, 60%, 50%)`,
                },
              }),
              m(MenuItem, {
                label: 'Join',
                icon: 'join',
                onclick: () => addNode(createJoinNode),
                style: {
                  borderLeft: `4px solid hsl(${NODE_CONFIGS.join.hue}, 60%, 50%)`,
                },
              }),
              m(MenuItem, {
                label: 'Union',
                icon: 'merge',
                onclick: () => addNode(createUnionNode),
                style: {
                  borderLeft: `4px solid hsl(${NODE_CONFIGS.union.hue}, 60%, 50%)`,
                },
              }),
              m(MenuItem, {
                label: 'Result',
                icon: 'output',
                onclick: () => addNode(createResultNode),
                style: {
                  borderLeft: `4px solid hsl(${NODE_CONFIGS.result.hue}, 60%, 50%)`,
                },
              }),
            ],
          ),
          m(Button, {
            label: 'Stress Test',
            icon: 'science',
            variant: ButtonVariant.Filled,
            title: 'Generate a large random graph for performance testing',
            onclick: () => runStressTest(),
          }),
        ],
        nodes: renderNodes(),
        connections: store.connections,
        selectedNodeIds: selectedNodeIds,
        multiselect: attrs.multiselect,
        contextMenuOnHover: attrs.contextMenuOnHover,
        onReady: (api: NodeGraphApi) => {
          graphApi = api;
        },
        onNodeMove: (nodeId: string, x: number, y: number) => {
          // Update position in store with history entry when node is dropped
          updateNode(nodeId, {x, y});
          console.log(`onNodeMove: ${nodeId} to (${x}, ${y})`);
        },
        onConnect: (conn: Connection) => {
          console.log('onConnect:', conn);
          updateStore((draft) => {
            draft.connections.push(conn);
          });
        },
        onConnectionRemove: (index: number) => {
          console.log('onConnectionRemove:', index);
          updateStore((draft) => {
            draft.connections.splice(index, 1);
          });
        },
        onNodeRemove: (nodeId: string) => {
          removeNode(nodeId);
          console.log(`onNodeRemove: ${nodeId}`);
        },
        onNodeSelect: (nodeId: string) => {
          selectedNodeIds.clear();
          selectedNodeIds.add(nodeId);
          m.redraw();
          console.log(`onNodeSelect: ${nodeId}`);
        },
        onNodeAddToSelection: (nodeId: string) => {
          selectedNodeIds.add(nodeId);
          m.redraw();
          console.log(
            `onNodeAddToSelection: ${nodeId} (total: ${selectedNodeIds.size})`,
          );
        },
        onNodeRemoveFromSelection: (nodeId: string) => {
          selectedNodeIds.delete(nodeId);
          m.redraw();
          console.log(
            `onNodeRemoveFromSelection: ${nodeId} (total: ${selectedNodeIds.size})`,
          );
        },
        onSelectionClear: () => {
          selectedNodeIds.clear();
          m.redraw();
          console.log(`onSelectionClear`);
        },
        onDock: (targetId: string, childNode: Omit<Node, 'x' | 'y'>) => {
          updateStore((draft) => {
            const target = draft.nodes.get(targetId);
            const child = draft.nodes.get(childNode.id);

            if (target && child) {
              target.nextId = child.id;
              console.log(`onDock: ${child.id} to ${targetId}`);
            }

            // If a connection already exists between these nodes, remove it
            for (let i = draft.connections.length - 1; i >= 0; i--) {
              const conn = draft.connections[i];
              if (
                (conn.fromNode === targetId && conn.fromPort === 0) ||
                (conn.toNode === child?.id && conn.toPort === 0)
              ) {
                draft.connections.splice(i, 1);
              }
            }
          });
        },
        onUndock: (parentId: string, nodeId: string, x: number, y: number) => {
          updateStore((draft) => {
            const parent = draft.nodes.get(parentId);
            const child = draft.nodes.get(nodeId);

            if (parent && child) {
              child.x = x;
              child.y = y;
              parent.nextId = undefined;

              console.log(
                `onUndock: ${nodeId} from ${parentId} at (${x}, ${y})`,
              );
            }
          });
        },
        labels: store.labels,
        onLabelMove: (labelId: string, x: number, y: number) => {
          updateStore((draft) => {
            const label = draft.labels.find((l) => l.id === labelId);
            if (label) {
              label.x = x;
              label.y = y;
              console.log(`onLabelMove: ${labelId} to (${x}, ${y})`);
            }
          });
        },
        onLabelResize: (labelId: string, width: number) => {
          updateStore((draft) => {
            const label = draft.labels.find((l) => l.id === labelId);
            if (label) {
              label.width = width;
              console.log(`onLabelResize: ${labelId} to width ${width}`);
            }
          });
        },
        onLabelRemove: (labelId: string) => {
          removeLabel(labelId);
          console.log(`onLabelRemove: ${labelId}`);
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
        multiselect: true,
        accentBars: true,
        titleBars: false,
        colors: true,
        contextMenus: true,
        contextMenuOnHover: false,
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
