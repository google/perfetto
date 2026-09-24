// Copyright (C) 2026 The Android Open Source Project
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

import './tabs.scss';
import m from 'mithril';
import {Gate, isEmptyVnodes} from '../base/mithril_utils';
import {Button} from './button';
import {Icons} from '../base/semantic_icons';
import {maybeUndefined} from '../base/utils';
import {TabStrip} from './tab_strip';

export interface TabsTab {
  // Unique identifier for the tab.
  readonly key: string;
  // Content to display in the tab handle.
  readonly title: m.Children;
  // Content to display when this tab is active.
  readonly content: m.Children;
  // When set, the content is not rendered until the tab is first activated.
  // Useful for expensive views that should not be built eagerly. Once
  // activated, the tab behaves like a regular tab: the content stays mounted
  // (and keeps its state) when the tab is deactivated.
  readonly lazy?: boolean;
  // Whether to show a close button on the tab.
  readonly closeButton?: boolean;
  // Icon to display on the left side of the tab title.
  readonly leftIcon?: string | m.Children;
  // Optional menu items to show in a dropdown menu on the tab.
  // When provided, a menu button appears on hover.
  readonly menuItems?: m.Children;
}

export interface TabsAttrs {
  // The tabs to display.
  readonly tabs: TabsTab[];
  // The currently active tab key (controlled mode).
  // If not provided, the component manages its own state (uncontrolled mode).
  readonly activeTabKey?: string;
  // Called when a tab is clicked.
  readonly onTabChange?: (key: string) => void;
  // Called when a tab's close button is clicked.
  readonly onTabClose?: (key: string) => void;
  // Called when a tab's title is renamed via inline editing. When set, tabs
  // with a string title become renamable on double-click (tabs with non-string
  // titles are not affected). If the input is cleared (empty after trim) or
  // Escape is pressed, the rename is cancelled and this callback is not fired.
  readonly onTabRename?: (key: string, newTitle: string) => void;
  // Whether tabs can be reordered via drag and drop.
  readonly reorderable?: boolean;
  // Called when tabs are reordered. Receives the key of the dragged tab and
  // the key of the tab it was dropped before (or undefined if dropped at end).
  readonly onTabReorder?: (
    draggedKey: string,
    beforeKey: string | undefined,
  ) => void;
  // Called when the "new tab" button is clicked. When set, a "+" button is
  // shown at the end of the tab bar.
  readonly onNewTab?: () => void;
  // Custom content to render in place of the default "+" button. When set,
  // onNewTab is ignored and this content is rendered instead.
  readonly newTabContent?: m.Children;
  // Content to render on the right side of the tab bar.
  readonly rightContent?: m.Children;
  // Visual style of the tab bar. 'card' (the default) renders classic
  // boxed tab handles on a secondary-background bar; 'underline' renders
  // flat text tabs with a primary underline on the active tab.
  // The bar is rendered by the TabStrip component.
  readonly variant?: 'card' | 'underline';
  // Additional class name for the container.
  readonly className?: string;
}

export class Tabs implements m.ClassComponent<TabsAttrs> {
  // Current active tab key (for uncontrolled mode).
  private internalActiveTab?: string;
  // Keys of the tabs that have been active at least once. Content of lazy
  // tabs is only rendered after their key lands here.
  private activatedKeys = new Set<string>();

  view({attrs}: m.CVnode<TabsAttrs>): m.Children {
    const {
      tabs,
      activeTabKey,
      onTabChange,
      onTabClose,
      onTabRename,
      reorderable,
      onTabReorder,
      onNewTab,
      newTabContent,
      rightContent,
      variant = 'card',
      className,
    } = attrs;

    // Get active tab key (controlled or uncontrolled)
    const activeKey = activeTabKey ?? this.internalActiveTab ?? tabs[0]?.key;
    // The active tab counts as activated, so a lazy tab renders its content
    // on the same render in which it becomes active.
    if (activeKey !== undefined) {
      this.activatedKeys.add(activeKey);
    }

    return m(
      '.pf-tabs',
      {className},
      m(
        TabStrip,
        {
          variant,
          reorderable,
          onReorder: (from: number, to: number) => {
            const draggedKey = tabs[from].key;
            // `to` is the dragged tab's final index, so the tab it ends up
            // before is at `to` in the list with the dragged tab removed.
            const rest = tabs.filter((_, i) => i !== from);
            onTabReorder?.(draggedKey, maybeUndefined(rest[to])?.key);
          },
        },
        tabs.map((tab) =>
          m(
            TabStrip.Tab,
            {
              key: tab.key,
              active: tab.key === activeKey,
              closeButton: tab.closeButton,
              leftIcon: tab.leftIcon,
              menuItems: tab.menuItems,
              onpointerdown: () => {
                this.internalActiveTab = tab.key;
                onTabChange?.(tab.key);
              },
              onRename: onTabRename
                ? (newName: string) => onTabRename(tab.key, newName)
                : undefined,
              onClose: () => onTabClose?.(tab.key),
            },
            tab.title,
          ),
        ),
        newTabContent ??
          (onNewTab &&
            m(Button, {
              icon: Icons.Add,
              className: 'pf-tabs__new-tab-btn',
              onclick: () => onNewTab(),
            })),
        !isEmptyVnodes(rightContent) &&
          m('.pf-tabs__right-content', rightContent),
      ),
      m(
        '.pf-tabs__content',
        tabs.map((tab) =>
          m(
            Gate,
            {key: tab.key, open: tab.key === activeKey},
            // Lazy tabs render no content until they are first activated.
            tab.lazy && !this.activatedKeys.has(tab.key)
              ? undefined
              : tab.content,
          ),
        ),
      ),
    );
  }
}
