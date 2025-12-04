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
import {MenuItem} from '../../../../widgets/menu';
import {NodeDescriptor, nodeRegistry} from '../node_registry';

/**
 * Build menu items for a specific node type.
 *
 * @param nodeType - Type of nodes to include
 * @param onAddNode - Callback when a menu item is clicked
 * @returns Array of Mithril children representing the menu items
 */
export function buildMenuItems(
  nodeType: 'source' | 'multisource' | 'modification',
  onAddNode: (id: string) => void,
): m.Children[] {
  const nodes = nodeRegistry
    .list()
    .filter(([_id, descriptor]) => descriptor.type === nodeType);

  return buildCategorizedMenuItems(nodes, onAddNode);
}

/**
 * Build categorized menu items from a list of node descriptors.
 *
 * Nodes with the same `category` will be grouped into a submenu.
 * If a category only has one node, it will be shown directly without a submenu.
 * Uncategorized nodes (category === undefined) will be shown at the end.
 *
 * @param nodes - Array of [id, descriptor] pairs
 * @param onClickHandler - Callback when a menu item is clicked, receives the node id
 * @returns Array of Mithril children representing the menu items
 */
export function buildCategorizedMenuItems(
  nodes: Array<[string, NodeDescriptor]>,
  onClickHandler: (id: string) => void,
): m.Children[] {
  // Group nodes by category
  const grouped = new Map<
    string | undefined,
    Array<[string, NodeDescriptor]>
  >();
  for (const [id, descriptor] of nodes) {
    const category = descriptor.category;
    if (!grouped.has(category)) {
      grouped.set(category, []);
    }
    grouped.get(category)!.push([id, descriptor]);
  }

  const menuItems: m.Child[] = [];

  // First, add categorized nodes (like "Columns")
  for (const [category, catNodes] of grouped.entries()) {
    if (category === undefined) continue;

    if (catNodes.length === 1) {
      // Single node in category - show it directly without submenu
      const [id, descriptor] = catNodes[0];
      menuItems.push(
        m(MenuItem, {
          label: descriptor.name,
          onclick: () => onClickHandler(id),
        }),
      );
    } else {
      // Multiple nodes in category - show submenu
      menuItems.push(
        m(
          MenuItem,
          {
            label: category,
          },
          catNodes.map(([id, descriptor]) =>
            m(MenuItem, {
              label: descriptor.name,
              onclick: () => onClickHandler(id),
            }),
          ),
        ),
      );
    }
  }

  // Then, add uncategorized nodes
  const uncategorized = grouped.get(undefined) ?? [];
  for (const [id, descriptor] of uncategorized) {
    menuItems.push(
      m(MenuItem, {
        label: descriptor.name,
        onclick: () => onClickHandler(id),
      }),
    );
  }

  return menuItems;
}
