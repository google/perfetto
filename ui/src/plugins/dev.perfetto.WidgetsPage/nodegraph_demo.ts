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
import {Select} from '../../widgets/select';
import {TextInput} from '../../widgets/text_input';
import {Checkbox} from '../../widgets/checkbox';
import {MenuItem} from '../../widgets/menu';

export function NodeGraphDemo() {
  let joinType = 'INNER';
  let joinColumn = 'id';
  let filterCondition = 'dur > 1000';
  let selectedNodeId: string | null = null;
  const columnOptions = {
    id: true,
    name: true,
    ts: true,
    dur: false,
    utid: false,
  };

  const nodes: Node[] = [
    {
      id: 'table_slice',
      title: 'Table: slice',
      x: 50,
      y: 80,
      outputs: ['Output'],
      canDockAbove: false,
      contextMenu: [
        m(MenuItem, {
          label: 'Item A',
          icon: 'info',
        }),
        m(MenuItem, {
          label: 'Item B',
          icon: 'filter_alt',
        }),
      ],
      next: {
        id: 'table_slice_filter',
        title: 'WHERE',
        inputs: ['Input'],
        outputs: ['Output'],
        content: m(TextInput, {
          value: filterCondition,
          placeholder: 'Enter condition...',
          oninput: (e: Event) => {
            filterCondition = (e.target as HTMLInputElement).value;
          },
        }),
        contextMenu: [
          m(MenuItem, {
            label: 'Item A',
            icon: 'info',
          }),
          m(MenuItem, {
            label: 'Item B',
            icon: 'filter_alt',
          }),
        ],
      },
    },
    {
      id: 'table_thread',
      title: 'Table: thread',
      x: 50,
      y: 220,
      canDockAbove: false,
      outputs: ['Output'],
      contextMenu: [
        m(MenuItem, {
          label: 'Item A',
          icon: 'info',
        }),
        m(MenuItem, {
          label: 'Item B',
          icon: 'filter_alt',
        }),
      ],
    },
    {
      id: 'join',
      title: 'JOIN',
      x: 320,
      y: 100,
      inputs: ['Left', 'Right'],
      outputs: ['Output'],
      content: m('', [
        m(
          Select,
          {
            value: joinType,
            onchange: (e: Event) => {
              joinType = (e.target as HTMLSelectElement).value;
            },
          },
          [
            m('option', {value: 'INNER'}, 'INNER JOIN'),
            m('option', {value: 'LEFT'}, 'LEFT JOIN'),
            m('option', {value: 'RIGHT'}, 'RIGHT JOIN'),
            m('option', {value: 'FULL'}, 'FULL JOIN'),
          ],
        ),
        m(
          '',
          {
            style: {
              marginTop: '8px',
              fontSize: '12px',
              color: 'var(--pf-color-text-muted)',
            },
          },
          'ON:',
        ),
        m(
          Select,
          {
            value: joinColumn,
            onchange: (e: Event) => {
              joinColumn = (e.target as HTMLSelectElement).value;
            },
          },
          [
            m('option', {value: 'id'}, 'id'),
            m('option', {value: 'utid'}, 'utid'),
            m('option', {value: 'name'}, 'name'),
          ],
        ),
      ]),
      contextMenu: [
        m(MenuItem, {
          label: 'Item A',
          icon: 'info',
        }),
        m(MenuItem, {
          label: 'Item B',
          icon: 'filter_alt',
        }),
      ],
    },
    {
      id: 'filter',
      title: 'WHERE',
      x: 570,
      y: 100,
      inputs: ['Input'],
      outputs: ['Output'],
      content: m(TextInput, {
        value: filterCondition,
        placeholder: 'Enter condition...',
        oninput: (e: Event) => {
          filterCondition = (e.target as HTMLInputElement).value;
        },
      }),
      contextMenu: [
        m(MenuItem, {
          label: 'Item A',
          icon: 'info',
        }),
        m(MenuItem, {
          label: 'Item B',
          icon: 'filter_alt',
        }),
      ],
    },
    {
      id: 'select',
      title: 'SELECT',
      x: 820,
      y: 100,
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
      contextMenu: [
        m(MenuItem, {
          label: 'Item A',
          icon: 'info',
        }),
        m(MenuItem, {
          label: 'Item B',
          icon: 'filter_alt',
        }),
      ],
    },
  ];

  const connections: Connection[] = [
    {fromNode: 'table_slice', fromPort: 0, toNode: 'join', toPort: 0},
    {fromNode: 'table_thread', fromPort: 0, toNode: 'join', toPort: 1},
    {fromNode: 'join', fromPort: 0, toNode: 'filter', toPort: 0},
    {fromNode: 'filter', fromPort: 0, toNode: 'select', toPort: 0},
  ];

  return {
    view: () => {
      return m(
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
          nodes: nodes,
          connections: connections,
          selectedNodeId: selectedNodeId,
          onConnect: (conn: Connection) => {
            console.log('New connection created:', conn);
            connections.push(conn);
          },
          onNodeDrag: (nodeId: string, x: number, y: number) => {
            const node = nodes.find((n) => n.id === nodeId);
            if (node) {
              node.x = x;
              node.y = y;
            }
          },
          onConnectionRemove: (index: number) => {
            console.log('Connection removed at index:', index);
            connections.splice(index, 1);
          },
          onNodeSelect: (nodeId: string | null) => {
            selectedNodeId = nodeId;
            console.log('Node selected:', nodeId);
          },
          onDock: (targetId: string, childNode: Omit<Node, 'x' | 'y'>) => {
            // targetId is the ID of the last node in the chain
            // We need to find this node and attach the child to it

            // First, check if targetId is a root node
            let targetNode: Node | Omit<Node, 'x' | 'y'> | undefined =
              nodes.find((n) => n.id === targetId);

            // If not found in root nodes, search in chains
            if (!targetNode) {
              for (const rootNode of nodes) {
                let current: Node | Omit<Node, 'x' | 'y'> = rootNode;
                while (current.next) {
                  if (current.next.id === targetId) {
                    targetNode = current.next;
                    break;
                  }
                  current = current.next;
                }
                if (targetNode) break;
              }
            }

            if (targetNode) {
              // Remove child from nodes array (it's now in the linked list)
              const childIndex = nodes.findIndex((n) => n.id === childNode.id);
              if (childIndex !== -1) {
                nodes.splice(childIndex, 1);
              }

              // Attach child to target's next
              targetNode.next = childNode;
              console.log(`Node ${childNode.id} docked to ${targetId}`);
            }
          },
          onUndock: (parentId: string) => {
            // Find the parent node (could be root or in a chain)
            let parent: Node | Omit<Node, 'x' | 'y'> | undefined = nodes.find(
              (n) => n.id === parentId,
            );

            // If not found in root nodes, search in chains
            if (!parent) {
              for (const rootNode of nodes) {
                let current: Node | Omit<Node, 'x' | 'y'> = rootNode;
                while (current.next) {
                  if (current.next.id === parentId) {
                    parent = current.next;
                    break;
                  }
                  current = current.next;
                }
                if (parent) break;
              }
            }

            if (parent && parent.next) {
              const childNode = parent.next;

              // Determine position for new root node
              let x = 0;
              let y = 0;
              if ('x' in parent) {
                // Parent is a root node
                x = parent.x;
                y = parent.y + 100;
              } else {
                // Parent is in a chain, find the root to get x coordinate
                const root = nodes.find((n) => {
                  let curr: Node | Omit<Node, 'x' | 'y'> | undefined = n;
                  while (curr !== undefined) {
                    if (curr.id === parent!.id) return true;
                    if (!curr.next) break;
                    curr = curr.next;
                  }
                  return false;
                });
                if (root) {
                  x = root.x;
                  y = root.y + 200; // Offset more for chained parent
                }
              }

              // Create a new root node from the child (KEEPS its entire chain)
              const newNode: Node = {
                ...childNode,
                x,
                y,
              };

              // Break the link (child keeps its own next chain)
              parent.next = undefined;

              // Add back to nodes array as independent root node (with its chain)
              nodes.push(newNode);
              console.log(`Node ${childNode.id} undocked from ${parentId}`);
            }
          },
        }),
      );
    },
  };
}
