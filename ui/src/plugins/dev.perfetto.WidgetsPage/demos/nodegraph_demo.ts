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

interface NodeModelKernel<StateT = unknown> {
  readonly name: string;
  readonly inputs?: ReadonlyArray<NodePort>;
  readonly outputs?: ReadonlyArray<NodePort>;
  readonly canDockTop?: boolean;
  readonly canDockBottom?: boolean;
  readonly hue: number;
  readonly state?: StateT;
  renderContent?: () => m.Children;
}

interface NodeModel {
  readonly id: string;
  readonly kernel: NodeModelKernel;

  // The following properties are mutable and modified by the NodeGraph
  x: number;
  y: number;
  nextId?: string; // ID of next node in chain
}

function tableNode(): NodeModelKernel<{table: string}> {
  let table = 'slice';

  return {
    name: 'table',
    outputs: [{content: 'Output', direction: 'bottom'}],
    canDockBottom: true,
    hue: 200,
    get state() {
      return {table};
    },
    renderContent: () =>
      m(
        Select,
        {
          value: table,
          onchange: (e: Event) => {
            table = (e.target as HTMLSelectElement).value;
          },
        },
        [
          m('option', {value: 'slice'}, 'slice'),
          m('option', {value: 'sched'}, 'sched'),
          m('option', {value: 'thread'}, 'thread'),
          m('option', {value: 'process'}, 'process'),
        ],
      ),
  };
}

function selectNode(): NodeModelKernel<{columns: Record<string, boolean>}> {
  const columns: Record<string, boolean> = {
    id: true,
    name: true,
    cpu: false,
    duration: false,
    timestamp: false,
  };

  return {
    name: 'select',
    inputs: [{content: 'Input', direction: 'top'}],
    outputs: [{content: 'Output', direction: 'bottom'}],
    canDockTop: true,
    canDockBottom: true,
    hue: 100,
    get state() {
      return {columns};
    },
    renderContent: () =>
      m(
        '',
        {style: {display: 'flex', flexDirection: 'column', gap: '4px'}},
        Object.entries(columns).map(([col, checked]) =>
          m(Checkbox, {
            label: col,
            checked,
            onchange: () => {
              columns[col as keyof typeof columns] = !checked;
            },
          }),
        ),
      ),
  };
}

function resultNode(): NodeModelKernel {
  return {
    name: 'result',
    inputs: [{content: 'Input', direction: 'top'}],
    canDockTop: true,
    hue: 0,
    renderContent: () => 'Result',
  };
}

function filterNode(): NodeModelKernel<{filterExpression: string}> {
  let filterExpression = '';

  return {
    name: 'filter',
    inputs: [{content: 'Input', direction: 'top'}],
    outputs: [{content: 'Output', direction: 'bottom'}],
    canDockTop: true,
    canDockBottom: true,
    hue: 50,
    get state() {
      return {filterExpression};
    },
    renderContent: () =>
      m(TextInput, {
        placeholder: 'Filter expression...',
        value: filterExpression,
        oninput: (e: InputEvent) => {
          const target = e.target as HTMLInputElement;
          filterExpression = target.value;
        },
      }),
  };
}

function joinNode(): NodeModelKernel<{joinType: string; joinOn: string}> {
  let joinType = 'INNER';
  let joinOn = '';

  return {
    name: 'join',
    inputs: [
      {content: 'Left', direction: 'top'},
      {content: 'Right', direction: 'left'},
    ],
    outputs: [{content: 'Output', direction: 'bottom'}],
    canDockTop: true,
    canDockBottom: true,
    hue: 300,
    get state() {
      return {joinType, joinOn};
    },
    renderContent: () =>
      m('', {style: {display: 'flex', flexDirection: 'column', gap: '4px'}}, [
        m(
          Select,
          {
            value: joinType,
            onchange: (e: Event) => {
              joinType = (e.target as HTMLSelectElement).value;
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
          value: joinOn,
          oninput: (e: InputEvent) => {
            const target = e.target as HTMLInputElement;
            joinOn = target.value;
          },
        }),
      ]),
  };
}

function unionNode(): NodeModelKernel {
  let unionType: string = 'UNION ALL';

  return {
    name: 'union',
    inputs: [
      {content: 'Input 1', direction: 'top'},
      {content: 'Input 2', direction: 'left'},
    ],
    outputs: [{content: 'Output', direction: 'bottom'}],
    canDockTop: true,
    canDockBottom: true,
    hue: 240,
    get state() {
      return {unionType};
    },
    renderContent: () =>
      m(
        Select,
        {
          value: unionType,
          onchange: (e: Event) => {
            unionType = (e.target as HTMLSelectElement).value;
          },
        },
        [
          m('option', {value: 'UNION'}, 'UNION'),
          m('option', {value: 'UNION ALL'}, 'UNION ALL'),
        ],
      ),
  };
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
  const selectedNodeIds = new Set<string>();

  // Helper to find the parent node (node that has this node as nextId)
  function findDockedParent(nodeId: string): NodeModel | undefined {
    for (const node of nodes.values()) {
      if (node.nextId === nodeId) {
        return node;
      }
    }
    return undefined;
  }

  // Helper to find input nodes via connections
  function findConnectedInputs(nodeId: string): Map<number, NodeModel> {
    const inputs = new Map<number, NodeModel>();
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

  function renderNodeContextMenu(model: NodeModel) {
    return [
      m(MenuItem, {
        label: 'Delete',
        icon: 'delete',
        onclick: () => {
          removeNode(model.id);
          console.log(`Context Menu: onNodeRemove: ${model.id}`);
        },
      }),
    ];
  }

  function removeNode(nodeId: string) {
    // Find the node to remove
    const nodeToDelete = nodes.get(nodeId);
    if (!nodeToDelete) return;

    // Dock any child node to its parent
    for (const parent of nodes.values()) {
      if (parent.nextId === nodeId) {
        parent.nextId = nodeToDelete.nextId;
      }
    }

    // Clear selection if needed
    selectedNodeIds.delete(nodeId);

    // Remove any connections to/from this node
    for (let i = connections.length - 1; i >= 0; i--) {
      if (
        connections[i].fromNode === nodeId ||
        connections[i].toNode === nodeId
      ) {
        connections.splice(i, 1);
      }
    }

    // Finally remove the node
    nodes.delete(nodeId);

    console.log(`removeNode: ${nodeId}`);
  }

  // Build SQL query from a node by traversing upwards
  function buildSqlFromNode(nodeId: string): string {
    const node = nodes.get(nodeId);
    if (!node) return '';

    // First check for docked parent
    const dockedParent = findDockedParent(nodeId);
    const connectedInputs = findConnectedInputs(nodeId);

    switch (node.kernel.name) {
      case 'table': {
        const state = node.kernel.state as {table: string} | undefined;
        return state?.table || 'unknown_table';
      }

      case 'select': {
        const state = node.kernel.state as
          | {columns: Record<string, boolean>}
          | undefined;
        const selectedCols = state
          ? Object.entries(state.columns)
              .filter(([_, checked]) => checked)
              .map(([col]) => col)
          : [];
        const colList = selectedCols.length > 0 ? selectedCols.join(', ') : '*';

        const inputSql = dockedParent
          ? buildSqlFromNode(dockedParent.id)
          : connectedInputs.get(0)
            ? buildSqlFromNode(connectedInputs.get(0)!.id)
            : '';

        if (!inputSql) return `SELECT ${colList}`;
        return `SELECT ${colList} FROM (${inputSql})`;
      }

      case 'filter': {
        const state = node.kernel.state as
          | {filterExpression: string}
          | undefined;
        const filterExpr = state?.filterExpression || '';

        const inputSql = dockedParent
          ? buildSqlFromNode(dockedParent.id)
          : connectedInputs.get(0)
            ? buildSqlFromNode(connectedInputs.get(0)!.id)
            : '';

        if (!inputSql) return '';
        if (!filterExpr) return inputSql;
        return `SELECT * FROM (${inputSql}) WHERE ${filterExpr}`;
      }

      case 'join': {
        const state = node.kernel.state as
          | {joinType: string; joinOn: string}
          | undefined;
        const joinType = state?.joinType || 'INNER';
        const joinOn = state?.joinOn || 'true';

        // Join needs two inputs: one docked (or from top connection) and one from left connection
        const leftInput = dockedParent
          ? buildSqlFromNode(dockedParent.id)
          : connectedInputs.get(0)
            ? buildSqlFromNode(connectedInputs.get(0)!.id)
            : '';

        const rightInput = connectedInputs.get(1)
          ? buildSqlFromNode(connectedInputs.get(1)!.id)
          : '';

        if (!leftInput || !rightInput) return leftInput || rightInput || '';
        return `SELECT * FROM (${leftInput}) ${joinType} JOIN (${rightInput}) ON ${joinOn}`;
      }

      case 'union': {
        const state = node.kernel.state as {unionType: string} | undefined;
        const unionType = state?.unionType || '';

        const inputs: string[] = [];

        // Collect all inputs (docked + connections)
        if (dockedParent) {
          inputs.push(buildSqlFromNode(dockedParent.id));
        }
        for (const [_, inputNode] of connectedInputs) {
          inputs.push(buildSqlFromNode(inputNode.id));
        }

        const validInputs = inputs.filter((sql) => sql);
        if (validInputs.length === 0) return '';
        if (validInputs.length === 1) return validInputs[0];
        return validInputs.map((sql) => `(${sql})`).join(` ${unionType} `);
      }

      case 'result': {
        const inputSql = dockedParent
          ? buildSqlFromNode(dockedParent.id)
          : connectedInputs.get(0)
            ? buildSqlFromNode(connectedInputs.get(0)!.id)
            : '';
        return inputSql;
      }

      default:
        return '';
    }
  }

  // Helper to create a node template for placement calculation
  function createNodeModel(
    id: string,
    factory: () => NodeModelKernel,
  ): Omit<NodeModel, 'x' | 'y'> {
    const kernel = factory();
    return {id, kernel};
  }

  // Function to add a new node
  function addNode(factory: () => NodeModelKernel, toNodeId?: string) {
    const id = uuidv4();

    let x: number;
    let y: number;

    // Use API to find optimal placement if available
    if (graphApi && !toNodeId) {
      const nodeTemplate = createNodeModel(id, factory);
      const placement = graphApi.findPlacementForNode(nodeTemplate);
      x = placement.x;
      y = placement.y;
    } else {
      // Fallback to random position
      x = 100 + Math.random() * 200;
      y = 50 + Math.random() * 200;
    }

    const newNode: NodeModel = {
      ...createNodeModel(id, factory),
      x,
      y,
    };
    nodes.set(newNode.id, newNode);
    if (toNodeId) {
      const parentNode = nodes.get(toNodeId);
      if (parentNode) {
        newNode.nextId = parentNode.nextId;
        parentNode.nextId = id;
      }

      // Find any connection connected to the bottom port of this node
      const bottomConnectionIdx = connections.findIndex(
        (c) => c.fromNode === toNodeId && c.fromPort === 0,
      );
      if (bottomConnectionIdx > -1) {
        connections[bottomConnectionIdx] = {
          ...connections[bottomConnectionIdx],
          fromNode: id,
          fromPort: 0,
        };
      }
    }
  }

  // Model state - persists across renders
  const nodes: Map<string, NodeModel> = new Map();
  const connections: Connection[] = [];

  // Add a single table node to start with
  addNode(tableNode);

  function renderAddNodeMenu(toNode: string) {
    return [
      m(MenuItem, {
        label: 'Select',
        icon: 'filter_alt',
        onclick: () => addNode(selectNode, toNode),
      }),
      m(MenuItem, {
        label: 'Filter',
        icon: 'filter_list',
        onclick: () => addNode(filterNode, toNode),
      }),
      m(MenuItem, {
        label: 'Join',
        icon: 'join',
        onclick: () => addNode(joinNode, toNode),
      }),
      m(MenuItem, {
        label: 'Union',
        icon: 'merge',
        onclick: () => addNode(unionNode, toNode),
      }),
      m(MenuItem, {
        label: 'Result',
        icon: 'output',
        onclick: () => addNode(resultNode, toNode),
      }),
    ];
  }

  // Find root nodes (not referenced by any other node's nextId)
  function getRootNodeIds(): string[] {
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
      for (const node of nodes.values()) {
        if (node.kernel.name === 'result') {
          const sql = buildSqlFromNode(node.id);
          queries.push(sql);
        }
      }
      if (queries.length > 0) {
        console.log('Generated SQL queries for result nodes:', queries);
      }

      // Render a model node and its chain
      function renderNodeChain(model: NodeModel): Node {
        const hasNext = model.nextId !== undefined;
        const nextModel = hasNext ? nodes.get(model.nextId!) : undefined;

        return {
          id: model.id,
          x: model.x,
          y: model.y,
          inputs: model.kernel.inputs,
          outputs: model.kernel.outputs?.map((out) => {
            return {...out, contextMenuItems: renderAddNodeMenu(model.id)};
          }),
          content: model.kernel.renderContent?.(),
          canDockBottom: model.kernel.canDockBottom,
          canDockTop: model.kernel.canDockTop,
          next: nextModel ? renderChildNode(nextModel) : undefined,
          accentBar: attrs.accentBars,
          titleBar: attrs.titleBars
            ? {title: model.kernel.name.toUpperCase()}
            : undefined,
          hue: attrs.colors ? model.kernel.hue : undefined,
          contextMenuItems: attrs.contextMenus
            ? renderNodeContextMenu(model)
            : undefined,
        };
      }

      // Render child node (keep all ports visible)
      function renderChildNode(model: NodeModel): Omit<Node, 'x' | 'y'> {
        const hasNext = model.nextId !== undefined;
        const nextModel = hasNext ? nodes.get(model.nextId!) : undefined;

        return {
          id: model.id,
          inputs: model.kernel.inputs,
          outputs: model.kernel.outputs?.map((out) => {
            return {...out, contextMenuItems: renderAddNodeMenu(model.id)};
          }),
          content: model.kernel.renderContent?.(),
          canDockBottom: model.kernel.canDockBottom,
          canDockTop: model.kernel.canDockTop,
          next: nextModel ? renderChildNode(nextModel) : undefined,
          accentBar: attrs.accentBars,
          titleBar: attrs.titleBars
            ? {title: model.kernel.name.toUpperCase()}
            : undefined,
          hue: attrs.colors ? model.kernel.hue : undefined,
          contextMenuItems: attrs.contextMenus
            ? renderNodeContextMenu(model)
            : undefined,
        };
      }

      // Render model state into NodeGraph nodes
      function renderNodes(): Node[] {
        const rootIds = getRootNodeIds();
        return rootIds
          .map((id) => {
            const model = nodes.get(id);
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
              onclick: () => addNode(tableNode),
            }),
            m(MenuItem, {
              label: 'Select',
              icon: 'filter_alt',
              onclick: () => addNode(selectNode),
            }),
            m(MenuItem, {
              label: 'Filter',
              icon: 'filter_list',
              onclick: () => addNode(filterNode),
            }),
            m(MenuItem, {
              label: 'Join',
              icon: 'join',
              onclick: () => addNode(joinNode),
            }),
            m(MenuItem, {
              label: 'Union',
              icon: 'merge',
              onclick: () => addNode(unionNode),
            }),
            m(MenuItem, {
              label: 'Result',
              icon: 'output',
              onclick: () => addNode(resultNode),
            }),
          ],
        ),
        nodes: renderNodes(),
        connections: connections,
        selectedNodeIds: selectedNodeIds,
        multiselect: attrs.multiselect,
        onReady: (api: NodeGraphApi) => {
          graphApi = api;
        },
        onNodeDrag: (nodeId: string, x: number, y: number) => {
          const model = nodes.get(nodeId);
          if (model) {
            model.x = x;
            model.y = y;
          }
        },
        onConnect: (conn: Connection) => {
          console.log('onConnect:', conn);
          connections.push(conn);
        },
        onConnectionRemove: (index: number) => {
          console.log('onConnectionRemove:', index);
          connections.splice(index, 1);
        },
        onNodeRemove: (nodeId: string) => {
          removeNode(nodeId);
          console.log(`onNodeRemove: ${nodeId}`);
        },
        onNodeSelect: (nodeId: string) => {
          selectedNodeIds.clear();
          selectedNodeIds.add(nodeId);
          console.log(`onNodeSelect: ${nodeId}`);
        },
        onNodeAddToSelection: (nodeId: string) => {
          selectedNodeIds.add(nodeId);
          console.log(
            `onNodeAddToSelection: ${nodeId} (total: ${selectedNodeIds.size})`,
          );
        },
        onNodeRemoveFromSelection: (nodeId: string) => {
          selectedNodeIds.delete(nodeId);
          console.log(
            `onNodeRemoveFromSelection: ${nodeId} (total: ${selectedNodeIds.size})`,
          );
        },
        onSelectionClear: () => {
          selectedNodeIds.clear();
          console.log(`onSelectionClear`);
        },
        onDock: (targetId: string, childNode: Omit<Node, 'x' | 'y'>) => {
          const target = nodes.get(targetId);
          const child = nodes.get(childNode.id);

          if (target && child) {
            target.nextId = child.id;
            console.log(`onDock: ${child.id} to ${targetId}`);
          }

          // If a connection already exists between these nodes, remove it
          for (let i = connections.length - 1; i >= 0; i--) {
            const conn = connections[i];
            if (
              (conn.fromNode === targetId && conn.fromPort === 0) ||
              (conn.toNode === child?.id && conn.toPort === 0)
            ) {
              connections.splice(i, 1);
            }
          }
        },
        onUndock: (parentId: string) => {
          const parent = nodes.get(parentId);

          if (parent && parent.nextId) {
            const child = nodes.get(parent.nextId);

            if (child) {
              child.x = parent.x;
              child.y = parent.y + 150;
              parent.nextId = undefined;

              console.log(`onUndock: ${child.id} from ${parentId}`);
            }
          }
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
