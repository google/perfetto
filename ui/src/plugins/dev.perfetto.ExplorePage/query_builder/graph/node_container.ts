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

import {classNames} from '../../../../base/classnames';
import m from 'mithril';
import {QueryNode} from '../../query_node';
import {NodeBoxLayout} from './node_box';

export interface NodeContainerAttrs {
  node: QueryNode;
  layout: NodeBoxLayout;
  isSelected: boolean;
  onNodeDragStart: (
    node: QueryNode,
    event: DragEvent,
    layout: NodeBoxLayout,
  ) => void;
  onNodeRendered: (node: QueryNode, element: HTMLElement) => void;
}

export const NodeContainer: m.Component<NodeContainerAttrs> = {
  oncreate({attrs, dom}) {
    attrs.onNodeRendered(attrs.node, dom as HTMLElement);
  },
  onupdate({attrs, dom}) {
    attrs.onNodeRendered(attrs.node, dom as HTMLElement);
  },
  view({attrs, children}) {
    const {layout, onNodeDragStart, isSelected, node} = attrs;

    const boxClass = classNames(
      'pf-exp-node-container',
      isSelected && 'pf-exp-node-container--selected',
    );

    const boxStyle = {
      left: `${layout.x}px`,
      top: `${layout.y}px`,
    };

    return m(
      '.pf-exp-node-container',
      {
        class: boxClass,
        style: boxStyle,
        draggable: true,
        ondragstart: (event: DragEvent) => onNodeDragStart(node, event, layout),
      },
      children,
    );
  },
};
