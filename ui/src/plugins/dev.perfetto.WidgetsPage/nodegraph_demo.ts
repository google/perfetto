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
import {Connection, Node, NodeGraph} from '../../widgets/nodegraph';
import {Checkbox} from '../../widgets/checkbox';
import {PopupMenu} from '../../widgets/menu';
import {MenuItem} from '../../widgets/menu';
import {Button} from '../../widgets/button';
import {TextInput} from '../../widgets/text_input';
import {uuidv4} from '../../base/uuid';
import {Select} from '../../widgets/select';

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
  inputs: string[];
  outputs: string[];
  content?: m.Children;
}

export function NodeGraphDemo() {
  let selectedNodeId: string | null = null;

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

  // Function to add a new node
  function addNode(
    type: 'table' | 'select' | 'filter' | 'join',
    toNodeId?: string,
  ) {
    const id = uuidv4();
    const newNode: ModelNode = {
      id: id,
      type: type,
      x: 100 + Math.random() * 200, // Random offset
      y: 50 + Math.random() * 200,
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
  addNode('select');

  // Template renderers - map from type to node template
  const nodeTemplates: Record<string, NodeTemplate> = {
    table: {
      inputs: [],
      outputs: ['Output'],
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
      inputs: ['Input'],
      outputs: ['Output'],
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
      inputs: ['Input'],
      outputs: ['Output'],
      content: m(TextInput, {
        placeholder: 'Filter expression...',
        value: filterExpression,
        onInput: (value: string) => {
          filterExpression = value;
        },
      }),
    },
    join: {
      inputs: ['Left', 'Right'],
      outputs: ['Output'],
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
      next: nextModel ? renderChildNode(nextModel) : undefined,
      addMenuItems: [
        m(MenuItem, {
          label: 'Select',
          icon: 'filter_alt',
          onclick: () => addNode('select', model.id),
        }),
        m(MenuItem, {
          label: 'Filter',
          icon: 'filter_list',
          onclick: () => addNode('filter', model.id),
        }),
        m(MenuItem, {
          label: 'Join',
          icon: 'join',
          onclick: () => addNode('join', model.id),
        }),
      ],
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
      next: nextModel ? renderChildNode(nextModel) : undefined,
      addMenuItems: [
        m(MenuItem, {
          label: 'Select',
          icon: 'filter_alt',
          onclick: () => addNode('select', model.id),
        }),
        m(MenuItem, {
          label: 'Filter',
          icon: 'filter_list',
          onclick: () => addNode('filter', model.id),
        }),
        m(MenuItem, {
          label: 'Join',
          icon: 'join',
          onclick: () => addNode('join', model.id),
        }),
      ],
    };
  }

  const connections: Connection[] = [];

  return {
    view: () => {
      return m('div', [
        // Add Node button
        m(
          'div',
          {
            style: {
              marginBottom: '8px',
            },
          },
          m(
            PopupMenu,
            {
              trigger: m(Button, {
                label: 'Add Node',
                icon: 'add',
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
        ),

        // NodeGraph
        m(
          'div',
          {
            style: {
              width: '600px',
              height: '400px',
              overflow: 'hidden',
              border: '1px solid #444',
            },
          },
          m(NodeGraph, {
            nodes: renderNodes(),
            connections: connections,
            selectedNodeId: selectedNodeId,
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
              if (selectedNodeId === nodeId) selectedNodeId = null;

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
            onNodeSelect: (nodeId: string | null) => {
              selectedNodeId = nodeId;
              console.log(`onNodeSelect: ${nodeId}`);
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
          }),
        ),
      ]);
    },
  };
}
