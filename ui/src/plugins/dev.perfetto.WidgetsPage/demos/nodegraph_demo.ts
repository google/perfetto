
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
  Node,
  NodeGraph,
  NodeGraphApi,
  NodeGraphAttrs,
  NodePort,
} from '../../../widgets/nodegraph';
import {Select} from '../../../widgets/select';
import {TextInput} from '../../../widgets/text_input';
import {renderDocSection, renderWidgetShowcase} from '../widgets_page_utils';

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

// Store interface
interface NodeGraphStore {
  readonly nodes: Map<string, NodeData>;
  readonly connections: Connection[];
  readonly selectedNodeIds: Set<string>;
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
    // Note: inputs computed dynamically
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
  return m('', {style: {display: 'flex', flexDirection: 'column', gap: '4px'}}, [
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
            sortOrder: (e.target as HTMLSelectElement).value as 'ASC' | 'DESC',
          });
        },
      },
      [m('option', {value: 'ASC'}, 'ASC'), m('option', {value: 'DESC'}, 'DESC')],
    ),
  ]);
}

function renderJoinNode(
  node: JoinNodeData,
  updateNode: (updates: Partial<Omit<JoinNodeData, 'type' | 'id'>>) => void,
): m.Children {
  return m('', {style: {display: 'flex', flexDirection: 'column', gap: '4px'}}, [
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
  ]);
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
}

export function NodeGraphDemo(): m.Component<NodeGraphDemoAttrs> {
  let graphApi: NodeGraphApi | undefined;

  // Initialize store with a single table node
  const initialId = uuidv4();
  let store: NodeGraphStore = {
    nodes: new Map([[initialId, createTableNode(initialId, 150, 100)]]),
    connections: [],
    selectedNodeIds: new Set<string>(),
  };

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

  // Helper to count connected left ports for a union node
  function countConnectedLeftPorts(
    connections: Connection[],
    nodeId: string,
  ): number {
    let count = 0;
    for (const conn of connections) {
      if (conn.toNode === nodeId && conn.toPort > 0) {
        count++;
      }
    }
    return count;
  }

  // Helper to compute dynamic inputs for union nodes
  function computeUnionInputs(
    connections: Connection[],
    nodeId: string,
  ): ReadonlyArray<NodePort> {
    const connectedLeftPorts = countConnectedLeftPorts(connections, nodeId);
    const numLeftPorts = connectedLeftPorts + 1; // Always N+1

    const inputs: NodePort[] = [{content: 'Input 1', direction: 'top'}];

    for (let i = 0; i < numLeftPorts; i++) {
      inputs.push({
        content: `Input ${i + 2}`,
        direction: 'left',
      });
    }

    return inputs;
  }

  // Update helpers using Immer
  const updateStore = (updater: (draft: NodeGraphStore) => void) => {
    store = produce(store, updater);
    m.redraw();
  };

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
        id: tempNode.id,
        inputs: config.inputs,
        outputs: config.outputs,
        canDockTop: config.canDockTop,
        canDockBottom: config.canDockBottom,
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

      // Clear selection if needed
      draft.selectedNodeIds.delete(nodeId);

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

    console.log(`removeNode: ${nodeId}`);
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
    return [
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

  function renderAddNodeMenu(toNode: string) {
    return [
      m(MenuItem, {
        label: 'Select',
        icon: 'filter_alt',
        onclick: () => addNode(createSelectNode, toNode),
      }),
      m(MenuItem, {
        label: 'Filter',
        icon: 'filter_list',
        onclick: () => addNode(createFilterNode, toNode),
      }),
      m(MenuItem, {
        label: 'Sort',
        icon: 'sort',
        onclick: () => addNode(createSortNode, toNode),
      }),
      m(MenuItem, {
        label: 'Join',
        icon: 'join',
        onclick: () => addNode(createJoinNode, toNode),
      }),
      m(MenuItem, {
        label: 'Union',
        icon: 'merge',
        onclick: () => addNode(createUnionNode, toNode),
      }),
      m(MenuItem, {
        label: 'Result',
        icon: 'output',
        onclick: () => addNode(createResultNode, toNode),
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

      // Render a model node and its chain
      function renderNodeChain(nodeData: NodeData): Node {
        const hasNext = nodeData.nextId !== undefined;
        const nextModel = hasNext ? store.nodes.get(nodeData.nextId!) : undefined;

        const config = NODE_CONFIGS[nodeData.type];

        // Compute inputs dynamically for union nodes
        const inputs =
          nodeData.type === 'union'
            ? computeUnionInputs(store.connections, nodeData.id)
            : config.inputs;

        return {
          id: nodeData.id,
          x: nodeData.x,
          y: nodeData.y,
          inputs: inputs,
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
        };
      }

      // Render child node (keep all ports visible)
      function renderChildNode(nodeData: NodeData): Omit<Node, 'x' | 'y'> {
        const hasNext = nodeData.nextId !== undefined;
        const nextModel = hasNext ? store.nodes.get(nodeData.nextId!) : undefined;

        const config = NODE_CONFIGS[nodeData.type];

        // Compute inputs dynamically for union nodes
        const inputs =
          nodeData.type === 'union'
            ? computeUnionInputs(store.connections, nodeData.id)
            : config.inputs;

        return {
          id: nodeData.id,
          inputs: inputs,
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
        toolbarItems: m(
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
            }),
            m(MenuItem, {
              label: 'Select',
              icon: 'filter_alt',
              onclick: () => addNode(createSelectNode),
            }),
            m(MenuItem, {
              label: 'Filter',
              icon: 'filter_list',
              onclick: () => addNode(createFilterNode),
            }),
            m(MenuItem, {
              label: 'Sort',
              icon: 'sort',
              onclick: () => addNode(createSortNode),
            }),
            m(MenuItem, {
              label: 'Join',
              icon: 'join',
              onclick: () => addNode(createJoinNode),
            }),
            m(MenuItem, {
              label: 'Union',
              icon: 'merge',
              onclick: () => addNode(createUnionNode),
            }),
            m(MenuItem, {
              label: 'Result',
              icon: 'output',
              onclick: () => addNode(createResultNode),
            }),
          ],
        ),
        nodes: renderNodes(),
        connections: store.connections,
        selectedNodeIds: store.selectedNodeIds,
        multiselect: attrs.multiselect,
        onReady: (api: NodeGraphApi) => {
          graphApi = api;
        },
        onNodeDrag: (nodeId: string, x: number, y: number) => {
          updateNode(nodeId, {x, y});
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
          updateStore((draft) => {
            draft.selectedNodeIds.clear();
            draft.selectedNodeIds.add(nodeId);
          });
          console.log(`onNodeSelect: ${nodeId}`);
        },
        onNodeAddToSelection: (nodeId: string) => {
          updateStore((draft) => {
            draft.selectedNodeIds.add(nodeId);
          });
          console.log(
            `onNodeAddToSelection: ${nodeId} (total: ${store.selectedNodeIds.size})`,
          );
        },
        onNodeRemoveFromSelection: (nodeId: string) => {
          updateStore((draft) => {
            draft.selectedNodeIds.delete(nodeId);
          });
          console.log(
            `onNodeRemoveFromSelection: ${nodeId} (total: ${store.selectedNodeIds.size})`,
          );
        },
        onSelectionClear: () => {
          updateStore((draft) => {
            draft.selectedNodeIds.clear();
          });
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
        onUndock: (parentId: string) => {
          updateStore((draft) => {
            const parent = draft.nodes.get(parentId);

            if (parent && parent.nextId) {
              const child = draft.nodes.get(parent.nextId);

              if (child) {
                child.x = parent.x;
                child.y = parent.y + 150;
                parent.nextId = undefined;

                console.log(`onUndock: ${child.id} from ${parentId}`);
              }
            }
          });
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
