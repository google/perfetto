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
import {Gate} from '../base/mithril_utils';
import {Button} from './button';
import {Icon} from './icon';
import {Icons} from '../base/semantic_icons';
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
  // Whether tabs can be reordered via drag and drop.
  readonly reorderable?: boolean;
  // Called when tabs are reordered. Receives the key of the dragged tab and
  // the key of the tab it was dropped before (or undefined if dropped at end).
  onTabReorder?(draggedKey: string, beforeKey: string | undefined): void;
  // Additional class name for the container.
  readonly className?: string;
}

interface TabHandleAttrs {
  readonly active?: boolean;
  readonly hasCloseButton?: boolean;
  readonly onClose?: () => void;
  readonly onclick?: () => void;
  readonly leftIcon?: string | m.Children;
  readonly tabKey?: string;
  readonly reorderable?: boolean;
  readonly onDragStart?: (key: string) => void;
  readonly onDragEnd?: () => void;
  readonly onDragOver?: (key: string, position: 'before' | 'after') => void;
  readonly onDragLeave?: () => void;
  readonly onDrop?: (key: string) => void;
}

class TabHandle implements m.ClassComponent<TabHandleAttrs> {
  view({attrs, children}: m.CVnode<TabHandleAttrs>): m.Children {
    const {
      active,
      hasCloseButton,
      onClose,
      onclick,
      leftIcon,
      tabKey,
      reorderable,
      onDragStart,
      onDragEnd,
      onDragOver,
      onDragLeave,
      onDrop,
    } = attrs;

    const renderLeftIcon = () => {
      if (leftIcon === undefined) {
        return undefined;
      }
      const style = {alignSelf: 'center'};
      if (typeof leftIcon === 'string') {
        return m(Icon, {icon: leftIcon, className: 'pf-tabs__tab-icon', style});
      }
      return m('.pf-tabs__tab-icon', {style}, leftIcon);
    };

    return m(
      '.pf-tabs__tab',
      {
        className: classNames(active && 'pf-tabs__tab--active'),
        onclick,
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
      m('.pf-tabs__tab-title', children),
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

  view({attrs}: m.CVnode<TabsAttrs>): m.Children {
    const {
      tabs,
      activeTabKey,
      onTabChange,
      onTabClose,
      reorderable,
      onTabReorder,
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
                tabKey: tab.key,
                reorderable,
                onclick: () => {
                  this.internalActiveTab = tab.key;
                  onTabChange?.(tab.key);
                },
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
