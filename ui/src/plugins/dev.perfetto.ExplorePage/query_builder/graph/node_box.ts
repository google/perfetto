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
import {PopupMenu, MenuDivider, MenuTitle} from '../../../../widgets/menu';
import {QueryNode} from '../../query_node';
import {Icon} from '../../../../widgets/icon';
import {buildMenuItems} from './menu_utils';
import {nodeRegistry} from '../node_registry';
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

  const allowedChildren = nodeRegistry.getAllowedChildrenFor(node.type);
  if (allowedChildren.length === 0) {
    return null;
  }

  const multisourceMenuItems = buildMenuItems(
    'multisource',
    (id) => onAddOperationNode(id, node),
    allowedChildren,
  );

  const modificationMenuItems = buildMenuItems(
    'modification',
    (id) => onAddOperationNode(id, node),
    allowedChildren,
  );

  if (modificationMenuItems.length === 0 && multisourceMenuItems.length === 0) {
    return null;
  }

  const menuItems: m.Children[] = [];
  if (modificationMenuItems.length > 0) {
    menuItems.push(m(MenuTitle, {label: 'Modification nodes'}));
    menuItems.push(...modificationMenuItems);
  }
  if (modificationMenuItems.length > 0 && multisourceMenuItems.length > 0) {
    menuItems.push(m(MenuDivider));
  }
  if (multisourceMenuItems.length > 0) {
    menuItems.push(m(MenuTitle, {label: 'Operations'}));
    menuItems.push(...multisourceMenuItems);
  }

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
          class: classNames(node.type),
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
