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

import {
  NodeBoxAttrs,
  NodeBoxContent,
  renderAddButton,
  renderFilters,
  renderWarningIcon,
} from './node_box';
import {QueryNode} from '../../query_node';
import {NodeContainer} from './node_container';

export interface NodeBlockAttrs extends Omit<NodeBoxAttrs, 'node'> {
  nodes: QueryNode[];
}

export const NodeBlock: m.Component<NodeBlockAttrs> = {
  view({attrs}) {
    const {
      nodes,
      layout,
      onNodeSelected,
      onNodeDragStart,
      isSelected,
      onNodeRendered,
    } = attrs;
    const firstNode = nodes[0];
    const lastNode = nodes[nodes.length - 1];

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
        '.pf-node-block',
        nodes.map((n) =>
          m(
            '.pf-node-block__node',
            {onclick: () => onNodeSelected(n)},
            m(NodeBoxContent, {node: n}),
            renderFilters({...attrs, node: n}),
          ),
        ),
        renderWarningIcon(lastNode),
        renderAddButton({...attrs, node: lastNode}),
      ),
    );
  },
};
