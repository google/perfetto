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

import {classNames} from '../../../base/classnames';
import {Icons} from '../../../base/semantic_icons';
import {Button} from '../../../widgets/button';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {QueryNode} from '../query_node';
import {Icon} from '../../../widgets/icon';

export const PADDING = 20;
export const NODE_HEIGHT = 50;
export const DEFAULT_NODE_WIDTH = 100;

export interface NodeBoxLayout {
  x: number;
  y: number;
  width?: number;
  height?: number;
}

export interface NodeBoxAttrs {
  readonly node: QueryNode;
  readonly layout: NodeBoxLayout;
  readonly isSelected: boolean;
  readonly isDragging: boolean;
  readonly onNodeSelected: (node: QueryNode) => void;
  readonly onNodeDragStart: (node: QueryNode, event: DragEvent) => void;
  readonly onDuplicateNode: (node: QueryNode) => void;
  readonly onDeleteNode: (node: QueryNode) => void;
  readonly onNodeRendered: (node: QueryNode, element: HTMLElement) => void;
}

function renderWarningIcon(node: QueryNode): m.Child {
  const error =
    node.state.queryError || node.state.responseError || node.state.dataError;
  if (!error) return null;

  const iconClasses = classNames('pf-node-box__warning-icon');

  return m(Icon, {
    className: iconClasses,
    icon: 'warning',
    title: error.message,
  });
}

function renderContextMenu(attrs: NodeBoxAttrs): m.Child {
  const {node, onDuplicateNode, onDeleteNode} = attrs;
  return m(
    PopupMenu,
    {
      trigger: m(Button, {
        iconFilled: true,
        icon: Icons.ContextMenuAlt,
      }),
    },
    m(MenuItem, {
      label: 'Duplicate',
      onclick: () => onDuplicateNode(node),
    }),
    m(MenuItem, {
      label: 'Delete',
      onclick: () => onDeleteNode(node),
    }),
  );
}

export const NodeBox: m.Component<NodeBoxAttrs> = {
  oncreate({attrs, dom}) {
    attrs.onNodeRendered(attrs.node, dom as HTMLElement);
  },
  onupdate({attrs, dom}) {
    attrs.onNodeRendered(attrs.node, dom as HTMLElement);
  },
  view({attrs}) {
    const {
      node,
      layout,
      isSelected,
      isDragging,
      onNodeSelected,
      onNodeDragStart,
    } = attrs;

    const conditionalClasses = classNames(
      isSelected && 'pf-node-box__selected',
      !node.validate() && 'pf-node-box__invalid',
      node.state.queryError && 'pf-node-box__invalid-query',
      node.state.responseError && 'pf-node-box__invalid-response',
    );

    const boxStyle = {
      left: `${layout.x}px`,
      top: `${layout.y}px`,
      opacity: isDragging ? '0' : '1',
    };

    return m(
      '.pf-node-box',
      {
        class: conditionalClasses,
        style: boxStyle,
        onclick: () => onNodeSelected(node),
        draggable: true,
        ondragstart: (event: DragEvent) => onNodeDragStart(node, event),
      },
      renderWarningIcon(node),
      m('span.pf-node-box__title', node.getTitle()),
      renderContextMenu(attrs),
    );
  },
};
