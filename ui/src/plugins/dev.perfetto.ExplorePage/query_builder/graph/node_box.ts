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
import {Icon} from '../../../../widgets/icon';
import {Callout} from '../../../../widgets/callout';
import {Intent} from '../../../../widgets/common';
import {nodeRegistry} from '../node_registry';
import {buildCategorizedMenuItems} from './menu_utils';

export interface NodeActions {
  readonly onDuplicateNode: (node: QueryNode) => void;
  readonly onDeleteNode: (node: QueryNode) => void;
  readonly onAddOperationNode: (id: string, node: QueryNode) => void;
}

export interface NodeBoxAttrs extends NodeActions {
  readonly node: QueryNode;
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
  const {node, onAddOperationNode} = attrs;
  const operationNodes = nodeRegistry
    .list()
    .filter(([_id, descriptor]) => descriptor.type === 'modification');

  if (operationNodes.length === 0) {
    return null;
  }

  const menuItems = buildCategorizedMenuItems(operationNodes, (id) =>
    onAddOperationNode(id, node),
  );

  return m(
    PopupMenu,
    {
      trigger: m(Icon, {
        className: 'pf-exp-node-box-add-button',
        icon: 'add',
      }),
    },
    ...menuItems,
  );
}

export const NodeBox: m.Component<NodeBoxAttrs> = {
  view({attrs}) {
    const {node} = attrs;
    const shouldShowTitle = !singleNodeOperation(node.type);

    return [
      m(
        '.pf-exp-node-box__content',
        {
          class: classNames(NodeType[node.type]),
        },
        shouldShowTitle && m('span.pf-exp-node-box__title', node.getTitle()),
        node.state.comment &&
          m(Callout, {intent: Intent.None}, node.state.comment),
        m('.pf-exp-node-box__details', node.nodeDetails?.()),
      ),
      m(
        '.pf-exp-node-box__actions',
        renderAddButton(attrs),
        renderWarningIcon(node),
        renderContextMenu(attrs),
      ),
    ];
  },
};
