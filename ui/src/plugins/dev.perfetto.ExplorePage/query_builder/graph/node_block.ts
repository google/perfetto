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

import {NodeActions, NodeBox} from './node_box';
import {QueryNode} from '../../query_node';
import {NodeContainer, NodeContainerLayout} from './node_container';
import {classNames} from '../../../../base/classnames';

export interface NodeBlockAttrs extends NodeActions {
  readonly nodes: QueryNode[];
  readonly layout: NodeContainerLayout;
  readonly selectedNode?: QueryNode;
  readonly onNodeSelected: (node: QueryNode) => void;
  readonly onNodeDragStart: (
    node: QueryNode,
    event: DragEvent,
    layout: NodeContainerLayout,
  ) => void;
  readonly onNodeRendered: (node: QueryNode, element: HTMLElement) => void;
}

export const NodeBlock: m.Component<NodeBlockAttrs> = {
  view({attrs}) {
    const {
      nodes,
      layout,
      onNodeSelected,
      onNodeDragStart,
      selectedNode,
      onNodeRendered,
    } = attrs;
    const firstNode = nodes[0];

    const isSelected = selectedNode ? nodes.includes(selectedNode) : false;

    return m(
      NodeContainer,
      {
        node: firstNode,
        layout,
        isSelected,
        onNodeDragStart,
        onNodeRendered,
      },
      m(
        '.pf-exp-node-block',
        nodes.map((n) =>
          m(
            '.pf-exp-node-block__node',
            {
              class: classNames(
                n === selectedNode && 'pf-exp-node-block__node--selected',
              ),
              onclick: () => onNodeSelected(n),
            },
            m(NodeBox, {
              ...attrs,
              node: n,
            }),
          ),
        ),
      ),
    );
  },
};
