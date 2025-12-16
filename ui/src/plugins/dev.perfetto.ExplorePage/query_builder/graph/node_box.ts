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
import {PopupMenu} from '../../../../widgets/menu';
import {QueryNode, NodeType} from '../../query_node';
import {Icon} from '../../../../widgets/icon';
import {nodeRegistry} from '../node_registry';
import {buildCategorizedMenuItems} from './menu_utils';
import {NodeDetailsAttrs} from '../node_explorer_types';
import {NodeDetailsContent} from '../node_styling_widgets';

export interface NodeBoxAttrs {
  readonly node: QueryNode;
  readonly onAddOperationNode: (id: string, node: QueryNode) => void;
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

function renderDetailsView(node: QueryNode): m.Child {
  const attrs: NodeDetailsAttrs = node.nodeDetails();
  return NodeDetailsContent(attrs.content);
}

export const NodeBox: m.Component<NodeBoxAttrs> = {
  view({attrs}) {
    const {node} = attrs;

    return [
      m(
        '.pf-exp-node-box__content',
        {
          class: classNames(NodeType[node.type]),
        },
        m('.pf-exp-node-box__details', renderDetailsView(node)),
      ),
      m(
        '.pf-exp-node-box__actions',
        renderAddButton(attrs),
        renderWarningIcon(node),
      ),
    ];
  },
};
