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

import {classNames} from '../../../../base/classnames';
import {QueryNode} from '../../query_node';
import {UIFilter} from '../operations/filter';

import {NodeContainer, NodeContainerLayout} from './node_container';
import {NodeBox} from './node_box';

export const PADDING = 20;
export const NODE_HEIGHT = 50;
export const DEFAULT_NODE_WIDTH = 100;

export interface SingleNodeAttrs {
  readonly node: QueryNode;
  readonly layout: NodeContainerLayout;
  readonly isSelected: boolean;
  readonly onNodeSelected: (node: QueryNode) => void;
  readonly onNodeDragStart: (
    node: QueryNode,
    event: DragEvent,
    layout: NodeContainerLayout,
  ) => void;
  readonly onDuplicateNode: (node: QueryNode) => void;
  readonly onDeleteNode: (node: QueryNode) => void;
  readonly onAddOperationNode: (id: string, node: QueryNode) => void;
  readonly onNodeRendered: (node: QueryNode, element: HTMLElement) => void;
  readonly onRemoveFilter: (node: QueryNode, filter: UIFilter) => void;
}

export const SingleNode: m.Component<SingleNodeAttrs> = {
  view({attrs}) {
    const {
      node,
      layout,
      isSelected,
      onNodeSelected,
      onNodeDragStart,
      onNodeRendered,
    } = attrs;

    const conditionalClasses = classNames(
      !node.validate() && 'pf-exp-node-box__invalid',
      node.state.issues?.queryError && 'pf-exp-node-box__invalid-query',
      node.state.issues?.responseError && 'pf-exp-node-box__invalid-response',
    );

    return m(
      NodeContainer,
      {
        node,
        layout,
        isSelected,
        onNodeDragStart,
        onNodeRendered,
      },
      m(
        '.pf-exp-node-box',
        {
          class: conditionalClasses,
          onclick: () => onNodeSelected(node),
        },
        m(NodeBox, {...attrs}),
      ),
    );
  },
};
