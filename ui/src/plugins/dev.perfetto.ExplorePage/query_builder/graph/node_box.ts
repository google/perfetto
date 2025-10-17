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
import {Icons} from '../../../../base/semantic_icons';
import {Button} from '../../../../widgets/button';
import {MenuItem, PopupMenu} from '../../../../widgets/menu';
import {QueryNode, singleNodeOperation, NodeType} from '../../query_node';
import {FilterDefinition} from '../../../../components/widgets/data_grid/common';
import {Chip} from '../../../../widgets/chip';
import {Icon} from '../../../../widgets/icon';
import {Callout} from '../../../../widgets/callout';
import {Intent} from '../../../../widgets/common';

import {NodeContainer} from './node_container';

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
  readonly onNodeDragStart: (
    node: QueryNode,
    event: DragEvent,
    layout: NodeBoxLayout,
  ) => void;
  readonly onDuplicateNode: (node: QueryNode) => void;
  readonly onDeleteNode: (node: QueryNode) => void;
  readonly onAddDerivedNode: (id: string, node: QueryNode) => void;
  readonly onNodeRendered: (node: QueryNode, element: HTMLElement) => void;
  readonly onRemoveFilter: (node: QueryNode, filter: FilterDefinition) => void;
}

export function renderWarningIcon(node: QueryNode): m.Child {
  if (!node.state.issues || !node.state.issues.hasIssues()) return null;

  const iconClasses = classNames('pf-exp-node-box__warning-icon');

  return m(Icon, {
    className: iconClasses,
    icon: 'warning',
    title: node.state.issues.getTitle(),
  });
}

import {nodeRegistry} from '../node_registry';

export function renderContextMenu(attrs: NodeBoxAttrs): m.Child {
  const {node, onDuplicateNode, onDeleteNode} = attrs;
  const menuItems: m.Child[] = [
    m(MenuItem, {
      label: 'Duplicate',
      onclick: () => onDuplicateNode(node),
    }),
    m(MenuItem, {
      label: 'Delete',
      onclick: () => onDeleteNode(node),
    }),
  ];

  return m(
    PopupMenu,
    {
      trigger: m(Button, {
        iconFilled: true,
        icon: Icons.ContextMenu,
      }),
    },
    ...menuItems,
  );
}

export function renderAddButton(attrs: NodeBoxAttrs): m.Child {
  const {node, onAddDerivedNode} = attrs;
  const derivedNodes = nodeRegistry
    .list()
    .filter(([_id, descriptor]) => descriptor.type === 'derived');

  if (derivedNodes.length === 0) {
    return null;
  }

  return m(
    PopupMenu,
    {
      trigger: m(Icon, {
        className: 'pf-exp-node-box-add-button',
        icon: 'add',
      }),
    },
    ...derivedNodes.map(([id, descriptor]) => {
      return m(MenuItem, {
        label: descriptor.name,
        onclick: () => onAddDerivedNode(id, node),
      });
    }),
  );
}

export function renderFilters(attrs: NodeBoxAttrs): m.Child {
  const {node, onRemoveFilter} = attrs;
  if (!node.state.filters || node.state.filters.length === 0) return null;

  return m(
    '.pf-exp-node-box__filters',
    node.state.filters?.map((filter) => {
      const label =
        'value' in filter
          ? `${filter.column} ${filter.op} ${filter.value}`
          : `${filter.column} ${filter.op}`;
      return m(Chip, {
        label,
        removable: true,
        onRemove: () => onRemoveFilter(node, filter),
      });
    }),
  );
}

export const NodeBoxContent: m.Component<NodeBoxAttrs> = {
  view({attrs}) {
    const {node} = attrs;
    const hasCustomTitle = node.state.customTitle !== undefined;
    const shouldShowTitle = !singleNodeOperation(node.type) || hasCustomTitle;

    return m(
      '.pf-exp-node-box__content',
      shouldShowTitle && m('span.pf-exp-node-box__title', node.getTitle()),
      node.state.comment &&
        m(Callout, {intent: Intent.None}, node.state.comment),
      m('.pf-exp-node-box__details', node.nodeDetails?.()),
      renderFilters(attrs),
    );
  },
};

export const NodeBox: m.Component<NodeBoxAttrs> = {
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
      NodeType[node.type],
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
        ('prevNode' in node
          ? [node.prevNode]
          : 'prevNodes' in node
            ? node.prevNodes
            : []
        ).map((_, i, arr) => {
          const portCount = arr.length;
          const left = `calc(${((i + 1) * 100) / (portCount + 1)}% - 5px)`;
          return m('.pf-exp-node-box-port.pf-exp-node-box-port-top', {
            style: {left},
          });
        }),
        renderWarningIcon(node),
        m(NodeBoxContent, attrs),
        renderContextMenu(attrs),
        node.nextNodes.map((_, i) => {
          const portCount = node.nextNodes.length;
          const left = `calc(${((i + 1) * 100) / (portCount + 1)}% - 5px)`;
          return m('.pf-exp-node-box-port.pf-exp-node-box-port-bottom', {
            style: {left},
          });
        }),
        renderAddButton(attrs),
      ),
    );
  },
};
