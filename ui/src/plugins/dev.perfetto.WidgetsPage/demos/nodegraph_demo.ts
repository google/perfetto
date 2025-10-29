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

// Simple model state - just data
interface ModelNode {
  id: string;
  type: 'table' | 'select' | 'filter' | 'join';
  x: number;
  y: number;
  nextId?: string; // ID of next node in chain
}

// Node template definition
interface NodeTemplate {
  readonly inputs: ReadonlyArray<NodePort>;
  readonly outputs: ReadonlyArray<NodePort>;
  readonly canDockTop?: boolean;
  readonly canDockBottom?: boolean;
  readonly content?: m.Children;
  readonly hue: number;
}

interface NodeGraphDemoAttrs {
  readonly multiselect?: boolean;
  readonly titleBars?: boolean;
  readonly accentBars?: boolean;
  readonly colors?: boolean;
}

export function NodeGraphDemo(): m.Component<NodeGraphDemoAttrs> {
  let graphApi: NodeGraphApi | undefined;
  const selectedNodeIds = new Set<string>();

  // State for select node checkboxes
  const columnOptions = {
    id: true,
    name: true,
    ts: false,
    dur: false,
  };

  // State for join type
  let joinType = 'INNER';

  // State for join condition
  let joinOn = '';

  let table = 'slice';

  // State for filter expression
  let filterExpression = '';

  // Helper to create a node template for placement calculation
  function createNodeTemplate(
    id: string,
    type: 'table' | 'select' | 'filter' | 'join',
  ): Omit<Node, 'x' | 'y'> {
    const template = nodeTemplates[type];
    return {id, ...template};
  }

  // Function to add a new node
  function addNode(
    type: 'table' | 'select' | 'filter' | 'join',
    toNodeId?: string,
  ) {
    const id = uuidv4();

    let x: number;
    let y: number;

    // Use API to find optimal placement if available
    if (graphApi && !toNodeId) {
      const nodeTemplate = createNodeTemplate(id, type);
      const placement = graphApi.findPlacementForNode(nodeTemplate);
      x = placement.x;
      y = placement.y;
    } else {
      // Fallback to random position
      x = 100 + Math.random() * 200;
      y = 50 + Math.random() * 200;
    }

    const newNode: ModelNode = {
      id: id,
      type: type,
      x,
      y,
    };
    modelNodes.set(newNode.id, newNode);
    if (toNodeId) {
      const parentNode = modelNodes.get(toNodeId);
      if (parentNode) {
        parentNode.nextId = id;
      }
    }
  }

  // Model state - persists across renders
  const modelNodes: Map<string, ModelNode> = new Map();
  addNode('table');

  // Template renderers - map from type to node template
  const nodeTemplates: Record<string, NodeTemplate> = {
    table: {
      inputs: [],
      outputs: [{content: 'Output', direction: 'bottom'}],
      canDockBottom: true,
      hue: 200,
      content: m(
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
    },
    select: {
      inputs: [{content: 'Input', direction: 'top'}],
      outputs: [{content: 'Output', direction: 'bottom'}],
      canDockTop: true,
      canDockBottom: true,
      hue: 100,
      content: m(
        '',
        {style: {display: 'flex', flexDirection: 'column', gap: '4px'}},
        Object.entries(columnOptions).map(([col, checked]) =>
          m(Checkbox, {
            label: col,
            checked,
            onchange: () => {
              columnOptions[col as keyof typeof columnOptions] = !checked;
            },
          }),
        ),
      ),
    },
    filter: {
      inputs: [{content: 'Input', direction: 'top'}],
      outputs: [{content: 'Output', direction: 'bottom'}],
      canDockTop: true,
      canDockBottom: true,
      hue: 50,
      content: m(TextInput, {
        placeholder: 'Filter expression...',
        value: filterExpression,
        onInput: (value: string) => {
          filterExpression = value;
        },
      }),
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
      content: m(
        '',
        {style: {display: 'flex', flexDirection: 'column', gap: '4px'}},
        [
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
            onInput: (value: string) => {
              joinOn = value;
            },
          }),
        ],
      ),
    },
  };

  // Find root nodes (not referenced by any other node's nextId)
  function getRootNodeIds(): string[] {
    const referenced = new Set<string>();
    for (const node of modelNodes.values()) {
      if (node.nextId) referenced.add(node.nextId);
    }
    return Array.from(modelNodes.keys()).filter((id) => !referenced.has(id));
  }

  const connections: Connection[] = [];

  return {
    view: ({attrs}: m.Vnode<NodeGraphDemoAttrs>) => {
      // Render a model node and its chain
      function renderNodeChain(model: ModelNode): Node {
        const template = nodeTemplates[model.type];
        const hasNext = model.nextId !== undefined;
        const nextModel = hasNext ? modelNodes.get(model.nextId!) : undefined;

        return {
          id: model.id,
          x: model.x,
          y: model.y,
          inputs: template.inputs,
          outputs: template.outputs,
          content: template.content,
          canDockBottom: template.canDockBottom,
          canDockTop: template.canDockTop,
          next: nextModel ? renderChildNode(nextModel) : undefined,
          accentBar: attrs.accentBars,
          titleBar: attrs.titleBars
            ? {title: model.type.toUpperCase()}
            : undefined,
          hue: attrs.colors ? template.hue : undefined,
        };
      }

      // Render child node (keep all ports visible)
      function renderChildNode(model: ModelNode): Omit<Node, 'x' | 'y'> {
        const template = nodeTemplates[model.type];
        const hasNext = model.nextId !== undefined;
        const nextModel = hasNext ? modelNodes.get(model.nextId!) : undefined;

        return {
          id: model.id,
          inputs: template.inputs,
          outputs: template.outputs,
          content: template.content,
          canDockBottom: template.canDockBottom,
          canDockTop: template.canDockTop,
          next: nextModel ? renderChildNode(nextModel) : undefined,
          accentBar: attrs.accentBars,
          titleBar: attrs.titleBars
            ? {title: model.type.toUpperCase()}
            : undefined,
          hue: attrs.colors ? template.hue : undefined,
        };
      }

      // Render model state into NodeGraph nodes
      function renderNodes(): Node[] {
        const rootIds = getRootNodeIds();
        return rootIds
          .map((id) => {
            const model = modelNodes.get(id);
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
              onclick: () => addNode('table'),
            }),
            m(MenuItem, {
              label: 'Select',
              icon: 'filter_alt',
              onclick: () => addNode('select'),
            }),
            m(MenuItem, {
              label: 'Filter',
              icon: 'filter_list',
              onclick: () => addNode('filter'),
            }),
            m(MenuItem, {
              label: 'Join',
              icon: 'join',
              onclick: () => addNode('join'),
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
          const model = modelNodes.get(nodeId);
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
          // Find the node to remove
          const nodeToDelete = modelNodes.get(nodeId);
          if (!nodeToDelete) return;

          // Dock any child node to its parent
          for (const parent of modelNodes.values()) {
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
          modelNodes.delete(nodeId);

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
          const target = modelNodes.get(targetId);
          const child = modelNodes.get(childNode.id);

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
          const parent = modelNodes.get(parentId);

          if (parent && parent.nextId) {
            const child = modelNodes.get(parent.nextId);

            if (child) {
              child.x = parent.x;
              child.y = parent.y + 150;
              parent.nextId = undefined;

              // Connect the previously docker nodes together with a
              // connection
              connections.push({
                fromNode: parent.id,
                fromPort: 0,
                toNode: child.id,
                toPort: 0,
              });

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
        accentBars: false,
        titleBars: false,
        colors: false,
      },
      noPadding: true,
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
