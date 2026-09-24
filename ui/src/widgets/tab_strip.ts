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

import './tab_strip.scss';
import m from 'mithril';
import {classNames} from '../base/classnames';
import {Button} from './button';
import {Icon} from './icon';
import {Icons} from '../base/semantic_icons';
import {PopupMenu} from './menu';
import {PopupPosition} from './popup';
import {assertUnreachable} from '../base/assert';
import type {HTMLAttrs} from './common';

export interface TabStripAttrs {
  // Additional class name for the container.
  readonly className?: string;
  // Visual style of the tab bar. 'card' (the default) renders classic
  // boxed tab handles on a secondary-background bar; 'underline' renders
  // flat text tabs with a primary underline on the active tab.
  readonly variant?: 'card' | 'underline';
  // Whether the tabs can be reordered by dragging them.
  readonly reorderable?: boolean;
  // Called when a tab is dragged to a new position (only when `reorderable`).
  // `from` is the index of the dragged tab and `to` is the index it should end
  // up at, i.e. remove the tab at `from` then insert it at `to`. Indices only
  // count `TabStrip.Tab` children. Not called if the tab is dropped in place.
  readonly onReorder?: (from: number, to: number) => void;
}

export interface TabStripTabAttrs {
  // Style this tab as the active tab.
  readonly active?: boolean;
  // Style this tab as a disabled tab and prevent interaction.
  readonly disabled?: boolean;
  // If provided, the tab will be rendered as a link with this href.
  readonly href?: string;
  // Additional class name for the tab.
  readonly className?: string;
  // Icon to display on the left side of the tab title.
  readonly leftIcon?: string | m.Children;
  // Icon to display on the right side of the tab title.
  readonly rightIcon?: string | m.Children;
  // Whether to show a close button on the tab.
  readonly closeButton?: boolean;
  // Called when the tab's close button is clicked.
  readonly onClose?: () => void;
  // Optional menu items to show in a dropdown menu on the tab.
  // When provided, a menu button appears on hover.
  readonly menuItems?: m.Children;
  // If provided, the tab can be renamed inline by double-clicking it. Called
  // with the new (trimmed, non-empty) name when the rename is committed
  // (Enter or blur). Pressing Escape cancels without calling this.
  readonly onRename?: (newName: string) => void;
  readonly onClick?: () => void;
}

class Tab implements m.ClassComponent<TabStripTabAttrs> {
  // Inline rename state. `renameValue` is only meaningful while `renaming`.
  private renaming = false;
  private renameValue = '';

  view({attrs, children}: m.CVnode<TabStripTabAttrs>): m.Children {
    const {
      active,
      className,
      leftIcon,
      rightIcon,
      closeButton,
      onClose,
      menuItems,
      onRename,
      disabled,
      href,
      onClick,
    } = attrs;

    const renderIcon = (
      icon: string | m.Children | undefined,
      iconClassName: string,
    ) => {
      if (icon === undefined) {
        return undefined;
      }
      if (typeof icon === 'string') {
        return m(Icon, {icon, className: iconClassName});
      }
      return m('.pf-tab-strip__tab-icon', {className: iconClassName}, icon);
    };

    const tag = href ? 'a' : 'button';

    const commitRename = () => {
      if (!this.renaming) return;
      this.renaming = false;
      const newName = this.renameValue.trim();
      if (newName) {
        onRename?.(newName);
      }
    };

    const cancelRename = () => {
      this.renaming = false;
    };

    return m(
      tag + '.pf-tab-strip__tab',
      {
        'tabIndex': disabled ? -1 : 0,
        'className': classNames(
          className,
          active && 'pf-tab-strip__tab--active',
          disabled && 'pf-tab-strip__tab--disabled',
        ),
        'ondblclick': (e: PointerEvent) => {
          if (onRename && !this.renaming) {
            // Seed the input with the currently rendered title text.
            const titleEl = (e.currentTarget as HTMLElement).querySelector(
              '.pf-tab-strip__tab-title',
            );
            this.renameValue = titleEl?.textContent ?? '';
            this.renaming = true;
          }
        },
        'onauxclick': onClose,
        'onclick': onClick,
        // A disabled link drops its href so it can't be followed, and a
        // disabled button uses the native disabled attribute.
        'href': disabled ? undefined : href,
        'disabled': tag === 'button' ? disabled : undefined,
        'aria-disabled': disabled ? 'true' : undefined,
      },
      [
        renderIcon(leftIcon, 'pf-tab-strip__tab-icon--left'),
        this.renaming
          ? m('input.pf-tab-strip__tab-rename-input', {
              value: this.renameValue,
              oncreate: (vnode: m.VnodeDOM) => {
                const el = vnode.dom as HTMLInputElement;
                el.focus();
                el.select();
              },
              oninput: (e: InputEvent) => {
                const target = e.target as HTMLInputElement;
                this.renameValue = target.value;
              },
              onkeydown: (e: KeyboardEvent) => {
                if (e.key === 'Enter') {
                  commitRename();
                  e.preventDefault();
                } else if (e.key === 'Escape') {
                  cancelRename();
                  e.preventDefault();
                }
                e.stopPropagation();
              },
              onblur: commitRename,
              onclick: (e: Event) => e.stopPropagation(),
            })
          : m('span.pf-tab-strip__tab-title', children),
        renderIcon(rightIcon, 'pf-tab-strip__tab-icon--right'),
        menuItems !== undefined &&
          m(
            PopupMenu,
            {
              trigger: m(Button, {
                rounded: true,
                icon: Icons.ContextMenuAlt,
                className: 'pf-tab-strip__tab-btn pf-tab-strip__tab-menu-btn',
              }),
              position: PopupPosition.Bottom,
            },
            menuItems,
          ),
        closeButton &&
          m(Button, {
            rounded: true,
            icon: Icons.Close,
            className: 'pf-tab-strip__tab-btn',
            onclick: (e: Event) => {
              e.stopPropagation();
              onClose?.();
            },
          }),
      ],
    );
  }
}

/**
 * A horizontal tab bar. Tabs are passed as children using the
 * `TabStrip.Tab` sub-component:
 *
 * ```ts
 * m(
 *   TabStrip,
 *   m(TabStrip.Tab, {active: true, onclick: () => {}}, 'Content'),
 *   m(TabStrip.Tab, {onclick: () => {}}, 'Other'),
 * );
 * ```
 */
export class TabStrip implements m.ClassComponent<TabStripAttrs> {
  static readonly Tab = Tab;

  // Drag state for reordering. Indices count TabStrip.Tab children only.
  private dragIndex?: number;
  private dropIndex?: number;
  private dropPosition?: 'before' | 'after';

  view({attrs, children}: m.CVnode<TabStripAttrs>): m.Children {
    const {className, variant = 'card', reorderable, onReorder} = attrs;
    const tabs = reorderable
      ? this.withReorderAttrs(children, {index: 0}, onReorder)
      : children;
    return m(
      '.pf-tab-strip',
      {
        className: classNames(className, variantToClassName(variant)),
      },
      m('.pf-tab-strip__tabs', tabs),
    );
  }

  private resetDrag() {
    this.dragIndex = undefined;
    this.dropIndex = undefined;
    this.dropPosition = undefined;
  }

  // Walks the children, preserving their (possibly nested) structure, and
  // re-creates each TabStrip.Tab vnode with drag handlers and drop-indicator
  // classes attached. Other children are passed through untouched.
  private withReorderAttrs(
    children: m.Children,
    counter: {index: number},
    onReorder: TabStripAttrs['onReorder'],
  ): m.Children {
    if (Array.isArray(children)) {
      return children.map((child) =>
        this.withReorderAttrs(child, counter, onReorder),
      );
    }
    if (
      children === null ||
      typeof children !== 'object' ||
      (children as m.Vnode).tag !== Tab
    ) {
      return children;
    }

    const vnode = children as m.Vnode<TabStripTabAttrs>;
    const index = counter.index++;
    const isDragging = this.dragIndex === index;
    const isDropTarget =
      this.dropIndex === index && this.dragIndex !== undefined && !isDragging;

    return m(
      Tab,
      {
        ...vnode.attrs,
        key: vnode.key,
        className: classNames(
          vnode.attrs.className,
          isDragging && 'pf-tab-strip__tab--dragging',
          isDropTarget &&
            this.dropPosition === 'before' &&
            'pf-tab-strip__tab--drop-before',
          isDropTarget &&
            this.dropPosition === 'after' &&
            'pf-tab-strip__tab--drop-after',
        ),
        draggable: true,
        ondragstart: (e: DragEvent) => {
          // Some browsers won't start a drag without data set.
          e.dataTransfer?.setData('text/plain', String(index));
          this.dragIndex = index;
        },
        ondragover: (e: DragEvent) => {
          // Ignore drags that didn't start from this strip.
          if (this.dragIndex === undefined) return;
          e.preventDefault();
          const rect = (e.currentTarget as HTMLElement).getBoundingClientRect();
          this.dropIndex = index;
          this.dropPosition =
            e.clientX < rect.left + rect.width / 2 ? 'before' : 'after';
        },
        ondragleave: (e: DragEvent) => {
          const target = e.currentTarget as HTMLElement;
          const related = e.relatedTarget as Node | null;
          if (related === null || !target.contains(related)) {
            this.dropIndex = undefined;
            this.dropPosition = undefined;
          }
        },
        ondrop: (e: DragEvent) => {
          e.preventDefault();
          const from = this.dragIndex;
          if (from !== undefined) {
            const insertBefore =
              this.dropPosition === 'before' ? index : index + 1;
            // Account for the dragged tab being removed before re-insertion.
            const to = insertBefore > from ? insertBefore - 1 : insertBefore;
            if (to !== from) {
              onReorder?.(from, to);
            }
          }
          this.resetDrag();
        },
        ondragend: () => this.resetDrag(),
      },
      vnode.children as m.Children,
    );
  }
}

function variantToClassName(variant: 'card' | 'underline'): string {
  switch (variant) {
    case 'card':
      return 'pf-tab-strip--card';
    case 'underline':
      return 'pf-tab-strip--underline';
    default:
      assertUnreachable(variant);
  }
}
