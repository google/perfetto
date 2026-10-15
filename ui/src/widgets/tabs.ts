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
  // Called when this tab's close button is clicked (before
  // TabsAttrs.onTabClose).
  readonly onClose?: () => void;
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
  readonly variant?: 'card' | 'underline';
  // When true, hides the tab bar and renders only the active tab's content.
  readonly hideTabBar?: boolean;
  // Additional class name for the container.
  readonly className?: string;
}

// A TabStrip that also manages the active tab and renders its content. The
// tab handles themselves (icons, close buttons, menus, renaming) are provided
// by TabStrip.Tab; this component adds active-tab tracking, lazy content, and
// drag-and-drop reordering on top.
export class Tabs implements m.ClassComponent<TabsAttrs> {
  // Current active tab key (for uncontrolled mode).
  private internalActiveTab?: string;
  // Drag state for reordering.
  private draggedKey?: string;
  private dropTargetKey?: string;
  private dropPosition?: 'before' | 'after';
  // Keys of the tabs that have been active at least once. Content of lazy
  // tabs is only rendered after their key lands here.
  private activatedKeys = new Set<string>();

  view({attrs}: m.CVnode<TabsAttrs>): m.Children {
    const {
      tabs,
      activeTabKey,
      onNewTab,
      newTabContent,
      rightContent,
      variant = 'card',
      hideTabBar,
      className,
    } = attrs;

    // Get active tab key (controlled or uncontrolled)
    const activeKey = activeTabKey ?? this.internalActiveTab ?? tabs[0]?.key;
    // The active tab counts as activated, so a lazy tab renders its content
    // on the same render in which it becomes active.
    if (activeKey !== undefined) {
      this.activatedKeys.add(activeKey);
    }

    return m('.pf-tabs', {className}, [
      !hideTabBar &&
        m(
          TabStrip,
          {
            variant,
            rightContent: isEmptyVnodes(rightContent)
              ? undefined
              : rightContent,
          },
          tabs.map((tab, index) =>
            this.renderTab(attrs, tab, index, tab.key === activeKey),
          ),
          newTabContent ??
            (onNewTab &&
              m(Button, {
                icon: Icons.Add,
                className: 'pf-tabs__new-tab-btn',
                onclick: () => onNewTab(),
              })),
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
    ]);
  }

  private renderTab(
    attrs: TabsAttrs,
    tab: TabsTab,
    index: number,
    active: boolean,
  ): m.Children {
    const {tabs, onTabChange, onTabClose, onTabRename, reorderable} = attrs;

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
      TabStrip.Tab,
      {
        key: tab.key,
        className: classNames(
          showDropBefore && 'pf-tabs__tab--drop-before',
          (showDropAfter || showDropAfterFromNext) &&
            'pf-tabs__tab--drop-after',
          this.draggedKey === tab.key && 'pf-tabs__tab--dragging',
        ),
        active,
        icon: tab.leftIcon,
        menuItems: tab.menuItems,
        onClose: tab.closeButton
          ? () => {
              tab.onClose?.();
              onTabClose?.(tab.key);
            }
          : undefined,
        onRename:
          onTabRename && typeof tab.title === 'string'
            ? (newTitle: string) => onTabRename(tab.key, newTitle)
            : undefined,
        onpointerdown: () => {
          this.internalActiveTab = tab.key;
          onTabChange?.(tab.key);
        },
        ...(reorderable && this.dragAttrs(attrs, tab.key)),
      },
      tab.title,
    );
  }

  private dragAttrs(attrs: TabsAttrs, tabKey: string) {
    const {tabs, onTabReorder} = attrs;
    const resetDrag = () => {
      this.draggedKey = undefined;
      this.dropTargetKey = undefined;
      this.dropPosition = undefined;
    };
    return {
      draggable: true,
      ondragstart: (e: DragEvent) => {
        e.dataTransfer?.setData('text/plain', tabKey);
        this.draggedKey = tabKey;
      },
      ondragend: resetDrag,
      ondragover: (e: DragEvent) => {
        e.preventDefault();
        const target = e.currentTarget as HTMLElement;
        const rect = target.getBoundingClientRect();
        const midpoint = rect.left + rect.width / 2;
        this.dropTargetKey = tabKey;
        this.dropPosition = e.clientX < midpoint ? 'before' : 'after';
      },
      ondragleave: (e: DragEvent) => {
        const target = e.currentTarget as HTMLElement;
        const related = e.relatedTarget as HTMLElement | null;
        if (related && !target.contains(related)) {
          this.dropTargetKey = undefined;
          this.dropPosition = undefined;
        }
      },
      ondrop: (e: DragEvent) => {
        e.preventDefault();
        if (this.draggedKey && this.draggedKey !== tabKey && onTabReorder) {
          // Find the key of the tab to insert before
          const targetIndex = tabs.findIndex((t) => t.key === tabKey);
          let beforeKey: string | undefined;
          if (this.dropPosition === 'before') {
            beforeKey = tabKey;
          } else {
            // 'after' - insert before the next tab
            beforeKey = tabs[targetIndex + 1]?.key;
          }
          onTabReorder(this.draggedKey, beforeKey);
        }
        resetDrag();
      },
    };
  }
}
