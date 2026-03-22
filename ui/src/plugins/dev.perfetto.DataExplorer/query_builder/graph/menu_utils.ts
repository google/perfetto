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
import {Keycap} from '../../../../widgets/hotkey_glyphs';

/**
 * Build menu items for a specific node type.
 *
 * @param nodeType - Type of nodes to include
 * @param onAddNode - Callback when a menu item is clicked
 * @returns Array of Mithril children representing the menu items
 */
export function buildMenuItems(
  nodeType: 'source' | 'multisource' | 'modification' | 'export',
  onAddNode: (id: string) => void,
  allowedIds?: ReadonlyArray<string>,
): m.Children[] {
  const nodes = nodeRegistry
    .list()
    .filter(([_id, descriptor]) => descriptor.type === nodeType)
    .filter(
      ([id, _descriptor]) =>
        allowedIds === undefined || allowedIds.includes(id),
    );

  return buildCategorizedMenuItems(nodes, onAddNode);
}

/**
 * Generate label with optional hotkey for a node descriptor.
 *
 * @param descriptor - Node descriptor
 * @returns Mithril children for the label with hotkey if available
 */
function getLabelWithHotkey(descriptor: NodeDescriptor): m.Children {
  const hotkey =
    descriptor.hotkey && typeof descriptor.hotkey === 'string'
      ? descriptor.hotkey.toUpperCase()
      : undefined;

  if (hotkey) {
    return m('.pf-exp-menu-label-with-hotkey', [
      m('span', descriptor.name),
      m(Keycap, hotkey),
    ]);
  }

  return descriptor.name;
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
  // Group nodes by category, preserving first-seen order for interleaving.
  const grouped = new Map<
    string | undefined,
    Array<[string, NodeDescriptor]>
  >();
  const categoryOrder: Array<string | undefined> = [];
  for (const [id, descriptor] of nodes) {
    const category = descriptor.category;
    if (!grouped.has(category)) {
      grouped.set(category, []);
      categoryOrder.push(category);
    }
    grouped.get(category)?.push([id, descriptor]);
  }

  const menuItems: m.Child[] = [];

  // Render in first-seen order, so uncategorized and categorized items
  // are interleaved based on registration order.
  for (const category of categoryOrder) {
    const catNodes = grouped.get(category);
    if (catNodes === undefined) continue;
    if (category === undefined) {
      // Uncategorized nodes - render directly
      for (const [id, descriptor] of catNodes) {
        menuItems.push(
          m(MenuItem, {
            label: getLabelWithHotkey(descriptor),
            onclick: () => onClickHandler(id),
          }),
        );
      }
    } else {
      // Categorized nodes - render as submenu
      menuItems.push(
        m(
          MenuItem,
          {
            label: category,
          },
          catNodes.map(([id, descriptor]) =>
            m(MenuItem, {
              label: getLabelWithHotkey(descriptor),
              onclick: () => onClickHandler(id),
            }),
          ),
        ),
      );
    }
  }

  return menuItems;
}
