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

import m from 'mithril';
import {classNames} from '../base/classnames';
import {Gate, isEmptyVnodes} from '../base/mithril_utils';
import {Button} from './button';
import {Icon} from './icon';
import {Icons} from '../base/semantic_icons';
import {PopupMenu} from './menu';
import {PopupPosition} from './popup';
import {maybeUndefined} from '../base/utils';

export interface TabsTab {
  // Unique identifier for the tab.
  readonly key: string;
  // Content to display in the tab handle.
  readonly title: m.Children;
  // Content to display when this tab is active.
  readonly content: m.Children;
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
  onTabChange?(key: string): void;
  // Called when a tab's close button is clicked.
  onTabClose?(key: string): void;
  // Called when a tab's title is renamed via inline editing. When set, tabs
  // with a string title become renamable on double-click (tabs with non-string
  // titles are not affected). If the input is cleared (empty after trim) or
  // Escape is pressed, the rename is cancelled and this callback is not fired.
  onTabRename?(key: string, newTitle: string): void;
  // Whether tabs can be reordered via drag and drop.
  readonly reorderable?: boolean;
  // Called when tabs are reordered. Receives the key of the dragged tab and
  // the key of the tab it was dropped before (or undefined if dropped at end).
  onTabReorder?(draggedKey: string, beforeKey: string | undefined): void;
  // Called when the "new tab" button is clicked. When set, a "+" button is
  // shown at the end of the tab bar.
  onNewTab?(): void;
  // Custom content to render in place of the default "+" button. When set,
  // onNewTab is ignored and this content is rendered instead.
  readonly newTabContent?: m.Children;
  // Content to render on the right side of the tab bar.
  readonly rightContent?: m.Children;
  // Additional class name for the container.
  readonly className?: string;
}

interface TabHandleAttrs {
  readonly active?: boolean;
  readonly hasCloseButton?: boolean;
  readonly onClose?: () => void;
  readonly onpointerdown?: () => void;
  readonly ondblclick?: () => void;
  readonly leftIcon?: string | m.Children;
  readonly tabKey?: string;
  readonly reorderable?: boolean;
  readonly renaming?: boolean;
  readonly renameValue?: string;
  readonly onRenameInput?: (value: string) => void;
  readonly onRenameCommit?: () => void;
  readonly onRenameCancel?: () => void;
  readonly onDragStart?: (key: string) => void;
  readonly onDragEnd?: () => void;
  readonly onDragOver?: (key: string, position: 'before' | 'after') => void;
  readonly onDragLeave?: () => void;
  readonly onDrop?: (key: string) => void;
  readonly menuItems?: m.Children;
}

class TabHandle implements m.ClassComponent<TabHandleAttrs> {
  view({attrs, children}: m.CVnode<TabHandleAttrs>): m.Children {
    const {
      active,
      hasCloseButton,
      onClose,
      onpointerdown,
      ondblclick,
      leftIcon,
      tabKey,
      reorderable,
      renaming,
      renameValue,
      onRenameInput,
      onRenameCommit,
      onRenameCancel,
      onDragStart,
      onDragEnd,
      onDragOver,
      onDragLeave,
      onDrop,
      menuItems,
    } = attrs;

    const renderLeftIcon = () => {
      if (leftIcon === undefined) {
        return undefined;
      }
      if (typeof leftIcon === 'string') {
        return m(Icon, {icon: leftIcon, className: 'pf-tabs__tab-icon'});
      }
      return m('.pf-tabs__tab-icon', leftIcon);
    };

    return m(
      '.pf-tabs__tab',
      {
        className: classNames(active && 'pf-tabs__tab--active'),
        onpointerdown,
        ondblclick,
        onauxclick: () => onClose?.(),
        draggable: reorderable,
        ondragstart: reorderable
          ? (e: DragEvent) => {
              if (tabKey) {
                e.dataTransfer?.setData('text/plain', tabKey);
                onDragStart?.(tabKey);
              }
            }
          : undefined,
        ondragend: reorderable ? () => onDragEnd?.() : undefined,
        ondragover: reorderable
          ? (e: DragEvent) => {
              e.preventDefault();
              if (tabKey) {
                const target = e.currentTarget as HTMLElement;
                const rect = target.getBoundingClientRect();
                const midpoint = rect.left + rect.width / 2;
                const position = e.clientX < midpoint ? 'before' : 'after';
                onDragOver?.(tabKey, position);
              }
            }
          : undefined,
        ondragleave: reorderable
          ? (e: DragEvent) => {
              const target = e.currentTarget as HTMLElement;
              const related = e.relatedTarget as HTMLElement | null;
              if (related && !target.contains(related)) {
                onDragLeave?.();
              }
            }
          : undefined,
        ondrop: reorderable
          ? (e: DragEvent) => {
              e.preventDefault();
              if (tabKey) {
                onDrop?.(tabKey);
              }
            }
          : undefined,
      },
      renderLeftIcon(),
      renaming
        ? m('input.pf-tabs__tab-rename-input', {
            value: renameValue,
            oncreate: (vnode: m.VnodeDOM) => {
              const el = vnode.dom as HTMLInputElement;
              el.focus();
              el.select();
            },
            oninput: (e: InputEvent) => {
              const target = e.target as HTMLInputElement;
              onRenameInput?.(target.value);
            },
            onkeydown: (e: KeyboardEvent) => {
              if (e.key === 'Enter') {
                onRenameCommit?.();
                e.preventDefault();
              } else if (e.key === 'Escape') {
                onRenameCancel?.();
                e.preventDefault();
              }
              e.stopPropagation();
            },
            onblur: () => onRenameCommit?.(),
            onclick: (e: Event) => e.stopPropagation(),
          })
        : m('.pf-tabs__tab-title', children),
      menuItems !== undefined &&
        m(
          PopupMenu,
          {
            trigger: m(Button, {
              compact: true,
              icon: Icons.ContextMenuAlt,
              className: 'pf-tabs__tab-menu-btn',
            }),
            position: PopupPosition.Bottom,
          },
          menuItems,
        ),
      hasCloseButton &&
        m(Button, {
          compact: true,
          icon: Icons.Close,
          onclick: (e: Event) => {
            e.stopPropagation();
            onClose?.();
          },
        }),
    );
  }
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
      className,
    } = attrs;

    // Get active tab key (controlled or uncontrolled)
    const activeKey = activeTabKey ?? this.internalActiveTab ?? tabs[0]?.key;

    return m(
      '.pf-tabs',
      {className},
      m(
        '.pf-tabs__tabs',
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
              TabHandle,
              {
                active: tab.key === activeKey,
                hasCloseButton: tab.closeButton,
                leftIcon: tab.leftIcon,
                menuItems: tab.menuItems,
                tabKey: tab.key,
                reorderable,
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
                onDragStart: (key) => {
                  this.draggedKey = key;
                },
                onDragEnd: () => {
                  this.draggedKey = undefined;
                  this.dropTargetKey = undefined;
                  this.dropPosition = undefined;
                },
                onDragOver: (key, position) => {
                  this.dropTargetKey = key;
                  this.dropPosition = position;
                },
                onDragLeave: () => {
                  this.dropTargetKey = undefined;
                  this.dropPosition = undefined;
                },
                onDrop: (targetKey) => {
                  if (
                    this.draggedKey &&
                    this.draggedKey !== targetKey &&
                    onTabReorder
                  ) {
                    // Find the key of the tab to insert before
                    const targetIndex = tabs.findIndex(
                      (t) => t.key === targetKey,
                    );
                    let beforeKey: string | undefined;
                    if (this.dropPosition === 'before') {
                      beforeKey = targetKey;
                    } else {
                      // 'after' - insert before the next tab
                      beforeKey = tabs[targetIndex + 1]?.key;
                    }
                    onTabReorder(this.draggedKey, beforeKey);
                  }
                  this.draggedKey = undefined;
                  this.dropTargetKey = undefined;
                  this.dropPosition = undefined;
                },
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
          m(Gate, {key: tab.key, open: tab.key === activeKey}, tab.content),
        ),
      ),
    );
  }
}
