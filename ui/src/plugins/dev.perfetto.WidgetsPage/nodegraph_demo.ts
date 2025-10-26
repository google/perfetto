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
import {uuidv4} from '../../base/uuid';

// Simple model state - just data
interface ModelNode {
  id: string;
  type: 'table' | 'select';
  x: number;
  y: number;
  nextId?: string; // ID of next node in chain
}

// Node template definition
interface NodeTemplate {
  title: string;
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

  // Function to add a new node
  function addNode(type: 'table' | 'select') {
    const id = uuidv4();
    const newNode: ModelNode = {
      id: id,
      type: type,
      x: 100 + Math.random() * 200, // Random offset
      y: 50 + Math.random() * 200,
    };
    modelNodes.set(newNode.id, newNode);
  }

  // Model state - persists across renders
  const modelNodes: Map<string, ModelNode> = new Map();
  addNode('table');
  addNode('select');

  // Template renderers - map from type to node template
  const nodeTemplates: Record<string, NodeTemplate> = {
    table: {
      title: 'Table',
      inputs: [],
      outputs: ['Data'],
    },
    select: {
      title: 'SELECT',
      inputs: ['Data'],
      outputs: ['Result'],
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
      title: template.title,
      x: model.x,
      y: model.y,
      inputs: template.inputs,
      outputs: template.outputs,
      content: template.content,
      next: nextModel ? renderChildNode(nextModel) : undefined,
    };
  }

  // Render child node (keep all ports visible)
  function renderChildNode(model: ModelNode): Omit<Node, 'x' | 'y'> {
    const template = nodeTemplates[model.type];
    const hasNext = model.nextId !== undefined;
    const nextModel = hasNext ? modelNodes.get(model.nextId!) : undefined;

    return {
      id: model.id,
      title: template.title,
      inputs: template.inputs,
      outputs: template.outputs,
      content: template.content,
      next: nextModel ? renderChildNode(nextModel) : undefined,
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
                label: 'SELECT',
                icon: 'filter_alt',
                onclick: () => addNode('select'),
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
            onConnect: (conn: Connection) => {
              console.log('Connection created:', conn);
              connections.push(conn);
            },
            onNodeDrag: (nodeId: string, x: number, y: number) => {
              const model = modelNodes.get(nodeId);
              if (model) {
                model.x = x;
                model.y = y;
              }
            },
            onConnectionRemove: (index: number) => {
              console.log('Connection removed:', index);
              connections.splice(index, 1);
            },
            onNodeRemove: (nodeId: string) => {
              modelNodes.delete(nodeId);
              if (selectedNodeId === nodeId) selectedNodeId = null;
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
                console.log(`Docked ${child.id} to ${targetId}`);
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
                  console.log(`Undocked ${child.id} from ${parentId}`);
                }
              }
            },
          }),
        ),
      ]);
    },
  };
}
