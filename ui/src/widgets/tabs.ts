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
import {classNames} from '../base/classnames';
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
  // Drag state for reordering.
  private draggedKey?: string;
  private dropTargetKey?: string;
  private dropPosition?: 'before' | 'after';
  // Rename state.
  private renamingTabKey?: string;
  private renameInputValue = '';
  private renameCancelled = false;
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
        {variant},
        tabs.map((tab, index) => {
          const isDragTarget = this.dropTargetKey === tab.key;
          const showDropBefore =
            isDragTarget &&
            this.dropPosition === 'before' &&
            this.draggedKey !== tab.key;
          const showDropAfter =
            isDragTarget &&
            this.dropPosition === 'after' &&
            this.draggedKey !== tab.key;
          // Also show drop-after on the previous tab if we're dropping before
          const prevTab = maybeUndefined(tabs[index - 1]);
          const showDropAfterFromNext =
            prevTab &&
            this.dropTargetKey === tabs[index]?.key &&
            this.dropPosition === 'before' &&
            this.draggedKey !== prevTab.key &&
            this.draggedKey !== tab.key;

          return m(
            '.pf-tabs__tab-wrapper',
            {
              key: tab.key,
              className: classNames(
                showDropBefore && 'pf-tabs__tab-wrapper--drop-before',
                (showDropAfter || showDropAfterFromNext) &&
                  'pf-tabs__tab-wrapper--drop-after',
                this.draggedKey === tab.key && 'pf-tabs__tab-wrapper--dragging',
              ),
            },
            m(
              TabStrip.Tab,
              {
                active: tab.key === activeKey,
                closeButton: tab.closeButton,
                leftIcon: tab.leftIcon,
                menuItems: tab.menuItems,
                draggable: reorderable,
                onpointerdown: () => {
                  this.internalActiveTab = tab.key;
                  onTabChange?.(tab.key);
                },
                ondblclick: onTabRename
                  ? () => {
                      if (typeof tab.title === 'string') {
                        this.renameInputValue = tab.title;
                        this.renamingTabKey = tab.key;
                        this.renameCancelled = false;
                      }
                    }
                  : undefined,
                ...(this.renamingTabKey === tab.key && {
                  renaming: true,
                  renameValue: this.renameInputValue,
                  onRenameInput: (value: string) => {
                    this.renameInputValue = value;
                  },
                  onRenameCommit: () => {
                    if (this.renameCancelled) return;
                    const newName = this.renameInputValue.trim();
                    if (newName) {
                      onTabRename?.(tab.key, newName);
                    }
                    this.renamingTabKey = undefined;
                  },
                  onRenameCancel: () => {
                    this.renameCancelled = true;
                    this.renamingTabKey = undefined;
                  },
                }),
                onClose: () => onTabClose?.(tab.key),
                ondragstart: reorderable
                  ? (e: DragEvent) => {
                      e.dataTransfer?.setData('text/plain', tab.key);
                      this.draggedKey = tab.key;
                    }
                  : undefined,
                ondragend: reorderable
                  ? () => {
                      this.draggedKey = undefined;
                      this.dropTargetKey = undefined;
                      this.dropPosition = undefined;
                    }
                  : undefined,
                ondragover: reorderable
                  ? (e: DragEvent) => {
                      e.preventDefault();
                      const target = e.currentTarget as HTMLElement;
                      const rect = target.getBoundingClientRect();
                      const midpoint = rect.left + rect.width / 2;
                      this.dropTargetKey = tab.key;
                      this.dropPosition =
                        e.clientX < midpoint ? 'before' : 'after';
                    }
                  : undefined,
                ondragleave: reorderable
                  ? (e: DragEvent) => {
                      const target = e.currentTarget as HTMLElement;
                      const related = e.relatedTarget as HTMLElement | null;
                      if (related && !target.contains(related)) {
                        this.dropTargetKey = undefined;
                        this.dropPosition = undefined;
                      }
                    }
                  : undefined,
                ondrop: reorderable
                  ? (e: DragEvent) => {
                      e.preventDefault();
                      if (
                        this.draggedKey &&
                        this.draggedKey !== tab.key &&
                        onTabReorder
                      ) {
                        // Find the key of the tab to insert before
                        const targetIndex = tabs.findIndex(
                          (t) => t.key === tab.key,
                        );
                        let beforeKey: string | undefined;
                        if (this.dropPosition === 'before') {
                          beforeKey = tab.key;
                        } else {
                          // 'after' - insert before the next tab
                          beforeKey = tabs[targetIndex + 1]?.key;
                        }
                        onTabReorder(this.draggedKey, beforeKey);
                      }
                      this.draggedKey = undefined;
                      this.dropTargetKey = undefined;
                      this.dropPosition = undefined;
                    }
                  : undefined,
              },
              tab.title,
            ),
          );
        }),
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
